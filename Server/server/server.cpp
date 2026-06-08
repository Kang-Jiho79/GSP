#include "stdafx.h"
#include "GameData.h"
#include "Session.h"
#include "Player.h"
#include "NPC.h"
#include "DB.h"
#include "LuaManager.h"

#pragma comment(lib, "lua55.lib")
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
        if (pl->_state != ST_INGAME) return;
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

    if (add_obj->is_pc()) {
        auto pl = std::static_pointer_cast<Player>(add_obj);
        strcpy_s(p.obj_name, pl->_name);
        p.hp = pl->stat.hp; p.max_hp = pl->stat.max_hp; p.exp = pl->stat.exp; p.level = pl->stat.level;
        p.visual_id = 0;
    }
    else {
        auto npc = std::static_pointer_cast<NPC>(add_obj);
        sprintf_s(p.obj_name, "%s", npc->name.c_str());
        p.hp = npc->stat.hp; p.max_hp = npc->stat.max_hp;
        p.level = npc->level; p.exp = 0;

		int matched_visual_id = 0;
        for (size_t t = 0; t < MonsterTemplates.size(); ++t) {
            if (MonsterTemplates[t].name == npc->name) {
                matched_visual_id = static_cast<int>(t) + 1;
                break;
            }
        }
        p.visual_id = matched_visual_id;
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

        // ⭐ [버그 수정 2] 좌표 가중 멀티스레드 보호를 위해 플레이어 락을 조기에 잠급니다.
        lock_guard<mutex> move_lock(pl->_lock);

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

        // ⭐ 안전 경계 보정 규칙 추가 (섹터 이탈 방지)
        if (new_sx < 0) new_sx = 0; else if (new_sx >= SECTOR_COUNT_X) new_sx = SECTOR_COUNT_X - 1;
        if (new_sy < 0) new_sy = 0; else if (new_sy >= SECTOR_COUNT_Y) new_sy = SECTOR_COUNT_Y - 1;

        if (old_sx != new_sx || old_sy != new_sy) {
            if (old_sx >= 0 && old_sx < SECTOR_COUNT_X && old_sy >= 0 && old_sy < SECTOR_COUNT_Y)
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

        int sx = pl->x / SECTOR_SIZE;
        int sy = pl->y / SECTOR_SIZE;
        unordered_set<int> hit_targets;

        int damage = WeaponReinforceTable.at({ pl->stat.weapon, pl->stat.reinforce_level }).damage;

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
                    case sword:
                        if (max(dist_x, dist_y) <= 1) is_hit = true;
                        break;
                    case hammer:
                        if (dist_x + dist_y <= 1) is_hit = true;
                        break;
                    case spear:
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
                t_pl->stat.hp -= damage; if (t_pl->stat.hp < 0) t_pl->stat.hp = 0;
                new_hp = t_pl->stat.hp; t_max_hp = t_pl->stat.max_hp; t_level = t_pl->stat.level; t_exp = t_pl->stat.exp;
            }
            else {
                // 💥 몬스터(NPC) 타격 시 로직 정밀 보정
                auto npc = std::static_pointer_cast<NPC>(t_obj);
                lock_guard<mutex> ll(npc->_lock);

                // ① 유저의 무기 데미지만큼 몬스터의 실시간 현재 체력을 차감합니다.
                npc->stat.hp -= damage;
                if (npc->stat.hp < 0) npc->stat.hp = 0;

                // ② 주변 유저들에게 이 몬스터의 체력이 깎였다는 상태 변화 패킷을 보냅니다.
                // (이 코드가 있어야 몬스터 머리 위의 HP 바가 실시간으로 줄어듭니다!)
                S2C_StatusChange mon_status_p;
                mon_status_p.size = sizeof(S2C_StatusChange);
                mon_status_p.type = S2C_STATUS_CHANGE;
                mon_status_p.object_id = t_id;
                mon_status_p.hp = npc->stat.hp;
                mon_status_p.max_hp = npc->stat.max_hp;
                mon_status_p.level = npc->level;
                mon_status_p.exp = 0;

                // 내 화면 및 주변 유저 시야에 실시간 체력 변화 브로드캐스트
                send_packet_to_player(c_id, &mon_status_p);
                pl->_vl_lock.lock(); auto my_view_for_mon = pl->_view_list; pl->_vl_lock.unlock();
                for (auto v_id : my_view_for_mon) {
                    send_packet_to_player(v_id, &mon_status_p);
                }

                // ③ 실제 체력이 0이 되었을 때만 사망 판정을 내립니다!
                bool is_monster_dead = (npc->stat.hp <= 0);

                if (is_monster_dead) {
                    npc->_state = ST_FREE;
                    g_sectors[npc->y / SECTOR_SIZE][npc->x / SECTOR_SIZE].players[t_id] = 0;

                    unsigned long long reward_exp = npc->level * npc->level * 2;

                    if (npc->ai_type == "Agro") { reward_exp *= 2; }
                    if (npc->move_type == "로밍") { reward_exp *= 2; }

                    pl->stat.exp += reward_exp;
                    pl->stat.gold += npc->gold_reward;
                    send_chat_message(c_id, "💥 몬스터를 처치하여 경험치를 획득했습니다!");

                    // 경험치 초기화/차감형 레벨업 루프 가동
                    bool leveled_up = false;
                    while (pl->stat.level < 50) {
                        unsigned long long required_exp = LevelMaxHpTable[pl->stat.level].exp;

                        if (pl->stat.exp >= required_exp) {
                            pl->stat.exp -= required_exp;
                            pl->stat.level++;
                            leveled_up = true;
                        }
                        else {
                            break;
                        }
                    }

                    if (leveled_up) {
                        pl->stat.max_hp = LevelMaxHpTable[pl->stat.level].max_hp;
                        pl->stat.hp = pl->stat.max_hp;
                        send_chat_message(c_id, "🎉 축하합니다! 레벨이 상승했습니다!");
                    }

                    send_status_change(c_id, pl->stat.hp, pl->stat.max_hp, pl->stat.exp, pl->stat.level);
                    send_remove_object_packet(c_id, t_id);

                    pl->_vl_lock.lock(); auto my_view = pl->_view_list; pl->_vl_lock.unlock();
                    for (auto v_id : my_view) {
                        send_remove_object_packet(v_id, t_id);
                        send_status_change(v_id, pl->stat.hp, pl->stat.max_hp, pl->stat.exp, pl->stat.level);
                    }

                    // 30초 후 리스폰 등록
                    event_type respawn_ev;
                    respawn_ev.obj_id = t_id;
                    respawn_ev.event_id = EVENT_RESPAWN;
                    respawn_ev.wakeup_time = system_clock::now() + milliseconds(30000);
                    timer_queue.push(respawn_ev);
                }
                // 아직 살아있다면 다음 타겟팅 연산을 위해 계속 루프 진행
                continue;
            }

            S2C_StatusChange status_p;
            status_p.size = sizeof(S2C_StatusChange); status_p.type = S2C_STATUS_CHANGE;
            status_p.object_id = t_id; status_p.hp = new_hp; status_p.max_hp = t_max_hp; status_p.level = t_level; status_p.exp = t_exp;

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
        const PortalInfo* current_portal = nullptr;
        for (const auto& portal : Portals) {
            if (pl->x == portal.src_x && pl->y == portal.src_y) {
                current_portal = &portal;
                break;
            }
        }

        if (current_portal == nullptr) {
            break;
        }

        if (pl->stat.level < current_portal->required_level) {
            char err_msg[256];
            sprintf_s(err_msg, "던전 입장 레벨이 부족합니다. (필요 레벨: %d)", current_portal->required_level);
            send_chat_message(c_id, err_msg);
            send_dungeon_result(c_id, current_portal->dungeon, false, pl->x, pl->y);
            break;
        }

        short to_x = 0;
        short to_y = 0;

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

            to_x = START_ROOM_X + (allocated_x * GRID_STEP);
            to_y = START_ROOM_Y - (allocated_y * GRID_STEP);

            send_chat_message(c_id, "파이널 보스 전장에 입장했습니다! 스페이스바 상호작용 성공!");
        }
        else {
            to_x = current_portal->dest_x;
            to_y = current_portal->dest_y;
            send_chat_message(c_id, "던전에 입장했습니다.");
        }

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

        auto key = std::make_pair(pl->stat.weapon, static_cast<short>(pl->stat.reinforce_level));
        auto it = WeaponReinforceTable.find(key);

        if (it == WeaponReinforceTable.end()) {
            send_chat_message(c_id, "더 이상 강화할 수 없거나 잘못된 무기 정보입니다.");
            break;
        }

        const ReinforceData& ref_info = it->second;
        int cost = ref_info.cost;
        float rate = ref_info.success_rate;

        int current_gold = pl->stat.gold;

        if (current_gold >= cost) {
            current_gold -= cost;

            float random_value = static_cast<float>(rand() % 1000) / 10.0f;
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
            target_pl->stat.invited_by = c_id;
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

        if (!inviter_obj || !inviter_obj->is_pc() || inviter_obj->_state != ST_INGAME) {
            send_chat_message(c_id, "초대자가 게임에 없거나 접속이 끊겼습니다.");
            pl->stat.invited_by = -1;
            break;
        }

        auto inviter_pl = std::static_pointer_cast<Player>(inviter_obj);
        pl->stat.invited_by = -1;

        int target_party_id = 0;
        std::shared_ptr<Party> party = nullptr;

        {
            std::lock_guard<std::mutex> lock(g_parties_lock);

            target_party_id = inviter_pl->stat.party_id;

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
        }

        if (party) {
            std::lock_guard<std::mutex> p_lock(party->p_lock);
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
        pl->stat.party_id = 0;

        {
            std::lock_guard<std::mutex> lock(g_parties_lock);
            if (g_parties.count(my_party_id)) {
                auto party = g_parties[my_party_id];
                std::lock_guard<std::mutex> p_lock(party->p_lock);

                party->members.erase(std::remove(party->members.begin(), party->members.end(), c_id), party->members.end());

                send_chat_message(c_id, "파티에서 탈퇴했습니다.");

                if (party->members.empty()) {
                    g_parties.erase(my_party_id);
                }
            }
        }

        S2C_PartyUpdate empty_packet;
        empty_packet.size = sizeof(S2C_PartyUpdate);
        empty_packet.type = S2C_PARTY_UPDATE;
        empty_packet.playerId = c_id;
        empty_packet.party_member_count = 0;
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
            if (!obj || obj->_state != ST_INGAME) break;
            auto npc = std::static_pointer_cast<NPC>(obj);

            int target_player_id = -1;
            bool has_nearby_player = false;
            int sx = npc->x / SECTOR_SIZE; int sy = npc->y / SECTOR_SIZE;

            // 섹터 범위 예외 안전망
            if (sx < 0 || sx >= SECTOR_COUNT_X || sy < 0 || sy >= SECTOR_COUNT_Y) break;

            for (int dy = -1; dy <= 1 && !has_nearby_player; ++dy) {
                for (int dx = -1; dx <= 1 && !has_nearby_player; ++dx) {
                    int nx = sx + dx; int ny = sy + dy;
                    if (nx < 0 || nx >= SECTOR_COUNT_X || ny < 0 || ny >= SECTOR_COUNT_Y) continue;
                    for (auto& sec_pl : g_sectors[ny][nx].players) {
                        if (sec_pl.second == 0) continue;
                        int p_id = sec_pl.first;

                        // 진짜 유저(0 ~ MAX_PLAYERS) 범위만 필터링
                        if (p_id < 0 || p_id >= MAX_PLAYERS) continue;

                        auto other = g_objects[p_id].load();
                        if (other && other->is_pc() && other->_state == ST_INGAME && can_see(npc_id, p_id)) {
                            target_player_id = p_id;
                            has_nearby_player = true;
                            break;
                        }
                    }
                }
            }

            // 진짜 타겟 유저가 있거나, 타겟은 없지만 평화 상태의 로밍/고정 이동 규칙을 수행해야 할 때 가동
            g_lua_mgr.RunAI(npc_id, target_player_id);

            // 계속 유저가 감지된다면 무한 루프 과부하 방지를 위해 1초(MOVE_COOL_TIME) 딜레이 뒤 다시 알람을 주도록 세팅
            if (has_nearby_player) {
                event_type ev; ev.event_id = EVENT_MOVE; ev.obj_id = npc_id; ev.target_id = -1;
                ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
                timer_queue.push(ev);
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
                else if (ev.event_id == EVENT_RESPAWN) {
                    auto obj = g_objects[ev.obj_id].load();
                    if (obj) {
                        auto npc = std::static_pointer_cast<NPC>(obj);
                        npc->x = npc->spawn_x;
                        npc->y = npc->spawn_y;
                        npc->_state = ST_INGAME;
                        npc->_active_npc = false;
                        g_sectors[npc->y / SECTOR_SIZE][npc->x / SECTOR_SIZE].players[ev.obj_id] = 1;
                        std::cout << "[부활 알림] 몬스터 " << ev.obj_id << "번이 지정 좌표에 리스폰되었습니다." << std::endl;
                    }
                }
            }
            else { timer_queue.push(ev); this_thread::sleep_for(chrono::milliseconds(1)); }
        }
        else { this_thread::sleep_for(chrono::milliseconds(1)); }
    }
}

