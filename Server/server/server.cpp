#include "stdafx.h"
#include "Session.h"
#include "Player.h"
#include "NPC.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")

struct Sector { tbb::concurrent_unordered_map<int, int> players; };
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

tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<Session>>> g_sessions;
tbb::concurrent_unordered_map<int, std::atomic<std::shared_ptr<GameObject>>> g_objects;

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
    } else {
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

void send_interact_result(int send_to_id, bool success, char* message) {
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

void send_party_apply_notification(int send_to_id, int applicant_id) {
    S2C_PartyApplyNoti p;
    p.size = sizeof(S2C_PartyApplyNoti);
    p.type = S2C_PARTY_APPLY_NOTI;
    p.playerId = applicant_id;
    strncpy_s(p.applicant_username, g_objects[applicant_id].load() ? std::static_pointer_cast<Player>(g_objects[applicant_id].load())->_name : "Unknown", MAX_NAME_LEN);
    send_packet_to_player(send_to_id, &p);
}

void send_party_update(int send_to_id, int member_id, bool joined) {
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
            lock_guard<mutex> ll{ pl->_lock };
            pl->x = rand() % WORLD_WIDTH; pl->y = rand() % WORLD_HEIGHT;
            pl->_state = ST_INGAME;
            pl->stat.weapon = SWORD; pl->stat.hp = pl->stat.max_hp = 100;
        }

        int sx = pl->x / SECTOR_SIZE; int sy = pl->y / SECTOR_SIZE;
        g_sectors[sy][sx].players[c_id] = 1;

        S2C_AvatarInfo info;
        info.size = sizeof(S2C_AvatarInfo); info.type = S2C_AVATAR_INFO; info.playerId = c_id;
        strcpy_s(info.username, pl->_name);
        info.x = pl->x; info.y = pl->y; info.hp = pl->stat.hp; info.max_hp = pl->stat.max_hp;
        info.level = pl->stat.level; info.exp = pl->stat.exp;
        send_packet_to_player(c_id, &info);

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
        break;
    }
    case C2S_MOVE: {
        C2S_Move* p = reinterpret_cast<C2S_Move*>(packet);
        pl->last_move_time = p->move_time;
        int old_sx = pl->x / SECTOR_SIZE; int old_sy = pl->y / SECTOR_SIZE;

        short nx = pl->x + p->x; short ny = pl->y + p->y;
        if (nx < 0) nx = 0; else if (nx >= WORLD_WIDTH) nx = WORLD_WIDTH - 1;
        if (ny < 0) ny = 0; else if (ny >= WORLD_HEIGHT) ny = WORLD_HEIGHT - 1;
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
        int damage = 10;
        int max_search_range = 1; // 탐색을 위한 최대 사거리

        switch (pl->stat.weapon) {
        case HAMMER: max_search_range = 1; damage = 20; break;
        case SWORD:  max_search_range = 1; damage = 10; break;
        case SPEAR:  max_search_range = 2; damage = 15; break;
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

                    switch (pl->stat.weapon) {
                    case HAMMER:
                        if (dist_x + dist_y <= 1) is_hit = true;
                        break;
                    case SWORD:
                        if (max(dist_x, dist_y) <= 1) is_hit = true;
                        break;
                    case SPEAR:
                        if (max(dist_x, dist_y) <= 2) is_hit = true;
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
        break;
    }
    case C2S_REQUEST_INFO: {
        C2S_RequestInfo* p = reinterpret_cast<C2S_RequestInfo*>(packet);
        bool found = false;

        for (int i = 0; i < MAX_PLAYERS; ++i) {
            auto other_obj = g_objects[i].load();
            if (!other_obj || !other_obj->is_pc() || other_obj->_state != ST_INGAME) continue;
            auto other_pl = std::static_pointer_cast<Player>(other_obj);

            if (strncmp(other_pl->_name, p->target_username, MAX_NAME_LEN) == 0) {
                lock_guard<mutex> ll(other_pl->_lock);

                // TODO: Player.h 에 gold 변수 추가 시 other_pl->gold 로 변경 
                int current_gold = 1000;
                // TODO: Player.h 에 party_id 변수 추가 시 (other_pl->party_id != -1) 로 확인
                bool in_party = false;

                send_info_result(c_id, i, 0, other_pl->_name, other_pl->x, other_pl->y,
                    other_pl->stat.weapon, other_pl->stat.hp, other_pl->stat.max_hp,
                    current_gold, other_pl->stat.reinforce_level,
                    other_pl->stat.exp, other_pl->stat.level, in_party);
                found = true;
                break;
            }
        }

        if (!found) send_chat_message(c_id, "해당 유저를 찾을 수 없습니다.");
        break;
    }
    case C2S_DUNGEON_ENTRY: {
        C2S_DungeonEntry* p = reinterpret_cast<C2S_DungeonEntry*>(packet);

        // 예시용 레벨 제한 규칙
        int required_level = (p->dungeon + 1) * 5;

        if (pl->stat.level >= required_level) {
            // 입장 성공 시 던전 고유 좌표 이동
            short to_x = 100 + (p->dungeon * 100);
            short to_y = 100;

            send_dungeon_result(c_id, p->dungeon, true, to_x, to_y);

            // 이동 브로드캐스트 처리 (텔레포트 재사용)
            C2S_Teleport tp;
            tp.size = sizeof(C2S_Teleport); tp.type = C2S_TELEPORT; tp.x = to_x; tp.y = to_y;
            process_packet(c_id, reinterpret_cast<unsigned char*>(&tp));
        }
        else {
            send_dungeon_result(c_id, p->dungeon, false, pl->x, pl->y);
            send_chat_message(c_id, "던전 입장 레벨이 부족합니다.");
        }
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
        bool npc_found = false;
        int sx = pl->x / SECTOR_SIZE; int sy = pl->y / SECTOR_SIZE;
        for (int dy = -1; dy <= 1 && !npc_found; ++dy) {
            for (int dx = -1; dx <= 1 && !npc_found; ++dx) {
                int nx = sx + dx; int ny = sy + dy;
                if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
                for (auto& sec_pl : g_sectors[ny][nx].players) {
                    if (sec_pl.second == 0) continue;
                    int id = sec_pl.first;
                    auto target_obj = g_objects[id].load();
                    if (target_obj && target_obj->is_npc()) {
                        int dist = max(abs(pl->x - target_obj->x), abs(pl->y - target_obj->y));
                        if (dist <= 2) {
                            npc_found = true;
                            break;
                        }
                    }
                }
            }
        }

        if (npc_found) send_interact_result(c_id, true, "강화 상인과 대화합니다.");
        else send_interact_result(c_id, false, "주변에 상호작용 할 NPC가 없습니다.");
        break;
    }
    case C2S_REINFORCE: {
        C2S_Reinforce* p = reinterpret_cast<C2S_Reinforce*>(packet);
        lock_guard<mutex> ll(pl->_lock);

        // 현재 강화 수치에 비례한 골드 소모량 확인
        int current_reinforce = pl->stat.reinforce_level;
        int cost = 100 * (current_reinforce + 1);

        // TODO: Player.h에 gold 변수 추가 시 pl->gold 로 변경 
        int current_gold = 10000;

        if (current_gold >= cost) {
            current_gold -= cost; // pl->gold -= cost;

            // 결과 확률 판정 (50%)
            bool success = (rand() % 100) < 50;
            if (success) {
                pl->stat.reinforce_level++;
                send_chat_message(c_id, "강화에 성공했습니다!");
            }
            else {
                send_chat_message(c_id, "강화에 실패했습니다.");
            }
            send_reinforce_result(c_id, success, pl->stat.reinforce_level, current_gold);
        }
        else {
            send_reinforce_result(c_id, false, current_reinforce, current_gold);
            send_chat_message(c_id, "골드가 부족합니다.");
        }
        break;
    }
    case C2S_INVITE_PARTY: {
        C2S_InviteParty* p = reinterpret_cast<C2S_InviteParty*>(packet);
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            if (i == c_id) continue;
            auto other_obj = g_objects[i].load();
            if (!other_obj || !other_obj->is_pc() || other_obj->_state != ST_INGAME) continue;
            auto other_pl = std::static_pointer_cast<Player>(other_obj);
            if (strncmp(other_pl->_name, p->target_username, MAX_NAME_LEN) == 0) {
                send_party_invite_notification(i, c_id);
                send_chat_message(c_id, "파티 초대를 보냈습니다.");
                break;
            }
        }
        break;
    }
    case C2S_APPLY_PARTY: {
        C2S_ApplyParty* p = reinterpret_cast<C2S_ApplyParty*>(packet);
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            if (i == c_id) continue;
            auto other_obj = g_objects[i].load();
            if (!other_obj || !other_obj->is_pc() || other_obj->_state != ST_INGAME) continue;
            auto other_pl = std::static_pointer_cast<Player>(other_obj);
            if (strncmp(other_pl->_name, p->target_username, MAX_NAME_LEN) == 0) {
                send_party_apply_notification(i, c_id);
                send_chat_message(c_id, "파티 가입을 요청했습니다.");
                break;
            }
        }
        break;
    }
    case C2S_ACCEPT_PARTY: {
        send_chat_message(c_id, "파티를 수락했습니다.");
        break;
    }
    case C2S_REFUSE_PARTY: {
        send_chat_message(c_id, "요청을 거절했습니다.");
        break;
    }
    case C2S_LEAVE_PARTY: {
        send_chat_message(c_id, "파티에서 탈퇴했습니다.");
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

void disconnect(int c_id) {
    auto session = g_sessions[c_id].load();
    auto obj = g_objects[c_id].load();

    if (obj && obj->is_pc()) {
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
        } else {
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
            } else {
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
            int npc_id = static_cast<int>(key); do_npc_random_move(npc_id);
            auto obj = g_objects[npc_id].load();
            if (!obj || !obj->is_npc()) break;
            auto npc = std::static_pointer_cast<NPC>(obj);

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
                            has_nearby_player = true; break;
                        }
                    }
                }
            }
            if (has_nearby_player) {
                event_type ev; ev.event_id = EVENT_MOVE; ev.obj_id = npc_id; ev.target_id = -1;
                ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME); timer_queue.push(ev);
            } else { npc->_active_npc = false; }
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
            } else {
                timer_queue.push(ev); this_thread::sleep_for(chrono::milliseconds(1));
            }
        } else { this_thread::sleep_for(chrono::milliseconds(1)); }
    }
}

int main() {
    WSADATA WSAData; WSAStartup(MAKEWORD(2, 2), &WSAData);
    g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr; memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; server_addr.sin_port = htons(PORT); server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)); listen(g_s_socket, SOMAXCONN);

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
    for (int i = 0; i < std::thread::hardware_concurrency(); ++i) worker_threads.emplace_back(worker_thread, h_iocp);
    for (auto& th : worker_threads) th.join();
    timer_th.join(); closesocket(g_s_socket); WSACleanup();
}