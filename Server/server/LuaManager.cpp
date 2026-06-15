#include "LuaManager.h"
#include "stdafx.h"
#include "GameData.h"
#include "NPC.h"
#include "Player.h"
#include <queue>
#include <vector>
#include <cmath>
#include <iostream>

extern Sector g_sectors[SECTOR_COUNT_Y][SECTOR_COUNT_X];
extern std::vector<std::vector<int>> g_map;
extern tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<GameObject>>> g_objects;

extern bool can_see(int from, int to);
extern void send_add_object_packet(int send_to_id, int add_obj_id);
extern void send_remove_object_packet(int send_to_id, int remove_obj_id);
extern void send_move_object_packet(int send_to_id, int move_obj_id);
extern void send_status_change(int send_to_id, int hp, int max_hp, unsigned long long exp, unsigned char level);
extern void send_system_message(int send_to_id, const char* message);
extern void send_packet_to_player(int target_client_id, void* packet);

extern HANDLE h_iocp;

struct event_type {
    int obj_id;
    std::chrono::system_clock::time_point wakeup_time;
    int event_id;
    int target_id;
    constexpr bool operator < (const event_type& _Left) const { return (wakeup_time > _Left.wakeup_time); }
};

constexpr int NUM_TIMER_QUEUES = 16;
extern concurrency::concurrent_priority_queue<event_type> timer_queues[NUM_TIMER_QUEUES];

LuaManager g_lua_mgr;
std::mutex g_lua_lock;

struct AStarNode {
    int x, y;
    int g, h, f;
    AStarNode* parent;
    AStarNode() : x(0), y(0), g(0), h(0), f(0), parent(nullptr) {}
    AStarNode(int _x, int _y, int _g, int _h, AStarNode* _p = nullptr)
        : x(_x), y(_y), g(_g), h(_h), f(_g + _h), parent(_p) {
    }
};

struct CompareNode {
    bool operator()(const AStarNode* a, const AStarNode* b) { return a->f > b->f; }
};

