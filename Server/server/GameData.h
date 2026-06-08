#pragma once
#include "stdafx.h"
#include <unordered_map>

constexpr int VIEW_RANGE = 7;
constexpr int MOVE_COOL_TIME = 1000; // ms
constexpr int SECTOR_SIZE = VIEW_RANGE;
constexpr int SECTOR_COUNT_X = WORLD_WIDTH / SECTOR_SIZE + 1;
constexpr int SECTOR_COUNT_Y = WORLD_HEIGHT / SECTOR_SIZE + 1;
constexpr int EVENT_MOVE = 1;

struct ObjectInfo {
	int hp = 100;
	int max_hp = 100;
	unsigned long long exp = 0;
	unsigned char level = 1;
	unsigned short damage = 10;
};

struct position {
    short x, y;
};

const std::unordered_map<DUNGEON_TYPE, position> DungeonEntrances = {
    { DUNGEON_1, { 10, 0 } },   // 1번 던전 위치
    { DUNGEON_2, { 150, 5 } }, // 2번 던전 위치
    { DUNGEON_3, { -50, 0 } }  // 3번 던전 위치
};

struct ReinforceData {
    int   damage;          // 데미지
    int   cost;            // 강화 비용 (골드)
    float success_rate;    // 성공 확률 (0.0f ~ 1.0f 또는 0 ~ 100)
};

struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& pair) const {
        return std::hash<T1>()(pair.first) ^ (std::hash<T2>()(pair.second) << 1);
    }
};

const std::unordered_map<std::pair<WEAPON_TYPE, short>, ReinforceData, PairHash> WeaponReinforceTable = {
    // === sword (한손검) ===
    // { {무기타입, 강화도}, {데미지, 비용, 확률%} }
    { {sword, 0}, { 100, 100,  80.0f } }, 
    { {sword, 1}, { 120, 200,  65.0f } },
    { {sword, 2}, { 150, 400,  50.0f } },
    { {sword, 3}, { 200, 800,  35.0f } },
	{ {sword, 4}, { 300, 1600, 20.0f } },
	{ {sword, 5}, { 500, 3200, 10.0f } },

    // === hammer (둔기) ===
    { {hammer, 0}, { 150, 100, 80.0f } },
    { {hammer, 1}, { 180, 200, 65.0f } },
    { {hammer, 2}, { 220, 400, 50.0f } },
	{ {hammer, 3}, { 300, 800, 35.0f } },
    { {hammer, 4}, { 450, 1600, 20.0f } },
	{ {hammer, 5}, { 700, 3200, 10.0f } },

    // === spear (창) ===
    { {spear, 0}, { 90, 100, 80.0f } },
    { {spear, 1}, { 110, 200, 65.0f } },
	{ {spear, 2}, { 140, 400, 50.0f } },
    { {spear, 3}, { 190, 800, 35.0f } },
    { {spear, 4}, { 250, 1600, 20.0f } },
	{ {spear, 5}, { 400, 3200, 10.0f } }
};

const int GRID_STEP = 110;
const int START_ROOM_X = 730;
const int START_ROOM_Y = 649;

struct Sector { tbb::concurrent_unordered_map<int, int> players; };