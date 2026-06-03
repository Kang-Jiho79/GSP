#include "Session.h"

OVER_EXP::OVER_EXP() {
	_wsabuf.len = BUF_SIZE;
	_wsabuf.buf = _send_buf;
	_comp_type = OP_RECV;
	ZeroMemory(&_over, sizeof(_over));
}

OVER_EXP::OVER_EXP(char* packet) {
	unsigned char packet_size = static_cast<unsigned char>(packet[0]);
	_wsabuf.len = packet_size;
	_wsabuf.buf = _send_buf;
	_comp_type = OP_SEND;
	ZeroMemory(&_over, sizeof(_over));
	memcpy(_send_buf, packet, packet_size);
}

Session::Session(int id = -1, SOCKET sock = 0) : _id(id), _socket(sock), _head(0), _tail(0), _count(0) {
	ZeroMemory(&_recv_over._over, sizeof(_recv_over._over));
	_recv_over._comp_type = OP_RECV;
}

Session::~Session() {
	if (_socket != 0 && _socket != INVALID_SOCKET) {
		closesocket(_socket);
	}
}

void Session::do_recv() {
	DWORD recv_flag = 0;
	memset(&_recv_over._over, 0, sizeof(_recv_over._over));

	if (_count == RING_BUF_SIZE) return;

	WSABUF wsa_bufs[2];
	int bufs_count = 1;

	if (_head > _tail) {
		wsa_bufs[0].len = _head - _tail;
		wsa_bufs[0].buf = _recv_buf + _tail;
	}
	else {
		wsa_bufs[0].len = RING_BUF_SIZE - _tail;
		wsa_bufs[0].buf = _recv_buf + _tail;

		if (_head > 0) {
			wsa_bufs[1].len = _head;
			wsa_bufs[1].buf = _recv_buf; 
			bufs_count = 2;
		}
	}

	WSARecv(_socket, wsa_bufs, bufs_count, 0, &recv_flag, &_recv_over._over, 0);
}

void Session::do_send(void* packet) {
	OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
	WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, 0);
}