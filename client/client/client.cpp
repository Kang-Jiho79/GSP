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

#include "..\..\Server\server\protocol_2026.h"

#define MAX_LOADSTRING 100
#define winLength 800


const int MAP_WIDTH = 2000;
const int MAP_HEIGHT = 2000;
std::vector<std::vector<int>> g_map(MAP_HEIGHT, std::vector<int>(MAP_WIDTH, 0));

// --- 전역 변수 ---
HWND g_hWnd;
HINSTANCE hInst;                                // 현재 인스턴스입니다.
WCHAR szTitle[MAX_LOADSTRING] = L"9-Realms Client";                  // 제목 표시줄 텍스트입니다.
WCHAR szWindowClass[MAX_LOADSTRING] = L"IOCPCLIENT";            // 기본 창 클래스 이름입니다.

char g_recv_buf[BUF_SIZE * 2]; 
WSABUF g_recv_wsa_buf{ sizeof(g_recv_buf), g_recv_buf };
WSAOVERLAPPED g_recv_over{};
SOCKET g_s_socket;

int g_myid = -1;
int g_prev_size = 0;

int g_selected_target_id = -1;    
bool g_show_reinforce_ui = false; 

const DWORD ATTACK_EFFECT_DURATION = 150; 
bool g_show_party_invite_popup = false;  
int g_inviter_id = -1;                  
char g_inviter_name[MAX_NAME_LEN] = "";  
bool g_is_space_pressed = false;
int g_next_reinforce_cost = -1;
bool g_am_I_confused = false;
bool g_show_warn_zone = false;
short g_warn_x = 0, g_warn_y = 0;
int g_warn_radius = 0;
DWORD g_warn_start_tick = 0, g_warn_duration = 0;
RECT g_rect_invite_btn = { 0, 0, 0, 0 };
RECT g_rect_leave_btn = { 0, 0, 0, 0 };
RECT g_rect_party_accept_btn = { 0, 0, 0, 0 };
RECT g_rect_party_refuse_btn = { 0, 0, 0, 0 };
RECT g_rect_reinforce_btn = { 0, 0, 0, 0 };
RECT g_rect_close_reinforce_btn = { 0, 0, 0, 0 };

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

std::unordered_map<int, ClientObject> g_objects;

HWND g_hChatLog;
HWND g_hChatInput;
WNDPROC g_pEditOldProc;

struct EX_OVERLAPPED {
	WSAOVERLAPPED over;
	char* send_mem_buf;
};

// --- 함수 선언 ---
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);
void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);
void error_display(const wchar_t* msg, int err_no);
LRESULT CALLBACK ChatInputSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void AppendChatLog(const std::string& message);
void DirectSendPacket(void* packet_struct, size_t packet_size);

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

int GetWeaponDamage(WEAPON_TYPE weapon, unsigned char reinforce_level) {
	if (weapon == null) return 10;

	unsigned char safe_level = (reinforce_level > 5) ? 5 : reinforce_level;

	if (weapon == sword) {
		int table[] = { 100, 120, 150, 200, 300, 500 };
		return table[safe_level];
	}
	else if (weapon == hammer) {
		int table[] = { 150, 180, 220, 300, 450, 700 };
		return table[safe_level];
	}
	else if (weapon == spear) {
		int table[] = { 90, 110, 140, 190, 250, 400 };
		return table[safe_level];
	}
	return 10;
}

void DirectSendPacket(void* packet_struct, size_t packet_size) {
	char* allocated_buffer = new char[packet_size];
	memcpy(allocated_buffer, packet_struct, packet_size);

	EX_OVERLAPPED* ex_over = new EX_OVERLAPPED;
	ZeroMemory(ex_over, sizeof(EX_OVERLAPPED));
	ex_over->send_mem_buf = allocated_buffer;

	WSABUF wsa_buf;
	wsa_buf.buf = allocated_buffer;
	wsa_buf.len = static_cast<ULONG>(packet_size);

	DWORD sent_size = 0;
	int ret = WSASend(g_s_socket, &wsa_buf, 1, &sent_size, 0, &(ex_over->over), send_callback);

	if (ret == SOCKET_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			delete[] ex_over->send_mem_buf;
			delete ex_over;
		}
	}
}

