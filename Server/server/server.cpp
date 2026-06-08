#include "stdafx.h"
#include "GameData.h"
#include "Session.h"
#include "Player.h"
#include "NPC.h"
#include "DB.h"
#include "LuaManager.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")

std::vector<std::vector<int>> g_map(WORLD_HEIGHT, std::vector<int>(WORLD_WIDTH, 0));

Sector g_sectors[SECTOR_COUNT_Y][SECTOR_COUNT_X];

struct event_type {
    int obj_id; system_clock::time_point wakeup_time; int event_id; int target_id;
    constexpr bool operator < (const event_type& _Left) const { return (wakeup_time > _Left.wakeup_time); }
};
concurrency::concurrent_priority_queue<event_type> timer_queue;

HANDLE h_iocp;
SOCKET g_s_socket, g_c_socket;
OVER_EXP g_a_over;
std::atomic<int> player_index_count{ 0 };

std::unordered_map<std::string, int> g_name_to_id;
std::mutex g_name_lock;

struct Party {
    int leader_id;
    std::vector<int> members;
    std::mutex p_lock;
};
std::atomic<int> g_party_id_gen{ 1 };
std::unordered_map<int, std::shared_ptr<Party>> g_parties;
std::mutex g_parties_lock;

tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<Session>>> g_sessions;
tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<GameObject>>> g_objects;

bool g_boss_rooms[6][6] = { false, };
std::mutex g_boss_room_lock;

DBManager g_db_mgr;

void PushPlayerSaveTask(int c_id) {
    auto obj = g_objects[c_id].load();
    if (!obj || !obj->is_pc()) return;
    auto pl = std::static_pointer_cast<Player>(obj);

    DB_Task task;
    task.type = TASK_SAVE;
    task.username = pl->_name;

    {
        std::lock_guard<std::mutex> ll(pl->_lock);
        if (pl->_state != ST_INGAME) return; // 로그인 완료된 유저만 세이브 허용
        task.x = pl->x; task.y = pl->y;
        task.level = pl->stat.level; task.exp = pl->stat.exp;
        task.max_hp = pl->stat.max_hp;
        task.gold = pl->stat.gold;
        task.weapon = pl->stat.weapon;
        task.reinforce_level = pl->stat.reinforce_level;
    }
    g_db_mgr.PushTask(task);
}

bool LoadMapCSV(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "world.csv 파일 열기 실패!" << std::endl;
        return false;
    }
    std::string line;
    int y = 0;
    while (std::getline(file, line) && y < WORLD_HEIGHT) {
        std::stringstream ss(line);
        std::string token;
        int x = 0;
        while (std::getline(ss, token, ',') && x < WORLD_WIDTH) {
            g_map[y][x] = std::stoi(token);
            x++;
        }
        y++;
    }
    std::cout << "서버 맵 로딩 완료!" << std::endl;
    return true;
}

bool can_see(int from, int to) {
    auto p_from = g_objects[from].load();
    auto p_to = g_objects[to].load();
    if (!p_from || !p_to) return false;
    if (abs(p_from->x - p_to->x) > VIEW_RANGE) return false;
    return abs(p_from->y - p_to->y) <= VIEW_RANGE;
}

void wake_up_npc(int npc_id) {
    auto obj = g_objects[npc_id].load();
    if (!obj || !obj->is_npc()) return;
    auto npc = std::static_pointer_cast<NPC>(obj);
    bool expected = false;
    if (!npc->_active_npc.compare_exchange_strong(expected, true)) return;

    event_type ev; ev.obj_id = npc_id; ev.event_id = EVENT_MOVE; ev.target_id = -1;
    ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
    timer_queue.push(ev);
}

void send_packet_to_player(int target_client_id, void* packet) {
    auto obj = g_objects[target_client_id].load();
    if (obj && obj->is_pc()) {
        auto pl = std::static_pointer_cast<Player>(obj);
        if (pl->_session) pl->_session->do_send(packet);
    }
}

void send_add_object_packet(int send_to_id, int add_obj_id) {
    auto add_obj = g_objects[add_obj_id].load();
    if (!add_obj) return;

    S2C_AddObject p;
    p.size = sizeof(S2C_AddObject);
    p.type = S2C_ADD_OBJECT;
    p.object_id = add_obj_id;
    p.x = add_obj->x; p.y = add_obj->y;
    p.visual_id = 0;

    if (add_obj->is_pc()) {
        auto pl = std::static_pointer_cast<Player>(add_obj);
        strcpy_s(p.obj_name, pl->_name);
        p.hp = pl->stat.hp; p.max_hp = pl->stat.max_hp; p.exp = pl->stat.exp; p.level = pl->stat.level;
    }
    else {
        sprintf_s(p.obj_name, "NPC%d", add_obj_id);
        p.hp = p.max_hp = 100; p.level = 1; p.exp = 0;
    }
    send_packet_to_player(send_to_id, &p);
}

void send_remove_object_packet(int send_to_id, int remove_obj_id) {
    S2C_RemoveObject p;
    p.size = sizeof(S2C_RemoveObject);
    p.type = S2C_REMOVE_OBJECT;
    p.object_id = remove_obj_id;
    send_packet_to_player(send_to_id, &p);
}

void send_move_object_packet(int send_to_id, int move_obj_id) {
    auto move_obj = g_objects[move_obj_id].load();
    if (!move_obj) return;
    S2C_MoveObject p;
    p.size = sizeof(S2C_MoveObject);
    p.type = S2C_MOVE_OBJECT;
    p.object_id = move_obj_id;
    p.x = move_obj->x; p.y = move_obj->y;
    p.move_time = move_obj->is_pc() ? std::static_pointer_cast<Player>(move_obj)->last_move_time : 0;
    send_packet_to_player(send_to_id, &p);
}

void send_chat_message(int send_to_id, const char* message) {
    S2C_ChatMessage p;
    p.size = sizeof(S2C_ChatMessage);
    p.type = S2C_CHAT_MESSAGE;
    p.object_id = send_to_id;
    strncpy_s(p.message, message, sizeof(p.message));
    send_packet_to_player(send_to_id, &p);
}

