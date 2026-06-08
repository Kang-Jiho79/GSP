// NPC.h 파일 전면 교체
#pragma once
#include "GameObject.h"
#include "GameData.h"
#include <string>
#include <chrono>
#include <atomic>

// GameObject가 상속하는 기본 스탯 구조(ObjectInfo) 외에 NPC만의 세부 속성 확장
class NPC : public GameObject {
public:
	std::atomic<bool> _active_npc;
	std::chrono::system_clock::time_point npc_last_move_time;

	ObjectInfo stat;

	// 💥 기획서 데이터 테이블 및 리스폰 세팅 연동을 위한 멤버 변수 추가
	std::string name;
	unsigned char level;
	std::string ai_type;
	std::string move_type;

	short spawn_x; // 30초 후 부활을 위한 최초 스폰 위치 영구 보존 격자
	short spawn_y;

	NPC(int id);
};