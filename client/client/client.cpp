#include "framework.h"
#include <iostream>
#include <string>
#include <unordered_map> // NPC ID 처리를 위해 추가
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(linker, "/entry:wWinMainCRTStartup /subsystem:console") 

#include "..\..\Server\server\protocol_2026.h" // 새로운 프로토콜 헤더

#define MAX_LOADSTRING 100
#define winLength 800

constexpr char SERVER_IP[] = "127.0.0.1";

// --- 전역 변수 ---
HWND g_hWnd;
char g_recv_buf[BUF_SIZE * 2]; // 넉넉하게 2배
char g_send_buf[BUF_SIZE];
WSABUF g_recv_wsa_buf{ sizeof(g_recv_buf), g_recv_buf };
WSABUF g_send_wsa_buf{ BUF_SIZE, g_send_buf };
WSAOVERLAPPED g_recv_over{}, g_send_over{};
SOCKET g_s_socket;

int g_myid = -1;
int g_prev_size = 0;

// --- 새 프로토콜에 맞춘 클라이언트 객체 구조 ---
struct ClientObject {
    int id;
    short x, y;
    int hp, max_hp;
    unsigned char level;
    char username[MAX_NAME_LEN];
    bool is_npc;
};

// 배열 대신 map을 사용하여 NPC ID(1,000,000 이상) 대응
std::unordered_map<int, ClientObject> g_objects;

// --- 함수 선언 ---
void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);
void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags);

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

// --- 패킷 수신 및 처리 로직 ---
void process_packet(unsigned char* p)
{
    PACKET_TYPE type = static_cast<PACKET_TYPE>(p[1]); // p[1]에 type이 위치함

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
        obj.is_npc = false;
        strncpy_s(obj.username, packet->username, MAX_NAME_LEN);

        g_objects[g_myid] = obj;
        break;
    }
    case S2C_ADD_OBJECT: // 예전 S2C_ADD_PLAYER 대응
    {
        S2C_AddObject* packet = reinterpret_cast<S2C_AddObject*>(p);
        ClientObject obj;
        obj.id = packet->object_id;
        obj.x = packet->x; obj.y = packet->y;
        obj.hp = packet->hp; obj.max_hp = packet->max_hp;
        obj.level = packet->level;
        strncpy_s(obj.username, packet->obj_name, MAX_NAME_LEN);
        obj.is_npc = (packet->object_id >= NPC_ID_START); // NPC 식별

        g_objects[obj.id] = obj;
        break;
    }
    case S2C_MOVE_OBJECT: // 예전 S2C_MOVE_PLAYER 대응
    {
        S2C_MoveObject* packet = reinterpret_cast<S2C_MoveObject*>(p);
        if (g_objects.count(packet->object_id)) {
            g_objects[packet->object_id].x = packet->x;
            g_objects[packet->object_id].y = packet->y;
        }
        break;
    }
    case S2C_REMOVE_OBJECT: // 예전 S2C_REMOVE_PLAYER 대응
    {
        S2C_RemoveObject* packet = reinterpret_cast<S2C_RemoveObject*>(p);
        g_objects.erase(packet->object_id);
        break;
    }
    case S2C_STATUS_CHANGE:
    {
        S2C_StatusChange* packet = reinterpret_cast<S2C_StatusChange*>(p);
        if (g_objects.count(packet->object_id)) {
            g_objects[packet->object_id].hp = packet->hp;
            g_objects[packet->object_id].level = packet->level;
        }
        break;
    }
    }

    if (g_hWnd) {
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

// --- 윈도우 프로시저 (입력 및 그리기 부분 수정) ---
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
        }
        if (dx != 0 || dy != 0) {
            send_move_packet(dx, dy);
        }
        break;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // 맵이 2000x2000 이므로 화면 비율에 맞춰 축소해서 그리는 임시 로직
        for (auto& pair : g_objects) {
            ClientObject& obj = pair.second;

            // 임시 스케일링 (전체 맵을 화면에 다 담는 용도)
            int drawX = (obj.x * winLength) / WORLD_WIDTH;
            int drawY = (obj.y * winLength) / WORLD_HEIGHT;

            HBRUSH hBrush = obj.id == g_myid ? CreateSolidBrush(RGB(255, 0, 0)) :
                obj.is_npc ? CreateSolidBrush(RGB(0, 255, 0)) :
                CreateSolidBrush(RGB(0, 0, 255));

            HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
            Ellipse(hdc, drawX - 10, drawY - 10, drawX + 10, drawY + 10);
            SelectObject(hdc, hOldBrush);
            DeleteObject(hBrush);

            TextOutA(hdc, drawX - 15, drawY - 25, obj.username, strlen(obj.username));
        }

        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}