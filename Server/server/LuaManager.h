#pragma once
#include "stdafx.h"

extern "C" {
#include "include\lua.h"
#include "include\lualib.h"
#include "include\lauxlib.h"
}
#pragma comment(lib, "lua54.lib")

class LuaManager {
private:
    lua_State* L = nullptr;

public:
    LuaManager();
    ~LuaManager();

    bool Initialize();
    void RunAI(int monster_id, int target_player_id);

    // 루아 가상머신 포인터 획득 (필요시)
    lua_State* GetState() { return L; }
};

// 서버 전역에서 사용할 루아 매니저 객체 외부 참조 선언
extern LuaManager g_lua_mgr;