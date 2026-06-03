#include "GameObject.h"

GameObject::GameObject(int id, ObjType type) 
	: _id(id), _type(type), _state(ST_FREE), x(0), y(0) 
{
}
