#pragma once

constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 2000;
constexpr int WORLD_HEIGHT = 2000;
constexpr int MAX_PLAYERS = 10000;
constexpr int NUM_NPCS = 200000;
constexpr int NPC_ID_START = 1000000;
constexpr int NPC_MOVE_INTERVAL = 1000; // in milliseconds
constexpr int MAX_NAME_LEN = 20;
constexpr int MAX_CHAT_MSG_LEN = 200;
constexpr int BUF_SIZE = 200;
constexpr int MAX_PARTY_SIZE = 4;

enum PACKET_TYPE { 
	C2S_LOGIN,			// Client to Server: Login request
						// 사용자 이름을 포함한 로그인 요청 패킷	
	C2S_MOVE,			// Client to Server: Move request
						// 이동 방향과 이동 시간을 포함한 이동 요청 패킷
	C2S_CHAT,			// Client to Server: Chat message
						// 채팅 메시지를 포함한 채팅 요청 패킷
	C2S_ATTACK,			// Client to Server: Attack request
						// 공격 요청 패킷 (4 방향 동시 공격)
	C2S_TELEPORT,		// Client to Server: Teleport request
						// 텔레포트 요청 패킷 (목적지 좌표 포함)
						// STRESS TEST용으로 추가한 패킷입니다. 시작 마을에 몰리는 것을 방지.
	C2S_SELECT_WEAPON,		// Client to Server: Select weapon request
	C2S_REQUEST_INFO,	// Client to Server: Request other player info
	// dungeon related actions
	C2S_DUNGEON_ENTRY,	// Client to Server: Dungeon entry request
	C2S_DUNGEON_EXIT,	// Client to Server: Dungeon exit request
	// npc interaction and reinforcement
	C2S_INTERACT,		// Client to Server: Interact with NPC (reinforce)
	C2S_REINFORCE,		// Client to Server: Reinforce request (weapon upgrade)
	// party related actions
	C2S_INVITE_PARTY,	// Client to Server: Invite party request
	C2S_APPLY_PARTY,	// Client to Server: Apply party request
	C2S_ACCEPT_PARTY,	// Client to Server: Accept party request
	C2S_REFUSE_PARTY,	// Client to Server: Refuse party request
	C2S_LEAVE_PARTY,	// Client to Server: Leave party request

	C2S_LOGOUT,			// Client to Server: Logout request

	S2C_LOGIN_RESULT,	//	Server to Client: Login result
						// 로그인 결과 패킷 (성공 여부와 메시지 포함)
	S2C_AVATAR_INFO,	//	Server to Client: Avatar information
	S2C_ADD_OBJECT,		//	Server to Client: Add player or NPC		
	S2C_REMOVE_OBJECT,	//	Server to Client: Remove player or NPC
	S2C_MOVE_OBJECT,	//	Server to Client: Move player or NPC
	S2C_CHAT_MESSAGE,	//	Server to Client: Chat message
	S2C_STATUS_CHANGE,	//	Server to Client: Update player or NPC status (e.g., health, buffs)
	S2C_DUNGEON_RESULT,	//	Server to Client: Dungeon result
	S2C_INFO_RESULT,	//	Server to Client: Response to information request
	// npc interaction and reinforcement results
	S2C_INTERACT_RESULT,//	Server to Client: Result of interaction
	S2C_REINFORCE_RESULT,//	Server to Client: Result of reinforcement
	// party related updates
	S2C_PARTY_INVITE_NOTI,	//	Server to Client: Party invite notification
	S2C_PARTY_APPLY_NOTI,		//	Server to Client: Party application notification
	S2C_PARTY_UPDATE,			//	Server to Client: Party status update (e.g., new member, member left)
};

enum WEAPON_TYPE {
	SWORD,
	HAMMER,
	SPEAR
};

enum DUNGEON_TYPE {
	DUNGEON_1,
	DUNGEON_2,
	DUNGEON_3,
	DUNGEON_4,
	DUNGEON_5,
	DUNGEON_6,
	DUNGEON_7,
	FINAL_BOSS
};


#pragma pack(push, 1) // Ensure no padding between struct members