void send_login_packet()
{
	C2S_Login packet{};
	packet.size = sizeof(C2S_Login);
	packet.type = C2S_LOGIN;

	std::string username;
	std::cout << "Enter username: ";
	std::getline(std::cin, username);
	strncpy_s(packet.username, MAX_NAME_LEN, username.c_str(), MAX_NAME_LEN - 1);

	DirectSendPacket(&packet, sizeof(C2S_Login));
	std::cout << "Login packet sent for username: " << username << std::endl;
}

void send_move_packet(short dx, short dy)
{
	C2S_Move packet{};
	packet.size = sizeof(C2S_Move);
	packet.type = C2S_MOVE;
	packet.x = dx;
	packet.y = dy;
	packet.move_time = 100; 
	DirectSendPacket(&packet, sizeof(C2S_Move));
}

void send_chat_packet(const std::string& message)
{
	C2S_Chat packet{};
	packet.size = sizeof(C2S_Chat);
	packet.type = C2S_CHAT;
	strncpy_s(packet.message, MAX_CHAT_MSG_LEN, message.c_str(), _TRUNCATE);

	DirectSendPacket(&packet, sizeof(C2S_Chat));
}

void send_attack_packet()
{
	C2S_Attack packet{};
	packet.size = sizeof(C2S_Attack);
	packet.type = C2S_ATTACK;

	DirectSendPacket(&packet, sizeof(C2S_Attack));
}

void send_teleport_packet(short dest_x, short dest_y)
{
	C2S_Teleport packet{};
	packet.size = sizeof(C2S_Teleport);
	packet.type = C2S_TELEPORT;
	packet.x = dest_x;
	packet.y = dest_y;

	DirectSendPacket(&packet, sizeof(C2S_Teleport));
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

	DirectSendPacket(&packet, sizeof(C2S_SelectWeapon));
}

void send_request_info_packet(const std::string& target_username)
{
	C2S_RequestInfo packet{};
	packet.size = sizeof(C2S_RequestInfo);
	packet.type = C2S_REQUEST_INFO;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);

	DirectSendPacket(&packet, sizeof(C2S_RequestInfo));
}

void send_dungeon_entry_packet(DUNGEON_TYPE dungeon)
{
	C2S_DungeonEntry packet{};
	packet.size = sizeof(C2S_DungeonEntry);
	packet.type = C2S_DUNGEON_ENTRY;
	packet.dungeon = dungeon;

	DirectSendPacket(&packet, sizeof(C2S_DungeonEntry));
}

void send_interact_packet()
{
	C2S_Interact packet{};
	packet.size = sizeof(C2S_Interact);
	packet.type = C2S_INTERACT;

	DirectSendPacket(&packet, sizeof(C2S_Interact));
}

void send_reinforce_packet(WEAPON_TYPE weapon, unsigned char reinforce_level, int gold)
{
	C2S_Reinforce packet{};
	packet.size = sizeof(C2S_Reinforce);
	packet.type = C2S_REINFORCE;
	packet.weapon = weapon;
	packet.reinforce_level = reinforce_level;

	DirectSendPacket(&packet, sizeof(C2S_Reinforce));
}

void send_party_invite_packet(const std::string& target_username)
{
	C2S_InviteParty packet{};
	packet.size = sizeof(C2S_InviteParty);
	packet.type = C2S_INVITE_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);

	DirectSendPacket(&packet, sizeof(C2S_InviteParty));
}

void send_party_accept_packet(const std::string& target_username)
{
	C2S_AcceptParty packet{};
	packet.size = sizeof(C2S_AcceptParty);
	packet.type = C2S_ACCEPT_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);

	DirectSendPacket(&packet, sizeof(C2S_AcceptParty));
}