void send_status_change(int send_to_id, int hp, int max_hp, unsigned long long exp, unsigned char level) {
    S2C_StatusChange p;
    p.size = sizeof(S2C_StatusChange);
    p.type = S2C_STATUS_CHANGE;
    p.object_id = send_to_id;
    p.hp = hp; p.max_hp = max_hp; p.exp = exp; p.level = level;
    send_packet_to_player(send_to_id, &p);
}

void send_dungeon_result(int send_to_id, DUNGEON_TYPE dungeon, bool success, short to_x, short to_y) {
    S2C_DungeonResult p;
    p.size = sizeof(S2C_DungeonResult);
    p.type = S2C_DUNGEON_RESULT;
    int playerId = send_to_id;
    p.dungeon = dungeon; p.success = success; p.x = to_x; p.y = to_y;
    send_packet_to_player(send_to_id, &p);
}

void send_info_result(int send_to_id, int playerId, int visualId, char* username, short x, short y, WEAPON_TYPE weapon, int hp, int max_hp, int gold,
    unsigned char reinforce_level, unsigned long long exp, unsigned char level, bool in_party) {
    S2C_InfoResult p;
    p.size = sizeof(S2C_InfoResult);
    p.type = S2C_INFO_RESULT;
    p.playerId = playerId;
    p.visualId = visualId; strncpy_s(p.username, username, MAX_NAME_LEN); p.x = x; p.y = y; p.weapon = weapon;
    p.hp = hp; p.max_hp = max_hp; p.gold = gold; p.reinforce_level = reinforce_level; p.exp = exp; p.level = level; p.in_party = in_party;
    send_packet_to_player(send_to_id, &p);
}

void send_interact_result(int send_to_id, bool success, const char* message) {
    S2C_InteractResult p;
    p.size = sizeof(S2C_InteractResult);
    p.type = S2C_INTERACT_RESULT;
    p.playerId = send_to_id;
    p.success = success; strncpy_s(p.message, message, sizeof(p.message));
    send_packet_to_player(send_to_id, &p);
}

void send_reinforce_result(int send_to_id, bool success, unsigned char reinforce_level, int gold) {
    S2C_ReinforceResult p;
    p.size = sizeof(S2C_ReinforceResult);
    p.type = S2C_REINFORCE_RESULT;
    p.playerId = send_to_id;
    p.success = success; p.reinforce_level = reinforce_level; p.gold = gold;
    send_packet_to_player(send_to_id, &p);
}

void send_party_invite_notification(int send_to_id, int inviter_id) {
    S2C_PartyInviteNoti p;
    p.size = sizeof(S2C_PartyInviteNoti);
    p.type = S2C_PARTY_INVITE_NOTI;
    p.playerId = inviter_id;
    strncpy_s(p.inviter_username, g_objects[inviter_id].load() ? std::static_pointer_cast<Player>(g_objects[inviter_id].load())->_name : "Unknown", MAX_NAME_LEN);
    send_packet_to_player(send_to_id, &p);
}

void broadcast_party_update(int party_id) {
    std::shared_ptr<Party> party;
    {
        std::lock_guard<std::mutex> ll(g_parties_lock);
        if (g_parties.count(party_id) == 0) return;
        party = g_parties[party_id];
    }

    S2C_PartyUpdate p;
    p.size = sizeof(S2C_PartyUpdate);
    p.type = S2C_PARTY_UPDATE;

    std::lock_guard<std::mutex> plock(party->p_lock);
    p.party_member_count = static_cast<int>(party->members.size());

    for (int i = 0; i < p.party_member_count; ++i) {
        int m_id = party->members[i];
        auto m_obj = g_objects[m_id].load();
        if (m_obj && m_obj->is_pc()) {
            auto m_pl = std::static_pointer_cast<Player>(m_obj);
            p.party_members[i].playerId = m_id;
            strncpy_s(p.party_members[i].username, m_pl->_name, MAX_NAME_LEN);
            p.party_members[i].hp = m_pl->stat.hp;
            p.party_members[i].max_hp = m_pl->stat.max_hp;
            p.party_members[i].level = m_pl->stat.level;
        }
    }

    for (int m_id : party->members) {
        p.playerId = m_id;
        send_packet_to_player(m_id, &p);
    }
}

void send_attack_broadcast(int send_to_id, int attacker_id, WEAPON_TYPE weapon) {
    S2C_AttackBroadcast p;
    p.size = sizeof(S2C_AttackBroadcast);
    p.type = S2C_ATTACK_BROADCAST;
    p.attacker_id = attacker_id;
    p.weapon = weapon;
    send_packet_to_player(send_to_id, &p);
}

// disconnect 함수 위치를 위로 올림
void disconnect(int c_id) {
    auto session = g_sessions[c_id].load();
    auto obj = g_objects[c_id].load();

    if (obj && obj->is_pc()) {
        PushPlayerSaveTask(c_id);
        auto pl = std::static_pointer_cast<Player>(obj);
        pl->_vl_lock.lock(); unordered_set<int> vl = pl->_view_list; pl->_vl_lock.unlock();
        for (auto v_id : vl) {
            auto other = g_objects[v_id].load();
            if (other) {
                other->_vl_lock.lock(); other->_view_list.erase(c_id); other->_vl_lock.unlock();
                send_remove_object_packet(v_id, c_id);
            }
        }
        int sx = pl->x / SECTOR_SIZE; int sy = pl->y / SECTOR_SIZE;
        if (sx >= 0 && sx < SECTOR_COUNT_X && sy >= 0 && sy < SECTOR_COUNT_Y) g_sectors[sy][sx].players[c_id] = 0;

        {
            lock_guard<mutex> nl(g_name_lock);
            g_name_to_id.erase(pl->_name);
        }

        lock_guard<mutex> ll(pl->_lock);
        pl->_state = ST_FREE; pl->_session = nullptr;
    }

    if (session) {
        if (session->_socket != 0 && session->_socket != INVALID_SOCKET) {
            closesocket(session->_socket); session->_socket = INVALID_SOCKET;
        }
        g_sessions[c_id].store(nullptr);
    }
}

