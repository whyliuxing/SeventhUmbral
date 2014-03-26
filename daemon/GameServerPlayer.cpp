#include <stdio.h>
#include <map>
#include <thread>
#include "GameServerPlayer.h"
#include "DatabaseConnectionManager.h"
#include "PacketUtils.h"
#include "GameServer_Login.h"
#include "GameServer_MoveOutOfRoom.h"
#include "GameServer_Ingame.h"
#include "Character.h"
#include "PathUtils.h"
#include "Log.h"
#include "AppConfig.h"
#include "string_format.h"

#include "packets/SetInitialPositionPacket.h"
#include "packets/SetWeatherPacket.h"
#include "packets/SetMusicPacket.h"
#include "packets/SetMapPacket.h"
#include "packets/SetInventoryPacket.h"
#include "packets/SetActorStatePacket.h"
#include "packets/SetActorPropertyPacket.h"
#include "packets/BattleActionPacket.h"
#include "packets/DisplayMessagePacket.h"
#include "packets/CompositePacket.h"

#define LOG_NAME "GameServerPlayer"

#define PLAYER_ID	0x029B2941

#define PLAYER_AUTO_ATTACK_DELAY	0x100
#define ENEMY_AUTO_ATTACK_DELAY		0x180

#define ENEMY_INITIAL_HP			0x100

#define ACTOR_ID_ENEMY_1			0x44D8002D
#define ACTOR_ID_ENEMY_2			0x45100D56
#define ACTOR_ID_ENEMY_3			0x46D8002D
#define ACTOR_ID_ENEMY_4			0x47D8002D

#define INITIAL_POSITION_GRIDANIA_INN	   58.92f,   4.00f, -1219.07f, 0.52f
#define INITIAL_POSITION_MOR_DHONA		 -208.08f,  19.00f,  -669.79f, 0.00f
#define INITIAL_POSITION_COERTHAS		  219.59f, 302.00f,  -246.00f, 0.00f
#define INITIAL_POSITION_NOSCEA			 1111.33f,  54.00f,  -456.08f, 0.00f
#define INITIAL_POSITION_THANALAN		 1247.79f, 264.10f,  -562.08f, 0.00f
#define INITIAL_POSITION_RIVENROAD		    0.00f,   0.00f,     0.00f, 0.00f
#define INITIAL_POSITION_LARGEBOAT		    0.00f,  15.00f,     0.00f, 0.00f
#define INITIAL_POSITION_SMALLBOAT		    0.00f,  15.00f,     0.00f, 0.00f

#define STRING_ID_YOUNG_RAPTOR		(0x2F4E33)
#define STRING_ID_ANTELOPE_DOE		(0x2F4E8D)
#define STRING_ID_STAR_MARMOT		(0x2F5D09)
#define STRING_ID_FOREST_FUNGUAR	(0x2F646D)
#define STRING_ID_IFRIT				(0x2F69E5)
#define STRING_ID_IXALI_FEARCALLER	(0x2F666B)

#define APPEARANCE_ID_MARMOT		(0x2906)
#define APPEARANCE_ID_FUNGUAR		(0x2914)
#define APPEARANCE_ID_ANTELOPE		(0x2714)
#define APPEARANCE_ID_IFRIT			(0x2A64)
#define APPEARANCE_ID_TITAN			(0x2A66)
#define APPEARANCE_ID_IXALI			(0x2A96)

CGameServerPlayer::CGameServerPlayer(SOCKET clientSocket)
: m_clientSocket(clientSocket)
, m_disconnect(false)
, m_alreadyMovedOutOfRoom(false)
, m_zoneMasterCreated(false)
{
	m_dbConnection = CDatabaseConnectionManager::GetInstance().CreateConnection();
}

CGameServerPlayer::~CGameServerPlayer()
{

}

bool CGameServerPlayer::IsConnected() const
{
	return !m_disconnect;
}

static PacketData GetMotd()
{
	std::vector<const char*> messages;
	messages.push_back("Welcome to the Seventh Umbral Server");
	messages.push_back("Compiled on: " __DATE__);
	messages.push_back("---------------");

	CCompositePacket outputPacket;

	for(auto message : messages)
	{
		CDisplayMessagePacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetMessage(message);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	return outputPacket.ToPacketData();
}

PacketData CGameServerPlayer::GetCharacterInfo()
{
	PacketData outgoingPacket(std::begin(g_client0_login8), std::end(g_client0_login8));

	{
		const uint32 setInitialPositionBase = 0x320;
		CSetInitialPositionPacket setInitialPosition;
		setInitialPosition.SetSourceId(PLAYER_ID);
		setInitialPosition.SetTargetId(PLAYER_ID);
		setInitialPosition.SetX(157.55f);
		setInitialPosition.SetY(0);
		setInitialPosition.SetZ(165.05f);
		setInitialPosition.SetAngle(-1.53f);
		auto setInitialPositionPacket = setInitialPosition.ToPacketData();

		memcpy(outgoingPacket.data() + setInitialPositionBase, setInitialPositionPacket.data(), setInitialPositionPacket.size());
	}

	CCharacter character;

	try
	{
		auto query = string_format("SELECT * FROM ffxiv_characters WHERE id = %d", m_characterId);
		auto result = m_dbConnection.Query(query.c_str());
		if(result.GetRowCount() != 0)
		{
			character = CCharacter(result);
		}
	}
	catch(const std::exception& exception)
	{
		CLog::GetInstance().LogError(LOG_NAME, "Failed to fetch character (id = %d): %s", m_characterId, exception.what());
		m_disconnect = true;
		return PacketData();
	}

	const uint32 characterInfoBase = 0x368;

	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x20]) = CCharacter::GetModelFromTribe(character.tribe);
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x28]) = character.size;
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x30]) = character.GetColorInfo();
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x38]) = character.GetFaceInfo();
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x40]) = character.hairStyle << 10;
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x48]) = character.voice;
//	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x50]) = 0xC901014;				//weapon 1
//	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x58]) = 0xE20040A;				//weapon 2
//	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x68]) = 0xDD0040B;				//weapon 3?

	//0xC901014 -> Bow
	//0xE20040A -> Quiver
	//0xDD0040B -> Arrow;

	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x88]) = character.headGear;		//headGear
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x90]) = character.bodyGear;		//bodyGear
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0x98]) = character.legsGear;		//legsGear
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0xA0]) = character.handsGear;	//handsGear
	*reinterpret_cast<uint32*>(&outgoingPacket[characterInfoBase + 0xA8]) = character.feetGear;		//feetGear

	//Insert character name
	for(unsigned int i = 0; i < character.name.size(); i++)
	{
		outgoingPacket[characterInfoBase + 0x14C + i] = character.name[i];
	}
	outgoingPacket[characterInfoBase + 0x14C + character.name.size()] = 0;

	return outgoingPacket;
}

