#include "LuaManager.h"
#include "stdafx.h"
#include "GameData.h"
#include "NPC.h"
#include "Player.h"
#include <queue>
#include <vector>
#include <cmath>
#include <iostream>

// 전역 변수 및 함수 연결
extern Sector g_sectors[SECTOR_COUNT_Y][SECTOR_COUNT_X];
extern std::vector<std::vector<int>> g_map;
extern tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<GameObject>>> g_objects;

extern bool can_see(int from, int to);
extern void send_add_object_packet(int send_to_id, int add_obj_id);
extern void send_remove_object_packet(int send_to_id, int remove_obj_id);
extern void send_move_object_packet(int send_to_id, int move_obj_id);
extern void send_status_change(int send_to_id, int hp, int max_hp, unsigned long long exp, unsigned char level);

LuaManager g_lua_mgr;

// =================================================================
// 🧭 고성능 A* 알고리즘 엔진 구현 (world.csv 벽 회피용)
// =================================================================
struct AStarNode {
    int x, y;
    int g, h, f;
    AStarNode* parent;
    AStarNode(int _x, int _y, int _g, int _h, AStarNode* _p = nullptr)
        : x(_x), y(_y), g(_g), h(_h), f(_g + _h), parent(_p) {
    }
};

struct CompareNode {
    bool operator()(const AStarNode* a, const AStarNode* b) { return a->f > b->f; }
};

std::pair<int, int> CalcAStarNextStep(int start_x, int start_y, int dest_x, int dest_y) {
    if (dest_x < 0 || dest_x >= WORLD_WIDTH || dest_y < 0 || dest_y >= WORLD_HEIGHT) return { start_x, start_y };
    if (g_map[dest_y][dest_x] == 1) return { start_x, start_y }; // 목적지가 벽

    std::priority_queue<AStarNode*, std::vector<AStarNode*>, CompareNode> open_set;

    // 부하 방지를 위해 시야 반경(최대 10칸)으로 길찾기 타일 영역 제한
    int min_x = max(0, start_x - 10), max_x = min(WORLD_WIDTH - 1, start_x + 10);
    int min_y = max(0, start_y - 10), max_y = min(WORLD_HEIGHT - 1, start_y + 10);

    std::vector<std::vector<bool>> closed_set(WORLD_HEIGHT, std::vector<bool>(WORLD_WIDTH, false));
    open_set.push(new AStarNode(start_x, start_y, 0, abs(start_x - dest_x) + abs(start_y - dest_y)));

    int dx[] = { 0, 0, -1, 1 }; int dy[] = { -1, 1, 0, 0 };
    AStarNode* end_node = nullptr;

    while (!open_set.empty()) {
        AStarNode* curr = open_set.top(); open_set.pop();

        if (curr->x == dest_x && curr->y == dest_y) { end_node = curr; break; }
        closed_set[curr->y][curr->x] = true;

        for (int i = 0; i < 4; ++i) {
            int nx = curr->x + dx[i]; int ny = curr->y + dy[i];
            if (nx < min_x || nx > max_x || ny < min_y || ny > max_y) continue;
            if (g_map[ny][nx] == 1 || closed_set[ny][nx]) continue; // 벽(1) 충돌 무시

            int next_g = curr->g + 1;
            int next_h = abs(nx - dest_x) + abs(ny - dest_y);
            open_set.push(new AStarNode(nx, ny, next_g, next_h, curr));
        }
    }

    std::pair<int, int> next_step = { start_x, start_y };
    if (end_node) {
        AStarNode* path = end_node;
        while (path->parent && (path->parent->x != start_x || path->parent->y != start_y)) {
            path = path->parent;
        }
        next_step = { path->x, path->y };
    }

    while (!open_set.empty()) { delete open_set.top(); open_set.pop(); }
    return next_step;
}

