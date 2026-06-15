#pragma once
#include "GameObject.h"
#include "Session.h"
#include "GameData.h"

struct PlayerInfo
{
	WEAPON_TYPE			weapon;
	int					hp;
	int					max_hp;
	int					gold;
	unsigned char			reinforce_level;
	unsigned long long		exp;
	unsigned char			level;
	int party_id = 0;       
	int invited_by = -1;    
};


class Player : public GameObject {
public:
	char				_name[MAX_NAME_LEN];
	int				last_move_time;
	PlayerInfo		stat;
	Session*			_session; 
	bool				is_confused = false;

	Player(int id);

	void get_name(char* buffer) {
		strncpy_s(buffer, MAX_NAME_LEN, _name, MAX_NAME_LEN);
	}
};