static PacketData GetInventoryInfo()
{
	PacketData outgoingPacket;

	unsigned int itemCount = 0xC8;			//Item count can be less than 200, but we have to make sure no equipped items are using item indices over the itemCount
	while(itemCount != 0)
	{
		CCompositePacket compositePacket;
		{
			unsigned int itemsToCopy = std::min<unsigned int>(itemCount, 32);
			CSetInventoryPacket setInventoryPacket;
			setInventoryPacket.SetSourceId(PLAYER_ID);
			setInventoryPacket.SetTargetId(PLAYER_ID);
			setInventoryPacket.SetItemCount(itemsToCopy);
			setInventoryPacket.SetItemBase(itemCount - itemsToCopy);
			compositePacket.AddPacket(setInventoryPacket.ToPacketData());
			itemCount -= itemsToCopy;
		}
		auto compositePacketData = compositePacket.ToPacketData();
		outgoingPacket.insert(std::end(outgoingPacket), std::begin(compositePacketData), std::end(compositePacketData));
	}

	return outgoingPacket;
}

PacketData CGameServerPlayer::SpawnNpc(uint32 id, uint32 appearanceId, uint32 stringId, float x, float y, float z, float angle)
{
	PacketData outgoingPacket(std::begin(g_spawnNpc), std::end(g_spawnNpc));

	uint32 packetIndex = 0x10;
	while(packetIndex < outgoingPacket.size())
	{
		auto packetPtr = outgoingPacket.data() + packetIndex;
		uint16 subPacketSize = *reinterpret_cast<uint16*>(packetPtr + 0x00);
		uint16 subPacketCmd = *reinterpret_cast<uint16*>(packetPtr + 0x12);
		*reinterpret_cast<uint32*>(packetPtr + 0x4) = id;
		if(subPacketCmd == 0xCE || subPacketCmd == 0xCF)
		{
			*reinterpret_cast<float*>(packetPtr + 0x28) = x;
			*reinterpret_cast<float*>(packetPtr + 0x2C) = y;
			*reinterpret_cast<float*>(packetPtr + 0x30) = z;
			*reinterpret_cast<float*>(packetPtr + 0x34) = angle;
		}
		else if(subPacketCmd == 0xCC)
		{
			static int magicNumber = 0;
			*reinterpret_cast<uint8*>(packetPtr + 0x40) = 0x30 + magicNumber;
			magicNumber++;
		}
		else if(subPacketCmd == 0xD6)
		{
			*reinterpret_cast<uint32*>(packetPtr + 0x20) = appearanceId;
		}
		else if(subPacketCmd == 0x13D)
		{
			*reinterpret_cast<uint32*>(packetPtr + 0x20) = stringId;
		}
		packetIndex += subPacketSize;
	}
	assert(packetIndex == outgoingPacket.size());
	return outgoingPacket;
}

void CGameServerPlayer::QueuePacket(const PacketData& packet)
{
	m_packetQueue.push_back(packet);
}