void process_packet(int c_id, unsigned char* packet) {
    auto obj = g_objects[c_id].load();
    if (!obj || !obj->is_pc()) return;
    auto pl = std::static_pointer_cast<Player>(obj);

    switch (packet[1]) {
    case C2S_LOGIN: {
        C2S_Login* p = reinterpret_cast<C2S_Login*>(packet);

        strncpy_s(pl->_name, p->username, MAX_NAME_LEN);

        {
            lock_guard<mutex> nl(g_name_lock);
            g_name_to_id[pl->_name] = c_id;
        }

        DB_Task task;
        task.type = TASK_LOGIN;
        task.client_id = c_id;
        task.username = p->username;

        g_db_mgr.PushTask(task);
        break;
    }
    case C2S_MOVE: {
        C2S_Move* p = reinterpret_cast<C2S_Move*>(packet);
        pl->last_move_time = p->move_time;
        int old_sx = pl->x / SECTOR_SIZE; int old_sy = pl->y / SECTOR_SIZE;

        short nx = pl->x + p->x; short ny = pl->y + p->y;
        if (nx < 0) nx = 0; else if (nx >= WORLD_WIDTH) nx = WORLD_WIDTH - 1;
        if (ny < 0) ny = 0; else if (ny >= WORLD_HEIGHT) ny = WORLD_HEIGHT - 1;
        if (g_map[ny][nx] == 1) {
            nx = pl->x;
            ny = pl->y;
        }
        pl->x = nx; pl->y = ny;

        int new_sx = pl->x / SECTOR_SIZE; int new_sy = pl->y / SECTOR_SIZE;
        if (old_sx != new_sx || old_sy != new_sy) {
            g_sectors[old_sy][old_sx].players[c_id] = 0;
            g_sectors[new_sy][new_sx].players[c_id] = 1;
        }

        unordered_set<int> near_list;
        pl->_vl_lock.lock(); unordered_set<int> old_vlist = pl->_view_list; pl->_vl_lock.unlock();

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int sx = new_sx + dx; int sy = new_sy + dy;
                if (sx < 0 || sx >= SECTOR_COUNT_X || sy < 0 || sy >= SECTOR_COUNT_Y) continue;
                for (auto& sec_pl : g_sectors[sy][sx].players) {
                    if (sec_pl.second == 0) continue;
                    int other_id = sec_pl.first;
                    if (other_id == c_id) continue;
                    auto other = g_objects[other_id].load();
                    if (other && other->_state == ST_INGAME && can_see(c_id, other_id)) {
                        near_list.insert(other_id);
                    }
                }
            }
        }

        send_move_object_packet(c_id, c_id);

        for (auto& p_id : near_list) {
            auto other = g_objects[p_id].load();
            if (!other) continue;
            if (old_vlist.count(p_id)) {
                send_move_object_packet(p_id, c_id);
            }
            else {
                pl->_vl_lock.lock(); pl->_view_list.insert(p_id); pl->_vl_lock.unlock();
                other->_vl_lock.lock(); other->_view_list.insert(c_id); other->_vl_lock.unlock();
                send_add_object_packet(c_id, p_id); send_add_object_packet(p_id, c_id);
                if (other->is_npc()) wake_up_npc(p_id);
            }
        }

        for (auto& p_id : old_vlist) {
            if (near_list.count(p_id) == 0) {
                auto other = g_objects[p_id].load();
                pl->_vl_lock.lock(); pl->_view_list.erase(p_id); pl->_vl_lock.unlock();
                if (other) {
                    other->_vl_lock.lock(); other->_view_list.erase(c_id); other->_vl_lock.unlock();
                }
                send_remove_object_packet(c_id, p_id); send_remove_object_packet(p_id, c_id);
            }
        }
        break;
    }
    case C2S_CHAT: {
        C2S_Chat* p = reinterpret_cast<C2S_Chat*>(packet);
        pl->_vl_lock.lock(); unordered_set<int> vlist = pl->_view_list; pl->_vl_lock.unlock();
        for (auto v_id : vlist) send_chat_message(v_id, p->message);
        break;
    }
    case C2S_ATTACK: {
        pl->_vl_lock.lock();
        unordered_set<int> vlist = pl->_view_list;
        pl->_vl_lock.unlock();

        for (auto v_id : vlist) {
            send_attack_broadcast(v_id, c_id, pl->stat.weapon);
        }

        int damage = 10;
        int max_search_range = 1; // 탐색을 위한 최대 사거리

        switch (pl->stat.weapon) {
        case hammer: max_search_range = 1; damage = 20; break;
        case sword:  max_search_range = 1; damage = 10; break;
        case spear:  max_search_range = 2; damage = 15; break;
        }

        int sx = pl->x / SECTOR_SIZE;
        int sy = pl->y / SECTOR_SIZE;
        unordered_set<int> hit_targets;

        // 인접 섹터 순회
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = sx + dx; int ny = sy + dy;
                if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;

                for (auto& sec_pl : g_sectors[ny][nx].players) {
                    if (sec_pl.second == 0) continue;
                    int t_id = sec_pl.first;
                    if (t_id == c_id) continue;

                    auto t_obj = g_objects[t_id].load();
                    if (!t_obj || t_obj->_state != ST_INGAME) continue;

                    int dist_x = abs(pl->x - t_obj->x);
                    int dist_y = abs(pl->y - t_obj->y);

                    bool is_hit = false;

                    // ⭐ 기획하신 무기별 공격 범위 수학적 판정
                    switch (pl->stat.weapon) {
                    case sword:
                        if (max(dist_x, dist_y) <= 1) is_hit = true; // 주변 1칸 (대각선 포함)
                        break;
                    case hammer:
                        if (dist_x + dist_y <= 1) is_hit = true;     // 십자 1칸 (상하좌우)
                        break;
                    case spear:
                        if (max(dist_x, dist_y) <= 2) is_hit = true; // 주변 2칸 (대각선 포함)
                        break;
                    }

                    if (is_hit) {
                        hit_targets.insert(t_id);
                    }
                }
            }
        }

        for (int t_id : hit_targets) {
            auto t_obj = g_objects[t_id].load();
            if (!t_obj) continue;

            int new_hp = 0; int t_max_hp = 100; unsigned char t_level = 1; unsigned long long t_exp = 0;

            if (t_obj->is_pc()) {
                auto t_pl = std::static_pointer_cast<Player>(t_obj);
                lock_guard<mutex> ll(t_pl->_lock);
                t_pl->stat.hp -= damage;
                if (t_pl->stat.hp < 0) t_pl->stat.hp = 0;

                new_hp = t_pl->stat.hp; t_max_hp = t_pl->stat.max_hp;
                t_level = t_pl->stat.level; t_exp = t_pl->stat.exp;
            }
            else {
                lock_guard<mutex> ll(t_obj->_lock);
                new_hp = 80;
            }

            S2C_StatusChange status_p;
            status_p.size = sizeof(S2C_StatusChange);
            status_p.type = S2C_STATUS_CHANGE;
            status_p.object_id = t_id;
            status_p.hp = new_hp;
            status_p.max_hp = t_max_hp;
            status_p.level = t_level;
            status_p.exp = t_exp;

            send_packet_to_player(t_id, &status_p);

            t_obj->_vl_lock.lock(); auto t_view = t_obj->_view_list; t_obj->_vl_lock.unlock();
            for (auto v_id : t_view) send_packet_to_player(v_id, &status_p);
        }
        break;
    }
    case C2S_TELEPORT: {
        C2S_Teleport* p = reinterpret_cast<C2S_Teleport*>(packet);
        lock_guard<mutex> ll(pl->_lock);

        int old_sx = pl->x / SECTOR_SIZE; int old_sy = pl->y / SECTOR_SIZE;
        pl->x = p->x; pl->y = p->y;

        // 섹터 범위 보정
        if (pl->x < 0) pl->x = 0; else if (pl->x >= WORLD_WIDTH) pl->x = WORLD_WIDTH - 1;
        if (pl->y < 0) pl->y = 0; else if (pl->y >= WORLD_HEIGHT) pl->y = WORLD_HEIGHT - 1;

        int new_sx = pl->x / SECTOR_SIZE; int new_sy = pl->y / SECTOR_SIZE;
        if (old_sx != new_sx || old_sy != new_sy) {
            if (old_sx >= 0 && old_sx < SECTOR_COUNT_X && old_sy >= 0 && old_sy < SECTOR_COUNT_Y)
                g_sectors[old_sy][old_sx].players[c_id] = 0;
            if (new_sx >= 0 && new_sx < SECTOR_COUNT_X && new_sy >= 0 && new_sy < SECTOR_COUNT_Y)
                g_sectors[new_sy][new_sx].players[c_id] = 1;
        }

        pl->_vl_lock.lock(); unordered_set<int> old_vlist = pl->_view_list; pl->_vl_lock.unlock();
        unordered_set<int> near_list;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int sx = new_sx + dx; int sy = new_sy + dy;
                if (sx < 0 || sx >= SECTOR_COUNT_X || sy < 0 || sy >= SECTOR_COUNT_Y) continue;
                for (auto& sec_pl : g_sectors[sy][sx].players) {
                    if (sec_pl.second == 0) continue;
                    int other_id = sec_pl.first;
                    if (other_id == c_id) continue;
                    auto other = g_objects[other_id].load();
                    if (other && other->_state == ST_INGAME && can_see(c_id, other_id)) {
                        near_list.insert(other_id);
                    }
                }
            }
        }

        send_move_object_packet(c_id, c_id);

        for (auto& p_id : near_list) {
            auto other = g_objects[p_id].load();
            if (!other) continue;
            if (old_vlist.count(p_id)) {
                send_move_object_packet(p_id, c_id);
            }
            else {
                pl->_vl_lock.lock(); pl->_view_list.insert(p_id); pl->_vl_lock.unlock();
                other->_vl_lock.lock(); other->_view_list.insert(c_id); other->_vl_lock.unlock();
                send_add_object_packet(c_id, p_id); send_add_object_packet(p_id, c_id);
                if (other->is_npc()) wake_up_npc(p_id);
            }
        }

        for (auto& p_id : old_vlist) {
            if (near_list.count(p_id) == 0) {
                auto other = g_objects[p_id].load();
                pl->_vl_lock.lock(); pl->_view_list.erase(p_id); pl->_vl_lock.unlock();
                if (other) {
                    other->_vl_lock.lock(); other->_view_list.erase(c_id); other->_vl_lock.unlock();
                    send_remove_object_packet(p_id, c_id);
                }
                send_remove_object_packet(c_id, p_id);
            }
        }
        break;
    }
    case C2S_SELECT_WEAPON: {
        C2S_SelectWeapon* p = reinterpret_cast<C2S_SelectWeapon*>(packet);
        lock_guard<mutex> ll(pl->_lock);
        pl->stat.weapon = p->weapon;
        send_info_result(c_id, c_id, 0, pl->_name, pl->x, pl->y, pl->stat.weapon, pl->stat.hp, pl->stat.max_hp,
			pl->stat.gold, pl->stat.reinforce_level, pl->stat.exp, pl->stat.level, false);
        break;
    }
    case C2S_REQUEST_INFO: {
        C2S_RequestInfo* p = reinterpret_cast<C2S_RequestInfo*>(packet);
        int target_id = -1;
        {
            std::lock_guard<std::mutex> nl(g_name_lock);
            auto it = g_name_to_id.find(p->target_username);
            if (it != g_name_to_id.end()) target_id = it->second;
        }

        if (target_id != -1) {
            auto other_obj = g_objects[target_id].load();
            if (other_obj && other_obj->is_pc() && other_obj->_state == ST_INGAME) {
                auto other_pl = std::static_pointer_cast<Player>(other_obj);
                lock_guard<mutex> ll(other_pl->_lock);
                int current_gold = 1000;
                bool in_party = (other_pl->stat.party_id != 0);
                send_info_result(c_id, target_id, 0, other_pl->_name, other_pl->x, other_pl->y,
                    other_pl->stat.weapon, other_pl->stat.hp, other_pl->stat.max_hp,
                    other_pl->stat.gold, other_pl->stat.reinforce_level,
                    other_pl->stat.exp, other_pl->stat.level, in_party);
            }
        }
        else {
            send_chat_message(c_id, "해당 유저를 찾을 수 없거나 미접속 상태입니다.");
        }
        break;
    }
    case C2S_DUNGEON_ENTRY: {
        // ⭐ [핵심 변경] 클라가 보내준 데이터에 의존하지 않고, 
        // 현재 플레이어가 서 있는 '서버 좌표'를 기준으로 어떤 포탈인지 역추적합니다.
        const PortalInfo* current_portal = nullptr;
        for (const auto& portal : Portals) {
            if (pl->x == portal.src_x && pl->y == portal.src_y) {
                current_portal = &portal;
                break;
            }
        }

        // 포탈이 없는 맨땅에서 스페이스바를 누른 경우 무시 처리
        if (current_portal == nullptr) {
            break;
        }

        // 1. 레벨 제한 검사
        if (pl->stat.level < current_portal->required_level) {
            char err_msg[256];
            sprintf_s(err_msg, "던전 입장 레벨이 부족합니다. (필요 레벨: %d)", current_portal->required_level);
            send_chat_message(c_id, err_msg);
            send_dungeon_result(c_id, current_portal->dungeon, false, pl->x, pl->y);
            break;
        }

        // 2. 진입 조건 통과: 목적지 좌표 설정
        short to_x = 0;
        short to_y = 0;

        // 파이널 보스방 처리 (6x6 격자 던전 배정)
        if (current_portal->dungeon == FINAL_BOSS) {
            int allocated_x = -1;
            int allocated_y = -1;

            {
                std::lock_guard<std::mutex> lock(g_boss_room_lock);
                for (int y = 0; y < 6; ++y) {
                    for (int x = 0; x < 6; ++x) {
                        if (!g_boss_rooms[y][x]) {
                            g_boss_rooms[y][x] = true;
                            allocated_x = x;
                            allocated_y = y;
                            break;
                        }
                    }
                    if (allocated_x != -1) break;
                }
            }

            if (allocated_x == -1) {
                send_chat_message(c_id, "모든 보스 전장이 가득 찼습니다. 잠시 후 다시 시도해 주세요.");
                send_dungeon_result(c_id, current_portal->dungeon, false, pl->x, pl->y);
                break;
            }

            // 보스방 격자 물리 좌표 계산 (간격 110)
            to_x = START_ROOM_X + (allocated_x * GRID_STEP);
            to_y = START_ROOM_Y - (allocated_y * GRID_STEP);

            // 1:1 보스 몬스터 동적 생성 및 스폰
            

            send_chat_message(c_id, "파이널 보스 전장에 입장했습니다! 스페이스바 상호작용 성공!");
        }
        else {
            // 일반 던전 1~7번 이동
            to_x = current_portal->dest_x;
            to_y = current_portal->dest_y;
            send_chat_message(c_id, "던전에 입장했습니다.");
        }

        // 3. 성공 결과 브로드캐스트 및 실제 텔레포트 이동 처리
        send_dungeon_result(c_id, current_portal->dungeon, true, to_x, to_y);

        C2S_Teleport tp;
        tp.size = sizeof(C2S_Teleport);
        tp.type = C2S_TELEPORT;
        tp.x = to_x;
        tp.y = to_y;
        process_packet(c_id, reinterpret_cast<unsigned char*>(&tp));

        break;
    }
    case C2S_DUNGEON_EXIT: {
        C2S_DungeonExit* p = reinterpret_cast<C2S_DungeonExit*>(packet);
        short town_x = WORLD_WIDTH / 2; short town_y = WORLD_HEIGHT / 2;

        send_dungeon_result(c_id, p->dungeon, true, town_x, town_y);

        C2S_Teleport tp;
        tp.size = sizeof(C2S_Teleport); tp.type = C2S_TELEPORT; tp.x = town_x; tp.y = town_y;
        process_packet(c_id, reinterpret_cast<unsigned char*>(&tp));
        break;
    }
    case C2S_INTERACT: {
        bool is_near_npc = false;
        std::string npc_name = "";

        for (const auto& npc : g_npc_spawns) {
            int dist_x = abs(pl->x - npc.x);
            int dist_y = abs(pl->y - npc.y);

            if (dist_x <= 1 && dist_y <= 1) {
                is_near_npc = true;
                npc_name = npc.name;
                break;
            }
        }

        if (is_near_npc) {
            std::string msg = npc_name + " 대장간 창을 엽니다.";
            send_interact_result(c_id, true, msg.c_str());
        }
        else {
            send_interact_result(c_id, false, "주변에 상호작용할 상인이 없습니다.");
        }
        break;
    }
    case C2S_REINFORCE: {
        C2S_Reinforce* p = reinterpret_cast<C2S_Reinforce*>(packet);
        lock_guard<mutex> ll(pl->_lock);

        // 1. 현재 내 무기 타입과 강화도에 맞는 데이터가 테이블에 있는지 조회
        auto key = std::make_pair(pl->stat.weapon, static_cast<short>(pl->stat.reinforce_level));
        auto it = WeaponReinforceTable.find(key);

        // 만약 만렙 강화를 찍었거나 없는 무기 정보라면 차단
        if (it == WeaponReinforceTable.end()) {
            send_chat_message(c_id, "더 이상 강화할 수 없거나 잘못된 무기 정보입니다.");
            break;
        }

        // ⭐ 테이블에서 비용, 확률 정보를 다이렉트로 꺼내옴!
        const ReinforceData& ref_info = it->second;
        int cost = ref_info.cost;
        float rate = ref_info.success_rate;

        // TODO: 실제 유저 소지 골드 변수(pl->gold)로 연동
		int current_gold = pl->stat.gold;

        // 2. 골드 검증
        if (current_gold >= cost) {
            current_gold -= cost; // 골드 차감

            // 3. 테이블에 적힌 확률로 정확하게 판정 (0.0 ~ 100.0f 기준)
            float random_value = static_cast<float>(rand() % 1000) / 10.0f; // 0.0 ~ 99.9
            bool success = (random_value < rate);

            if (success) {
                pl->stat.reinforce_level++;
                send_chat_message(c_id, "강화에 성공했습니다!");
            }
            else {
                send_chat_message(c_id, "강화에 실패했습니다.");
            }

            PushPlayerSaveTask(c_id);

            send_reinforce_result(c_id, success, pl->stat.reinforce_level, current_gold);
        }
        else {
            send_reinforce_result(c_id, false, pl->stat.reinforce_level, current_gold);
            send_chat_message(c_id, "보유 자금이 부족하여 강화를 시도할 수 없습니다.");
        }
        break;
    }
    case C2S_INVITE_PARTY: {
        C2S_InviteParty* p = reinterpret_cast<C2S_InviteParty*>(packet);
        int target_id = -1;
        {
            std::lock_guard<std::mutex> nl(g_name_lock);
            auto it = g_name_to_id.find(p->target_username);
            if (it != g_name_to_id.end()) target_id = it->second;
        }

        if (target_id != -1 && target_id != c_id) {
            auto target_pl = std::static_pointer_cast<Player>(g_objects[target_id].load());
            target_pl->stat.invited_by = c_id; // 초대자 기록
            send_party_invite_notification(target_id, c_id);
            send_chat_message(c_id, "파티 초대를 보냈습니다.");
        }
        else {
            send_chat_message(c_id, "대상을 찾을 수 없습니다.");
        }
        break;
    }
    case C2S_ACCEPT_PARTY: {
        if (pl->stat.invited_by == -1) {
            send_chat_message(c_id, "받은 파티 초대가 없습니다.");
            break;
        }
        if (pl->stat.party_id != 0) {
            send_chat_message(c_id, "이미 다른 파티에 속해 있습니다.");
            break;
        }

        int inviter_id = pl->stat.invited_by;
        auto inviter_obj = g_objects[inviter_id].load();

        // ⭐ [방어 코드 1] 초대한 유저가 접속해 있는지 검증
        if (!inviter_obj || !inviter_obj->is_pc() || inviter_obj->_state != ST_INGAME) {
            send_chat_message(c_id, "초대자가 게임에 없거나 접속이 끊겼습니다.");
            pl->stat.invited_by = -1; // 초대장 소모
            break;
        }

        auto inviter_pl = std::static_pointer_cast<Player>(inviter_obj);
        pl->stat.invited_by = -1; // 초대장 소모

        int target_party_id = 0;
        std::shared_ptr<Party> party = nullptr;

        // ⭐ [방어 코드 2] g_parties_lock의 범위를 최소화하여 데드락 방지
        {
            std::lock_guard<std::mutex> lock(g_parties_lock);

            target_party_id = inviter_pl->stat.party_id;

            // 초대한 사람이 아직 파티가 없다면 새 파티 생성
            if (target_party_id == 0) {
                int new_party_id = g_party_id_gen++;
                party = std::make_shared<Party>();
                party->leader_id = inviter_id;
                party->members.push_back(inviter_id);

                inviter_pl->stat.party_id = new_party_id;
                g_parties[new_party_id] = party;
                target_party_id = new_party_id;
            }
            else {
                if (g_parties.count(target_party_id)) {
                    party = g_parties[target_party_id];
                }
            }
        } // 락 해제: g_parties_lock은 여기서 즉시 해제되어 안전합니다.

        // 파티 객체가 정상적으로 확보되었을 때만 가입 절차 진행
        if (party) {
            std::lock_guard<std::mutex> p_lock(party->p_lock); // 개별 파티 락만 독립적으로 획득
            if (party->members.size() >= MAX_PARTY_SIZE) {
                send_chat_message(c_id, "해당 파티의 인원이 꽉 찼습니다.");
            }
            else {
                party->members.push_back(c_id);
                pl->stat.party_id = target_party_id;
                send_chat_message(c_id, "파티에 가입했습니다!");
            }
        }
        else {
            send_chat_message(c_id, "파티를 생성하거나 찾을 수 없습니다.");
        }

        // 가입 완료 후 파티원 전체에게 UI 업데이트 브로드캐스트
        if (pl->stat.party_id != 0) {
            broadcast_party_update(pl->stat.party_id);
        }
        break;
    }
    case C2S_REFUSE_PARTY: {
        send_chat_message(c_id, "요청을 거절했습니다.");
        break;
    }
    case C2S_LEAVE_PARTY: {
        if (pl->stat.party_id == 0) {
            send_chat_message(c_id, "속한 파티가 없습니다.");
            break;
        }

        int my_party_id = pl->stat.party_id;
        pl->stat.party_id = 0; // 내 파티 정보 초기화

        {
            std::lock_guard<std::mutex> lock(g_parties_lock);
            if (g_parties.count(my_party_id)) {
                auto party = g_parties[my_party_id];
                std::lock_guard<std::mutex> p_lock(party->p_lock);

                // 멤버 리스트에서 나를 제거
                party->members.erase(std::remove(party->members.begin(), party->members.end(), c_id), party->members.end());

                send_chat_message(c_id, "파티에서 탈퇴했습니다.");

                if (party->members.empty()) {
                    // 남은 인원이 없으면 파티 파괴
                    g_parties.erase(my_party_id);
                }
            }
        }

        // ⭐ [해결책] 탈퇴한 '나'에게 파티원이 0명이 되었다는 빈 패킷을 강제로 전송
        S2C_PartyUpdate empty_packet;
        empty_packet.size = sizeof(S2C_PartyUpdate);
        empty_packet.type = S2C_PARTY_UPDATE;
        empty_packet.playerId = c_id;
        empty_packet.party_member_count = 0; // 0명으로 세팅
        send_packet_to_player(c_id, &empty_packet);

        if (g_parties.count(my_party_id)) {
            broadcast_party_update(my_party_id);
        }
        break;
    }
    case C2S_LOGOUT: {
        disconnect(c_id);
        break;
    }
    default: {
        break;
    }
    }
}

