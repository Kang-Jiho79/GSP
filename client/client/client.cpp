#include "framework.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <ws2tcpip.h>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(linker, "/entry:wWinMainCRTStartup /subsystem:console") 

#include "..\..\Server\server\protocol_2026.h" // 새로운 프로토콜 헤더 경로 확인 필수!

#define MAX_LOADSTRING 100
#define winLength 800

constexpr char SERVER_IP[] = "127.0.0.1";

const int MAP_WIDTH = 2000;
const int MAP_HEIGHT = 2000;
std::vector<std::vector<int>> g_map(MAP_HEIGHT, std::vector<int>(MAP_WIDTH, 0));

// --- 전역 변수 ---
HWND g_hWnd;
HINSTANCE hInst;                                // 현재 인스턴스입니다.
WCHAR szTitle[MAX_LOADSTRING] = L"9-Realms Client";                  // 제목 표시줄 텍스트입니다.
WCHAR szWindowClass[MAX_LOADSTRING] = L"IOCPCLIENT";            // 기본 창 클래스 이름입니다.

char g_recv_buf[BUF_SIZE * 2]; // 넉넉하게 2배
char g_send_buf[BUF_SIZE];
WSABUF g_recv_wsa_buf{ sizeof(g_recv_buf), g_recv_buf };
WSABUF g_send_wsa_buf{ BUF_SIZE, g_send_buf };
WSAOVERLAPPED g_recv_over{}, g_send_over{};
SOCKET g_s_socket;

int g_myid = -1;
int g_prev_size = 0;

// --- UI 상태 관리용 전역 변수 ---
int g_selected_target_id = -1;    // 마우스로 클릭한 타겟 ID
bool g_show_reinforce_ui = false; // 강화창 열림 여부

const DWORD ATTACK_EFFECT_DURATION = 150; // 이펙트 지속 시간 (150 밀리초)

bool g_show_party_invite_popup = false;  // 파티 초대 팝업 열림 여부
int g_inviter_id = -1;                   // 나를 초대한 사람의 ID
char g_inviter_name[MAX_NAME_LEN] = "";  // 나를 초대한 사람의 이름
bool g_is_space_pressed = false;

// 마우스 클릭 시 사용할 임시 사각형(버튼 영역) 선언
RECT g_rect_invite_btn = { 0, 0, 0, 0 };
RECT g_rect_leave_btn = { 0, 0, 0, 0 };
RECT g_rect_party_accept_btn = { 0, 0, 0, 0 };
RECT g_rect_party_refuse_btn = { 0, 0, 0, 0 };
RECT g_rect_reinforce_btn = { 0, 0, 0, 0 };
RECT g_rect_close_reinforce_btn = { 0, 0, 0, 0 };

// --- 새 프로토콜에 맞춘 클라이언트 객체 구조 ---
struct ClientObject {
	int                 id;
	int                 visual_id; // for future use (different visual appearances)
	short               x, y;
	int                 hp, max_hp;
	unsigned long long  exp;
	unsigned char        level;
	char                username[MAX_NAME_LEN];
	WEAPON_TYPE         weapon = null;
	int                 gold;
	unsigned char       reinforce_level;
	bool                in_party;
	DWORD               last_attack_time = 0;
};

struct PartyMemberUI {
	int  id;
	char username[MAX_NAME_LEN];
	int  hp;
	int  max_hp;
	unsigned char level;
};

PartyMemberUI g_party_members[4];
int g_party_member_count = 0;

// 배열 대신 map을 사용하여 NPC ID(1,000,000 이상) 대응
std::unordered_map<int, ClientObject> g_objects;

// --- 함수 선언 ---
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);
void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);
void error_display(const wchar_t* msg, int err_no);


// --- 에러 출력 함수 ---
void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg;
	std::wcout << L" 에러 " << lpMsgBuf << std::endl;
	// 디버깅 용
	LocalFree(lpMsgBuf);
}

bool LoadMapCSV(const std::string& filename) {
	std::ifstream file(filename);
	if (!file.is_open()) return false;

	std::string line;
	int y = 0;
	while (std::getline(file, line) && y < MAP_HEIGHT) {
		std::stringstream ss(line);
		std::string token;
		int x = 0;
		while (std::getline(ss, token, ',') && x < MAP_WIDTH) {
			g_map[y][x] = std::stoi(token);
			x++;
		}
		y++;
	}
	return true;
}

// --- 패킷 전송 함수 ---
void send_login_packet()
{
	C2S_Login packet{};
	packet.size = sizeof(C2S_Login);
	packet.type = C2S_LOGIN;

	std::string username;
	std::cout << "Enter username: ";
	std::getline(std::cin, username);
	strncpy_s(packet.username, MAX_NAME_LEN, username.c_str(), MAX_NAME_LEN - 1);

	memcpy(g_send_buf, &packet, sizeof(C2S_Login));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(C2S_Login);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
	std::cout << "Login packet sent for username: " << username << std::endl;
}

