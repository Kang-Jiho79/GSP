#include "DB.h"

void DBManager::ShowODBCError(SQLSMALLINT handleType, SQLHANDLE handle) {
    WCHAR sqlState[6], errorMsg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError;
    SQLSMALLINT msgLen;
    if (SQLGetDiagRec(handleType, handle, 1, sqlState, &nativeError, errorMsg, sizeof(errorMsg), &msgLen) == SQL_SUCCESS) {
        std::wcout << "[DB 에러] State: " << sqlState << ", Msg: " << errorMsg << std::endl;
    }
}

DBManager::DBManager() {}
DBManager::~DBManager() {
    is_running = false;
    if (db_worker_thread.joinable()) db_worker_thread.join();
    if (hDbc != SQL_NULL_HDBC) { SQLDisconnect(hDbc); SQLFreeHandle(SQL_HANDLE_DBC, hDbc); }
    if (hEnv != SQL_NULL_HENV) { SQLFreeHandle(SQL_HANDLE_ENV, hEnv); }
}

bool DBManager::Initialize() {
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv) != SQL_SUCCESS) return false;
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc) != SQL_SUCCESS) return false;

    SQLRETURN ret = SQLConnectA(hDbc, (SQLCHAR*)"2022182002_gsp", SQL_NTS, NULL, 0, NULL, 0);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        ShowODBCError(SQL_HANDLE_DBC, hDbc);
        return false;
    }

    std::cout << "Database (2022182002_gsp) 비동기 엔진 연동 성공!" << std::endl;

    db_worker_thread = std::thread(&DBManager::UpdateDBLoop, this);
    return true;
}

void DBManager::PushTask(const DB_Task& task) {
    std::lock_guard<std::mutex> lock(queue_lock);
    task_queue.push(task);
}

void DBManager::UpdateDBLoop() {
    SQLHSTMT hStmt = SQL_NULL_HSTMT;
    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);

    while (is_running) {
        DB_Task task;
        bool has_task = false;

        {
            std::lock_guard<std::mutex> lock(queue_lock);
            if (!task_queue.empty()) {
                task = task_queue.front();
                task_queue.pop();
                has_task = true;
            }
        }

        if (!has_task) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (task.type == TASK_LOGIN) {
            ProcessLoginTask(hStmt, task);
        }
        else if (task.type == TASK_SAVE) {
            ProcessSaveTask(hStmt, task);
        }

        SQLCloseCursor(hStmt);
    }

    if (hStmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
}

void DBManager::ProcessLoginTask(SQLHSTMT hStmt, const DB_Task& task) {
    char query[512];
    
    // 1. 실제 DB 항목명과 순서 동기화
    sprintf_s(query, "SELECT [user_name], [x], [y], [Level], [Exp], [Max_Hp], [Gold], [WeaponType], [ReinforceLevel] FROM user_data WHERE [user_name]='%s'", task.username.c_str());

    SQLLEN cbLen;
    DB_Task db_res;

    if (SQLExecDirectA(hStmt, (SQLCHAR*)query, SQL_NTS) == SQL_SUCCESS) {
        if (SQLFetch(hStmt) == SQL_SUCCESS) {
            
            // 임시 변수 대입용 정수 버퍼 선언 (메모리 침범 및 오버플로우 방지용)
            int temp_level = 0;
            int temp_weapon = 0;
            int temp_reinforce = 0;

            // ⭐ [핵심 수정] DB가 nchar(문자열)이라도 SQL_C_LONG을 지정하면 ODBC가 알아서 숫자로 파싱해줍니다!
            // 1번: [user_name] -> 조회 조건이므로 스킵
            SQLGetData(hStmt, 2, SQL_C_SHORT,    &db_res.x, 0, &cbLen);         // 2번: [x] (smallint -> short)
            SQLGetData(hStmt, 3, SQL_C_SHORT,    &db_res.y, 0, &cbLen);         // 3번: [y] (smallint -> short)
            SQLGetData(hStmt, 4, SQL_C_LONG,     &temp_level, 0, &cbLen);       // 4번: [Level] (nchar -> int로 안전하게 변환)
            SQLGetData(hStmt, 5, SQL_C_UBIGINT,  &db_res.exp, 0, &cbLen);       // 5번: [Exp] (int -> unsigned long long)
            SQLGetData(hStmt, 6, SQL_C_LONG,     &db_res.max_hp, 0, &cbLen);    // 6번: [Max_Hp] (int -> int)
            SQLGetData(hStmt, 7, SQL_C_LONG,     &db_res.gold, 0, &cbLen);      // 7번: [Gold] (int -> int)
            SQLGetData(hStmt, 8, SQL_C_LONG,     &temp_weapon, 0, &cbLen);      // 8번: [WeaponType] (nchar -> int로 안전하게 변환)
            SQLGetData(hStmt, 9, SQL_C_LONG,     &temp_reinforce, 0, &cbLen);   // 9번: [ReinforceLevel] (nchar -> int로 안전하게 변환)

            cout << "DB에서 로그인 정보 조회 성공! (user: " << task.username << ", x: " << db_res.x << ", y: " << db_res.y 
                 << ", level: " << temp_level << ", exp: " << db_res.exp << ", max_hp: " << db_res.max_hp 
                 << ", gold: " << db_res.gold << ", weapon: " << temp_weapon << ", reinforce: " << temp_reinforce 
				<< ")" << endl;

            // 안전하게 받아온 int 데이터를 원래 구조체 타입에 맞게 안전하게 캐스팅 대입
            db_res.level = static_cast<unsigned char>(temp_level);
            db_res.weapon = static_cast<WEAPON_TYPE>(temp_weapon);
            db_res.reinforce_level = static_cast<unsigned char>(temp_reinforce);

            InitPlayerFromDB(task.client_id, task.username, db_res);
            return;
        }
    }
    else {
        ShowODBCError(SQL_HANDLE_STMT, hStmt);
    }

    // [신규 가입 및 세이브 로직은 기존과 동일]
    SQLCloseCursor(hStmt);
    sprintf_s(query, "INSERT INTO user_data ([user_name], [x], [y], [Level], [Exp], [Max_Hp], [Gold], [WeaponType], [ReinforceLevel]) VALUES ('%s', 1000, 1000, '1', 0, 100, 0, '0', '0')", task.username.c_str());
    
    if (SQLExecDirectA(hStmt, (SQLCHAR*)query, SQL_NTS) != SQL_SUCCESS) {
        ShowODBCError(SQL_HANDLE_STMT, hStmt);
    }

    db_res.level = 1; db_res.exp = 0; db_res.max_hp = 100; db_res.gold = 0;
    db_res.weapon = null; db_res.reinforce_level = 0; db_res.x = 1000; db_res.y = 1000;

    InitPlayerFromDB(task.client_id, task.username, db_res);
}

void DBManager::ProcessSaveTask(SQLHSTMT hStmt, const DB_Task& task) {
    char query[512];

    sprintf_s(query, "UPDATE user_data SET [x]=%d, [y]=%d, [Level]='%d', [Exp]=%llu, [Max_Hp]=%d, [Gold]=%d, [WeaponType]='%d', [ReinforceLevel]='%d' WHERE [user_name]='%s'",
        task.x, task.y, task.level, task.exp, task.max_hp, task.gold, (int)task.weapon, task.reinforce_level, task.username.c_str());

    if (SQLExecDirectA(hStmt, (SQLCHAR*)query, SQL_NTS) != SQL_SUCCESS) {
        ShowODBCError(SQL_HANDLE_STMT, hStmt);
    }
}