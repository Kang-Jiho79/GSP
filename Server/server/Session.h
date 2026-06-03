#pragma once
#include "stdafx.h"

class OVER_EXP {
public:
	WSAOVERLAPPED			_over;
	WSABUF				_wsabuf;
	char					_send_buf[BUF_SIZE];
	COMP_TYPE				_comp_type;
	int					_ai_target_obj;
	
	OVER_EXP();
	OVER_EXP(char* packet);
};

class Session
{
public:
	char _recv_buf[RING_BUF_SIZE];

	int _head;
	int _tail;
	int _count;

	OVER_EXP					_recv_over;
	int						_id;
	SOCKET					_socket;

	Session(int id, SOCKET sock);
	~Session();
	void do_recv();
	void do_send(void* packet);
};