void InitPlayerFromDB(int c_id, std::string name, const DB_Task& data) {
    auto obj = g_objects[c_id].load();
    if (!obj || !obj->is_pc()) return;
    auto pl = std::static_pointer_cast<Player>(obj);

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

        pl->_state = ST_INGAME;
    }

    int sx = pl->x / SECTOR_SIZE;
    int sy = pl->y / SECTOR_SIZE;
    g_sectors[sy][sx].players[c_id] = 1;

    S2C_AvatarInfo info;
    info.size = sizeof(S2C_AvatarInfo);
    info.type = S2C_AVATAR_INFO;
    info.playerId = c_id;

    // ⭐ [버그 수정 3] 하드코딩된 대문자 'player'는 컴파일 에러 유발 요소이므로 
    // 실제 DB에서 복구해낸 무기 타입 번호로 정확하게 채워줍니다.
    info.visualId = static_cast<int>(data.weapon);

    strcpy_s(info.username, pl->_name);
    info.x = pl->x; info.y = pl->y;
    info.hp = pl->stat.hp; info.max_hp = pl->stat.max_hp;
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
    if (pl->stat.weapon != 0) {
        send_info_result(c_id, c_id, 0, pl->_name, pl->x, pl->y, pl->stat.weapon, pl->stat.hp, pl->stat.max_hp,
            pl->stat.gold, pl->stat.reinforce_level, pl->stat.exp, pl->stat.level, false);
    }
    std::cout << "[DB 컴플리션] 유저 불러오기 완료: " << name << " (Lv." << (int)data.level << ")" << std::endl;
}

