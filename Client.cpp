#include <arpa/inet.h>
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

#include "EventChannel.h"
#include "Logger.h"

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

EpollTcpClient::EpollTcpClient(const std::string& serverIp, uint16_t serverPort)
	: serverIp_ { serverIp }
	, serverPort_ { serverPort }
{
}

EpollTcpClient::~EpollTcpClient()
{
	stop();
}

bool EpollTcpClient::start()
{
	epollFd_ = epoll_create(1024);
	if (epollFd_ < 0) {
		ERROR("epoll_create failed!");
		return false;
	}
	connFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (connFd_ < 0) {
		ERROR("create socket failed!");
		return false;
	}

	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(serverPort_);
	serverAddr.sin_addr.s_addr = inet_addr(serverIp_.c_str());

	int ret = ::connect(connFd_, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	if (ret < 0) {
		ERROR("connect failed! ret=%d errno:%d", ret, errno);
		return false;
	}

	int flags = fcntl(connFd_, F_GETFL, 0);
	if (flags < 0) {
		ERROR("fcntl failed!");
		return false;
	}
	ret = fcntl(connFd_, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ERROR("fcntl failed!");
		return false;
	}

	INFO("EpollTcpClient Init success!");

	struct epoll_event evt;
	memset(&evt, 0, sizeof(evt));
	evt.events = EPOLLIN | EPOLLOUT | EPOLLET;
	evt.data.fd = connFd_;
	INFO("%s fd %d events read %d write %d\n", "add", connFd_, evt.events & EPOLLIN, evt.events & EPOLLOUT);
	ret = epoll_ctl(epollFd_, EPOLL_CTL_ADD, connFd_, &evt);
	if (ret < 0) {
		ERROR("epoll_ctl failed!");
		::close(connFd_);
		return false;
	}

	assert(!reactorThread_);
	assert(!readWorkerThread_);
	assert(!writeWorkerThread_);
	readWorkerThread_ = std::make_unique<std::thread>(&EpollTcpClient::readWorkerThreadFn, this);
	if (!readWorkerThread_) {
		ERROR("Fail to start worker thread");
		return false;
	}
	readWorkerThread_->detach();
	writeWorkerThread_ = std::make_unique<std::thread>(&EpollTcpClient::writeWorkerThreadFn, this);
	if (!writeWorkerThread_) {
		ERROR("Fail to start worker thread");
		return false;
	}
	writeWorkerThread_->detach();

	reactorThread_ = std::make_unique<std::thread>(&EpollTcpClient::eventLoop, this);
	if (!reactorThread_) {
		ERROR("Fail to start reactor thread");
		return false;
	}
	reactorThread_->detach();

	return true;
}

bool EpollTcpClient::stop()
{
	isShutdown_ = true;
	::close(connFd_);
	::close(epollFd_);
	INFO("stop epoll!");
	unRegisterOnRecvCallback();
	return true;
}

void EpollTcpClient::registerOnRecvCallback(CallbackRecv callback)
{
	assert(!recvCallback_);
	recvCallback_ = callback;
}

void EpollTcpClient::unRegisterOnRecvCallback()
{
	assert(recvCallback_);
	recvCallback_ = nullptr;
}

void EpollTcpClient::onReadEvent(struct RxMsg& rxMsg)
{
	int readBytes = -1;
	int fd = rxMsg.fd;
	while ((readBytes = ::read(fd, rxBuffer_, BUFFER_SIZE - 1)) > 0) {
		if (recvCallback_) {
			recvCallback_(rxBuffer_, readBytes);
		}
	}
	if (readBytes == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}
		::close(fd);
		return;
	}
	if (readBytes == 0) {
		::close(fd);
		return;
	}
}

bool EpollTcpClient::onWriteEvent(struct TxMsg& txMsg)
{
	int ret = ::write(connFd_, txMsg.payload, txMsg.len);
	INFO("fd: %d writeable! len %zu ret %d errno %d", txMsg.fd, txMsg.len, ret, errno);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return false;
		}
		::close(connFd_);
		ERROR("fd: %d write error, close it!", connFd_);
		return false;
	} else if (ret == (int)txMsg.len) {
		txBuffer_.putTxMsg(txMsg);
		return true;
	}
	return false;
}

