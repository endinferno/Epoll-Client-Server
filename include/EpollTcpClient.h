#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "EventChannel.h"
#include "Logger.h"
#include "Utils.h"

using CallbackRecv = std::function<void(const void* data, size_t size)>;

class EpollTcpClient {
public:
	EpollTcpClient() = default;
	EpollTcpClient(const EpollTcpClient& other) = delete;
	EpollTcpClient& operator=(const EpollTcpClient& other) = delete;
	EpollTcpClient(EpollTcpClient&& other) = delete;
	EpollTcpClient& operator=(EpollTcpClient&& other) = delete;
	~EpollTcpClient();

	EpollTcpClient(const std::string& server_ip, uint16_t server_port);

public:
	bool start();
	bool stop();
	int32_t sendData(const void* data, size_t size);
	void registerOnRecvCallback(CallbackRecv callback);
	void unRegisterOnRecvCallback();

protected:
	void onReadEvent(struct RxMsg& rxMsg);
	bool onWriteEvent(struct TxMsg& txMsg);
	void eventLoop();
	void readWorkerThreadFn();
	void writeWorkerThreadFn();

private:
	constexpr static uint32_t EPOLL_WAIT_TIME = 10;
	constexpr static uint32_t MAX_EPOLL_EVENT = 100;
	constexpr static int BUFFER_SIZE = 2048;
	std::string serverIp_;
	uint16_t serverPort_ = 0;
	int32_t connFd_ = -1;
	int32_t epollFd_ = -1;
	std::unique_ptr<std::thread> reactorThread_ = nullptr;
	std::unique_ptr<std::thread> readWorkerThread_ = nullptr;
	std::unique_ptr<std::thread> writeWorkerThread_ = nullptr;
	bool isShutdown_ = false;
	CallbackRecv recvCallback_ = nullptr;
	struct epoll_event events[MAX_EPOLL_EVENT];
	TxBuffer txBuffer_;
	char rxBuffer_[BUFFER_SIZE];
	EventChannel readEventChannel_;
	EventChannel writeEventChannel_;
	bool isKernelSendBufferFull_ = false;
	std::shared_mutex kernelSendBufferFullMtx_;
};
