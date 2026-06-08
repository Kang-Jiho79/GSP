#include "LuaManager.h"
#include "stdafx.h"
#include "GameData.h"
#include "NPC.h"
#include "Player.h"
#include <iostream>

// 1. 전역 변수 연결
// server.cpp에 정의된 섹터 배열과 맵 배열을 그대로 참조합니다.
extern Sector g_sectors[SECTOR_COUNT_Y][SECTOR_COUNT_X];
extern std::vector<std::vector<int>> g_map;
extern tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<GameObject>>> g_objects;

// 2. 전역 함수 연결
// server.cpp에 구현된 시야 체크 및 패킷 발송 함수들을 가져옵니다.
extern bool can_see(int from, int to);
extern void send_add_object_packet(int send_to_id, int add_obj_id);
extern void send_remove_object_packet(int send_to_id, int remove_obj_id);
extern void send_move_object_packet(int send_to_id, int move_obj_id);
extern void send_status_change(int send_to_id, int hp, int max_hp, unsigned long long exp, unsigned char level);

// 전역 객체 실체 생성
LuaManager g_lua_mgr;

// 1. [Lua] API_GetObjPos(obj_id) -> 리턴: x, y
int API_GetObjPos(lua_State* L) {
    int obj_id = (int)lua_tonumber(L, 1);

    auto obj = g_objects[obj_id].load();
    if (obj && obj->_state == ST_INGAME) {
        lua_pushnumber(L, obj->x);
        lua_pushnumber(L, obj->y);
        return 2; // 리턴값 2개 (x, y)
    }
    return 0;
}

// 2. [Lua] API_MoveTowards(monster_id, target_id) -> 유저를 향해 1칸 추적 이동
int API_MoveTowards(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    int target_id = (int)lua_tonumber(L, 2);

    auto m_obj = g_objects[monster_id].load();
    auto t_obj = g_objects[target_id].load();
    if (!m_obj || !t_obj) return 0;

    auto npc = std::static_pointer_cast<NPC>(m_obj);

    // 섹터 갱신을 동반한 유저 추적 이동 알고리즘
    int old_sx = npc->x / SECTOR_SIZE; int old_sy = npc->y / SECTOR_SIZE;
    npc->_vl_lock.lock(); std::unordered_set<int> old_vl = npc->_view_list; npc->_vl_lock.unlock();

    // 방향 판정 후 1칸 전진
    if (npc->x < t_obj->x) npc->x++;
    else if (npc->x > t_obj->x) npc->x--;

    if (npc->y < t_obj->y) npc->y++;
    else if (npc->y > t_obj->y) npc->y--;

    int new_sx = npc->x / SECTOR_SIZE; int new_sy = npc->y / SECTOR_SIZE;
    if (old_sx != new_sx || old_sy != new_sy) {
        g_sectors[old_sy][old_sx].players[monster_id] = 0;
        g_sectors[new_sy][new_sx].players[monster_id] = 1;
    }

    // 이동 브로드캐스트 및 시야 처리 (기존 npc 이동 패턴 이식)
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
    return 0;
}

// 3. [Lua] API_MonsterAttack(monster_id, target_id) -> 몬스터가 유저 공격
int API_MonsterAttack(lua_State* L) {
    int monster_id = (int)lua_tonumber(L, 1);
    int target_id = (int)lua_tonumber(L, 2);

    auto t_obj = g_objects[target_id].load();
    if (!t_obj || !t_obj->is_pc()) return 0;
    auto t_pl = std::static_pointer_cast<Player>(t_obj);

    // 유저 HP 차감 및 상태 변화 패킷 전송
    {
        std::lock_guard<std::mutex> ll(t_pl->_lock);
        t_pl->stat.hp -= 5; // 몬스터 공격 데미지 5
        if (t_pl->stat.hp < 0) t_pl->stat.hp = 0;
    }

    std::cout << "[Lua AI] 몬스터가 유저 [" << t_pl->_name << "] 공격! 잔여 HP: " << t_pl->stat.hp << std::endl;
    send_status_change(target_id, t_pl->stat.hp, t_pl->stat.max_hp, t_pl->stat.exp, t_pl->stat.level);

    // 유저 주변에도 브로드캐스트
    t_pl->_vl_lock.lock(); auto t_view = t_pl->_view_list; t_pl->_vl_lock.unlock();
    for (auto v_id : t_view) send_status_change(v_id, t_pl->stat.hp, t_pl->stat.max_hp, t_pl->stat.exp, t_pl->stat.level);

    return 0;
}

// =================================================================
// ⚙️ [Manager Class] 클래스 멤버 함수 구현
// =================================================================

LuaManager::LuaManager() {}
LuaManager::~LuaManager() {
    if (L) { lua_close(L); L = nullptr; }
}

bool LuaManager::Initialize() {
    L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);

    // C++ 접착 함수들을 루아 가상머신에 장착
    lua_register(L, "API_GetObjPos", API_GetObjPos);
    lua_register(L, "API_MoveTowards", API_MoveTowards);
    lua_register(L, "API_MonsterAttack", API_MonsterAttack);

    // 스크립트 파일 로드 및 컴파일
    if (luaL_dofile(L, "monster_ai.lua") != LUA_OK) {
        std::cout << "[Lua 에러] 파일 로드 실패: " << lua_tostring(L, -1) << std::endl;
        return false;
    }

    std::cout << "Monster AI 루아 스크립트 엔진 독립 가동 완료!" << std::endl;
    return true;
}

void LuaManager::RunAI(int monster_id, int target_player_id) {
    if (!L) return;

    lua_getglobal(L, "ProcessMonsterAI"); // 루아 함수 이름 가져오기
    lua_pushnumber(L, monster_id);         // 인자 1
    lua_pushnumber(L, target_player_id);    // 인자 2

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        std::cout << "[Lua 런타임 에러] " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
    }
}