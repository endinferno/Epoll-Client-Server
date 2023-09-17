#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cassert>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "EventChannel.h"
#include "Logger.h"
#include "TxMsg.h"

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
	void disconnectClient(int clientFd);

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

EpollTcpServer::EpollTcpServer(const std::string& localIp, uint16_t localPort)
	: localIp_ { localIp }
	, localPort_ { localPort }
{
	for (int i = 0; i < clientReactorThreadNum; i++) {
		int32_t epollFd = epoll_create(MAX_EPOLL_ADD_FD_NUM);
		clientEpollFd_.emplace_back(epollFd);
	}
	for (int i = 0; i < clientReadWorkerThreadNum; i++) {
		auto rxBuffer = std::make_unique<char[]>(BUFFER_SIZE);
		rxBuffer_.emplace_back(std::move(rxBuffer));
	}
	for (int i = 0; i < clientReadWorkerThreadNum; i++) {
		auto readEventChannel = std::make_shared<EventChannel>();
		readEventChannel_.emplace_back(std::move(readEventChannel));
	}
	for (int i = 0; i < clientWriteWorkerThreadNum; i++) {
		auto writeEventChannel = std::make_shared<EventChannel>();
		writeEventChannel_.emplace_back(std::move(writeEventChannel));
	}
	for (int i = 0; i < clientReactorThreadNum; i++) {
		auto clientReactorThread = std::make_unique<std::thread>(&EpollTcpServer::clientReactorThreadFn, this, i);
		clientReactorThread_.emplace_back(std::move(clientReactorThread));
	}
	for (int i = 0; i < clientReadWorkerThreadNum; i++) {
		auto clientReadWorkerThread = std::make_unique<std::thread>(&EpollTcpServer::clientReadWorkerThreadFn, this, i);
		clientReadWorkerThread_.emplace_back(std::move(clientReadWorkerThread));
	}
	for (int i = 0; i < clientWriteWorkerThreadNum; i++) {
		auto clientWorkerThread = std::make_unique<std::thread>(&EpollTcpServer::clientWriteWorkerThreadFn, this, i);
		clientWriteWorkerThread_.emplace_back(std::move(clientWorkerThread));
	}
	for (int i = 0; i < clientReactorThreadNum; i++) {
		clientReactorThread_[i]->detach();
	}
	for (int i = 0; i < clientReadWorkerThreadNum; i++) {
		clientReadWorkerThread_[i]->detach();
	}
	for (int i = 0; i < clientWriteWorkerThreadNum; i++) {
		clientWriteWorkerThread_[i]->detach();
	}
}

EpollTcpServer::~EpollTcpServer()
{
	stop();
}

bool EpollTcpServer::start()
{
	acceptEpollFd_ = epoll_create(MAX_EPOLL_ADD_FD_NUM);
	if (acceptEpollFd_ < 0) {
		ERROR("epoll_create failed!");
		return false;
	}

	listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd_ < 0) {
		ERROR("create socket %s:%u failed!", localIp_.c_str(), localPort_);
		return false;
	}

	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(localPort_);
	serverAddr.sin_addr.s_addr = inet_addr(localIp_.c_str());

	int ret = ::bind(listenFd_, (struct sockaddr*)&serverAddr, sizeof(struct sockaddr));
	if (ret != 0) {
		ERROR("bind socket %s:%u failed!", localIp_.c_str(), localPort_);
		::close(listenFd_);
		return false;
	}
	INFO("create and bind socket %s:%u success!", localIp_.c_str(), localPort_);

	int reuse = 1;
	ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse));
	if (ret < 0) {
		return false;
	}
	ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, (const void*)&reuse, sizeof(reuse));
	if (ret < 0) {
		return false;
	}

	int flags = fcntl(listenFd_, F_GETFL, 0);
	if (flags < 0) {
		ERROR("fcntl failed!");
		return false;
	}
	ret = fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ERROR("fcntl failed!");
		return false;
	}

	ret = ::listen(listenFd_, SOMAXCONN);
	if (ret < 0) {
		ERROR("listen failed!");
		return false;
	}
	INFO("EpollTcpServer Init success!");

	struct epoll_event evt;
	evt.events = EPOLLIN | EPOLLET;
	evt.data.fd = listenFd_;
	DEBUG("%s listen fd %d events read %d write %d", "add", listenFd_, !!(evt.events & EPOLLIN), !!(evt.events & EPOLLOUT));
	ret = epoll_ctl(acceptEpollFd_, EPOLL_CTL_ADD, listenFd_, &evt);
	if (ret < 0) {
		ERROR("epoll_ctl failed!");
		return false;
	}

	assert(!acceptReactorThread_);

	acceptReactorThread_ = std::make_unique<std::thread>(&EpollTcpServer::acceptReactorThreadFn, this);
	if (!acceptReactorThread_) {
		return false;
	}
	acceptReactorThread_->detach();

	return true;
}

bool EpollTcpServer::stop()
{
	isShutdown_ = true;
	::close(listenFd_);
	::close(acceptEpollFd_);
	for (size_t i = 0; i < clientEpollFd_.size(); i++) {
		::close(clientEpollFd_[i]);
	}
	INFO("stop epoll!");
	unRegisterOnRecvCallback();
	return true;
}