void do_npc_random_move(int npc_id) {
    auto obj = g_objects[npc_id].load();
    if (!obj || !obj->is_npc()) return;
    auto npc = std::static_pointer_cast<NPC>(obj);

    int old_sx = npc->x / SECTOR_SIZE; int old_sy = npc->y / SECTOR_SIZE;
    npc->_vl_lock.lock(); unordered_set<int> old_vl = npc->_view_list; npc->_vl_lock.unlock();

    switch (rand() % 4) {
    case 0: if (npc->x < (WORLD_WIDTH - 1)) npc->x++; break;
    case 1: if (npc->x > 0) npc->x--; break;
    case 2: if (npc->y < (WORLD_HEIGHT - 1)) npc->y++; break;
    case 3: if (npc->y > 0) npc->y--; break;
    }

    int new_sx = npc->x / SECTOR_SIZE; int new_sy = npc->y / SECTOR_SIZE;
    if (old_sx != new_sx || old_sy != new_sy) {
        g_sectors[old_sy][old_sx].players[npc_id] = 0;
        g_sectors[new_sy][new_sx].players[npc_id] = 1;
    }

    unordered_set<int> new_vl;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = new_sx + dx; int ny = new_sy + dy;
            if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
            for (auto& sec_pl : g_sectors[ny][nx].players) {
                if (sec_pl.second == 0) continue;
                int p_id = sec_pl.first;
                if (p_id == npc_id) continue;
                auto other = g_objects[p_id].load();
                if (!other || other->_state != ST_INGAME) continue;
                if (can_see(npc_id, p_id)) new_vl.insert(p_id);
            }
        }
    }

    for (auto p_id : new_vl) {
        auto other = g_objects[p_id].load();
        if (!other) continue;
        if (old_vl.count(p_id) == 0) {
            npc->_vl_lock.lock(); npc->_view_list.insert(p_id); npc->_vl_lock.unlock();
            other->_vl_lock.lock(); other->_view_list.insert(npc_id); other->_vl_lock.unlock();
            send_add_object_packet(p_id, npc_id); send_add_object_packet(npc_id, p_id);
        }
        else {
            send_move_object_packet(p_id, npc_id);
        }
    }

    for (auto p_id : old_vl) {
        if (new_vl.count(p_id) == 0) {
            auto other = g_objects[p_id].load();
            npc->_vl_lock.lock(); npc->_view_list.erase(p_id); npc->_vl_lock.unlock();
            if (other) {
                other->_vl_lock.lock(); other->_view_list.erase(npc_id); other->_vl_lock.unlock();
                send_remove_object_packet(p_id, npc_id);
            }
        }
    }
    npc->npc_last_move_time = system_clock::now();
}

