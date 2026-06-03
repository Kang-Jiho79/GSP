#pragma once
#include "GameObject.h"
#include "Session.h"

class Player : public GameObject {
public:
	char				_name[MAX_NAME_LEN];
	int				last_move_time;
	CharacterInfo		stat;
	Session*			_session; 

	Player(int id);

	void get_name(char* buffer) {
		strncpy_s(buffer, MAX_NAME_LEN, _name, MAX_NAME_LEN);
	}
};

