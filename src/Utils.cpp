#include "Utils.h"

bool setEpollCtl(int epollFd, int operation, int clientFd, uint32_t event)
{
	struct epoll_event evt;
	evt.events = event;
	evt.data.fd = clientFd;

	DEBUG("%s listen fd %d events read %d write %d",
		(operation == EPOLL_CTL_ADD) ? "add" : ((operation == EPOLL_CTL_MOD) ? "mod" : ((operation == EPOLL_CTL_DEL) ? "del" : "")),
		clientFd, !!(event & EPOLLIN), !!(event & EPOLLOUT));
	int ret = epoll_ctl(epollFd, operation, clientFd, &evt);
	if (ret < 0) {
		ERROR("epoll_ctl failed!");
		return false;
	}
	return true;
}

bool setClientFdNonBlock(int clientFd)
{
	int flags = fcntl(clientFd, F_GETFL, 0);
	if (flags < 0) {
		ERROR("fcntl failed!");
		return false;
	}
	int ret = fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ERROR("fcntl failed!");
		return false;
	}
	return true;
}

bool setSocketReUse(int fd)
{
	int reuse = 1;
	int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse));
	if (ret < 0) {
		return false;
	}
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const void*)&reuse, sizeof(reuse));
	if (ret < 0) {
		return false;
	}
	return true;
}