void send_party_refuse_packet(const std::string& target_username)
{
	C2S_RefuseParty packet{};
	packet.size = sizeof(C2S_RefuseParty);
	packet.type = C2S_REFUSE_PARTY;
	strncpy_s(packet.target_username, MAX_NAME_LEN, target_username.c_str(), MAX_NAME_LEN - 1);

	DirectSendPacket(&packet, sizeof(C2S_RefuseParty));
}

void send_party_leave_packet()
{
	C2S_LeaveParty packet{};
	packet.size = sizeof(C2S_LeaveParty);
	packet.type = C2S_LEAVE_PARTY;

	DirectSendPacket(&packet, sizeof(C2S_LeaveParty));
}

void send_logout_packet()
{
	C2S_Logout packet{};
	packet.size = sizeof(C2S_Logout);
	packet.type = C2S_LOGOUT;

	DirectSendPacket(&packet, sizeof(C2S_Logout));
}

void process_packet(unsigned char* p)
{
	PACKET_TYPE type = static_cast<PACKET_TYPE>(p[1]);
	bool should_repaint = false;

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
			std::cout << "\n[9-Realms] 캐릭터 스탯 및 소지금을 안전하게 동기화했습니다." << std::endl;
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
		AppendChatLog(packet->message);
		std::cout << packet->message << std::endl;
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

			if (g_hWnd) {
				SetFocus(g_hWnd);
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
		break;
	}
	case S2C_INTERACT_RESULT:
	{
		S2C_InteractResult* packet = reinterpret_cast<S2C_InteractResult*>(p);
		if (packet->success) {
			g_show_reinforce_ui = true;
			g_next_reinforce_cost = std::stoi(packet->message);
			should_repaint = true;
		}
		break;
	}
	case S2C_REINFORCE_RESULT:
	{
		S2C_ReinforceResult* packet = reinterpret_cast<S2C_ReinforceResult*>(p);

		if (g_myid != -1 && g_objects.count(g_myid)) {
			g_objects[g_myid].reinforce_level = packet->reinforce_level;
			g_objects[g_myid].gold = packet->gold;
			should_repaint = true;
		}
		break;
	}
	case S2C_PARTY_INVITE_NOTI:
	{
		S2C_PartyInviteNoti* packet = reinterpret_cast<S2C_PartyInviteNoti*>(p);
		g_inviter_id = packet->playerId;
		strncpy_s(g_inviter_name, packet->inviter_username, MAX_NAME_LEN);
		g_show_party_invite_popup = true;
		break;
	}
	case S2C_PARTY_UPDATE:
	{
		S2C_PartyUpdate* packet = reinterpret_cast<S2C_PartyUpdate*>(p);
		g_party_member_count = packet->party_member_count;

		if (g_party_member_count == 0) {
			memset(g_party_members, 0, sizeof(g_party_members));
		}
		else {
			for (int i = 0; i < g_party_member_count; ++i) {
				g_party_members[i].id = packet->party_members[i].playerId;
				strncpy_s(g_party_members[i].username, packet->party_members[i].username, MAX_NAME_LEN);
				g_party_members[i].hp = packet->party_members[i].hp;
				g_party_members[i].max_hp = packet->party_members[i].max_hp;
				g_party_members[i].level = packet->party_members[i].level;
			}
		}

		if (g_myid != -1 && g_objects.count(g_myid)) {
			g_objects[g_myid].in_party = (g_party_member_count > 1);
		}

		if (g_hWnd) InvalidateRect(g_hWnd, NULL, TRUE);
		break;
	}
	case S2C_ATTACK_BROADCAST:
	{
		S2C_AttackBroadcast* packet = reinterpret_cast<S2C_AttackBroadcast*>(p);
		if (g_objects.count(packet->attacker_id)) {
			g_objects[packet->attacker_id].last_attack_time = GetTickCount64();
			g_objects[packet->attacker_id].weapon = packet->weapon;
			should_repaint = true;
		}
		break;
	}
	case S2C_GOLD_UPDATE:
	{
		S2C_GoldUpdate* packet = reinterpret_cast<S2C_GoldUpdate*>(p);
		g_objects[g_myid].gold = packet->gold;
		should_repaint = true;
		break;
	}
	case S2C_BOSS_WARN_ZONE: {
		S2C_BossWarnZone* packet = reinterpret_cast<S2C_BossWarnZone*>(p);
		g_warn_x = packet->x; g_warn_y = packet->y; g_warn_radius = packet->radius;
		g_warn_duration = packet->duration_ms; g_warn_start_tick = GetTickCount();
		g_show_warn_zone = true; should_repaint = true;
		break;
	}
	case S2C_STATUS_EFFECT: {
		S2C_StatusEffect* packet = reinterpret_cast<S2C_StatusEffect*>(p);
		g_am_I_confused = packet->is_confused;
		break;
	}
	default:
		break;
	}

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
	if (overlapped) {
		EX_OVERLAPPED* ex_over = reinterpret_cast<EX_OVERLAPPED*>(overlapped);
		if (ex_over->send_mem_buf) {
			delete[] ex_over->send_mem_buf;
		}
		delete ex_over;
	}
}