void worker_thread(HANDLE h_iocp) {
    while (true) {
        DWORD num_bytes; ULONG_PTR key; WSAOVERLAPPED* over = nullptr;
        BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);
        OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);

        if (FALSE == ret || ((0 == num_bytes) && ((ex_over->_comp_type == OP_RECV) || (ex_over->_comp_type == OP_SEND)))) {
            if (ex_over->_comp_type != OP_ACCEPT) {
                disconnect(static_cast<int>(key));
                if (ex_over->_comp_type == OP_SEND) delete ex_over;
            }
            continue;
        }

        switch (ex_over->_comp_type) {
        case OP_ACCEPT: {
            int client_id = player_index_count++;
            if (client_id < MAX_PLAYERS) {
                auto new_sess = std::make_shared<Session>(client_id, g_c_socket);
                g_sessions[client_id].store(new_sess);
                auto new_pl = std::make_shared<Player>(client_id);
                new_pl->_session = new_sess.get(); g_objects[client_id].store(new_pl);

                CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_c_socket), h_iocp, client_id, 0);
                new_sess->do_recv();
                g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            }
            else {
                closesocket(g_c_socket); g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            }
            ZeroMemory(&g_a_over._over, sizeof(g_a_over._over));
            int addr_size = sizeof(SOCKADDR_IN);
            AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, NULL, &g_a_over._over);
            break;
        }
        case OP_RECV: {
            int c_id = static_cast<int>(key);
            auto session = g_sessions[c_id].load();
            if (!session) break;

            session->_count += num_bytes;
            session->_tail = (session->_tail + num_bytes) % RING_BUF_SIZE;

            while (session->_count > 0) {
                unsigned char packet_size = session->_recv_buf[session->_head];

                if (packet_size == 0 || session->_count < packet_size) {
                    break;
                }

                char complete_packet[BUF_SIZE];

                if (session->_head + packet_size <= RING_BUF_SIZE) {
                    memcpy(complete_packet, session->_recv_buf + session->_head, packet_size);
                }
                else {
                    int part1_len = RING_BUF_SIZE - session->_head;
                    int part2_len = packet_size - part1_len;
                    memcpy(complete_packet, session->_recv_buf + session->_head, part1_len);
                    memcpy(complete_packet + part1_len, session->_recv_buf, part2_len);
                }

                process_packet(c_id, reinterpret_cast<unsigned char*>(complete_packet));

                session->_head = (session->_head + packet_size) % RING_BUF_SIZE;
                session->_count -= packet_size;
            }

            session->do_recv();
            break;
        }
        case OP_SEND: delete ex_over; break;
        case OP_NPCMOVE: {
            delete ex_over;
            int npc_id = static_cast<int>(key);

            auto obj = g_objects[npc_id].load();
            if (!obj || !obj->is_npc()) break;
            auto npc = std::static_pointer_cast<NPC>(obj);

            int target_player_id = -1;
            bool has_nearby_player = false;
            int sx = npc->x / SECTOR_SIZE; int sy = npc->y / SECTOR_SIZE;

            for (int dy = -1; dy <= 1 && !has_nearby_player; ++dy) {
                for (int dx = -1; dx <= 1 && !has_nearby_player; ++dx) {
                    int nx = sx + dx; int ny = sy + dy;
                    if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
                    for (auto& sec_pl : g_sectors[ny][nx].players) {
                        if (sec_pl.second == 0) continue;
                        int p_id = sec_pl.first; auto other = g_objects[p_id].load();
                        if (other && other->is_pc() && other->_state == ST_INGAME && can_see(npc_id, p_id)) {
                            target_player_id = p_id;
                            has_nearby_player = true;
                            break;
                        }
                    }
                }
            }

            g_lua_mgr.RunAI(npc_id, target_player_id);

            if (has_nearby_player) {
                event_type ev; ev.event_id = EVENT_MOVE; ev.obj_id = npc_id; ev.target_id = -1;
                ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME); timer_queue.push(ev);
            }
            else {
                npc->_active_npc = false;
            }
            break;
        }
        }
    }
}