// =================================================================
// 🤝 C++ ➔ Lua 이동 내부 공용 동기화 서브루틴
// =================================================================
void MoveMonsterToGrid(std::shared_ptr<NPC>& npc, int next_x, int next_y, int monster_id) {
    if (g_map[next_y][next_x] == 1) return; // 최후방 안전장치(벽 검사)

    int old_sx = npc->x / SECTOR_SIZE; int old_sy = npc->y / SECTOR_SIZE;
    npc->_vl_lock.lock(); std::unordered_set<int> old_vl = npc->_view_list; npc->_vl_lock.unlock();

    npc->x = next_x; npc->y = next_y;

    int new_sx = npc->x / SECTOR_SIZE; int new_sy = npc->y / SECTOR_SIZE;
    if (old_sx != new_sx || old_sy != new_sy) {
        g_sectors[old_sy][old_sx].players[monster_id] = 0;
        g_sectors[new_sy][new_sx].players[monster_id] = 1;
    }

    std::unordered_set<int> new_vl;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = new_sx + dx; int ny = new_sy + dy;
            if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
            for (auto& sec_pl : g_sectors[ny][nx].players) {
                if (sec_pl.second == 0) continue;
                int p_id = sec_pl.first; if (p_id == monster_id) continue;
                auto other = g_objects[p_id].load();
                if (other && other->_state == ST_INGAME && can_see(monster_id, p_id)) new_vl.insert(p_id);
            }
        }
    }

    for (auto p_id : new_vl) {
        auto other = g_objects[p_id].load(); if (!other) continue;
        if (old_vl.count(p_id) == 0) {
            npc->_vl_lock.lock(); npc->_view_list.insert(p_id); npc->_vl_lock.unlock();
            other->_vl_lock.lock(); other->_view_list.insert(monster_id); other->_vl_lock.unlock();
            send_add_object_packet(p_id, monster_id); send_add_object_packet(monster_id, p_id);
        }
        else {
            send_move_object_packet(p_id, monster_id);
        }
    }
    for (auto p_id : old_vl) {
        if (new_vl.count(p_id) == 0) {
            auto other = g_objects[p_id].load();
            npc->_vl_lock.lock(); npc->_view_list.erase(p_id); npc->_vl_lock.unlock();
            if (other) { other->_vl_lock.lock(); other->_view_list.erase(monster_id); other->_vl_lock.unlock(); send_remove_object_packet(p_id, monster_id); }
        }
    }
    npc->npc_last_move_time = std::chrono::system_clock::now();
}

// =================================================================
// 🤝 Glue Functions (Lua 전용 C API 매핑 함수군)
// =================================================================

int API_GetObjPos(lua_State* L) {
    int obj_id = (int)lua_tonumber(L, 1);
    auto obj = g_objects[obj_id].load();
    if (obj && obj->_state == ST_INGAME) {
        lua_pushnumber(L, obj->x); lua_pushnumber(L, obj->y);
        return 2;
    }
    return 0;
}

int API_MoveTowards(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    int target_id = (int)lua_tonumber(L, 2);

    auto m_obj = g_objects[monster_id].load(); auto t_obj = g_objects[target_id].load();
    if (!m_obj || !t_obj) return 0;

    auto npc = std::static_pointer_cast<NPC>(m_obj);
    std::pair<int, int> next_pos = CalcAStarNextStep(npc->x, npc->y, t_obj->x, t_obj->y);
    MoveMonsterToGrid(npc, next_pos.first, next_pos.second, monster_id);
    return 0;
}

int API_MonsterAttack(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    int target_id = (int)lua_tonumber(L, 2);

    auto m_obj = g_objects[monster_id].load();
    auto t_obj = g_objects[target_id].load();
    if (!m_obj || !t_obj || !t_obj->is_pc()) return 0;

    auto npc = std::static_pointer_cast<NPC>(m_obj);
    auto t_pl = std::static_pointer_cast<Player>(t_obj);

    {
        std::lock_guard<std::mutex> ll(t_pl->_lock);
        // 💥 하드코딩된 '5' 대신 몬스터 테이블에서 세팅되어 주입된 고유 데미지로 피를 깎습니다!
        t_pl->stat.hp -= npc->stat.damage;
        if (t_pl->stat.hp < 0) t_pl->stat.hp = 0;
    }

    send_status_change(target_id, t_pl->stat.hp, t_pl->stat.max_hp, t_pl->stat.exp, t_pl->stat.level);
    t_pl->_vl_lock.lock(); auto t_view = t_pl->_view_list; t_pl->_vl_lock.unlock();
    for (auto v_id : t_view) send_status_change(v_id, t_pl->stat.hp, t_pl->stat.max_hp, t_pl->stat.exp, t_pl->stat.level);
    return 0;
}

