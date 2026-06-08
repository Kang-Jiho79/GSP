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
		strncpy_s(obj.username, packet->username, _TRUNCATE);

		g_objects[g_myid] = obj;
		should_repaint = true;

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
	case WM_KEYDOWN:
	{
		short dx = 0, dy = 0;
		switch (wParam) {
		case VK_LEFT:  dx = -1; break;
		case VK_RIGHT: dx = 1;  break;
		case VK_UP:    dy = -1; break;
		case VK_DOWN:  dy = 1;  break;
		case VK_SPACE:
			send_attack_packet();
			if (g_myid != -1 && g_objects.count(g_myid)) {
				g_objects[g_myid].last_attack_time = GetTickCount(); // ⭐ 내 공격 시간 갱신
			}
			InvalidateRect(hWnd, NULL, TRUE);
			SetTimer(hWnd, 1, ATTACK_EFFECT_DURATION, NULL);
			break;
		case 'A':
		case 'a':
		{
			if (g_myid != -1 && g_objects.count(g_myid)) {
				auto& myObj = g_objects[g_myid];

				// 내가 현재 포탈 위에 서 있는지 루프를 돌며 확인
				bool isOnPortal = false;
				for (const auto& portal : Portals) {
					if (myObj.x == portal.src_x && myObj.y == portal.src_y) {
						send_dungeon_entry_packet(portal.dungeon);
						std::cout << "[디버그] A키 상호작용! 던전 입장 요청을 보냈습니다. 타입: " << portal.dungeon << std::endl;
						isOnPortal = true;
						break; // 포탈을 찾았으니 루프 탈출
					}
				}

				if (!isOnPortal) {
					bool is_near_npc = false;

					// ⭐ NPC 테이블을 뒤져서 내가 상인 주변에 서 있는지 검사
					for (const auto& npc : g_npc_spawns) {
						if (abs(myObj.x - npc.x) <= 1 && abs(myObj.y - npc.y) <= 1) {
							is_near_npc = true;
							break;
						}
					}

					if (is_near_npc) {
						if (g_show_reinforce_ui) {
							g_show_reinforce_ui = false; // 토글식 닫기
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
		case VK_ESCAPE: // ESC 누르면 UI 닫기
			g_selected_target_id = -1;
			g_show_reinforce_ui = false;
			InvalidateRect(hWnd, NULL, TRUE);
			break;
		}
		if (dx != 0 || dy != 0) send_move_packet(dx, dy);
		break;
	}

	case WM_TIMER:
		if (wParam == 1) // 1번 타이머라면
		{
			InvalidateRect(hWnd, NULL, TRUE); // 화면 갱신
		}
		break;

	case WM_LBUTTONDOWN: // [기능 4, 5] 마우스 클릭 처리
	{
		int mouseX = LOWORD(lParam);
		int mouseY = HIWORD(lParam);
		POINT pt = { mouseX, mouseY };

		if (g_show_party_invite_popup) {
			if (PtInRect(&g_rect_party_accept_btn, pt)) {
				// 수락 버튼 클릭 시 서버로 수락 패킷 전송
				send_party_accept_packet(g_inviter_name);
				g_show_party_invite_popup = false; // 팝업 닫기
				return 0;
			}
			else if (PtInRect(&g_rect_party_refuse_btn, pt)) {
				// 거절 버튼 클릭 시 서버로 거절 패킷 전송
				send_party_refuse_packet(g_inviter_name);
				g_show_party_invite_popup = false; // 팝업 닫기
				return 0;
			}
		}

		// 1. 강화창이 열려있을 때 버튼 클릭 확인
		if (g_show_reinforce_ui) {
			if (PtInRect(&g_rect_reinforce_btn, pt)) {
				// 현재 무기와 강화 레벨을 서버로 전송
				ClientObject& myObj = g_objects[g_myid];
				send_reinforce_packet(myObj.weapon, myObj.reinforce_level, myObj.gold);
			}
			else if (PtInRect(&g_rect_close_reinforce_btn, pt)) {
				g_show_reinforce_ui = false; // 닫기 버튼
				InvalidateRect(hWnd, NULL, TRUE);
			}
			return 0; // UI 클릭 시 캐릭터 클릭은 무시
		}

		// 2. 타겟 상태창이 열려있을 때 파티 초대 버튼 클릭 확인
		if (g_selected_target_id != -1 && g_selected_target_id != g_myid) {
			if (PtInRect(&g_rect_invite_btn, pt)) {
				std::string targetName = g_objects[g_selected_target_id].username;
				send_party_invite_packet(targetName);
				return 0;
			}
		}

		// 2. [파티 탈퇴 버튼] - 타겟팅 상태와 무관하게 언제나 독립적으로 작동
		if (PtInRect(&g_rect_leave_btn, pt)) {
			send_party_leave_packet();
			return 0;
		}

		// 3. 필드 위 캐릭터(원형) 클릭 확인
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

					// 마우스 좌표와 오브젝트 중심 간의 거리 계산 (피타고라스)
					int distSq = (mouseX - drawX) * (mouseX - drawX) + (mouseY - drawY) * (mouseY - drawY);
					if (distSq <= radius * radius) {
						g_selected_target_id = obj.id; // 타겟 설정
						clicked_someone = true;
						InvalidateRect(hWnd, NULL, TRUE);
						break;
					}
				}
			}
			// 허공을 클릭하면 타겟 해제
			if (!clicked_someone) {
				g_selected_target_id = -1;
				InvalidateRect(hWnd, NULL, TRUE);
			}
		}
		break;
	}

	// ⭐ [핵심 1] 윈도우 기본 배경 지우기 무시 (깜빡임의 주원인 제거)
	case WM_ERASEBKGND:
		return 1; // 1(TRUE)을 반환하면 운영체제가 배경을 지우지 않음

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);

		// 현재 클라이언트 영역(창 크기) 가져오기
		RECT rect;
		GetClientRect(hWnd, &rect);
		int width = rect.right - rect.left;
		int height = rect.bottom - rect.top;

		// ⭐ [핵심 2] 더블 버퍼링을 위한 메모리 DC 및 비트맵 생성
		HDC memDC = CreateCompatibleDC(hdc);
		HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
		HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

		// 가상 도화지(memDC)의 배경을 흰색으로 초기화
		HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 255));
		FillRect(memDC, &rect, bgBrush);
		DeleteObject(bgBrush);

		// --- 렌더링 시작 (이제부터 모든 그리기 함수는 hdc가 아닌 memDC를 사용합니다!) ---
		if (g_myid != -1 && g_objects.count(g_myid)) {
			ClientObject& myObj = g_objects[g_myid];

			const int VIEW_RADIUS = 10;
			const int TILE_SIZE = winLength / (VIEW_RADIUS * 2);
			const int CENTER_X = winLength / 2;
			const int CENTER_Y = winLength / 2;

			// 맵 렌더링
			for (int dy = -VIEW_RADIUS; dy <= VIEW_RADIUS; ++dy) {
				for (int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; ++dx) {
					int mapX = myObj.x + dx;
					int mapY = myObj.y + dy;

					if (mapX >= 0 && mapX < MAP_WIDTH && mapY >= 0 && mapY < MAP_HEIGHT) {
						int drawX = CENTER_X + (dx * TILE_SIZE);
						int drawY = CENTER_Y + (dy * TILE_SIZE);

						// ⭐ [포탈 렌더링 핵심 로직 추가]
						HBRUSH tileBrush = nullptr;
						bool is_portal_tile = false;

						for (const auto& portal : Portals) {
							// 현재 그리고 있는 타일 좌표가 포탈 입구 좌표라면?
							if (mapX == portal.src_x && mapY == portal.src_y) {
								if (portal.dungeon == FINAL_BOSS) {
									tileBrush = CreateSolidBrush(RGB(255, 69, 0)); // 8번 최종 보스 포탈: 오렌지 레드
								}
								else {
									tileBrush = CreateSolidBrush(RGB(0, 191, 255)); // 1~7번 일반 던전 포탈: 딥 스카이블루
								}
								is_portal_tile = true;
								break;
							}
						}

						// 포탈 타일이 아닐 때만 기존 일반 타일(벽/바닥) 처리
						if (!is_portal_tile) {
							bool is_npc_tile = false;

							// ⭐ 현재 그리고 있는 타일 좌표가 NPC 스폰 좌표인지 테이블 검사
							for (const auto& npc : g_npc_spawns) {
								if (mapX == npc.x && mapY == npc.y) {
									tileBrush = CreateSolidBrush(RGB(255, 215, 0)); // 황금색으로 NPC 표시
									is_npc_tile = true;
									break;
								}
							}

							// NPC 타일도 아니라면 평범한 바닥/벽 렌더링
							if (!is_npc_tile) {
								tileBrush = (g_map[mapY][mapX] == 0) ?
									CreateSolidBrush(RGB(255, 255, 255)) :
									CreateSolidBrush(RGB(0, 0, 0));
							}
						}

						RECT tileRect = { drawX, drawY, drawX + TILE_SIZE, drawY + TILE_SIZE };
						FillRect(memDC, &tileRect, tileBrush); // memDC 사용
						DeleteObject(tileBrush);

						HBRUSH borderBrush = CreateSolidBrush(RGB(220, 220, 220));
						FrameRect(memDC, &tileRect, borderBrush); // memDC 사용
						DeleteObject(borderBrush);
					}
				}
			}

			// 오브젝트 렌더링
			for (auto& pair : g_objects) {
				ClientObject& obj = pair.second;

				int diffX = obj.x - myObj.x;
				int diffY = obj.y - myObj.y;

				if (abs(diffX) <= VIEW_RADIUS && abs(diffY) <= VIEW_RADIUS) {
					int drawX = CENTER_X + (diffX * TILE_SIZE) + (TILE_SIZE / 2);
					int drawY = CENTER_Y + (diffY * TILE_SIZE) + (TILE_SIZE / 2);

					// --- [기능 1] Visual ID 및 속성에 따른 색상 구분 렌더링 ---
					HBRUSH hBrush;
					if (obj.id == g_myid) {
						hBrush = CreateSolidBrush(RGB(255, 0, 0)); // 내 캐릭터 (빨강)
					}
					else if (obj.visual_id != player) {
						if (obj.visual_id == npc) hBrush = CreateSolidBrush(RGB(0, 255, 0)); // 강화 NPC (초록)
						else if (obj.visual_id == monster) hBrush = CreateSolidBrush(RGB(128, 0, 128)); // 몬스터 (보라)
						else hBrush = CreateSolidBrush(RGB(150, 150, 150)); // 기타 일반 NPC (회색)
					}
					else {
						hBrush = CreateSolidBrush(RGB(0, 0, 255)); // 타 플레이어 (파랑)
					}

					HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, hBrush);

					int radius = min(TILE_SIZE / 2, 20) - 2;
					Ellipse(memDC, drawX - radius, drawY - radius, drawX + radius, drawY + radius);
					SelectObject(memDC, hOldBrush);
					DeleteObject(hBrush);
					DWORD currentTime = GetTickCount();
					// ⭐ [변경됨] 내가 아니더라도, 150ms 내에 공격한 '모든' 캐릭터에 이펙트 그리기
					if (currentTime - obj.last_attack_time < ATTACK_EFFECT_DURATION) {
						HPEN effectPen = CreatePen(PS_SOLID, 3, RGB(255, 165, 0)); // 주황색 테두리 펜
						HPEN oldPen = (HPEN)SelectObject(memDC, effectPen);

						// 네모 속을 투명하게 칠하기 위해 NULL_BRUSH 사용
						HBRUSH transparentBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
						HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, transparentBrush);

						int max_range = (obj.weapon == spear) ? 2 : 1;

						// 타격 범위 시각화를 위한 이중 루프
						for (int ey = -max_range; ey <= max_range; ++ey) {
							for (int ex = -max_range; ex <= max_range; ++ex) {
								bool is_hit_tile = false;

								switch (obj.weapon) {
								case hammer: // 십자 1칸
									if (abs(ex) + abs(ey) <= 1) is_hit_tile = true;
									break;
								case spear:  // 주변 2칸
									if (max(abs(ex), abs(ey)) <= 2) is_hit_tile = true;
									break;
								case sword:  // 주변 1칸
								default:     // ⭐ [해결!] 무기 정보가 쓰레기값이거나 아직 모를 땐 기본(검) 형태로 무조건 그리기!
									if (max(abs(ex), abs(ey)) <= 1) is_hit_tile = true;
									break;
								}

								if (is_hit_tile) {
									// 이펙트를 그릴 타일의 중심 좌표 계산
									int effectCenterX = drawX + (ex * TILE_SIZE);
									int effectCenterY = drawY + (ey * TILE_SIZE);

									// 해당 타일 크기만한 네모 테두리 그리기
									Rectangle(memDC,
										effectCenterX - TILE_SIZE / 2,
										effectCenterY - TILE_SIZE / 2,
										effectCenterX + TILE_SIZE / 2,
										effectCenterY + TILE_SIZE / 2);
								}
							}
						}

						// 자원 해제
						SelectObject(memDC, oldBrush);
						SelectObject(memDC, oldPen);
						DeleteObject(effectPen);
					}

					TextOutA(memDC, drawX - 15, drawY - (radius + 15), obj.username, (int)strlen(obj.username));
				}
			}

			// --- [기능 3] 내 HUD 표시 (좌측 상단) ---
			char hudText[256];
			sprintf_s(hudText, "Name: %s (Lv.%d) | HP: %d/%d | Pos: (%d,%d) Gold: %dG",
				myObj.username, myObj.level, myObj.hp, myObj.max_hp, myObj.x, myObj.y, myObj.gold);
			TextOutA(memDC, 10, 10, hudText, (int)strlen(hudText));

			// --- [기능 4] 타겟 Stat 창 표시 (우측 하단) ---
			if (g_selected_target_id != -1 && g_objects.count(g_selected_target_id)) {
				ClientObject& target = g_objects[g_selected_target_id];

				RECT statRect = { width - 220, height - 150, width - 20, height - 20 };
				FillRect(memDC, &statRect, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

				char statText[256];
				sprintf_s(statText, "Target: %s\nLv: %d\nHP: %d/%d", target.username, target.level, target.hp, target.max_hp);

				RECT textRect = statRect; textRect.left += 10; textRect.top += 10;
				DrawTextA(memDC, statText, -1, &textRect, DT_LEFT);

				// 파티 초대 버튼 (나 자신이 아니고, NPC도 아닐 때만 표시)
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
					g_rect_invite_btn = { 0,0,0,0 }; // 안 그릴 때는 클릭 안되게 초기화
				}
			}

			// --- [기능 5] 강화 NPC UI 표시 (화면 중앙) ---
			if (g_show_reinforce_ui) {
				RECT uiRect = { CENTER_X - 150, CENTER_Y - 100, CENTER_X + 150, CENTER_Y + 100 };
				FillRect(memDC, &uiRect, (HBRUSH)GetStockObject(GRAY_BRUSH));

				char rnfText[256];
				sprintf_s(rnfText, "강화소 (현재 +%d강)\n비용: %dG", myObj.reinforce_level, (myObj.reinforce_level + 1) * 100);
				RECT textRect = uiRect; textRect.left += 20; textRect.top += 20;
				DrawTextA(memDC, rnfText, -1, &textRect, DT_LEFT);

				// 강화 시도 버튼
				g_rect_reinforce_btn = { uiRect.left + 20, uiRect.bottom - 50, uiRect.left + 120, uiRect.bottom - 20 };
				FillRect(memDC, &g_rect_reinforce_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "강화 시도", -1, &g_rect_reinforce_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

				// 닫기 버튼
				g_rect_close_reinforce_btn = { uiRect.right - 120, uiRect.bottom - 50, uiRect.right - 20, uiRect.bottom - 20 };
				FillRect(memDC, &g_rect_close_reinforce_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "닫기", -1, &g_rect_close_reinforce_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}

			if (g_show_party_invite_popup) {
				// 강화창과 겹치지 않게 약간 위쪽에 팝업 배치
				RECT popRect = { CENTER_X - 150, CENTER_Y - 150, CENTER_X + 150, CENTER_Y + 10 };
				FillRect(memDC, &popRect, (HBRUSH)GetStockObject(GRAY_BRUSH));

				// 초대 문구 출력
				char inviteText[256];
				sprintf_s(inviteText, " 파티 초대 요청\n\n [%s] 님이\n 당신을 초대했습니다.\n\n 수락하시겠습니까?", g_inviter_name);
				RECT textRect = popRect; textRect.left += 20; textRect.top += 15;
				DrawTextA(memDC, inviteText, -1, &textRect, DT_LEFT);

				// [수락] 버튼 영역 세팅 및 그리기
				g_rect_party_accept_btn = { popRect.left + 20, popRect.bottom - 45, popRect.left + 130, popRect.bottom - 15 };
				FillRect(memDC, &g_rect_party_accept_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "수락", -1, &g_rect_party_accept_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

				// [거절] 버튼 영역 세팅 및 그리기
				g_rect_party_refuse_btn = { popRect.right - 130, popRect.bottom - 45, popRect.right - 20, popRect.bottom - 15 };
				FillRect(memDC, &g_rect_party_refuse_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DrawTextA(memDC, "거절", -1, &g_rect_party_refuse_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}

			if (g_party_member_count > 1) {
				int startY = 40;

				TextOutA(memDC, 10, startY, "=== PARTY MEMBERS ===", 21);
				startY += 20;

				for (int i = 0; i < g_party_member_count; ++i) {
					if (g_party_members[i].id == g_myid) continue; // 나 자신은 제외

					char memberText[256];
					// ⭐ 이름 (Lv.레벨) | HP: 현재 / 최대 구조로 출력
					sprintf_s(memberText, "[파티원] %s (Lv.%d) | HP: %d/%d",
						g_party_members[i].username,
						g_party_members[i].level,
						g_party_members[i].hp,
						g_party_members[i].max_hp);

					TextOutA(memDC, 15, startY, memberText, (int)strlen(memberText));
					startY += 20;
				}
			}
		}
		// --- 렌더링 끝 ---

		// ⭐ [핵심 3] 완성된 메모리 도화지를 실제 모니터(hdc)로 고속 복사
		BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

		// 메모리 누수를 막기 위한 자원 반환 (매우 중요)
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