void timer_thread() {
    while (true) {
        event_type ev;
        if (timer_queue.try_pop(ev)) {
            if (ev.wakeup_time <= system_clock::now()) {
                if (ev.event_id == EVENT_MOVE) {
                    OVER_EXP* move_over = new OVER_EXP; move_over->_comp_type = OP_NPCMOVE;
                    PostQueuedCompletionStatus(h_iocp, -1, ev.obj_id, &move_over->_over);
                }
            }
            else {
                timer_queue.push(ev); this_thread::sleep_for(chrono::milliseconds(1));
            }
        }
        else { this_thread::sleep_for(chrono::milliseconds(1)); }
    }
}

void InitPlayerFromDB(int c_id, std::string name, const DB_Task& data) {
    auto obj = g_objects[c_id].load();
    if (!obj || !obj->is_pc()) return;
    auto pl = std::static_pointer_cast<Player>(obj);

    // ⭐ 락을 잠그고 안전하게 세이브 파일 정보를 메모리에 대입
    {
        std::lock_guard<std::mutex> ll{ pl->_lock };
        pl->x = data.x;
        pl->y = data.y;
        pl->stat.level = data.level;
        pl->stat.exp = data.exp;
		pl->stat.hp = data.max_hp;
        pl->stat.max_hp = data.max_hp;
        pl->stat.weapon = data.weapon;
        pl->stat.reinforce_level = data.reinforce_level;
		pl->stat.gold = data.gold;

        pl->_state = ST_INGAME; // 💥 이제 완벽히 게임 월드 라이브 상태로 승격!
    }

    int sx = pl->x / SECTOR_SIZE;
    int sy = pl->y / SECTOR_SIZE;
    g_sectors[sy][sx].players[c_id] = 1;

    // 클라이언트에 기분 좋은 로드 성공 소식을 전송
    S2C_AvatarInfo info;
    info.size = sizeof(S2C_AvatarInfo);
    info.type = S2C_AVATAR_INFO;
    info.playerId = c_id;
    info.visualId = player;
    strcpy_s(info.username, pl->_name);
    info.x = pl->x; info.y = pl->y;
    info.hp = pl->stat.hp; info.max_hp = pl->stat.max_hp;
    info.level = pl->stat.level; info.exp = pl->stat.exp;
    send_packet_to_player(c_id, &info);

    // 주변 시야 동기화 
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = sx + dx; int ny = sy + dy;
            if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
            for (auto& sec_pl : g_sectors[ny][nx].players) {
                if (sec_pl.second == 0) continue;
                int other_id = sec_pl.first;
                if (other_id == c_id) continue;
                auto other = g_objects[other_id].load();
                if (!other || other->_state != ST_INGAME || !can_see(c_id, other_id)) continue;

                pl->_vl_lock.lock(); pl->_view_list.insert(other_id); pl->_vl_lock.unlock();
                other->_vl_lock.lock(); other->_view_list.insert(c_id); other->_vl_lock.unlock();

                send_add_object_packet(c_id, other_id);
                send_add_object_packet(other_id, c_id);
                if (other->is_npc()) wake_up_npc(other_id);
            }
        }
    }
    if (pl->stat.weapon != 0) {
        send_info_result(c_id, c_id, 0, pl->_name, pl->x, pl->y, pl->stat.weapon, pl->stat.hp, pl->stat.max_hp,
            pl->stat.gold, pl->stat.reinforce_level, pl->stat.exp, pl->stat.level, false);
    }
    std::cout << "[DB 컴플리션] 유저 불러오기 완료: " << name << " (Lv." << (int)data.level << ")" << std::endl;
}