void CGameServerPlayer::Update()
{
	//Write to socket
	{
		int totalSent = 0;
		while(!m_packetQueue.empty())
		{
			if(totalSent >= 0x1000)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				break;
			}
			const auto& nextPacket = m_packetQueue.front();
			int sent = send(m_clientSocket, reinterpret_cast<const char*>(nextPacket.data()), nextPacket.size(), 0);
			if(sent != nextPacket.size())
			{
				CLog::GetInstance().LogError(LOG_NAME, "Failed to send packet to client. Disconnecting.\r\n");
				m_disconnect = true;
				return;
			}
			m_packetQueue.pop_front();
			totalSent += sent;
		}
	}

	//Read from socket
	{
		static const unsigned int maxPacketSize = 0x10000;
		uint8 incomingPacket[maxPacketSize];
		int read = recv(m_clientSocket, reinterpret_cast<char*>(incomingPacket), maxPacketSize, 0);
		if(read == 0)
		{
			//Client disconnected
			CLog::GetInstance().LogMessage(LOG_NAME, "Client disconnected.");
			m_disconnect = true;
			return;
		}
		if(read > 0)
		{
			m_incomingStream.Write(incomingPacket, read);
		}
	}

	if(CPacketUtils::HasPacket(m_incomingStream))
	{
		auto incomingPacket = CPacketUtils::ReadPacket(m_incomingStream);
		if(incomingPacket.size() == 0)
		{
			CLog::GetInstance().LogError(LOG_NAME, "Failed to read packet. Disconnecting.\r\n");
			m_disconnect = true;
			return;
		}

		unsigned int clientId = incomingPacket[0x02];
		auto subPackets = CPacketUtils::SplitPacket(incomingPacket);

		for(const auto& subPacket : subPackets)
		{
			uint16 commandId = CPacketUtils::GetSubPacketCommand(subPacket);
			switch(commandId)
			{
			case 0x0000:
				ProcessInitialHandshake(clientId, subPacket);
				break;
			case 0x0001:
				ProcessKeepAlive(subPacket);
				break;
			case 0x0003:
				ProcessChat(subPacket);
				break;
			case 0x00CA:
				ProcessSetPlayerPosition(subPacket);
				break;
			case 0x00CD:
				ProcessSetSelection(subPacket);
				break;
			case 0x012D:
				ProcessScriptCommand(subPacket);
				break;
			case 0x012E:
				ProcessScriptResult(subPacket);
				break;
			default:
				CLog::GetInstance().LogDebug(LOG_NAME, "Unknown command 0x%0.4X received.", commandId);
				break;
			}
		}
	}

	//Pseudo Simulation
	if(m_isActiveMode && m_lockOnId != EMPTY_LOCKON_ID)
	{
		m_playerAutoAttackTimer--;
		if(m_playerAutoAttackTimer < 0)
		{
			CCompositePacket outputPacket;

			static const uint32 autoAttackDamage = 10;

			{
				CBattleActionPacket packet;
				packet.SetSourceId(PLAYER_ID);
				packet.SetTargetId(PLAYER_ID);
				packet.SetActionSourceId(PLAYER_ID);
				packet.SetActionTargetId(m_lockOnId);
				packet.SetAnimationId(CBattleActionPacket::ANIMATION_PLAYER_ATTACK);
				packet.SetDescriptionId(CBattleActionPacket::DESCRIPTION_PLAYER_ATTACK);
				packet.SetDamageType(CBattleActionPacket::DAMAGE_NORMAL);
				packet.SetDamage(autoAttackDamage);
				packet.SetFeedbackId(CBattleActionPacket::FEEDBACK_NORMAL);
				packet.SetAttackSide(CBattleActionPacket::SIDE_FRONT);
				outputPacket.AddPacket(packet.ToPacketData());
			}

			ProcessDamageToNpc(outputPacket, m_lockOnId, autoAttackDamage);

			QueuePacket(outputPacket.ToPacketData());

			m_playerAutoAttackTimer = PLAYER_AUTO_ATTACK_DELAY;
		}

		m_enemyAutoAttackTimer--;
		if(m_enemyAutoAttackTimer < 0)
		{
			CCompositePacket outputPacket;

			{
				CBattleActionPacket packet;
				packet.SetSourceId(m_lockOnId);
				packet.SetTargetId(PLAYER_ID);
				packet.SetActionSourceId(m_lockOnId);
				packet.SetActionTargetId(PLAYER_ID);
				packet.SetAnimationId(CBattleActionPacket::ANIMATION_ENEMY_ATTACK);
				packet.SetDescriptionId(CBattleActionPacket::DESCRIPTION_ENEMY_ATTACK);
				packet.SetDamageType(CBattleActionPacket::DAMAGE_NORMAL);
				packet.SetDamage(0);
				packet.SetFeedbackId(CBattleActionPacket::FEEDBACK_NORMAL);
				packet.SetAttackSide(CBattleActionPacket::SIDE_FRONT);
				outputPacket.AddPacket(packet.ToPacketData());
			}

			QueuePacket(outputPacket.ToPacketData());

			m_enemyAutoAttackTimer = ENEMY_AUTO_ATTACK_DELAY;
		}
	}
}

void CGameServerPlayer::PrepareInitialPackets()
{
	QueuePacket(PacketData(std::begin(g_client0_login1), std::end(g_client0_login1)));
	QueuePacket(PacketData(std::begin(g_client0_login2), std::end(g_client0_login2)));
	QueuePacket(PacketData(std::begin(g_client0_login3), std::end(g_client0_login3)));
	QueuePacket(PacketData(std::begin(g_client0_login4), std::end(g_client0_login4)));
	QueuePacket(GetMotd());
	QueuePacket(PacketData(std::begin(g_client0_login7), std::end(g_client0_login7)));
	QueuePacket(GetCharacterInfo());
	QueuePacket(GetInventoryInfo());
	QueuePacket(PacketData(std::begin(g_client0_login11), std::end(g_client0_login11)));
	QueuePacket(PacketData(std::begin(g_client0_login12), std::end(g_client0_login12)));
	QueuePacket(PacketData(std::begin(g_client0_login13), std::end(g_client0_login13)));
	QueuePacket(PacketData(std::begin(g_client0_login14), std::end(g_client0_login14)));
//	QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_1, APPEARANCE_ID_MARMOT, STRING_ID_STAR_MARMOT, 163.81f, 0, 154.39f, 0)));
//	QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_2, APPEARANCE_ID_MARMOT, STRING_ID_STAR_MARMOT, 162.81f, 0, 154.39f, 0)));
//	QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_3, APPEARANCE_ID_MARMOT, STRING_ID_STAR_MARMOT, 161.81f, 0, 154.39f, 0)));
//	QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_4, APPEARANCE_ID_MARMOT, STRING_ID_STAR_MARMOT, 160.81f, 0, 154.39f, 0)));
//	m_npcHp[ACTOR_ID_ENEMY_1] = ENEMY_INITIAL_HP;
//	m_npcHp[ACTOR_ID_ENEMY_2] = ENEMY_INITIAL_HP;
//	m_npcHp[ACTOR_ID_ENEMY_3] = ENEMY_INITIAL_HP;
//	m_npcHp[ACTOR_ID_ENEMY_4] = ENEMY_INITIAL_HP;
}

void CGameServerPlayer::ProcessInitialHandshake(unsigned int clientId, const PacketData& subPacket)
{
	if(m_sentInitialHandshake) return;

	const char* characterIdString = reinterpret_cast<const char*>(subPacket.data() + 0x14);
	m_characterId = atoi(characterIdString);

	CLog::GetInstance().LogDebug(LOG_NAME, "Initial handshake for clientId = %d and characterId = 0x%0.8X", clientId, m_characterId);

	if(clientId == 1)
	{
		PrepareInitialPackets();
	}
	else if(clientId == 2)
	{
		QueuePacket(PacketData(std::begin(g_client1_login1), std::end(g_client1_login1)));
		QueuePacket(PacketData(std::begin(g_client1_login2), std::end(g_client1_login2)));
	}

	m_sentInitialHandshake = true;
}