int32_t EpollTcpClient::sendData(const void* data, size_t size)
{
	auto txMsgOption = txBuffer_.getTxMsg();
	if (!txMsgOption.has_value()) {
		return -1;
	}
	auto txMsg = txMsgOption.value();
	txMsg.fd = connFd_;
	txMsg.len = size;
	memcpy(txMsg.payload, data, size);
	struct WorkerEvent workerEvent;
	workerEvent.type = WRITE;
	workerEvent.msg.txMsg = txMsg;
	writeEventChannel_.push(workerEvent);
	return 0;
}

void EpollTcpClient::eventLoop()
{
	while (!isShutdown_) {
		int eventNum = epoll_wait(epollFd_, events, MAX_EPOLL_EVENT, EPOLL_WAIT_TIME);

		for (int i = 0; i < eventNum; ++i) {
			int fd = events[i].data.fd;
			int event = events[i].events;

			if ((event & EPOLLERR) || (event & EPOLLHUP)) {
				INFO("epoll_wait error!");
				::close(fd);
			} else if (event & EPOLLRDHUP) {
				INFO("fd: %d closed EPOLLRDHUP!", fd);
				::close(fd);
			} else if (event & EPOLLIN) {
				INFO("EPOLLIN event");
				struct RxMsg rxMsg;
				rxMsg.fd = fd;
				struct WorkerEvent workerEvent;
				workerEvent.type = READ;
				workerEvent.msg.rxMsg = rxMsg;
				readEventChannel_.push(workerEvent);
			} else if (event & EPOLLOUT) {
				INFO("EPOLLOUT event");
				kernelSendBufferFullMtx_.lock();
				isKernelSendBufferFull_ = false;
				kernelSendBufferFullMtx_.unlock();
			} else {
				ERROR("unknow epoll event!");
			}
		}
	}
}

void EpollTcpClient::readWorkerThreadFn()
{
	while (true) {
		auto workerEvent = readEventChannel_.pop();
		if (workerEvent.type == READ) {
			DEBUG("onReadEvent");
			onReadEvent(workerEvent.msg.rxMsg);
		} else {
			ERROR("Not read event");
			exit(1);
		}
	}
}

void EpollTcpClient::writeWorkerThreadFn()
{
	while (true) {
		auto workerEvent = writeEventChannel_.pop();
		if (workerEvent.type == WRITE) {
			DEBUG("onWriteEvent");
			bool kernelSendBufferFull = false;
			kernelSendBufferFullMtx_.lock_shared();
			kernelSendBufferFull = isKernelSendBufferFull_;
			kernelSendBufferFullMtx_.unlock_shared();
			if (kernelSendBufferFull) {
				writeEventChannel_.push(workerEvent);
				continue;
			}
			if (!onWriteEvent(workerEvent.msg.txMsg)) {
				writeEventChannel_.push(workerEvent);
				kernelSendBufferFullMtx_.lock();
				isKernelSendBufferFull_ = true;
				kernelSendBufferFullMtx_.unlock();
			}
		} else {
			ERROR("Not write event");
			exit(1);
		}
	}
}

int main(int argc, char* argv[])
{
	std::string serverIp { "127.0.0.1" };
	uint16_t serverPort { 6666 };

	auto tcpClient = std::make_shared<EpollTcpClient>(serverIp, serverPort);
	if (!tcpClient) {
		ERROR("tcpClient create faield!");
		exit(-1);
	}

	auto recvCall = [&](const void* data, size_t size) -> void {
		char* str = (char*)data;
		str[size] = '\0';
		INFO("recv: %s", str);
		return;
	};

	tcpClient->registerOnRecvCallback(recvCall);

	if (!tcpClient->start()) {
		ERROR("tcpClient start failed!");
		exit(1);
	}
	INFO("############tcpClient started!################");

	std::string msg('a', 100);
	// int cnt = 27100;
	// while (cnt--) {
	while (true) {
		// INFO("input:");
		// std::getline(std::cin, msg);
		int ret = tcpClient->sendData(msg.data(), msg.size());
		INFO("sendData ret %d", ret);
	}
	while (true)
		;

	tcpClient->stop();

	return 0;
}
