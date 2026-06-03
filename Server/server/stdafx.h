#pragma once
#include <iostream>
#include <array>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <concurrent_priority_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include "protocol_2026.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;
using namespace std::chrono;

constexpr int VIEW_RANGE = 5;
constexpr int MOVE_COOL_TIME = 1000; // ms
constexpr int SECTOR_SIZE = VIEW_RANGE;
constexpr int SECTOR_COUNT_X = WORLD_WIDTH / SECTOR_SIZE + 1;
constexpr int SECTOR_COUNT_Y = WORLD_HEIGHT / SECTOR_SIZE + 1;
constexpr int EVENT_MOVE = 1;
constexpr int RING_BUF_SIZE = 8192;

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPCMOVE };
enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
enum class ObjType { NONE, PLAYER, NPC };

struct CharacterInfo {
	WEAPON_TYPE weapon = SWORD;
	int hp = 100;
	int max_hp = 100;
	unsigned long long exp = 0;
	unsigned char level = 1;
	unsigned char reinforce_level = 0;
	unsigned short damage = 10;
};