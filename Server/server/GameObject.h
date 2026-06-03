#pragma once
#include "stdafx.h"

class GameObject {
public:
	mutex _lock;
	int _id;
	ObjType _type;
	S_STATE _state;
	short x, y;

	mutex _vl_lock;
	unordered_set<int> _view_list;

	GameObject(int id, ObjType type);
	virtual ~GameObject() = default;

	bool is_pc() const { return _type == ObjType::PLAYER; }
	bool is_npc() const { return _type == ObjType::NPC; }
};