int API_GetMonsterType(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    auto obj = g_objects[monster_id].load();
    if (!obj) { lua_pushstring(L, "Peace"); lua_pushstring(L, "고정"); return 2; }

    auto npc = std::static_pointer_cast<NPC>(obj);

    // ⭐ [완벽 연동] 몬스터 고유 객체에 저장된 도감 데이터를 루아 스크립트로 밀어넣어 줍니다.
    lua_pushstring(L, npc->ai_type.c_str());
    lua_pushstring(L, npc->move_type.c_str());
    return 2;
}

int API_GetSpawnPos(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    auto obj = g_objects[monster_id].load();
    if (obj) {
        auto npc = std::static_pointer_cast<NPC>(obj);
        lua_pushnumber(L, npc->spawn_x); lua_pushnumber(L, npc->spawn_y);
        return 2;
    }
    return 0;
}

int API_MoveToSpawn(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    auto obj = g_objects[monster_id].load(); if (!obj) return 0;
    auto npc = std::static_pointer_cast<NPC>(obj);

    std::pair<int, int> next_pos = CalcAStarNextStep(npc->x, npc->y, npc->spawn_x, npc->spawn_y);
    MoveMonsterToGrid(npc, next_pos.first, next_pos.second, monster_id);
    return 0;
}

int API_RoamTo(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    int target_x = (int)lua_tonumber(L, 2); int target_y = (int)lua_tonumber(L, 3);

    auto obj = g_objects[monster_id].load(); if (!obj) return 0;
    auto npc = std::static_pointer_cast<NPC>(obj);

    std::pair<int, int> next_pos = CalcAStarNextStep(npc->x, npc->y, target_x, target_y);
    MoveMonsterToGrid(npc, next_pos.first, next_pos.second, monster_id);
    return 0;
}

// =================================================================
// ⚙️ [Manager Class] 엔진 생성 및 바인딩부
// =================================================================
LuaManager::LuaManager() {}
LuaManager::~LuaManager() { if (L) { lua_close(L); L = nullptr; } }

bool LuaManager::Initialize() {
    L = luaL_newstate(); if (!L) return false;
    luaL_openlibs(L);

    // 컴파일 타임 에러 유발하던 바인딩 명칭 스키마 전면 교정 동기화
    lua_register(L, "API_GetObjPos", API_GetObjPos);
    lua_register(L, "API_MoveTowards", API_MoveTowards);
    lua_register(L, "API_MonsterAttack", API_MonsterAttack);
    lua_register(L, "API_GetMonsterType", API_GetMonsterType);
    lua_register(L, "API_GetSpawnPos", API_GetSpawnPos);
    lua_register(L, "API_MoveToSpawn", API_MoveToSpawn);
    lua_register(L, "API_RoamTo", API_RoamTo);

    if (luaL_dofile(L, "monster_ai.lua") != LUA_OK) {
        std::cout << "[Lua 에러] 파일 로드 실패: " << lua_tostring(L, -1) << std::endl;
        return false;
    }
    std::cout << "기획서 최적화 수립형 몬스터 AI 스크립트 가동 성공!" << std::endl;
    return true;
}

void LuaManager::RunAI(int monster_id, int target_player_id) {
    if (!L) return;
    lua_getglobal(L, "ProcessMonsterAI");
    lua_pushnumber(L, monster_id); lua_pushnumber(L, target_player_id);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        std::cout << "[Lua 런타임 오류] " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
    }
}