// --- 윈도우 프로시저 ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE: {
		g_hChatLog = CreateWindowW(L"EDIT", NULL,
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			10, 550, 450, 160, hWnd, (HMENU)9001, hInst, NULL);

		g_hChatInput = CreateWindowW(L"EDIT", NULL,
			WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL,
			10, 715, 450, 25, hWnd, (HMENU)9002, hInst, NULL);

		g_pEditOldProc = (WNDPROC)SetWindowLongPtrW(g_hChatInput, GWLP_WNDPROC, (LONG_PTR)ChatInputSubclassProc);
		break;
	}
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
		case VK_RETURN:
			SetFocus(g_hChatInput);
			break;

		case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
		{
			int portal_index = wParam - '1';

			if (portal_index >= 0 && portal_index < (int)Portals.size()) {
				send_teleport_packet(Portals[portal_index].src_x, Portals[portal_index].src_y);
			}
			break;
		}
		case VK_SPACE:
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
		if (dx != 0 || dy != 0) {
			static DWORD last_move_tick = 0;
			DWORD current_tick = GetTickCount();
			if (current_tick - last_move_tick > 60) {
				last_move_tick = current_tick;
				if (g_am_I_confused) { dx = -dx; dy = -dy; }
				send_move_packet(dx, dy);
			}
		}
		break;
	}
	case WM_TIMER: {
		if (wParam == 1)
		{
			InvalidateRect(hWnd, NULL, TRUE);
		}
		break;
	}
	case WM_LBUTTONDOWN:
	{
		int mouseX = LOWORD(lParam);
		int mouseY = HIWORD(lParam);
		POINT pt = { mouseX, mouseY };

		if (g_show_party_invite_popup) {
			if (PtInRect(&g_rect_party_accept_btn, pt)) {
				send_party_accept_packet(g_inviter_name);
				g_show_party_invite_popup = false;
				SetFocus(hWnd);
				return 0;
			}
			else if (PtInRect(&g_rect_party_refuse_btn, pt)) {
				send_party_refuse_packet(g_inviter_name);
				g_show_party_invite_popup = false;
				SetFocus(hWnd);
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
			SetFocus(hWnd);
			return 0;
		}

		if (g_selected_target_id != -1 && g_selected_target_id != g_myid) {
			if (PtInRect(&g_rect_invite_btn, pt)) {
				std::string targetName = g_objects[g_selected_target_id].username;
				send_party_invite_packet(targetName);
				SetFocus(hWnd);
				return 0;
			}
		}

		if (PtInRect(&g_rect_leave_btn, pt)) {
			send_party_leave_packet();
			SetFocus(hWnd);
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
		SetFocus(hWnd);
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

		SetBkMode(memDC, TRANSPARENT);

		if (g_myid != -1 && g_objects.count(g_myid)) {
			ClientObject& myObj = g_objects[g_myid];

			const int VIEW_RADIUS = 10;
			const int TILE_SIZE = winLength / (VIEW_RADIUS * 2);
			const int CENTER_X = winLength / 2;
			const int CENTER_Y = winLength / 2;

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

						if (g_show_warn_zone) {
							if (GetTickCount() - g_warn_start_tick > g_warn_duration) {
								g_show_warn_zone = false;
							}
							else {
								if (max(abs(g_warn_x - mapX), abs(g_warn_y - mapY)) <= g_warn_radius) {
									HBRUSH warnBrush = CreateHatchBrush(HS_DIAGCROSS, RGB(255, 0, 0));

									int oldBkMode = SetBkMode(memDC, TRANSPARENT);
									FillRect(memDC, &tileRect, warnBrush);

									SetBkMode(memDC, oldBkMode);
									DeleteObject(warnBrush);
								}
							}
						}

						HBRUSH borderBrush = CreateSolidBrush(RGB(220, 220, 220));
						FrameRect(memDC, &tileRect, borderBrush);
						DeleteObject(borderBrush);
						SetTextColor(memDC, RGB(255, 69, 0));
						SetBkMode(memDC, TRANSPARENT);
						if (is_portal_tile) {
							TextOutA(memDC, drawX - 25, drawY - 15, portal_name.c_str(), (int)portal_name.length());
						}
						if (is_npc_tile) {
							TextOutA(memDC, drawX - 20, drawY - 15, npc_display_name.c_str(), (int)npc_display_name.length());
						}
						SetTextColor(memDC, RGB(0, 0, 0));
						SetBkMode(memDC, OPAQUE);
					}
				}
			}

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
						case 7: hBrush = CreateSolidBrush(RGB(218, 165, 32));  break;
						case 8: hBrush = CreateSolidBrush(RGB(255, 0, 0));     break;
						default: hBrush = CreateSolidBrush(RGB(128, 128, 128)); break;
						}
					}
					else {
						hBrush = CreateSolidBrush(RGB(0, 0, 255));
					}

					HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, hBrush);
					int radius = min(TILE_SIZE / 2, 20) - 2;

					if (obj.visual_id == 8) {
						Ellipse(memDC, drawX - (radius + 10), drawY - (radius + 10), drawX + (radius + 10), drawY + (radius + 10));
					}
					else if (obj.visual_id == 7) {
						Ellipse(memDC, drawX - (radius + 5), drawY - (radius + 5), drawX + (radius + 5), drawY + (radius + 5));
					}
					else {
						Ellipse(memDC, drawX - radius, drawY - radius, drawX + radius, drawY + radius);
					}

					SelectObject(memDC, hOldBrush);
					DeleteObject(hBrush);

					if (obj.max_hp > 0) {
						int hpBarWidth = 40;
						int hpBarHeight = 5;
						int hpBarX = drawX - (hpBarWidth / 2);
						int hpBarY = drawY - radius - 24;

						RECT rcHpBg = { hpBarX, hpBarY, hpBarX + hpBarWidth, hpBarY + hpBarHeight };
						HBRUSH hBrushBg = CreateSolidBrush(RGB(180, 180, 180));
						FillRect(memDC, &rcHpBg, hBrushBg);
						DeleteObject(hBrushBg);

						float hpRatio = static_cast<float>(obj.hp) / static_cast<float>(obj.max_hp);
						if (hpRatio < 0.0f) hpRatio = 0.0f;
						int currentHpWidth = static_cast<int>(hpBarWidth * hpRatio);

						RECT rcHpGauge = { hpBarX, hpBarY, hpBarX + currentHpWidth, hpBarY + hpBarHeight };
						HBRUSH hBrushGauge = CreateSolidBrush(RGB(0, 220, 50));
						FillRect(memDC, &rcHpGauge, hBrushGauge);
						DeleteObject(hBrushGauge);
					}

					TextOutA(memDC, drawX - 20, drawY - radius - 16, obj.username, (int)strlen(obj.username));

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

			int my_damage = GetWeaponDamage(myObj.weapon, myObj.reinforce_level);

			char hudText[256];
			sprintf_s(hudText, "Name: %s (Lv.%d) | HP: %d/%d | Pos: (%d,%d) | DMG: %d | Gold: %dG",
				myObj.username, myObj.level, myObj.hp, myObj.max_hp, myObj.x, myObj.y, my_damage, myObj.gold);
			SetTextColor(memDC, RGB(255, 69, 0));
			SetBkMode(memDC, TRANSPARENT);
			TextOutA(memDC, 10, 10, hudText, (int)strlen(hudText));
			SetTextColor(memDC, RGB(0, 0, 0));
			SetBkMode(memDC, OPAQUE);


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

				if (myObj.reinforce_level >= 5 || g_next_reinforce_cost == -1) {
					sprintf_s(rnfText, "대장간 (현재 +%d강)\n✨ 최대 강화 도달! ✨", myObj.reinforce_level);

					g_rect_reinforce_btn = { 0, 0, 0, 0 };
				}
				else {
					sprintf_s(rnfText, "대장간 (현재 +%d강)\n비용: %dG", myObj.reinforce_level, g_next_reinforce_cost);

					g_rect_reinforce_btn = { uiRect.left + 20, uiRect.bottom - 50, uiRect.left + 120, uiRect.bottom - 20 };
					FillRect(memDC, &g_rect_reinforce_btn, (HBRUSH)GetStockObject(WHITE_BRUSH));
					DrawTextA(memDC, "강화 시도", -1, &g_rect_reinforce_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				}

				RECT textRect = uiRect; textRect.left += 20; textRect.top += 20;
				DrawTextA(memDC, rnfText, -1, &textRect, DT_LEFT);

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
				SetTextColor(memDC, RGB(255, 69, 0));
				SetBkMode(memDC, TRANSPARENT);
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
				SetTextColor(memDC, RGB(0, 0, 0));
				SetBkMode(memDC, OPAQUE);
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

LRESULT CALLBACK ChatInputSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_CHAR && wParam == VK_RETURN) {
		return 0;
	}

	if (message == WM_KEYDOWN) {
		if (wParam == VK_RETURN) {
			wchar_t w_msg[MAX_CHAT_MSG_LEN];
			GetWindowTextW(g_hChatInput, w_msg, MAX_CHAT_MSG_LEN);

			if (wcslen(w_msg) > 0) {
				std::wstring wstr(w_msg);
				std::string chat_str(wstr.begin(), wstr.end());

				send_chat_packet(chat_str);
				SetWindowTextW(g_hChatInput, L"");
			}
			SetFocus(g_hWnd);
			return 0;
		}
	}
	return CallWindowProc(g_pEditOldProc, hWnd, message, wParam, lParam);
}

