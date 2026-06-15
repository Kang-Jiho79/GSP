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
#include <fstream>
#include <sstream>

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;
using namespace std::chrono;


constexpr int RING_BUF_SIZE = 8192;

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPCMOVE, OP_BOSS_SKILL_EXPLODE, OP_BOSS_CURE_EFFECT, OP_TOWN_RECOVERY, OP_PLAYER_DIE };
enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
enum class ObjType { NONE, PLAYER, NPC };

