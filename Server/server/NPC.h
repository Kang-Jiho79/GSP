#pragma once
#include "GameObject.h"

class NPC : public GameObject {
public:
	std::atomic<bool> _active_npc;
	system_clock::time_point npc_last_move_time;

	NPC(int id);
};

