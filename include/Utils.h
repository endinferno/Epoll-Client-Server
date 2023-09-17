#pragma once

#include "Logger.h"

#include <cstdint>
#include <cstdio>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>

bool setEpollCtl(int epollFd, int operation, int fd, uint32_t event);
bool setSocketNonBlock(int fd);
bool setSocketReUse(int fd);