int main() {
    WSADATA WSAData; WSAStartup(MAKEWORD(2, 2), &WSAData);

    if (!g_db_mgr.Initialize()) {
        std::cout << "MS-SQL 연결 엔진 초기화 실패! 구동을 차단합니다." << std::endl;
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

    std::cout << "데이터 테이블 기반 사냥터 구역별 NPC 배치 시작 (20만 마리)...\n";

    int spawned_count = 0;

    // 20만 마리를 7개의 사냥터 구역에 똑같이 분배 (구역당 약 28,571마리씩)
    const int NPCS_PER_ZONE = NUM_NPCS / 7;

    // 사냥터 구역 테이블(HuntingZoneTable)을 하나씩 순회
    for (const auto& zone : HuntingZoneTable) {
        std::cout << "▶ [" << zone.zone_name << "] 지역 배치 시작... (템플릿: "
            << MonsterTemplates[zone.template_index].name << ")\n";

        // 구역 내부 가로세로 폭 계산
        int zone_width = zone.max_x - zone.min_x;
        int zone_height = zone.max_y - zone.min_y;

        int zone_spawned = 0;
        while (zone_spawned < NPCS_PER_ZONE) {
            int i = MAX_PLAYERS + spawned_count;
            auto new_npc = std::make_shared<NPC>(i);

            // 💥 [테이블 연동] 각 사냥터 테이블에 정의된 최소~최대 범위 안에서만 랜덤 좌표를 뽑아냅니다!
            int rx = zone.min_x + (rand() % zone_width);
            int ry = zone.min_y + (rand() % zone_height);

            // 월드 오버플로우 방지 및 보정
            if (rx >= WORLD_WIDTH)  rx = WORLD_WIDTH - 1;
            if (ry >= WORLD_HEIGHT) ry = WORLD_HEIGHT - 1;

            // world.csv 맵 파일과 대조해서 벽(1) 위에는 스폰을 완벽 차단합니다
            if (g_map[ry][rx] == 1) {
                continue;
            }

            // 몬스터 종류 템플릿 가져오기
            const auto& meta = MonsterTemplates[zone.template_index];

            // NPC 객체 메모리에 데이터 주입
            new_npc->x = rx;
            new_npc->y = ry;
            new_npc->spawn_x = rx;
            new_npc->spawn_y = ry;

            new_npc->name = meta.name;
            new_npc->level = meta.level;
            new_npc->stat.hp = meta.max_hp;
            new_npc->stat.max_hp = meta.max_hp;
            new_npc->stat.damage = meta.damage;
            new_npc->ai_type = meta.ai_type;
            new_npc->move_type = meta.move_type;
			new_npc->gold_reward = meta.gold_reward;

            new_npc->_state = ST_INGAME;
            g_objects[i].store(new_npc);
            g_sectors[ry / SECTOR_SIZE][rx / SECTOR_SIZE].players[i] = 1;

            zone_spawned++;
            spawned_count++;

            if (spawned_count >= NUM_NPCS) break;
        }
    }
    std::cout << "물리 테이블 기준 20만 마리 구역 분할 기획 배치 완료!\n";

    h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
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