#pragma once
#include "stdafx.h"
#include <unordered_map>

constexpr int VIEW_RANGE = 5;
constexpr int MOVE_COOL_TIME = 1000; // ms
constexpr int SECTOR_SIZE = VIEW_RANGE;
constexpr int SECTOR_COUNT_X = WORLD_WIDTH / SECTOR_SIZE + 1;
constexpr int SECTOR_COUNT_Y = WORLD_HEIGHT / SECTOR_SIZE + 1;
constexpr int EVENT_MOVE = 1;

struct CharacterInfo {
	WEAPON_TYPE weapon = SWORD;
	int hp = 100;
	int max_hp = 100;
	unsigned long long exp = 0;
	unsigned char level = 1;
	unsigned char reinforce_level = 0;
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

const std::unordered_map<std::pair<WEAPON_TYPE, short>, int> WeaponDamageTable = {
    { {SWORD, 0}, 100 },  // 무기1, 0강 -> 데미지 100
    { {SWORD, 1}, 120 },  // 무기1, 1강 -> 데미지 120
    { {SWORD, 2}, 150 },  // 무기1, 2강 -> 데미지 150

    { {HAMMER, 0}, 200 },  // 무기2, 0강 -> 데미지 200
    { {HAMMER, 1}, 230 }   // 무기2, 1강 -> 데미지 230
};