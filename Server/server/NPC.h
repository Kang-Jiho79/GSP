#pragma once
#include "GameObject.h"
#include "GameData.h"
#include <string>
#include <chrono>
#include <atomic>

class NPC : public GameObject {
public:
	std::atomic<bool> _active_npc;
	std::chrono::system_clock::time_point npc_last_move_time;

	ObjectInfo stat;

	std::string name;
	unsigned char level;
	std::string ai_type;
	std::string move_type;

	short spawn_x; 
	short spawn_y;

	int gold_reward; 

	int	visual_id = 0;             
	bool  is_boss = false;          
	bool  is_casting_skill = false;  
	DWORD last_skill_time = 0;       
	NPC(int id);
};