void CGameServerPlayer::ProcessKeepAlive(const PacketData& subPacket)
{
	//Some keep alive thing? (only time is updated here)
	uint32 clientTime = *reinterpret_cast<const uint32*>(&subPacket[0x18]);
	uint32 moreTime = *reinterpret_cast<const uint32*>(&subPacket[0x20]);

	uint8 keepAlivePacket[0x50] =
	{
		0x01, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x00, 0xEF, 0xCB, 0xA4, 0xEE, 0x3B, 0x01, 0x00, 0x00,
		0x40, 0x00, 0x03, 0x00, 0x41, 0x29, 0x9b, 0x02, 0x41, 0x29, 0x9b, 0x02, 0x00, 0xe0, 0xd2, 0xfe,
		0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcb, 0xee, 0xe0, 0x50, 0x00, 0x00, 0x00, 0x00,
		0x4a, 0x18, 0x9c, 0x0a, 0x4d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	*reinterpret_cast<uint32*>(&keepAlivePacket[0x28]) = clientTime;
	*reinterpret_cast<uint32*>(&keepAlivePacket[0x30]) = moreTime;

	QueuePacket(PacketData(std::begin(keepAlivePacket), std::end(keepAlivePacket)));
}

void CGameServerPlayer::ProcessChat(const PacketData& subPacket)
{
	const char* chatText = reinterpret_cast<const char*>(subPacket.data() + 0x3C);

	static std::map<std::string, uint32> weatherCommands;
	if(weatherCommands.empty())
	{
		weatherCommands["weather_clear"]		= CSetWeatherPacket::WEATHER_CLEAR;
		weatherCommands["weather_fine"]			= CSetWeatherPacket::WEATHER_FINE;
		weatherCommands["weather_cloudy"]		= CSetWeatherPacket::WEATHER_CLOUDY;
		weatherCommands["weather_foggy"]		= CSetWeatherPacket::WEATHER_FOGGY;
		weatherCommands["weather_blustery"]		= CSetWeatherPacket::WEATHER_BLUSTERY;
		weatherCommands["weather_rainy"]		= CSetWeatherPacket::WEATHER_RAINY;
		weatherCommands["weather_stormy"]		= CSetWeatherPacket::WEATHER_STORMY;
		weatherCommands["weather_sandy"]		= CSetWeatherPacket::WEATHER_SANDY;
		weatherCommands["weather_gloomy"]		= CSetWeatherPacket::WEATHER_GLOOMY;
		weatherCommands["weather_dalamud"]		= CSetWeatherPacket::WEATHER_DALAMUD;
	}

	auto weatherCommandIterator = weatherCommands.find(chatText);
	if(weatherCommandIterator != std::end(weatherCommands))
	{
		CCompositePacket result;
		
		{
			CSetWeatherPacket packet;
			packet.SetSourceId(PLAYER_ID);
			packet.SetTargetId(PLAYER_ID);
			packet.SetWeatherId(weatherCommandIterator->second);
			result.AddPacket(packet.ToPacketData());
		}

		QueuePacket(result.ToPacketData());
	}
	else if(!strcmp(chatText, "teleport_mordhona"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_MORDHONA, CSetMusicPacket::MUSIC_MORDHONA, INITIAL_POSITION_MOR_DHONA);
	}
	else if(!strcmp(chatText, "teleport_coerthas"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_COERTHAS, CSetMusicPacket::MUSIC_COERTHAS, INITIAL_POSITION_COERTHAS);
	}
	else if(!strcmp(chatText, "teleport_thanalan"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_THANALAN, CSetMusicPacket::MUSIC_THANALAN, INITIAL_POSITION_THANALAN);
	}
	else if(!strcmp(chatText, "teleport_lanoscea"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_NOSCEA, CSetMusicPacket::MUSIC_NOSCEA, INITIAL_POSITION_NOSCEA);
	}
	else if(!strcmp(chatText, "teleport_gridania"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_BLACKSHROUD, CSetMusicPacket::MUSIC_GRIDANIA, INITIAL_POSITION_GRIDANIA_INN);
	}
	else if(!strcmp(chatText, "teleport_rivenroad"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_RIVENROAD, CSetMusicPacket::MUSIC_MORDHONA, INITIAL_POSITION_RIVENROAD);
	}
	else if(!strcmp(chatText, "teleport_largeboat"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_LARGEBOAT, CSetMusicPacket::MUSIC_NOSCEA, INITIAL_POSITION_LARGEBOAT);
	}
	else if(!strcmp(chatText, "teleport_smallboat"))
	{
		SendTeleportSequence(CSetMapPacket::MAP_SMALLBOAT, CSetMusicPacket::MUSIC_NOSCEA, INITIAL_POSITION_SMALLBOAT);
	}
	else if(!strcmp(chatText, "ride_chocobo"))
	{
		QueuePacket(PacketData(std::begin(g_chocoboRider1), std::end(g_chocoboRider1)));
		QueuePacket(PacketData(std::begin(g_chocoboRider2), std::end(g_chocoboRider2)));
	}
//	printf("%s\r\n", chatText);
}

void CGameServerPlayer::ProcessSetPlayerPosition(const PacketData& subPacket)
{
	//Some keep alive thing?
	uint32 clientTime = *reinterpret_cast<const uint32*>(&subPacket[0x18]);
	float posX = *reinterpret_cast<const float*>(&subPacket[0x28]);
	float posY = *reinterpret_cast<const float*>(&subPacket[0x2C]);
	float posZ = *reinterpret_cast<const float*>(&subPacket[0x30]);

//	printf("%s: Keeping Alive. Time: 0x%0.8X, Pos: (X: %f, Y: %f, Z: %f).\r\n",
//		LOG_NAME, clientTime, posX, posY, posZ);
}

void CGameServerPlayer::ProcessSetSelection(const PacketData& subPacket)
{
	uint32 selectedId = *reinterpret_cast<const uint32*>(&subPacket[0x20]);
	uint32 lockOnId = *reinterpret_cast<const uint32*>(&subPacket[0x24]);

	CLog::GetInstance().LogDebug(LOG_NAME, "Selected Id: 0x%0.8X, Lock On Id: 0x%0.8X", selectedId, lockOnId);

	m_lockOnId = lockOnId;
}

void CGameServerPlayer::ProcessScriptCommand(const PacketData& subPacket)
{
	uint32 clientTime = *reinterpret_cast<const uint32*>(&subPacket[0x18]);
	uint32 sourceId = *reinterpret_cast<const uint32*>(&subPacket[0x20]);
	uint32 targetId = *reinterpret_cast<const uint32*>(&subPacket[0x24]);
	const char* commandName = reinterpret_cast<const char*>(subPacket.data()) + 0x31;

	CLog::GetInstance().LogDebug(LOG_NAME, "ProcessScriptCommand: %s Source Id = 0x%0.8X, Target Id = 0x%0.8X.", commandName, sourceId, targetId);

	if(!strcmp(commandName, "commandRequest"))
	{
		//commandRequest (emote, changing equipment, ...)

		switch(targetId)
		{
		case 0xA0F02EE9:
			ScriptCommand_EquipItem(subPacket, clientTime);
			break;
		case 0xA0F05E26:
			ScriptCommand_Emote(subPacket, clientTime);
			break;
		case 0xA0F05EA2:
			ScriptCommand_TrashItem(subPacket, clientTime);
			break;
		default:
			CLog::GetInstance().LogDebug(LOG_NAME, "Unknown target id (0x%0.8X).", targetId);
			break;
		}
	}
	else if(!strcmp(commandName, "commandContent"))
	{
		switch(targetId)
		{
		case 0xA0F05E9B:
			//Quit
			CLog::GetInstance().LogDebug(LOG_NAME, "Quit.");
			m_disconnect = true;
			break;
		case 0xA0F05E9C:
			//Teleport
			CLog::GetInstance().LogDebug(LOG_NAME, "Teleport.");
			m_disconnect = true;
			break;
		}
	}
	else if(!strcmp(commandName, "commandForced"))
	{
		CCompositePacket packet;

		switch(targetId)
		{
		case 0xA0F05209:
			ScriptCommand_SwitchToActiveMode(packet);
			break;
		case 0xA0F0520A:
			ScriptCommand_SwitchToPassiveMode(packet);
			break;
		default:
			CLog::GetInstance().LogDebug(LOG_NAME, "Unknown commandForced target id (0x%0.8X).", targetId);
			break;
		}

		packet.AddPacket(PacketData(std::begin(g_endCommandForcedPacket), std::end(g_endCommandForcedPacket)));
		QueuePacket(packet.ToPacketData());
	}
	else if(!strcmp(commandName, "commandDefault"))
	{
		CCompositePacket packet;

		static unsigned int descriptionId = 0x08106A30;

		switch(targetId)
		{
		case 0xA0F06A36:	//Heavy Swing
			ScriptCommand_BattleSkill(packet, CBattleActionPacket::ANIMATION_HEAVY_SWING, CBattleActionPacket::DESCRIPTION_HEAVY_SWING, 20);
			break;
		case 0xA0F06A37:	//Skull Sunder
			ScriptCommand_BattleSkill(packet, CBattleActionPacket::ANIMATION_SKULL_SUNDER, CBattleActionPacket::DESCRIPTION_SKULL_SUNDER, 30);
			break;
		case 0xA0F06A39:	//Brutal Swing
			ScriptCommand_BattleSkill(packet, CBattleActionPacket::ANIMATION_SAVAGE_BLADE, CBattleActionPacket::DESCRIPTION_BRUTAL_SWING, 40);
			break;
		case 0xA0F06A3E:	//Fracture
			ScriptCommand_BattleSkill(packet, CBattleActionPacket::ANIMATION_FRACTURE, CBattleActionPacket::DESCRIPTION_FRACTURE, 50);
			break;
		default:
			CLog::GetInstance().LogDebug(LOG_NAME, "Unknown commandDefault target id (0x%0.8X).", targetId);
			break;
		}

		packet.AddPacket(PacketData(std::begin(g_endCommandDefaultPacket), std::end(g_endCommandDefaultPacket)));
		QueuePacket(packet.ToPacketData());
	}
	else if(!strcmp(commandName, "talkDefault"))
	{
		switch(targetId)
		{
		case 0x47A00007:
			//Talking to the door inside the room
			{
				static const uint8 commandRequestPacket[] =
				{
					0x01, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x01, 0x00, 0x52, 0xE2, 0xA4, 0xEE, 0x3B, 0x01, 0x00, 0x00,
					0xb0, 0x00, 0x03, 0x00, 0x41, 0x29, 0x9b, 0x02, 0x41, 0x29, 0x9b, 0x02, 0x00, 0xe0, 0xd2, 0xfe,
					0x14, 0x00, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0xd3, 0xe9, 0xe0, 0x50, 0x00, 0x00, 0x00, 0x00,
					0x41, 0x29, 0x9b, 0x02, 0x07, 0x00, 0xa0, 0x47, 0x01, 0x74, 0x61, 0x6c, 0x6b, 0x44, 0x65, 0x66,
					0x61, 0x75, 0x6c, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x65, 0x6c, 0x65, 0x67, 0x61, 0x74,
					0x65, 0x45, 0x76, 0x65, 0x6e, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x02, 0x9b, 0x29, 0x41, 0x06, 0xa0,
					0xf1, 0xaf, 0xcd, 0x02, 0x64, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0x54, 0x61, 0x6c, 0x6b, 0x57,
					0x69, 0x74, 0x68, 0x49, 0x6e, 0x6e, 0x5f, 0x45, 0x78, 0x69, 0x74, 0x44, 0x6f, 0x6f, 0x72, 0x00,
					0x05, 0x05, 0x05, 0x05, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0xe8, 0x4e, 0x40, 0x00, 0x00, 0x00,
				};

				QueuePacket(PacketData(std::begin(commandRequestPacket), std::end(commandRequestPacket)));
			}
			break;
		default:
#if 0
			//Talking Test (doesn't work)
			{
				static const uint8 commandRequestPacket[] =
				{
					0x01, 0x01, 0x00, 0x00, 0xC0, 0x00, 0x01, 0x00, 0xD2, 0x16, 0x9E, 0xEE, 0x3B, 0x01, 0x00, 0x00, 
					0xB0, 0x00, 0x03, 0x00, 0x41, 0x29, 0x9B, 0x02, 0x41, 0x29, 0x9B, 0x02, 0x00, 0xE0, 0xD2, 0xFE, 
					0x14, 0x00, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x14, 0xED, 0xE0, 0x50, 0x00, 0x00, 0x00, 0x00, 
					0x41, 0x29, 0x9B, 0x02, 0x82, 0x00, 0x70, 0x46, 0x01, 0x74, 0x61, 0x6C, 0x6B, 0x44, 0x65, 0x66, 
					0x61, 0x75, 0x6C, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x77, 0x69, 0x74, 0x63, 0x68, 0x45, 
					0x76, 0x65, 0x6E, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xA0, 0xF1, 0xAF, 0xCD, 0x06, 0xA0, 
					0xF1, 0xB4, 0x00, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 
					0x00, 0x00, 0x03, 0xF1, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6C, 0xB8, 0x45, 0x40, 0x00, 0x00, 0x00, 
				};

				QueuePacket(PacketData(std::begin(commandRequestPacket), std::end(commandRequestPacket)));
			}
#endif
			m_disconnect = true;
			break;
		}
	}
	else
	{
		//Anything else will probably crash, so just bail
		m_disconnect = true;
	}
}

void CGameServerPlayer::ScriptCommand_EquipItem(const PacketData& subPacket, uint32 clientTime)
{
	CLog::GetInstance().LogDebug(LOG_NAME, "EquipItem");

//	uint32 itemId = *reinterpret_cast<const uint32*>(&subPacket[0x6E]);

//	CLog::GetInstance().LogDebug(LOG_NAME, "Equipping Item: 0x%0.8X", itemId);
	
//	CCompositePacket packet;
//	packet.AddPacket(PacketData(std::begin(unknownPacket1), std::end(unknownPacket1)));
//	packet.AddPacket(PacketData(std::begin(changeAppearancePacket), std::end(changeAppearancePacket)));
//	packet.AddPacket(PacketData(std::begin(unknownPacket), std::end(unknownPacket)));
//	QueuePacket(packet.ToPacketData());
}

void CGameServerPlayer::ScriptCommand_Emote(const PacketData& subPacket, uint32 clientTime)
{
	uint8 emoteId = subPacket[0x55];

	CLog::GetInstance().LogDebug(LOG_NAME, "Executing Emote 0x%0.2X", emoteId);

	uint8 commandRequestPacket[0x40] =
	{
		0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x01, 0x00, 0x52, 0xE2, 0xA4, 0xEE, 0x3B, 0x01, 0x00, 0x00,
		0x30, 0x00, 0x03, 0x00, 0x41, 0x29, 0x9b, 0x02, 0x41, 0x29, 0x9b, 0x02, 0x00, 0xe0, 0xd2, 0xfe,
		0x14, 0x00, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd1, 0xee, 0xe0, 0x50, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xb0, 0x00, 0x05, 0x41, 0x29, 0x9b, 0x02, 0x6e, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	//In: 0x6F, Out: (0x0500B000, 0x526E) -> Dance
	//In: 0x??, Out: (0x5000C000, 0x????) -> Angry Pointing
	//In: 0x??, Out: (0x5000D000, 0x????) -> Snooze
	//In: 0x??, Out: (0x5000E000, 0x????) -> Frustrated
	//In: 0x??, Out: (0x5000F000, 0x????) -> Military Sign
	//In: 0x??, Out: (0x50011000, 0x????) -> Shrug
	//In: 0x??, Out: (0x50012000, 0x????) -> Success Baby
	//In: 0x77, Out: (0x05013000, 0x52BE) -> Kneel
	//In: 0x??, Out: (0x50014000, 0x????) -> Chuckle
	//In: 0x??, Out: (0x50015000, 0x????) -> Laugh
	//In: 0x??, Out: (0x50016000, 0x????) -> Look
	//In: 0x??, Out: (0x50018000, 0x????) -> No
	//In: 0x??, Out: (0x50019000, 0x????) -> Never
					
	uint32 animationId = 0x0500B000;
	uint32 descriptionId = 0x526E;

	//Wrong emotes
	//gcsalute		-> grovel
	//grovel		-> serpent salute
	//blowkiss		-> disappointed
	//pray			-> firedance
	//airquote		-> pray
	//pose			-> blowkiss
	//happy			-> maelstorm salute
	//disappointed	-> pose

	if(emoteId >= 0x64 && emoteId < 0xA0)
	{
		animationId = 0x05000000 + ((emoteId - 0x64) << 12);
	}
/*
	switch(emoteId)
	{
	case 0x6A:		//Cheer
		animationId = 0x05006000;
		break;
	case 0x6F:		//Dance
		animationId = 0x0500B000;
		break;
	case 0x71:		//Doze
		animationId = 0x0500D000;
		break;
	case 0x75:		//Huh
		animationId = 0x05011000;
		break;
	case 0x78:		//Chuckle
		animationId = 0x05014000;
		break;
	case 0x79:		//Laugh
		animationId = 0x05015000;
		break;
	}
*/

	*reinterpret_cast<uint32*>(&commandRequestPacket[0x28]) = clientTime;
	*reinterpret_cast<uint32*>(&commandRequestPacket[0x30]) = animationId;
	*reinterpret_cast<uint32*>(&commandRequestPacket[0x38]) = descriptionId;

//	printf("Anim Id = 0x%0.8X, Desc Id = 0x%0.8X\r\n", animationId, descriptionId);
//	animationId += 0x1000;
//	descriptionId += 1;

	QueuePacket(PacketData(std::begin(commandRequestPacket), std::end(commandRequestPacket)));
}

void CGameServerPlayer::ScriptCommand_TrashItem(const PacketData& subPacket, uint32 clientTime)
{
	uint32 itemId = *reinterpret_cast<const uint32*>(&subPacket[0x6A]);
	CLog::GetInstance().LogDebug(LOG_NAME, "Trashing Item: 0x%0.8X", itemId);
}

void CGameServerPlayer::ScriptCommand_SwitchToActiveMode(CCompositePacket& outputPacket)
{
	{
		CSetActorStatePacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetState(CSetActorStatePacket::STATE_ACTIVE);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	{
		CSetActorPropertyPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.AddSetShort(CSetActorPropertyPacket::VALUE_TP, 3000);
		packet.AddTargetProperty("charaWork/stateAtQuicklyForAll");
		outputPacket.AddPacket(packet.ToPacketData());
	}

	{
		CBattleActionPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetActionSourceId(PLAYER_ID);
		packet.SetActionTargetId(PLAYER_ID);
		packet.SetAnimationId(CBattleActionPacket::ANIMATION_SHEATH_UNSHEATH);
		packet.SetDescriptionId(CBattleActionPacket::DESCRIPTION_ENTER_BATTLE);
		packet.SetFeedbackId(1);
		packet.SetAttackSide(CBattleActionPacket::SIDE_NORMAL);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	{
		CSetMusicPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetMusicId(CSetMusicPacket::MUSIC_BLACKSHROUD_BATTLE);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	m_isActiveMode = true;
	m_playerAutoAttackTimer = PLAYER_AUTO_ATTACK_DELAY;
	m_enemyAutoAttackTimer = ENEMY_AUTO_ATTACK_DELAY;
}

void CGameServerPlayer::ScriptCommand_SwitchToPassiveMode(CCompositePacket& outputPacket)
{
	{
		CSetActorStatePacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetState(CSetActorStatePacket::STATE_PASSIVE);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	{
		CBattleActionPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetActionSourceId(PLAYER_ID);
		packet.SetActionTargetId(PLAYER_ID);
		packet.SetAnimationId(CBattleActionPacket::ANIMATION_SHEATH_UNSHEATH);
		packet.SetDescriptionId(CBattleActionPacket::DESCRIPTION_LEAVE_BATTLE);
		packet.SetFeedbackId(1);
		packet.SetAttackSide(CBattleActionPacket::SIDE_NORMAL);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	{
		CSetMusicPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetMusicId(CSetMusicPacket::MUSIC_SHROUD);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	m_isActiveMode = false;
}

void CGameServerPlayer::ScriptCommand_BattleSkill(CCompositePacket& outputPacket, uint32 animationId, uint32 descriptionId, uint32 damage)
{
	{
		CBattleActionPacket packet;
		packet.SetSourceId(PLAYER_ID);
		packet.SetTargetId(PLAYER_ID);
		packet.SetActionSourceId(PLAYER_ID);
		packet.SetActionTargetId(m_lockOnId);
		packet.SetAnimationId(animationId);
		packet.SetDescriptionId(descriptionId);
		packet.SetDamageType(CBattleActionPacket::DAMAGE_NORMAL);
		packet.SetDamage(damage);
		packet.SetFeedbackId(CBattleActionPacket::FEEDBACK_NORMAL);
		packet.SetAttackSide(CBattleActionPacket::SIDE_FRONT);
		outputPacket.AddPacket(packet.ToPacketData());
	}

	ProcessDamageToNpc(outputPacket, m_lockOnId, damage);

	//Reset auto attack timer
	m_playerAutoAttackTimer = PLAYER_AUTO_ATTACK_DELAY;
}

void CGameServerPlayer::ProcessDamageToNpc(CCompositePacket& outputPacket, uint32 npcId, uint32 damage)
{
	auto npcHpIterator = m_npcHp.find(npcId);
	if(npcHpIterator == std::end(m_npcHp)) return;

	npcHpIterator->second = std::max<int32>(0, npcHpIterator->second - damage);

	{
		CSetActorPropertyPacket packet;
		packet.SetSourceId(npcId);
		packet.SetTargetId(PLAYER_ID);
		packet.AddSetShort(CSetActorPropertyPacket::VALUE_HP, npcHpIterator->second);
		packet.AddTargetProperty("charaWork/stateAtQuicklyForAll");
		outputPacket.AddPacket(packet.ToPacketData());
	}

	if(npcHpIterator->second == 0)
	{
		CSetActorStatePacket packet;
		packet.SetSourceId(m_lockOnId);
		packet.SetTargetId(PLAYER_ID);
		packet.SetState(CSetActorStatePacket::STATE_DEAD);
		outputPacket.AddPacket(packet.ToPacketData());
	}
}

void CGameServerPlayer::ProcessScriptResult(const PacketData& subPacket)
{
	uint32 someId1 = *reinterpret_cast<const uint32*>(&subPacket[0x2C]);
	uint32 someId2 = *reinterpret_cast<const uint32*>(&subPacket[0x30]);
	uint32 someId3 = *reinterpret_cast<const uint32*>(&subPacket[0x34]);

	CLog::GetInstance().LogDebug(LOG_NAME, "ProcessScriptResult: Id1 = 0x%0.8X, Id2 = 0x%0.8X, Id3 = 0x%0.8X.", someId1, someId2, someId3);

	if(!m_alreadyMovedOutOfRoom)
	{
		CLog::GetInstance().LogDebug(LOG_NAME, "Command 0x12E: Moving out of room");

		SendTeleportSequence(CSetMapPacket::MAP_BLACKSHROUD, CSetMusicPacket::MUSIC_GRIDANIA, INITIAL_POSITION_GRIDANIA_INN);

		m_alreadyMovedOutOfRoom = true;
	}
}

void CGameServerPlayer::SendTeleportSequence(uint32 levelId, uint32 musicId, float x, float y, float z, float angle)
{
	QueuePacket(PacketData(std::begin(g_client0_moor1), std::end(g_client0_moor1)));
	QueuePacket(PacketData(std::begin(g_client0_moor2), std::end(g_client0_moor2)));
	QueuePacket(PacketData(std::begin(g_client0_moor3), std::end(g_client0_moor3)));
	QueuePacket(PacketData(std::begin(g_client0_moor4), std::end(g_client0_moor4)));
	QueuePacket(PacketData(std::begin(g_client0_moor5), std::end(g_client0_moor5)));
	QueuePacket(PacketData(std::begin(g_client0_moor6), std::end(g_client0_moor6)));
	QueuePacket(PacketData(std::begin(g_client0_moor7), std::end(g_client0_moor7)));
	QueuePacket(PacketData(std::begin(g_client0_moor8), std::end(g_client0_moor8)));

	QueuePacket(PacketData(std::begin(g_client0_moor9), std::end(g_client0_moor9)));

	{
		CCompositePacket result;

		{
			CSetMusicPacket packet;
			packet.SetSourceId(PLAYER_ID);
			packet.SetTargetId(PLAYER_ID);
			packet.SetMusicId(musicId);
			result.AddPacket(packet.ToPacketData());
		}

		{
			CSetWeatherPacket packet;
			packet.SetSourceId(PLAYER_ID);
			packet.SetTargetId(PLAYER_ID);
			packet.SetWeatherId(CSetWeatherPacket::WEATHER_CLEAR);
			result.AddPacket(packet.ToPacketData());
		}

		{
			CSetMapPacket packet;
			packet.SetSourceId(PLAYER_ID);
			packet.SetTargetId(PLAYER_ID);
			packet.SetMapId(levelId);
			result.AddPacket(packet.ToPacketData());
		}

		QueuePacket(result.ToPacketData());
	}

	QueuePacket(PacketData(std::begin(g_client0_moor11), std::end(g_client0_moor11)));
	QueuePacket(PacketData(std::begin(g_client0_moor12), std::end(g_client0_moor12)));

	{
		PacketData outgoingPacket(std::begin(g_client0_moor13), std::end(g_client0_moor13));

		{
			const uint32 setInitialPositionBase = 0x360;

			CSetInitialPositionPacket setInitialPosition;
			setInitialPosition.SetSourceId(PLAYER_ID);
			setInitialPosition.SetTargetId(PLAYER_ID);
			setInitialPosition.SetX(x);
			setInitialPosition.SetY(y);
			setInitialPosition.SetZ(z);
			setInitialPosition.SetAngle(angle);
			auto setInitialPositionPacket = setInitialPosition.ToPacketData();

			memcpy(outgoingPacket.data() + setInitialPositionBase, setInitialPositionPacket.data(), setInitialPositionPacket.size());
		}

		QueuePacket(outgoingPacket);
	}

	QueuePacket(GetInventoryInfo());
	QueuePacket(PacketData(std::begin(g_client0_moor21), std::end(g_client0_moor21)));
	//QueuePacket(PacketData(std::begin(g_client0_moor22), std::end(g_client0_moor22)));
	
	if(!m_zoneMasterCreated)
	{
		//Zone Master
		QueuePacket(PacketData(std::begin(g_client0_moor23), std::end(g_client0_moor23)));

	/*
		QueuePacket(PacketData(std::begin(g_client0_moor24), std::end(g_client0_moor24)));
		QueuePacket(PacketData(std::begin(g_client0_moor25), std::end(g_client0_moor25)));

		QueuePacket(PacketData(std::begin(g_client0_moor26), std::end(g_client0_moor26)));
		QueuePacket(PacketData(std::begin(g_client0_moor27), std::end(g_client0_moor27)));
		QueuePacket(PacketData(std::begin(g_client0_moor28), std::end(g_client0_moor28)));
		QueuePacket(PacketData(std::begin(g_client0_moor29), std::end(g_client0_moor29)));

		QueuePacket(PacketData(std::begin(g_client0_moor30), std::end(g_client0_moor30)));
		QueuePacket(PacketData(std::begin(g_client0_moor31), std::end(g_client0_moor31)));

		QueuePacket(PacketData(std::begin(g_client0_moor32), std::end(g_client0_moor32)));
		QueuePacket(PacketData(std::begin(g_client0_moor33), std::end(g_client0_moor33)));
		QueuePacket(PacketData(std::begin(g_client0_moor34), std::end(g_client0_moor34)));
		QueuePacket(PacketData(std::begin(g_client0_moor35), std::end(g_client0_moor35)));
		QueuePacket(PacketData(std::begin(g_client0_moor36), std::end(g_client0_moor36)));
		QueuePacket(PacketData(std::begin(g_client0_moor37), std::end(g_client0_moor37)));
	*/
		//Enables chat?
	//	QueuePacket(PacketData(std::begin(g_client0_moor38), std::end(g_client0_moor38)));

		{
			CCompositePacket packet;
			packet.AddPacket(PacketData(std::begin(g_client0_moor38), std::end(g_client0_moor38)));
			QueuePacket(packet.ToPacketData());
		}

	//	QueuePacket(PacketData(std::begin(g_client0_moor39), std::end(g_client0_moor39)));

	//	QueuePacket(PacketData(std::begin(g_client0_moor40), std::end(g_client0_moor40)));

		QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_1, APPEARANCE_ID_FUNGUAR,	STRING_ID_FOREST_FUNGUAR,		356.61f,  4.38f, -943.42f, 0)));
		QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_2, APPEARANCE_ID_IFRIT,		STRING_ID_IFRIT,				374.98f,  4.98f, -930.43f, 0)));
		QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_3, APPEARANCE_ID_MARMOT,		STRING_ID_STAR_MARMOT,			379.04f,  6.87f, -901.23f, 0)));
		QueuePacket(PacketData(SpawnNpc(ACTOR_ID_ENEMY_4, APPEARANCE_ID_ANTELOPE,	STRING_ID_ANTELOPE_DOE,			365.38f, -0.79f, -834.23f, 0)));
		m_npcHp[ACTOR_ID_ENEMY_1] = ENEMY_INITIAL_HP;
		m_npcHp[ACTOR_ID_ENEMY_2] = ENEMY_INITIAL_HP;
		m_npcHp[ACTOR_ID_ENEMY_3] = ENEMY_INITIAL_HP;
		m_npcHp[ACTOR_ID_ENEMY_4] = ENEMY_INITIAL_HP;

		m_zoneMasterCreated = true;
	}

}