void send_move_packet(short dx, short dy)
{
	C2S_Move packet{};
	packet.size = sizeof(C2S_Move);
	packet.type = C2S_MOVE;
	packet.x = dx;
	packet.y = dy;
	packet.move_time = 100; // 가상의 프레임 이동 시간

	memcpy(g_send_buf, &packet, sizeof(C2S_Move));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(C2S_Move);

	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_chat_packet(const std::string& message)
{
	C2S_Chat packet{};
	packet.size = sizeof(C2S_Chat);
	packet.type = C2S_CHAT;
	strncpy_s(packet.message, MAX_CHAT_MSG_LEN, message.c_str(), _TRUNCATE);
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_attack_packet()
{
	C2S_Attack packet{};
	packet.size = sizeof(C2S_Attack);
	packet.type = C2S_ATTACK;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_teleport_packet(short dest_x, short dest_y)
{
	C2S_Teleport packet{};
	packet.size = sizeof(C2S_Teleport);
	packet.type = C2S_TELEPORT;
	packet.x = dest_x;
	packet.y = dest_y;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_select_weapon_packet()
{
	std::cout << "Select weapon (1: Sword, 2: Hammer, 3: Spear): ";
	int weapon_input;
	std::cin >> weapon_input;
	C2S_SelectWeapon packet{};
	packet.size = sizeof(C2S_SelectWeapon);
	packet.type = C2S_SELECT_WEAPON;
	packet.weapon = (WEAPON_TYPE)weapon_input;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_request_info_packet(const std::string& target_username)
{
	C2S_RequestInfo packet{};
	packet.size = sizeof(C2S_RequestInfo);
	packet.type = C2S_REQUEST_INFO;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_dungeon_entry_packet(DUNGEON_TYPE dungeon)
{
	C2S_DungeonEntry packet{};
	packet.size = sizeof(C2S_DungeonEntry);
	packet.type = C2S_DUNGEON_ENTRY;
	packet.dungeon = dungeon;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_dungeon_exit_packet()
{
	C2S_DungeonExit packet{};
	packet.size = sizeof(C2S_DungeonExit);
	packet.type = C2S_DUNGEON_EXIT;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_interact_packet()
{
	C2S_Interact packet{};
	packet.size = sizeof(C2S_Interact);
	packet.type = C2S_INTERACT;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_reinforce_packet(WEAPON_TYPE weapon, unsigned char reinforce_level, int gold)
{
	C2S_Reinforce packet{};
	packet.size = sizeof(C2S_Reinforce);
	packet.type = C2S_REINFORCE;
	packet.weapon = weapon;
	packet.reinforce_level = reinforce_level;
	packet.gold = gold;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_party_invite_packet(const std::string& target_username)
{
	C2S_InviteParty packet{};
	packet.size = sizeof(C2S_InviteParty);
	packet.type = C2S_INVITE_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_party_accept_packet(const std::string& target_username)
{
	C2S_AcceptParty packet{};
	packet.size = sizeof(C2S_AcceptParty);
	packet.type = C2S_ACCEPT_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_party_refuse_packet(const std::string& target_username)
{
	C2S_RefuseParty packet{};
	packet.size = sizeof(C2S_RefuseParty);
	packet.type = C2S_REFUSE_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_party_leave_packet()
{
	C2S_LeaveParty packet{};
	packet.size = sizeof(C2S_LeaveParty);
	packet.type = C2S_LEAVE_PARTY;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

void send_logout_packet()
{
	C2S_Logout packet{};
	packet.size = sizeof(C2S_Logout);
	packet.type = C2S_LOGOUT;
	memcpy(g_send_buf, &packet, sizeof(packet));
	g_send_wsa_buf.buf = g_send_buf;
	g_send_wsa_buf.len = sizeof(packet);
	ZeroMemory(&g_send_over, sizeof(g_send_over));
	DWORD sent_size = 0;
	WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_over, send_callback);
}

// --- 패킷 수신 및 처리 로직 ---
void process_packet(unsigned char* p)
{
	PACKET_TYPE type = static_cast<PACKET_TYPE>(p[1]); // p[1]에 type이 위치함
	bool should_repaint = false; // 화면을 다시 그릴 필요가 있는 패킷일 때만 true로 설정

	switch (type) {
	case S2C_LOGIN_RESULT:
	{
		S2C_LoginResult* packet = reinterpret_cast<S2C_LoginResult*>(p);
		if (!packet->success) {
			std::cout << "Login Failed : " << packet->message << std::endl;
		}
		else {
			std::cout << "Login Success : " << packet->message << std::endl;
		}
		break;
	}
	case S2C_AVATAR_INFO:
	{
		S2C_AvatarInfo* packet = reinterpret_cast<S2C_AvatarInfo*>(p);
		g_myid = packet->playerId;

		ClientObject obj;
		obj.id = packet->playerId;
		obj.x = packet->x; obj.y = packet->y;
		obj.hp = packet->hp; obj.max_hp = packet->max_hp;
		obj.level = packet->level;

		obj.weapon = static_cast<WEAPON_TYPE>(packet->visualId);

		strncpy_s(obj.username, packet->username, _TRUNCATE);

		g_objects[g_myid] = obj;
		should_repaint = true;

		if (obj.weapon == null) {
			std::cout << "\n=============================================" << std::endl;
			std::cout << " [9-Realms] 최초 접속을 환영합니다! 무기를 선택해 주세요." << std::endl;
			std::cout << "=============================================" << std::endl;
			send_select_weapon_packet();
		}
		else {
			std::cout << "\n[9-Realms] 기존 캐릭터 정보를 안전하게 로드했습니다. (현재 무기 코드: " << (int)obj.weapon << ")" << std::endl;
		}
		break;
	}
	case S2C_ADD_OBJECT:
	{
		S2C_AddObject* packet = reinterpret_cast<S2C_AddObject*>(p);
		ClientObject obj;
		obj.id = packet->object_id;
		obj.x = packet->x; obj.y = packet->y;
		obj.hp = packet->hp; obj.max_hp = packet->max_hp;
		obj.level = packet->level;
		strncpy_s(obj.username, packet->obj_name, _TRUNCATE);

		obj.visual_id = packet->visual_id;

		g_objects[obj.id] = obj;
		should_repaint = true;
		break;
	}
	case S2C_MOVE_OBJECT:
	{
		S2C_MoveObject* packet = reinterpret_cast<S2C_MoveObject*>(p);
		if (g_objects.count(packet->object_id)) {
			g_objects[packet->object_id].x = packet->x;
			g_objects[packet->object_id].y = packet->y;
			should_repaint = true;
		}
		break;
	}
	case S2C_REMOVE_OBJECT:
	{
		S2C_RemoveObject* packet = reinterpret_cast<S2C_RemoveObject*>(p);
		g_objects.erase(packet->object_id);
		should_repaint = true;
		break;
	}
	case S2C_STATUS_CHANGE:
	{
		S2C_StatusChange* packet = reinterpret_cast<S2C_StatusChange*>(p);
		if (g_objects.count(packet->object_id)) {
			g_objects[packet->object_id].hp = packet->hp;
			g_objects[packet->object_id].max_hp = packet->max_hp;
			g_objects[packet->object_id].level = packet->level;
			should_repaint = true;
		}
		break;
	}
	case S2C_CHAT_MESSAGE:
	{
		S2C_ChatMessage* packet = reinterpret_cast<S2C_ChatMessage*>(p);
		// g_objects에서 보낸 사람의 이름을 찾아 채팅창(콘솔)에 출력
		std::string sender_name = "Unknown";
		if (g_objects.count(packet->object_id)) {
			sender_name = g_objects[packet->object_id].username;
		}
		std::cout << "[" << sender_name << "]: " << packet->message << std::endl;
		break;
	}
	case S2C_DUNGEON_RESULT:
	{
		S2C_DungeonResult* packet = reinterpret_cast<S2C_DungeonResult*>(p);
		if (packet->success) {
			std::cout << "[던전] 이동 성공! 던전 번호: " << packet->dungeon << std::endl;
			if (g_objects.count(g_myid)) {
				g_objects[g_myid].x = packet->x;
				g_objects[g_myid].y = packet->y;
				should_repaint = true;
			}
		}
		else {
			std::cout << "[던전] 이동 실패: " << packet->message << std::endl;
		}
		break;
	}
	case S2C_INFO_RESULT:
	{
		S2C_InfoResult* packet = reinterpret_cast<S2C_InfoResult*>(p);
		std::cout << "=== 유저 정보 조회 결과 ===" << std::endl;
		std::cout << "이름: " << packet->username << " (Lv." << (int)packet->level << ")" << std::endl;
		std::cout << "좌표: (" << packet->x << ", " << packet->y << ")" << std::endl;
		std::cout << "HP: " << packet->hp << " / " << packet->max_hp << std::endl;
		std::cout << "무기 타입: " << packet->weapon << " (강화: +" << (int)packet->reinforce_level << "강)" << std::endl;
		std::cout << "소지 골드: " << packet->gold << " | 경험치: " << packet->exp << std::endl;
		std::cout << "파티 여부: " << (packet->in_party ? "참여 중" : "없음") << std::endl;
		std::cout << "==========================" << std::endl;
		if (g_objects.count(packet->playerId)) {
			g_objects[packet->playerId].x = packet->x;
			g_objects[packet->playerId].y = packet->y;
			g_objects[packet->playerId].hp = packet->hp;
			g_objects[packet->playerId].max_hp = packet->max_hp;
			g_objects[packet->playerId].level = packet->level;
			g_objects[packet->playerId].weapon = packet->weapon;
			g_objects[packet->playerId].reinforce_level = packet->reinforce_level;
			g_objects[packet->playerId].gold = packet->gold;
			g_objects[packet->playerId].exp = packet->exp;
			g_objects[packet->playerId].in_party = packet->in_party;

			should_repaint = true;
		}
		if (packet->weapon == null) {
			std::cout << "\n[9-Realms] 신규 가입을 환영합니다!" << std::endl;
			send_select_weapon_packet();
		}
		else {
			std::cout << "\n[9-Realms] 기존 캐릭터 정보를 안전하게 불러왔습니다! (무기 타입: " << (int)packet->weapon << ")" << std::endl;
		}
		break;
	}
	case S2C_INTERACT_RESULT:
	{
		S2C_InteractResult* packet = reinterpret_cast<S2C_InteractResult*>(p);
		if (packet->success) {
			g_show_reinforce_ui = true;
			should_repaint = true;
		}
		std::cout << "[상호작용] 결과: " << packet->message << std::endl;
		break;
	}
	case S2C_REINFORCE_RESULT:
	{
		S2C_ReinforceResult* packet = reinterpret_cast<S2C_ReinforceResult*>(p);
		if (packet->success) {
			std::cout << "[강화 성공] 현재 강화 레벨: +" << (int)packet->reinforce_level << "강 | 남은 골드: " << packet->gold << std::endl;
		}
		else {
			std::cout << "[강화 실패] 소지 골드: " << packet->gold << std::endl;
		}
		break;
	}
	case S2C_PARTY_INVITE_NOTI:
	{
		S2C_PartyInviteNoti* packet = reinterpret_cast<S2C_PartyInviteNoti*>(p);
		std::cout << "[파티 초대 알림] " << packet->inviter_username << " 님이 당신을 초대했습니다." << std::endl;

		// ⭐ [팝업 데이터 세팅 및 활성화]
		g_inviter_id = packet->playerId;
		strncpy_s(g_inviter_name, packet->inviter_username, MAX_NAME_LEN);
		g_show_party_invite_popup = true;
		break;
	}
	case S2C_PARTY_UPDATE:
	{
		S2C_PartyUpdate* packet = reinterpret_cast<S2C_PartyUpdate*>(p);

		g_party_member_count = packet->party_member_count;

		// ⭐ [지우는 로직 추가] 만약 파티원이 0명이면 배열 전체를 0으로 깨끗하게 밀어버립니다.
		if (g_party_member_count == 0) {
			memset(g_party_members, 0, sizeof(g_party_members));
		}
		else {
			// 파티원이 있을 때만 루프를 돌며 데이터 복사
			for (int i = 0; i < g_party_member_count; ++i) {
				g_party_members[i].id = packet->party_members[i].playerId;
				strncpy_s(g_party_members[i].username, packet->party_members[i].username, MAX_NAME_LEN);
				g_party_members[i].hp = packet->party_members[i].hp;
				g_party_members[i].max_hp = packet->party_members[i].max_hp;
				g_party_members[i].level = packet->party_members[i].level;
			}
		}

		// 내 로컬 오브젝트의 파티 플래그도 동기화
		if (g_myid != -1 && g_objects.count(g_myid)) {
			g_objects[g_myid].in_party = (g_party_member_count > 1);
		}

		std::cout << "[디버그] 파티 정보 업데이트! 현재 파티원 수: " << g_party_member_count << std::endl;

		// 60fps 루프가 돌고 있지만, 즉각적인 반응을 위해 강제 화면 갱신 트리거
		if (g_hWnd) InvalidateRect(g_hWnd, NULL, TRUE);
		break;
	}
	case S2C_ATTACK_BROADCAST:
	{
		S2C_AttackBroadcast* packet = reinterpret_cast<S2C_AttackBroadcast*>(p);
		std::cout << "[디버그] " << packet->attacker_id << "번 유저의 공격 패킷 수신!" << std::endl;
		if (g_objects.count(packet->attacker_id)) {
			// 다른 사람의 공격 시간을 현재로 갱신
			g_objects[packet->attacker_id].last_attack_time = GetTickCount64();
			g_objects[packet->attacker_id].weapon = packet->weapon;
			should_repaint = true;
		}
		break;
	}
	default:
		break;
	}

	// 오브젝트의 생성, 제거, 이동 등 윈도우 그리기 좌표계에 영향이 있는 패킷만 화면을 갱신하도록 최적화
	if (should_repaint && g_hWnd) {
		InvalidateRect(g_hWnd, NULL, TRUE);
	}
}

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	if (error != 0 || bytes_transferred == 0) return;

	unsigned char* ptr = reinterpret_cast<unsigned char*>(g_recv_buf);
	int remain_size = g_prev_size + bytes_transferred;

	while (remain_size > 0)
	{
		int packet_size = ptr[0];
		if (packet_size == 0 || packet_size > remain_size) break;
		process_packet(ptr);
		ptr += packet_size;
		remain_size -= packet_size;
	}

	if (remain_size > 0) {
		memmove(g_recv_buf, ptr, remain_size);
	}
	g_prev_size = remain_size;

	g_recv_wsa_buf.buf = g_recv_buf + g_prev_size;
	g_recv_wsa_buf.len = sizeof(g_recv_buf) - g_prev_size;

	DWORD recv_flag = 0;
	ZeroMemory(&g_recv_over, sizeof(g_recv_over));
	WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_over, recv_callback);
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
	// 에러 처리 외에 특별한 동작 불필요
}


// --- 윈도우 프로시저 ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		// 💥 [추가] 스페이스바에서 손을 떼는 순간을 감지하여 플래그를 초기화합니다.
	case WM_KEYUP:
	{
		if (wParam == VK_SPACE) {
			g_is_space_pressed = false;
		}
		break;
	}

	case WM_KEYDOWN:
	{
		short dx = 0, dy = 0;
		switch (wParam) {
		case VK_LEFT:  dx = -1; break;
		case VK_RIGHT: dx = 1;  break;
		case VK_UP:    dy = -1; break;
		case VK_DOWN:  dy = 1;  break;
		case VK_SPACE:
			// ⭐ [기능 1] 키 반복 입력 필터링: 누르고 있어도 최초 1회만 단발성 공격 감행!
			if (!g_is_space_pressed) {
				g_is_space_pressed = true;
				send_attack_packet();
				if (g_myid != -1 && g_objects.count(g_myid)) {
					g_objects[g_myid].last_attack_time = GetTickCount();
				}
				InvalidateRect(hWnd, NULL, TRUE);
				SetTimer(hWnd, 1, ATTACK_EFFECT_DURATION, NULL);
			}
			break;
		case 'A':
		case 'a':
		{
			if (g_myid != -1 && g_objects.count(g_myid)) {
				auto& myObj = g_objects[g_myid];
				bool isOnPortal = false;
				for (const auto& portal : Portals) {
					if (myObj.x == portal.src_x && myObj.y == portal.src_y) {
						send_dungeon_entry_packet(portal.dungeon);
						std::cout << "[디버그] A키 상호작용! 던전 입장 요청을 보냈습니다. 타입: " << portal.dungeon << std::endl;
						isOnPortal = true;
						break;
					}
				}

				if (!isOnPortal) {
					bool is_near_npc = false;
					for (const auto& npc : g_npc_spawns) {
						if (abs(myObj.x - npc.x) <= 1 && abs(myObj.y - npc.y) <= 1) {
							is_near_npc = true;
							break;
						}
					}

					if (is_near_npc) {
						if (g_show_reinforce_ui) {
							g_show_reinforce_ui = false;
							InvalidateRect(hWnd, NULL, TRUE);
						}
						else {
							send_interact_packet();
						}
					}
				}
			}
			break;
		}
		case 'T':
			send_teleport_packet(WORLD_WIDTH / 2, WORLD_HEIGHT / 2);
			break;
		case VK_ESCAPE:
			g_selected_target_id = -1;
			g_show_reinforce_ui = false;
			InvalidateRect(hWnd, NULL, TRUE);
			break;
		}
		if (dx != 0 || dy != 0) send_move_packet(dx, dy);
		break;
	}

	case WM_TIMER:
		if (wParam == 1)
		{
			InvalidateRect(hWnd, NULL, TRUE);
		}
		break;

	case WM_LBUTTONDOWN:
	{
		int mouseX = LOWORD(lParam);
		int mouseY = HIWORD(lParam);
		POINT pt = { mouseX, mouseY };

		if (g_show_party_invite_popup) {
			if (PtInRect(&g_rect_party_accept_btn, pt)) {
				send_party_accept_packet(g_inviter_name);
				g_show_party_invite_popup = false;
				return 0;
			}
			else if (PtInRect(&g_rect_party_refuse_btn, pt)) {
				send_party_refuse_packet(g_inviter_name);
				g_show_party_invite_popup = false;
				return 0;
			}
		}

		if (g_show_reinforce_ui) {
			if (PtInRect(&g_rect_reinforce_btn, pt)) {
				ClientObject& myObj = g_objects[g_myid];
				send_reinforce_packet(myObj.weapon, myObj.reinforce_level, myObj.gold);
			}
			else if (PtInRect(&g_rect_close_reinforce_btn, pt)) {
				g_show_reinforce_ui = false;
				InvalidateRect(hWnd, NULL, TRUE);
			}
			return 0;
		}

		if (g_selected_target_id != -1 && g_selected_target_id != g_myid) {
			if (PtInRect(&g_rect_invite_btn, pt)) {
				std::string targetName = g_objects[g_selected_target_id].username;
				send_party_invite_packet(targetName);
				return 0;
			}
		}

		if (PtInRect(&g_rect_leave_btn, pt)) {
			send_party_leave_packet();
			return 0;
		}

		if (g_myid != -1 && g_objects.count(g_myid)) {
			ClientObject& myObj = g_objects[g_myid];
			const int VIEW_RADIUS = 10;
			const int TILE_SIZE = winLength / (VIEW_RADIUS * 2);
			const int CENTER_X = winLength / 2;
			const int CENTER_Y = winLength / 2;
			int radius = min(TILE_SIZE / 2, 20);

			bool clicked_someone = false;
			for (auto& pair : g_objects) {
				ClientObject& obj = pair.second;
				int diffX = obj.x - myObj.x;
				int diffY = obj.y - myObj.y;

				if (abs(diffX) <= VIEW_RADIUS && abs(diffY) <= VIEW_RADIUS) {
					int drawX = CENTER_X + (diffX * TILE_SIZE) + (TILE_SIZE / 2);
					int drawY = CENTER_Y + (diffY * TILE_SIZE) + (TILE_SIZE / 2);

					int distSq = (mouseX - drawX) * (mouseX - drawX) + (mouseY - drawY) * (mouseY - drawY);
					if (distSq <= radius * radius) {
						g_selected_target_id = obj.id;
						clicked_someone = true;
						InvalidateRect(hWnd, NULL, TRUE);
						break;
					}
				}
			}
			if (!clicked_someone) {
				g_selected_target_id = -1;
				InvalidateRect(hWnd, NULL, TRUE);
			}
		}
		break;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);

		RECT rect;
		GetClientRect(hWnd, &rect);
		int width = rect.right - rect.left;
		int height = rect.bottom - rect.top;

		HDC memDC = CreateCompatibleDC(hdc);
		HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
		HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

		HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 255));
		FillRect(memDC, &rect, bgBrush);
		DeleteObject(bgBrush);

		// 배경 투명 텍스트 설정을 적용하여 글자 배경이 타일을 가리지 않게 조치
		SetBkMode(memDC, TRANSPARENT);

		if (g_myid != -1 && g_objects.count(g_myid)) {
			ClientObject& myObj = g_objects[g_myid];

			const int VIEW_RADIUS = 10;
			const int TILE_SIZE = winLength / (VIEW_RADIUS * 2);
			const int CENTER_X = winLength / 2;
			const int CENTER_Y = winLength / 2;

			// 1. 타일 및 고정 오브젝트 네임태그 렌더링
			for (int dy = -VIEW_RADIUS; dy <= VIEW_RADIUS; ++dy) {
				for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; ++dx) {
					int mapX = myObj.x + dx;
					int mapY = myObj.y + dy;

					if (mapX >= 0 && mapX < MAP_WIDTH && mapY >= 0 && mapY < MAP_HEIGHT) {
						int drawX = CENTER_X + (dx * TILE_SIZE);
						int drawY = CENTER_Y + (dy * TILE_SIZE);

						HBRUSH tileBrush = nullptr;
						bool is_portal_tile = false;
						std::string portal_name = "";

						// 포탈 순회 체크 및 이름 정의
						for (const auto& portal : Portals) {
							if (mapX == portal.src_x && mapY == portal.src_y) {
								if (portal.dungeon == FINAL_BOSS) {
									tileBrush = CreateSolidBrush(RGB(255, 69, 0));
									portal_name = "최종 보스 포탈";
								}
								else {
									tileBrush = CreateSolidBrush(RGB(0, 191, 255));
									portal_name = "던전 " + std::to_string(portal.dungeon + 1) + " 포탈";
								}
								is_portal_tile = true;
								break;
							}
						}

						bool is_npc_tile = false;
						std::string npc_display_name = "";

						if (!is_portal_tile) {
							for (const auto& npc : g_npc_spawns) {
								if (mapX == npc.x && mapY == npc.y) {
									tileBrush = CreateSolidBrush(RGB(255, 215, 0));
									npc_display_name = npc.name;
									is_npc_tile = true;
									break;
								}
							}

							if (!is_npc_tile) {
								tileBrush = (g_map[mapY][mapX] == 0) ?
									CreateSolidBrush(RGB(255, 255, 255)) :
									CreateSolidBrush(RGB(0, 0, 0));
							}
						}

						RECT tileRect = { drawX, drawY, drawX + TILE_SIZE, drawY + TILE_SIZE };
						FillRect(memDC, &tileRect, tileBrush);
						DeleteObject(tileBrush);

						HBRUSH borderBrush = CreateSolidBrush(RGB(220, 220, 220));
						FrameRect(memDC, &tileRect, borderBrush);
						DeleteObject(borderBrush);

						// ⭐ [기능 3] 포탈 및 강화 NPC 위에 이름 실시간 출력 (타일 중앙 상단 배치)
						if (is_portal_tile) {
							TextOutA(memDC, drawX - 25, drawY - 15, portal_name.c_str(), (int)portal_name.length());
						}
						if (is_npc_tile) {
							TextOutA(memDC, drawX - 20, drawY - 15, npc_display_name.c_str(), (int)npc_display_name.length());
						}
					}
				}
			}

			// 2. 동적 아바타 및 몬스터 개체 렌더링
			for (auto& pair : g_objects) {
				ClientObject& obj = pair.second;

				int diffX = obj.x - myObj.x;
				int diffY = obj.y - myObj.y;

				if (abs(diffX) <= VIEW_RADIUS && abs(diffY) <= VIEW_RADIUS) {
					int drawX = CENTER_X + (diffX * TILE_SIZE) + (TILE_SIZE / 2);
					int drawY = CENTER_Y + (diffY * TILE_SIZE) + (TILE_SIZE / 2);

					HBRUSH hBrush = nullptr;

					if (obj.id == g_myid) {
						hBrush = CreateSolidBrush(RGB(255, 0, 0));
					}
					else if (obj.id >= MAX_PLAYERS) {
						switch (obj.visual_id) {
						case 1: hBrush = CreateSolidBrush(RGB(0, 255, 128));   break;
						case 2: hBrush = CreateSolidBrush(RGB(139, 69, 19));   break;
						case 3: hBrush = CreateSolidBrush(RGB(75, 0, 130));    break;
						case 4: hBrush = CreateSolidBrush(RGB(240, 240, 240)); break;
						case 5: hBrush = CreateSolidBrush(RGB(47, 79, 79));    break;
						case 6: hBrush = CreateSolidBrush(RGB(30, 144, 255));  break;
						case 7: hBrush = CreateSolidBrush(RGB(255, 69, 0));    break;
						default: hBrush = CreateSolidBrush(RGB(128, 128, 128)); break;
						}
					}
					else {
						hBrush = CreateSolidBrush(RGB(0, 0, 255));
					}

					HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, hBrush);
					int radius = min(TILE_SIZE / 2, 20) - 2;

					if (obj.visual_id == 7) {
						Ellipse(memDC, drawX - (radius + 6), drawY - (radius + 6), drawX + (radius + 6), drawY + (radius + 6));
					}
					else {
						Ellipse(memDC, drawX - radius, drawY - radius, drawX + radius, drawY + radius);
					}

					SelectObject(memDC, hOldBrush);
					DeleteObject(hBrush);

					// ⭐ [기능 2] 몬스터 및 플레이어 머리 위에 체력 바(HP Bar) 직관적 렌더링
					// 개체 위쪽에 가로 40픽셀, 세로 5픽셀 크기로 배치
					if (obj.max_hp > 0) {
						int hpBarWidth = 40;
						int hpBarHeight = 5;
						int hpBarX = drawX - (hpBarWidth / 2);
						int hpBarY = drawY - radius - 24; // 이름표보다 위쪽에 정렬

						// 1. 체력바 배경 (회색 빈 통)
						RECT rcHpBg = { hpBarX, hpBarY, hpBarX + hpBarWidth, hpBarY + hpBarHeight };
						HBRUSH hBrushBg = CreateSolidBrush(RGB(180, 180, 180));
						FillRect(memDC, &rcHpBg, hBrushBg);
						DeleteObject(hBrushBg);

						// 2. 현재 잔여 체력 비율 계산 후 실시간 전방 바 (초록색) 채우기
						float hpRatio = static_cast<float>(obj.hp) / static_cast<float>(obj.max_hp);
						if (hpRatio < 0.0f) hpRatio = 0.0f;
						int currentHpWidth = static_cast<int>(hpBarWidth * hpRatio);

						RECT rcHpGauge = { hpBarX, hpBarY, hpBarX + currentHpWidth, hpBarY + hpBarHeight };
						HBRUSH hBrushGauge = CreateSolidBrush(RGB(0, 220, 50));
						FillRect(memDC, &rcHpGauge, hBrushGauge);
						DeleteObject(hBrushGauge);
					}

					// 이름표 렌더링 위치 조정 (체력바 아래, 오브젝트 위)
					TextOutA(memDC, drawX - 20, drawY - radius - 16, obj.username, (int)strlen(obj.username));

					// 어택 이펙트 드로잉 파트
					DWORD currentTime = GetTickCount();
					if (currentTime - obj.last_attack_time < ATTACK_EFFECT_DURATION) {
						HPEN effectPen = CreatePen(PS_SOLID, 3, RGB(255, 165, 0));
						HPEN oldPen = (HPEN)SelectObject(memDC, effectPen);

						HBRUSH transparentBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
						HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, transparentBrush);

						int max_range = (obj.weapon == spear) ? 2 : 1;

						for (int ey = -max_range; ey <= max_range; ++ey) {
							for (int ex = -max_range; ex <= max_range; ++ex) {
								bool is_hit_tile = false;

								switch (obj.weapon) {
								case hammer:
									if (abs(ex) + abs(ey) <= 1) is_hit_tile = true;
									break;
								case spear:
									if (max(abs(ex), abs(ey)) <= 2) is_hit_tile = true;
									break;
								case sword:
								default:
									if (max(abs(ex), abs(ey)) <= 1) is_hit_tile = true;
									break;
								}

								if (is_hit_tile) {
									int effectCenterX = drawX + (ex * TILE_SIZE);
									int effectCenterY = drawY + (ey * TILE_SIZE);

									Rectangle(memDC,
										effectCenterX - TILE_SIZE / 2,
										effectCenterY - TILE_SIZE / 2,
										effectCenterX + TILE_SIZE / 2,
										effectCenterY + TILE_SIZE / 2);
								}
							}
						}
						SelectObject(memDC, oldBrush);
						SelectObject(memDC, oldPen);
						DeleteObject(effectPen);
					}
				}
			}

			// HUD 및 기타 UI 컨텐츠 렌더링 유지
			char hudText[256];
			sprintf_s(hudText, "Name: %s (Lv.%d) | HP: %d/%d | Pos: (%d,%d) Gold: %dG",
				myObj.username, myObj.level, myObj.hp, myObj.max_hp, myObj.x, myObj.y, myObj.gold);
			TextOutA(memDC, 10, 10, hudText, (int)strlen(hudText));

			if (g_selected_target_id != -1 && g_objects.count(g_selected_target_id)) {
				ClientObject& target = g_objects[g_selected_target_id];
				RECT statRect = { width - 220, height - 150, width - 20, height - 20 };
				FillRect(memDC, &statRect, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

				char statText[256];
				sprintf_s(statText, "Target: %s\nLv: %d\nHP: %d/%d", target.username, target.level, target.hp, target.max_hp);
				RECT textRect = statRect; textRect.left += 10; textRect.top += 10;
				DrawTextA(memDC, statText, -1, &textRect, DT_LEFT);

				if (target.id != g_myid) {
					g_rect_invite_btn = { statRect.left + 10, statRect.bottom - 40, statRect.right - 10, statRect.bottom - 10 };
					FillRect(memDC, &g_rect_invite_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
					DrawTextA(memDC, "파티 초대", -1, &g_rect_invite_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					g_rect_leave_btn = { 0,0,0,0 };
				}
				else {
					g_rect_leave_btn = { statRect.left + 10, statRect.bottom - 40, statRect.right - 10, statRect.bottom - 10 };
					FillRect(memDC, &g_rect_leave_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
					DrawTextA(memDC, "파티 탈퇴", -1, &g_rect_leave_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					g_rect_invite_btn = { 0,0,0,0 };
				}
			}

			if (g_show_reinforce_ui) {
				RECT uiRect = { CENTER_X - 150, CENTER_Y - 100, CENTER_X + 150, CENTER_Y + 100 };
				FillRect(memDC, &uiRect, (HBRUSH)GetStockObject(GRAY_BRUSH));

				char rnfText[256];
				sprintf_s(rnfText, "강화소 (현재 +%d강)\n비용: %dG", myObj.reinforce_level, (myObj.reinforce_level + 1) * 100);
				RECT textRect = uiRect; textRect.left += 20; textRect.top += 20;
				DrawTextA(memDC, rnfText, -1, &textRect, DT_LEFT);

				g_rect_reinforce_btn = { uiRect.left + 20, uiRect.bottom - 50, uiRect.left + 120, uiRect.bottom - 20 };
				FillRect(memDC, &g_rect_reinforce_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "강화 시도", -1, &g_rect_reinforce_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

				g_rect_close_reinforce_btn = { uiRect.right - 120, uiRect.bottom - 50, uiRect.right - 20, uiRect.bottom - 20 };
				FillRect(memDC, &g_rect_close_reinforce_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "닫기", -1, &g_rect_close_reinforce_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}

			if (g_show_party_invite_popup) {
				RECT popRect = { CENTER_X - 150, CENTER_Y - 150, CENTER_X + 150, CENTER_Y + 10 };
				FillRect(memDC, &popRect, (HBRUSH)GetStockObject(GRAY_BRUSH));

				char inviteText[256];
				sprintf_s(inviteText, " 파티 초대 요청\n\n [%s] 님이\n 당신을 초대했습니다.\n\n 수락하시겠습니까?", g_inviter_name);
				RECT textRect = popRect; textRect.left += 20; textRect.top += 15;
				DrawTextA(memDC, inviteText, -1, &textRect, DT_LEFT);

				g_rect_party_accept_btn = { popRect.left + 20, popRect.bottom - 45, popRect.left + 130, popRect.bottom - 15 };
				FillRect(memDC, &g_rect_party_accept_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "수락", -1, &g_rect_party_accept_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

				g_rect_party_refuse_btn = { popRect.right - 130, popRect.bottom - 45, popRect.right - 20, popRect.bottom - 15 };
				FillRect(memDC, &g_rect_party_refuse_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "거절", -1, &g_rect_party_refuse_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}

			if (g_party_member_count > 1) {
				int startY = 40;
				TextOutA(memDC, 10, startY, "=== PARTY MEMBERS ===", 21);
				startY += 20;

				for (int i = 0; i < g_party_member_count; ++i) {
					if (g_party_members[i].id == g_myid) continue;

					char memberText[256];
					sprintf_s(memberText, "[파티원] %s (Lv.%d) | HP: %d/%d",
						g_party_members[i].username, g_party_members[i].level, g_party_members[i].hp, g_party_members[i].max_hp);
					TextOutA(memDC, 15, startY, memberText, (int)strlen(memberText));
					startY += 20;
				}
			}
		}

		BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
		SelectObject(memDC, oldBitmap);
		DeleteObject(memBitmap);
		DeleteDC(memDC);

		EndPaint(hWnd, &ps);
		break;
	}
	case WM_DESTROY:
		KillTimer(hWnd, 1);
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}


// --- 프로그램 진입점 및 윈도우 생성부 ---

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	MyRegisterClass(hInstance);

	// 애플리케이션 초기화를 수행합니다:
	if (!InitInstance(hInstance, nCmdShow))
	{
		return FALSE;
	}

	if (!LoadMapCSV("..\\..\\Resource\\world.csv")) {
		MessageBox(g_hWnd, L"world.csv 파일을 찾을 수 없습니다!", L"에러", MB_OK);
	}
	else {
		OutputDebugStringA("Map loaded successfully!\n");
	}

	MSG msg;

	std::wcout.imbue(std::locale("korean"));
	WSADATA wsa_data{};
	WSAStartup(MAKEWORD(2, 2), &wsa_data);
	g_s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

	// 서버 연결
	int result = WSAConnect(g_s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display(L"서버 연결 실패", WSAGetLastError());
		return 1;
	}

	std::cout << "서버 연결 성공!" << std::endl;

	// 서버 연결 직후 콘솔창에 로그인 입력 받기
	send_login_packet();

	DWORD recv_flag = 0;
	ZeroMemory(&g_recv_over, sizeof(g_recv_over));
	result = WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_over, recv_callback);
	if (result == SOCKET_ERROR) {
		int err_no = WSAGetLastError();
		if (err_no != WSA_IO_PENDING) {
			error_display(L"Recv 실패", WSAGetLastError());
			exit(1);
		}
	}

	SetTimer(g_hWnd, 1, 16, NULL);

	// 메인 메시지 루프
	while (true)
	{
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				break;

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			// APC 큐에 쌓인 콜백 함수들을 실행해주는 아주 중요한 부분!
			SleepEx(0, TRUE);
		}
	}

	closesocket(g_s_socket);
	WSACleanup();

	return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = szWindowClass;
	wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

	return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hInst = hInstance;

	RECT rect = { 0, 0, winLength, winLength };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, TRUE);

	int width = rect.right - rect.left;
	int height = rect.bottom - rect.top;

	HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, width, height, nullptr, nullptr, hInstance, nullptr);

	if (!hWnd)
	{
		return FALSE;
	}

	g_hWnd = hWnd;

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	return TRUE;
}