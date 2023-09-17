#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>

#include "EventChannel.h"
#include "TxMsg.h"
#include "Utils.h"

using CallbackRecv = std::function<void(const struct RxMsg& rxMsg)>;

class EpollTcpServer {
public:
	EpollTcpServer();
	EpollTcpServer(const EpollTcpServer& other) = delete;
	EpollTcpServer& operator=(const EpollTcpServer& other) = delete;
	EpollTcpServer(EpollTcpServer&& other) = delete;
	EpollTcpServer& operator=(EpollTcpServer&& other) = delete;
	~EpollTcpServer();

	EpollTcpServer(const std::string& localIp, uint16_t localPort);

public:
	bool start();
	bool stop();
	int32_t sendData(int fd, const void* data, size_t size);
	void registerOnRecvCallback(CallbackRecv callback);
	void unRegisterOnRecvCallback();

protected:
	void onAcceptEvent();
	void onReadEvent(struct RxMsg& rxMsg);
	int onWriteEvent(struct TxMsg& txMsg);
	void acceptReactorThreadFn();
	void clientReactorThreadFn(int handleClient);
	void clientReadWorkerThreadFn(int handleClient);
	void clientWriteWorkerThreadFn(int handleClient);
	bool isSendBufferFull(int clientFd);
	void setSendBufferFull(int clientFd, bool isSendBufFull);
	void disconnectClient(int clientFd);
	void submitReadEvent(int clientFd);
	void submitWriteEvent(int clientFd, const void* data, size_t size, struct TxMsg& txMsg);

private:
	constexpr static uint32_t EPOLL_WAIT_TIME = 10;
	constexpr static uint32_t MAX_EPOLL_EVENT = 100;
	constexpr static int MAX_EPOLL_ADD_FD_NUM = 1024;
	constexpr static int BUFFER_SIZE = 2048;
	std::string localIp_;
	uint16_t localPort_ = 0;
	int32_t listenFd_ = -1;
	int32_t acceptEpollFd_ = -1;
	std::unique_ptr<std::thread> acceptReactorThread_ = nullptr;
	bool isShutdown_ = false;
	CallbackRecv recvCallback_ = nullptr;
	int threadNum = std::thread::hardware_concurrency();
	int clientReactorThreadNum = threadNum / 4;
	int clientWorkerThreadNum = threadNum - clientReactorThreadNum;
	int clientReadWorkerThreadNum = clientWorkerThreadNum / 2;
	int clientWriteWorkerThreadNum = clientWorkerThreadNum / 2;
	std::vector<std::unique_ptr<std::thread>> clientReactorThread_;
	std::vector<std::unique_ptr<std::thread>> clientReadWorkerThread_;
	std::vector<std::unique_ptr<std::thread>> clientWriteWorkerThread_;
	std::vector<int32_t> clientEpollFd_;
	std::vector<std::unique_ptr<char[]>> rxBuffer_;
	TxBuffer txBuffer_;
	std::vector<std::shared_ptr<EventChannel>> readEventChannel_;
	std::vector<std::shared_ptr<EventChannel>> writeEventChannel_;
	std::unordered_map<int, bool> isKernelSendBufferFullMap_;
	std::shared_mutex isKernelSendBufferFullMapMtx_;
};