void AppendChatLog(const std::string& message) {
	if (g_hChatLog == NULL) return;

	int wlen = MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, NULL, 0);
	if (wlen <= 0) return;

	std::vector<wchar_t> wbuf(wlen);
	MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, &wbuf[0], wlen);

	std::wstring wmsg(&wbuf[0]);
	wmsg += L"\r\n";

	int len = GetWindowTextLengthW(g_hChatLog);

	SendMessageW(g_hChatLog, EM_SETSEL, len, len);
	SendMessageW(g_hChatLog, EM_REPLACESEL, FALSE, (LPARAM)wmsg.c_str());
	SendMessageW(g_hChatLog, EM_SCROLLCARET, 0, 0);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	MyRegisterClass(hInstance);

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

	std::string server_ip;
	std::cout << "접속할 서버 IP 주소를 입력하세요 (엔터 입력 시 기본값 127.0.0.1 접속): ";
	std::getline(std::cin, server_ip);

	if (server_ip.empty()) {
		server_ip = "127.0.0.1";
	}

	SOCKADDR_IN server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

	int result = WSAConnect(g_s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display(L"서버 연결 실패", WSAGetLastError());
		return 1;
	}

	std::cout << "서버 연결 성공!" << std::endl;

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

	HWND hWnd = CreateWindowW(szWindowClass, szTitle,
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, 0, winLength, winLength + 100,
		nullptr, nullptr, hInstance, nullptr);

	if (!hWnd)
	{
		return FALSE;
	}

	g_hWnd = hWnd;

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	return TRUE;
}