void EpollTcpServer::onAcceptEvent()
{
	while (true) {
		struct sockaddr_in clientAddr;
		socklen_t clientAddrLen = sizeof(clientAddr);

		int clientFd = accept(listenFd_, (struct sockaddr*)&clientAddr, &clientAddrLen);
		if (clientFd == -1) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				INFO("accept all coming connections!");
				break;
			} else {
				ERROR("accept error!");
				continue;
			}
		}
		INFO("accpet connection from %s:%d", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

		int flags = fcntl(clientFd, F_GETFL, 0);
		if (flags < 0) {
			ERROR("fcntl failed!");
			::close(clientFd);
			continue;
		}
		int ret = fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
		if (ret < 0) {
			ERROR("fcntl failed!");
			::close(clientFd);
			continue;
		}

		int32_t clientEpollFd = clientEpollFd_[clientFd % clientReactorThreadNum];
		struct epoll_event evt;
		evt.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
		evt.data.fd = clientFd;
		DEBUG("%s client fd %d events read %d write %d", "add", clientFd, !!(evt.events & EPOLLIN), !!(evt.events & EPOLLOUT));
		ret = epoll_ctl(clientEpollFd, EPOLL_CTL_ADD, clientFd, &evt);
		if (ret < 0) {
			ERROR("epoll_ctl failed!");
			::close(clientFd);
			continue;
		}
		isKernelSendBufferFullMapMtx_.lock();
		isKernelSendBufferFullMap_.insert({ clientFd, false });
		isKernelSendBufferFullMapMtx_.unlock();
	}
}

void EpollTcpServer::registerOnRecvCallback(CallbackRecv callback)
{
	assert(!recvCallback_);
	recvCallback_ = callback;
}

void EpollTcpServer::unRegisterOnRecvCallback()
{
	assert(recvCallback_);
	recvCallback_ = nullptr;
}

void EpollTcpServer::onReadEvent(struct RxMsg& rxMsg)
{
	int fd = rxMsg.fd;
	void* payload = (void*)rxMsg.payload;
	size_t size = rxMsg.len - 1;
	int readBytes = -1;
	while ((readBytes = ::read(fd, payload, size)) > 0) {
		INFO("fd: %d recv: %s", fd, (char*)payload);
		rxMsg.len = readBytes;
		if (recvCallback_) {
			recvCallback_(rxMsg);
		}
	}
	DEBUG("fd: %d readBytes: %d errno: %d", fd, readBytes, errno);
}

int EpollTcpServer::onWriteEvent(struct TxMsg& txMsg)
{
	int fd = txMsg.fd;
	size_t size = txMsg.len;
	const void* data = txMsg.payload;
	INFO("fd: %d writeable!", fd);
	int writeBytes = ::write(fd, data, size);
	if (writeBytes == (int)size) {
		txBuffer_.putTxMsg(txMsg);
		return 0;
	} else if (writeBytes == -1) {
		return errno;
	}
	// Send part of data successfully, change the TxMsg to unsend part
	memmove(txMsg.payload, txMsg.payload + writeBytes, size - writeBytes);
	txMsg.len -= writeBytes;
	DEBUG("fd: %d errno: %d write size: %d ok!", fd, errno, writeBytes);
	return EAGAIN;
}

int32_t EpollTcpServer::sendData(int fd, const void* data, size_t size)
{
	auto txMsgOption = txBuffer_.getTxMsg();
	if (!txMsgOption.has_value()) {
		return -1;
	}
	auto txMsg = txMsgOption.value();
	txMsg.fd = fd;
	txMsg.len = size;
	memcpy(txMsg.payload, data, size);
	struct WorkerEvent workerEvent;
	workerEvent.type = WRITE;
	workerEvent.msg.txMsg = txMsg;
	auto writeEventChannel = writeEventChannel_[fd % clientWriteWorkerThreadNum];
	writeEventChannel->push(workerEvent);
	return 0;
}

void EpollTcpServer::acceptReactorThreadFn()
{
	struct epoll_event events[MAX_EPOLL_EVENT];
	while (!isShutdown_) {
		int eventNum = epoll_wait(acceptEpollFd_, events, MAX_EPOLL_EVENT, EPOLL_WAIT_TIME);

		for (int i = 0; i < eventNum; ++i) {
			int fd = events[i].data.fd;
			int event = events[i].events;

			if (event & EPOLLIN) {
				DEBUG("epollin");
				if (fd == listenFd_) {
					onAcceptEvent();
				}
			} else {
				ERROR("fd %d Event %d should not handle in accept reactor", fd, event);
				exit(-1);
			}
		}
	}
}