struct PartyMemberInfo {
	int					playerId;
	char					username[MAX_NAME_LEN];
	WEAPON_TYPE			weapon;
	int					hp;
	int					max_hp;
	unsigned long long	exp;
	unsigned char			level;
};

struct C2S_Login {
	unsigned char size;
	PACKET_TYPE   type;
	char username[MAX_NAME_LEN];
};

struct C2S_Move {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct C2S_Chat {
	unsigned char size;
	PACKET_TYPE   type;
	char message[MAX_CHAT_MSG_LEN];
};

struct C2S_Attack {
	unsigned char size;
	PACKET_TYPE   type;
};

struct C2S_Teleport {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
};

struct C2S_SelectWeapon {
	unsigned char size;
	PACKET_TYPE   type;
	WEAPON_TYPE weapon;
};

struct C2S_RequestInfo {
	unsigned char			size;
	PACKET_TYPE			type;
	char					target_username[MAX_NAME_LEN];
};

struct C2S_DungeonEntry {
	unsigned char			size;
	PACKET_TYPE				type;
	DUNGEON_TYPE			dungeon;
};

struct C2S_DungeonExit
{
	unsigned char			size;
	PACKET_TYPE				type;
	DUNGEON_TYPE			dungeon;
};

struct C2S_Interact {
	unsigned char			size;
	PACKET_TYPE				type;
};

struct C2S_Reinforce {
	unsigned char			size;
	PACKET_TYPE				type;
	WEAPON_TYPE				weapon;
	unsigned char				reinforce_level;
	int						gold;
};

struct C2S_InviteParty {
	unsigned char			size;
	PACKET_TYPE				type;
	char					target_username[MAX_NAME_LEN];
};

struct C2S_ApplyParty {
	unsigned char			size;
	PACKET_TYPE				type;
	char					target_username[MAX_NAME_LEN];
};

struct C2S_AcceptParty {
	unsigned char			size;
	PACKET_TYPE				type;
	char					target_username[MAX_NAME_LEN];
};

struct C2S_RefuseParty {
	unsigned char			size;
	PACKET_TYPE				type;
	char					target_username[MAX_NAME_LEN];
};

struct C2S_Logout {
	unsigned char			size;
	PACKET_TYPE				type;
};

struct S2C_LoginResult {
	unsigned char			size;
	PACKET_TYPE				type;
	bool success;
	char message[50];
};

struct S2C_AvatarInfo {
	unsigned char size;
	PACKET_TYPE   type;
	int					playerId;
	int					visualId; // for future use (different visual appearances)
	char					username[MAX_NAME_LEN];
	short				x;
	short				y;
	int					hp;
	int					max_hp;
	unsigned long long	exp;
	unsigned char			level;
};

struct S2C_AddObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int visual_id; // for future use (different visual appearances)
	char obj_name[MAX_NAME_LEN];
	short x;
	short y;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_RemoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
};

struct S2C_MoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct S2C_ChatMessage {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	char message[MAX_CHAT_MSG_LEN];
};

struct S2C_StatusChange {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_DungeonResult {
	unsigned char			size;
	PACKET_TYPE			type;
	int 					playerId;
	DUNGEON_TYPE			dungeon;
	bool					success;
	short				x;
	short				y;
	char					message[100];
};

struct S2C_InfoResult
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	int					visualId; // for future use (different visual appearances)
	char					username[MAX_NAME_LEN];
	short				x;
	short				y;
	WEAPON_TYPE			weapon;
	int					hp;
	int					max_hp;
	int					gold;
	unsigned char				reinforce_level;
	unsigned long long	exp;
	unsigned char			level;
	bool					in_party;
};

struct S2C_InteractResult
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	bool					success;
	char					message[100];
};

struct S2C_ReinforceResult
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	bool					success;
	unsigned char			reinforce_level;
	int					gold;
};

struct S2C_PartyInviteNoti
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	char					inviter_username[MAX_NAME_LEN];
};

struct S2C_PartyApplyNoti
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	char					applicant_username[MAX_NAME_LEN];
};

struct S2C_PartyUpdate
{
	unsigned char			size;
	PACKET_TYPE			type;
	int					playerId;
	int					party_member_count;
	PartyMemberInfo		party_members[MAX_PARTY_SIZE];
};

#pragma pack(pop) // Restore default packing
