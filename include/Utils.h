#pragma once

#include "Logger.h"

#include <cstdint>
#include <cstdio>

#include <fcntl.h>
#include <sys/epoll.h>

bool setEpollCtl(int epollFd, int operation, int clientFd, uint32_t event);
bool setClientFdNonBlock(int clientFd);
