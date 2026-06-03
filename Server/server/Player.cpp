#include "Player.h"

Player::Player(int id) : GameObject(id, ObjType::PLAYER), last_move_time(0), _session(nullptr) {
	_name[0] = '\0';
}
