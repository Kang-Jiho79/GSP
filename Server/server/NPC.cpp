#include "NPC.h"

NPC::NPC(int id) : GameObject(id, ObjType::NPC) {
	_active_npc = false;
}
