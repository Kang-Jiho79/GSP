#pragma once
#include "stdafx.h"

extern "C" {
#include "include\lua.h"
#include "include\lualib.h"
#include "include\lauxlib.h"
}

#pragma comment(lib, "lua55.lib")

class LuaManager {
private:
    lua_State* L = nullptr;

public:
    LuaManager();
    ~LuaManager();

    bool Initialize();
    void RunAI(int monster_id, int target_player_id);
    lua_State* GetState() { return L; }
};

extern LuaManager g_lua_mgr;