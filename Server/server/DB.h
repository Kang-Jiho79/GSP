#pragma once
#include "stdafx.h"
#include "GameData.h"
#include "protocol_2026.h"
#include <sqlext.h>
#include <queue>
#include <mutex>
#include <thread>
#include <iostream>

void InitPlayerFromDB(int c_id, std::string name, const struct DB_Task& data);

enum DB_TASK_TYPE { TASK_LOGIN, TASK_SAVE };

#pragma pack(push, 1)

struct DB_Task {
    DB_TASK_TYPE type;
    int          client_id;
    std::string  username;
    short x, y;
    int max_hp, gold;
    unsigned char level;
    unsigned long long exp;
    WEAPON_TYPE weapon;
    unsigned char reinforce_level;
};

#pragma pack(pop)

class DBManager {
private:
    SQLHENV hEnv = SQL_NULL_HENV;
    SQLHDBC hDbc = SQL_NULL_HDBC;

    std::queue<DB_Task> task_queue;
    std::mutex          queue_lock;
    std::thread         db_worker_thread;
    bool                is_running = true;

    void ShowODBCError(SQLSMALLINT handleType, SQLHANDLE handle);

    void UpdateDBLoop();

    void ProcessLoginTask(SQLHSTMT hStmt, const DB_Task& task);

    void ProcessSaveTask(SQLHSTMT hStmt, const DB_Task& task);
public:
    DBManager();
    ~DBManager();

    bool Initialize();

    void PushTask(const DB_Task& task); 
};