std::pair<int, int> CalcAStarNextStep(int start_x, int start_y, int dest_x, int dest_y) {
    if (dest_x < 0 || dest_x >= WORLD_WIDTH || dest_y < 0 || dest_y >= WORLD_HEIGHT) return { start_x, start_y };
    if (g_map[dest_y][dest_x] == 1) return { start_x, start_y };

    int dist_x = abs(start_x - dest_x);
    int dist_y = abs(start_y - dest_y);

    if (max(dist_x, dist_y) > 10) {
        int dx = (dest_x > start_x) ? 1 : ((dest_x < start_x) ? -1 : 0);
        int dy = (dest_y > start_y) ? 1 : ((dest_y < start_y) ? -1 : 0);

        if (dx != 0 && dy != 0 && g_map[start_y + dy][start_x + dx] == 0) {
            return { start_x + dx, start_y + dy };
        }
        else if (dx != 0 && g_map[start_y][start_x + dx] == 0) {
            return { start_x + dx, start_y };
        }
        else if (dy != 0 && g_map[start_y + dy][start_x] == 0) {
            return { start_x, start_y + dy };
        }

        return { start_x, start_y };
    }

    int min_x = max(0, start_x - 10), max_x = min(WORLD_WIDTH - 1, start_x + 10);
    int min_y = max(0, start_y - 10), max_y = min(WORLD_HEIGHT - 1, start_y + 10);

    int range_x = max_x - min_x + 1;
    int range_y = max_y - min_y + 1;
    std::vector<std::vector<bool>> closed_set(range_y, std::vector<bool>(range_x, false));

    AStarNode node_pool[500];
    int node_count = 0;

    std::priority_queue<AStarNode*, std::vector<AStarNode*>, CompareNode> open_set;

    node_pool[node_count] = AStarNode(start_x, start_y, 0, abs(start_x - dest_x) + abs(start_y - dest_y), nullptr);
    open_set.push(&node_pool[node_count++]);

    int dx[] = { 0, 0, -1, 1 }; int dy[] = { -1, 1, 0, 0 };
    AStarNode* end_node = nullptr;

    while (!open_set.empty()) {
        AStarNode* curr = open_set.top(); open_set.pop();

        if (curr->x == dest_x && curr->y == dest_y) { end_node = curr; break; }

        closed_set[curr->y - min_y][curr->x - min_x] = true;

        for (int i = 0; i < 4; ++i) {
            int nx = curr->x + dx[i]; int ny = curr->y + dy[i];
            if (nx < min_x || nx > max_x || ny < min_y || ny > max_y) continue;
            if (g_map[ny][nx] == 1 || closed_set[ny - min_y][nx - min_x]) continue;

            int next_g = curr->g + 1;
            int next_h = abs(nx - dest_x) + abs(ny - dest_y);

            if (node_count < 490) {
                node_pool[node_count] = AStarNode(nx, ny, next_g, next_h, curr);
                open_set.push(&node_pool[node_count++]);
            }
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

    return next_step;
}

void MoveMonsterToGrid(std::shared_ptr<NPC>& npc, int next_x, int next_y, int monster_id) {
    if (g_map[next_y][next_x] == 1) return;

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

int API_GetObjPos(lua_State* L) {
    int obj_id = (int)lua_tonumber(L, 1);

    if (g_objects.count(obj_id) == 0) {
        lua_pushnumber(L, -1);
        lua_pushnumber(L, -1);
        return 2;
    }

    auto obj = g_objects[obj_id].load();
    if (obj && obj->_state == ST_INGAME) {
        lua_pushnumber(L, obj->x);
        lua_pushnumber(L, obj->y);
        return 2;
    }

    lua_pushnumber(L, -1);
    lua_pushnumber(L, -1);
    return 2;
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
        t_pl->stat.hp -= npc->stat.damage;
        if (t_pl->stat.hp <= 0) t_pl->stat.hp = 0; 
    }

    if (t_pl->stat.hp <= 0) {
        OVER_EXP* die_over = new OVER_EXP;
        die_over->_comp_type = OP_PLAYER_DIE;
        PostQueuedCompletionStatus(h_iocp, 0, target_id, &die_over->_over);
        return 0; 
    }

    char dmg_msg[256];
    sprintf_s(dmg_msg, "[%s]에게 공격당해 %d의 데미지를 입었습니다! (남은 HP: %d)",
        npc->name.c_str(), npc->stat.damage, t_pl->stat.hp);

    if (npc->is_boss || npc->stat.damage >= 50) {
        send_system_message(target_id, dmg_msg);
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

int API_BossCastEarthquake(lua_State* L) {
    int boss_id = (int)lua_tonumber(L, 1);
    int target_player_id = (int)lua_tonumber(L, 2);

    auto b_obj = g_objects[boss_id].load();
    if (!b_obj) {
        lua_pushboolean(L, false); 
        return 1;
    }
    auto npc = std::static_pointer_cast<NPC>(b_obj);

    DWORD current_tick = GetTickCount();
    if (current_tick - npc->last_skill_time < 15000) {
        lua_pushboolean(L, false); 
        return 1;
    }

    npc->is_casting_skill = true;
    npc->last_skill_time = current_tick;

    S2C_BossWarnZone warn_p;
    warn_p.size = sizeof(S2C_BossWarnZone);
    warn_p.type = S2C_BOSS_WARN_ZONE;

    warn_p.x = npc->x; warn_p.y = npc->y; warn_p.radius = 5; warn_p.duration_ms = 2000;

    send_packet_to_player(target_player_id, &warn_p);
    npc->_vl_lock.lock(); auto vlist = npc->_view_list; npc->_vl_lock.unlock();
    for (int v_id : vlist) send_packet_to_player(v_id, &warn_p);

    send_system_message(target_player_id, "[최종보스]: 대지가 분노하리라! (2초 뒤 폭발)");
    for (int v_id : vlist) send_system_message(v_id, "[최종보스]: 대지가 분노하리라! (2초 뒤 폭발)");

    event_type ev;
    ev.obj_id = boss_id;
    ev.event_id = EVENT_BOSS_EXPLOSION;
    ev.target_id = target_player_id;
    ev.wakeup_time = system_clock::now() + milliseconds(2000);
    timer_queues[boss_id % NUM_TIMER_QUEUES].push(ev);

    lua_pushboolean(L, true);
    return 1;
}

int API_IsBoss(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    auto m_obj = g_objects[monster_id].load();
    if (m_obj && m_obj->is_npc()) {
        auto npc = std::static_pointer_cast<NPC>(m_obj);
        lua_pushboolean(L, npc->is_boss);
        return 1;
    }
    lua_pushboolean(L, false);
    return 1;
}

LuaManager::LuaManager() {}
LuaManager::~LuaManager() { if (L) { lua_close(L); L = nullptr; } }

bool LuaManager::Initialize() {
    L = luaL_newstate(); if (!L) return false;
    luaL_openlibs(L);

    lua_register(L, "API_GetObjPos", API_GetObjPos);
    lua_register(L, "API_MoveTowards", API_MoveTowards);
    lua_register(L, "API_MonsterAttack", API_MonsterAttack);
    lua_register(L, "API_GetMonsterType", API_GetMonsterType);
    lua_register(L, "API_GetSpawnPos", API_GetSpawnPos);
    lua_register(L, "API_MoveToSpawn", API_MoveToSpawn);
    lua_register(L, "API_RoamTo", API_RoamTo);
    lua_register(L, "API_IsBoss", API_IsBoss);
    lua_register(L, "API_BossCastEarthquake", API_BossCastEarthquake);

    if (luaL_dofile(L, "monster_ai.lua") != LUA_OK) {
        std::cout << "[Lua 에러] 파일 로드 실패: " << lua_tostring(L, -1) << std::endl;
        return false;
    }
    std::cout << "기획서 최적화 수립형 몬스터 AI 스크립트 가동 성공!" << std::endl;
    return true;
}

void LuaManager::RunAI(int monster_id, int target_player_id) {
    if (!L) return;

    std::lock_guard<std::mutex> lock(g_lua_lock);

    lua_getglobal(L, "ProcessMonsterAI");
    lua_pushnumber(L, monster_id);
    lua_pushnumber(L, target_player_id);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        std::cout << "[Lua 런타임 오류] " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
    }
}