void EpollTcpServer::clientReactorThreadFn(int handleClient)
{
	int epollFd = clientEpollFd_[handleClient];
	struct epoll_event events[MAX_EPOLL_EVENT];
	while (true) {
		int eventNum = epoll_wait(epollFd, events, MAX_EPOLL_EVENT, EPOLL_WAIT_TIME);

		for (int i = 0; i < eventNum; ++i) {
			int fd = events[i].data.fd;
			int event = events[i].events;

			if ((event & EPOLLERR) || (event & EPOLLHUP)) {
				// ERROR("epoll_wait error EPOLLERR!");
				DEBUG("epoll_wait error EPOLLERR!");
				disconnectClient(fd);
			} else if (event & EPOLLRDHUP) {
				DEBUG("fd: %d closed EPOLLRDHUP!", fd);
				disconnectClient(fd);
			} else if (event & EPOLLIN) {
				DEBUG("epollin fd %d", fd);
				int readWorkerIdx = fd % clientReadWorkerThreadNum;
				struct RxMsg rxMsg;
				rxMsg.fd = fd;
				rxMsg.payload = rxBuffer_[readWorkerIdx].get();
				rxMsg.len = BUFFER_SIZE;
				struct WorkerEvent workerEvent;
				workerEvent.type = READ;
				workerEvent.msg.rxMsg = rxMsg;
				auto readEventChannel = readEventChannel_[readWorkerIdx];
				readEventChannel->push(workerEvent);
			} else if (event & EPOLLOUT) {
				DEBUG("epollout fd %d", fd);
				isKernelSendBufferFullMapMtx_.lock();
				isKernelSendBufferFullMap_[fd] = false;
				isKernelSendBufferFullMapMtx_.unlock();
			} else {
				ERROR("fd %d unknow epoll event %d!", fd, event);
			}
		}
	}
}

void EpollTcpServer::disconnectClient(int clientFd)
{
	::close(clientFd);
	auto readEventChannel = readEventChannel_[clientFd % clientReadWorkerThreadNum];
	readEventChannel->remove(clientFd, txBuffer_);
	auto writeEventChannel = writeEventChannel_[clientFd % clientReadWorkerThreadNum];
	writeEventChannel->remove(clientFd, txBuffer_);
	isKernelSendBufferFullMapMtx_.lock();
	isKernelSendBufferFullMap_.erase(clientFd);
	isKernelSendBufferFullMapMtx_.unlock();
}

bool EpollTcpServer::isSendBufferFull(int clientFd)
{
	bool sendBufferFull = false;
	isKernelSendBufferFullMapMtx_.lock_shared();
	auto iter = isKernelSendBufferFullMap_.find(clientFd);
	if (iter != isKernelSendBufferFullMap_.end()) {
		sendBufferFull = iter->second;
	}
	isKernelSendBufferFullMapMtx_.unlock_shared();
	return sendBufferFull;
}

void EpollTcpServer::clientReadWorkerThreadFn(int handleClient)
{
	auto readEventChannel = readEventChannel_[handleClient];
	while (true) {
		auto workerEvent = readEventChannel->pop();
		if (workerEvent.type == READ) {
			onReadEvent(workerEvent.msg.rxMsg);
		} else {
			ERROR("Not read event");
			exit(1);
		}
	}
}

void EpollTcpServer::clientWriteWorkerThreadFn(int handleClient)
{
	auto writeEventChannel = writeEventChannel_[handleClient];
	while (true) {
		auto workerEvent = writeEventChannel->pop();
		if (workerEvent.type == WRITE) {
			// Send buffer is full, push the event back
			// Will send when send buffer available
			int fd = workerEvent.msg.txMsg.fd;
			if (isSendBufferFull(fd)) {
				writeEventChannel->push(workerEvent);
				continue;
			}
			int ret = onWriteEvent(workerEvent.msg.txMsg);
			if (ret == 0) {
				// Means write successfully
				continue;
			} else if (ret == EAGAIN || ret == EWOULDBLOCK) {
				// Means kernel send buffer is full
				// Push the event back
				// Will send when send buffer available
				writeEventChannel->push(workerEvent);
				isKernelSendBufferFullMapMtx_.lock();
				isKernelSendBufferFullMap_[fd] = true;
				isKernelSendBufferFullMapMtx_.unlock();
			}
			// If not the above 2 situations
			// Means the client disconnect
			// Will clean all the event belonging to the client
			// Dont need to push the event back
		} else {
			ERROR("Not write event");
			exit(1);
		}
	}
}

int main(int argc, char* argv[])
{
	std::string localIp { "127.0.0.1" };
	uint16_t localPort { 6666 };
	if (argc >= 2) {
		localIp = std::string(argv[1]);
	}
	if (argc >= 3) {
		localPort = std::atoi(argv[2]);
	}
	auto epollServer = std::make_unique<EpollTcpServer>(localIp, localPort);
	if (!epollServer) {
		ERROR("tcp_server create faield!");
		exit(-1);
	}

	auto recvCall = [&](const struct RxMsg& rxMsg) -> void {
		int fd = rxMsg.fd;
		const void* data = (const void*)rxMsg.payload;
		size_t size = rxMsg.len;
		epollServer->sendData(fd, data, size);
		return;
	};

	epollServer->registerOnRecvCallback(recvCall);

	if (!epollServer->start()) {
		ERROR("tcp_server start failed!");
		exit(1);
	}
	INFO("############tcp_server started!################");

	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	epollServer->stop();

	return 0;
}
