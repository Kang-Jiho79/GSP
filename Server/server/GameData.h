#pragma once
#include "stdafx.h"
#include <unordered_map>

constexpr int VIEW_RANGE = 7;
constexpr int MOVE_COOL_TIME = 1000;
constexpr int SECTOR_SIZE = VIEW_RANGE;
constexpr int SECTOR_COUNT_X = WORLD_WIDTH / SECTOR_SIZE + 1;
constexpr int SECTOR_COUNT_Y = WORLD_HEIGHT / SECTOR_SIZE + 1;


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

struct ReinforceData {
    int   damage;  
    int   cost;            
    float success_rate;  
};

struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& pair) const {
        return std::hash<T1>()(pair.first) ^ (std::hash<T2>()(pair.second) << 1);
    }
};

const std::unordered_map<std::pair<WEAPON_TYPE, short>, ReinforceData, PairHash> WeaponReinforceTable = {

    { {sword, 0}, { 100, 100,  80.0f } }, 
    { {sword, 1}, { 120, 200,  65.0f } },
    { {sword, 2}, { 150, 400,  50.0f } },
    { {sword, 3}, { 200, 800,  35.0f } },
	{ {sword, 4}, { 300, 1600, 20.0f } },
	{ {sword, 5}, { 500, 3200, 10.0f } },

    { {hammer, 0}, { 150, 100, 80.0f } },
    { {hammer, 1}, { 180, 200, 65.0f } },
    { {hammer, 2}, { 220, 400, 50.0f } },
	{ {hammer, 3}, { 300, 800, 35.0f } },
    { {hammer, 4}, { 450, 1600, 20.0f } },
	{ {hammer, 5}, { 700, 3200, 10.0f } },

    { {spear, 0}, { 90, 100, 80.0f } },
    { {spear, 1}, { 110, 200, 65.0f } },
	{ {spear, 2}, { 140, 400, 50.0f } },
    { {spear, 3}, { 190, 800, 35.0f } },
    { {spear, 4}, { 250, 1600, 20.0f } },
	{ {spear, 5}, { 400, 3200, 10.0f } }
};

const int GRID_STEP = 110;
const int ROOM_LEFT_X = 680;
const int ROOM_BOTTOM_Y = 649;
constexpr int EVENT_MOVE = 1;
constexpr int EVENT_BOSS_EXPLOSION = 3;
constexpr int EVENT_CONFUSE_END = 4;
constexpr int EVENT_RESPAWN = 9;

struct Sector { tbb::concurrent_unordered_map<int, int> players; };

struct Level_Maxhp {
    unsigned char level;
	unsigned long long exp;
	int max_hp;
};

const std::vector<Level_Maxhp> LevelMaxHpTable = {
    {0, 0, 1000},
    {1, 100, 1100},
    {2, 300, 1200},
    {3, 600, 1300},
    {4, 1000, 1400},
    {5, 1500, 1500},
    {6, 2100, 1600},
    {7, 2800, 1700},
    {8, 3600, 1800},
    {9, 4500, 1900},
	{10, 5500, 2000},
    {11, 6600, 2100},
    {12, 7800, 2200},
    {13, 9100, 2300},
    {14, 10500, 2400},
    {15, 12000, 2500},
    {16, 13600, 2600},
    {17, 15300, 2700},
    {18, 17100, 2800},
	{19, 19000, 2900},
	{20, 21000, 3000},
    {21, 23100, 3100},
    {22, 25300, 3200},
    {23, 27600, 3300},
    {24, 30000, 3400},
    {25, 32500, 3500},
    {26, 35100, 3600},
    {27, 37800, 3700},
    {28, 40600, 3800},
	{29, 43500, 3900},
    {30, 46500, 4000},
    {31, 49600, 4100},
    {32, 52800, 4200},
    {33, 56100, 4300},
    {34, 59500, 4400},
    {35, 63000, 4500},
    {36, 66600, 4600},
    {37, 70300, 4700},
    {38, 74100, 4800},
    {39, 78000, 4900},
	{40, 82000, 5000},
    {41, 86100, 5100},
    {42, 90300, 5200},
    {43, 94600, 5300},
    {44, 99000, 5400},
    {45, 103500, 5500},
    {46, 108100, 5600},
    {47, 112800, 5700},
    {48, 117600, 5800},
	{49, 122500, 5900},
	{50, 127500, 6000}
};

struct MonsterTemplate {
    int          monster_type_id; 
    std::string  name;            
    unsigned char level;        
    int          max_hp;      
	int 		damage;         
    std::string  ai_type;   
    std::string  move_type;   
	int     gold_reward;     
};

const std::vector<MonsterTemplate> MonsterTemplates = {
    { 1, "슬라임", 5, 300, 20, "Agro", "로밍", 100 },
    { 2, "고블린", 10, 500, 40, "Agro", "로밍", 200 },
    { 3, "트롤", 15, 800, 60, "Agro", "로밍", 400 },
    { 4, "스켈레톤", 20, 1000, 80, "Agro", "로밍", 600 },
    { 5, "좀비", 25, 1300, 100, "Agro", "로밍", 1200 },
    { 6, "마법사", 30, 1600, 120, "Agro", "고정", 1500 },
    { 7, "드래곤", 50, 2000, 500, "Agro", "고정", 3000 },
    { 8, "보스 몬스터", 50, 10000, 1000, "Agro", "고정", 10000 }
};

struct HuntingZone {
    int zone_id;          
    std::string zone_name;
    short min_x, min_y;   
    short max_x, max_y;   
    int template_index;   
};

const std::vector<HuntingZone> HuntingZoneTable = {
    { 0, "슬라임 초원", 1350, 1, 1998, 649, 0 }, 
    { 1, "고블린 마을", 1350, 675, 1998, 1325, 1 }, 
    { 2, "트롤 군락지", 1350, 1350, 1998, 1998, 2 }, 
    { 3, "스켈레톤 무덤", 675, 1350, 1324, 1998, 3 }, 
    { 4, "좀비 늪지대", 1, 1350, 649, 1998, 4 },
    { 5, "마법사 신전", 1, 675, 649, 1325, 5 }, 
    { 6, "드래곤 둥지", 1, 1, 649, 649, 6 }  
};