int main() {
    WSADATA WSAData; WSAStartup(MAKEWORD(2, 2), &WSAData);

    if (!g_db_mgr.Initialize()) {
        std::cout << "MS-SQL (2022182002_gsp) 연결 엔진 초기화 실패! 구동을 차단합니다." << std::endl;
        return 0;
    }

    if (!g_lua_mgr.Initialize()) {
        std::cout << "루아 스크립트 엔진 컴파일 실패!" << std::endl;
        return 0;
    }

    g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr; memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; server_addr.sin_port = htons(PORT); server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)); listen(g_s_socket, SOMAXCONN);

    LoadMapCSV("..\\..\\Resource\\world.csv");

    cout << "NPC initialize begin.\n";
    for (int i = MAX_PLAYERS; i < MAX_PLAYERS + NUM_NPCS; ++i) {
        auto new_npc = std::make_shared<NPC>(i);
        new_npc->x = rand() % WORLD_WIDTH; new_npc->y = rand() % WORLD_HEIGHT; new_npc->_state = ST_INGAME;
        g_objects[i].store(new_npc); g_sectors[new_npc->y / SECTOR_SIZE][new_npc->x / SECTOR_SIZE].players[i] = 1;
    }
    cout << "NPC initialize end.\n";

    h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_s_socket), h_iocp, 9999, 0);

    g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    g_a_over._comp_type = OP_ACCEPT;
    int addr_size = sizeof(SOCKADDR_IN);
    AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, NULL, &g_a_over._over);

    vector<thread> worker_threads; thread timer_th(timer_thread);
    for (unsigned int i = 0; i < std::thread::hardware_concurrency(); ++i) worker_threads.emplace_back(worker_thread, h_iocp);
    for (auto& th : worker_threads) th.join();
    timer_th.join(); closesocket(g_s_socket); WSACleanup();
}