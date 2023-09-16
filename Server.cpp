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
#include <string>
#include <thread>

#include "Logger.h"

typedef struct Packet {
public:
	Packet()
		: msg { "" }
	{
	}
	Packet(const std::string& msg)
		: msg { msg }
	{
	}
	Packet(int fd, const std::string& msg)
		: fd(fd)
		, msg(msg)
	{
	}

	int fd { -1 };
	std::string msg;
} Packet;

typedef std::shared_ptr<Packet> PacketPtr;

using CallbackRecv = std::function<void(const PacketPtr& data)>;

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
	int32_t sendData(const PacketPtr& data);
	void registerOnRecvCallback(CallbackRecv callback);
	void unRegisterOnRecvCallback();

protected:
	void onAcceptEvent();
	void onSocketRead(int32_t fd);
	void onSocketWrite(int32_t fd);
	void acceptReactorThreadFn();
	void clientReactorThreadFn(int handleClient);
	void clientWorkerThreadFn();

private:
	constexpr static uint32_t EPOLL_WAIT_TIME = 10;
	constexpr static uint32_t MAX_EPOLL_EVENT = 100;
	constexpr static int MAX_EPOLL_ADD_FD_NUM = 1024;
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
	std::vector<std::unique_ptr<std::thread>> clientReactorThread_;
	std::vector<std::unique_ptr<std::thread>> clientWorkerThread_;
	std::vector<int32_t> clientEpollFd_;
};

EpollTcpServer::EpollTcpServer(const std::string& localIp, uint16_t localPort)
	: localIp_ { localIp }
	, localPort_ { localPort }
{
	for (int i = 0; i < clientReactorThreadNum; i++) {
		int32_t epollFd = epoll_create(MAX_EPOLL_ADD_FD_NUM);
		clientEpollFd_.emplace_back(epollFd);
	}
	for (int i = 0; i < clientReactorThreadNum; i++) {
		auto clientReactorThread = std::make_unique<std::thread>(&EpollTcpServer::clientReactorThreadFn, this, i);
		clientReactorThread_.emplace_back(std::move(clientReactorThread));
	}
	for (int i = 0; i < clientWorkerThreadNum; i++) {
		auto clientWorkerThread = std::make_unique<std::thread>(&EpollTcpServer::clientWorkerThreadFn, this);
		clientWorkerThread_.emplace_back(std::move(clientWorkerThread));
	}
	for (int i = 0; i < clientReactorThreadNum; i++) {
		clientReactorThread_[i]->detach();
	}
	for (int i = 0; i < clientWorkerThreadNum; i++) {
		clientWorkerThread_[i]->detach();
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
	DEBUG("%s fd %d events read %d write %d", "add", listenFd_, !!(evt.events & EPOLLIN), !!(evt.events & EPOLLOUT));
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
		DEBUG("%s fd %d events read %d write %d", "add", clientFd, !!(evt.events & EPOLLIN), !!(evt.events & EPOLLOUT));
		ret = epoll_ctl(clientEpollFd, EPOLL_CTL_ADD, clientFd, &evt);
		if (ret < 0) {
			ERROR("epoll_ctl failed!");
			::close(clientFd);
			continue;
		}
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

void EpollTcpServer::onSocketRead(int32_t fd)
{
	char readBuf[4096];
	bzero(readBuf, sizeof(readBuf));
	int n = -1;
	while ((n = ::read(fd, readBuf, sizeof(readBuf))) > 0) {
		INFO("fd: %d recv: %s", fd, readBuf);
		std::string msg(readBuf, n);
		PacketPtr data = std::make_shared<Packet>(fd, msg);
		if (recvCallback_) {
			recvCallback_(data);
		}
	}
	if (n == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}
		::close(fd);
		return;
	}
	if (n == 0) {
		::close(fd);
		return;
	}
}

void EpollTcpServer::onSocketWrite(int32_t fd)
{
	// TODO(smaugx) not care for now
	INFO("fd: %d writeable!", fd);
}

int32_t EpollTcpServer::sendData(const PacketPtr& data)
{
	if (data->fd == -1) {
		return -1;
	}
	int r = ::write(data->fd, data->msg.data(), data->msg.size());
	if (r == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return -1;
		}
		::close(data->fd);
		ERROR("fd: %d write error, close it!", data->fd);
		return -1;
	}
	INFO("fd: %d write size: %d ok!", data->fd, r);
	return r;
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
				ERROR("epoll_wait error!");
				::close(fd);
			} else if (event & EPOLLRDHUP) {
				ERROR("fd: %d closed EPOLLRDHUP!", fd);
				::close(fd);
			} else if (event & EPOLLIN) {
				DEBUG("epollin");
				onSocketRead(fd);
			} else if (event & EPOLLOUT) {
				DEBUG("epollout");
				onSocketWrite(fd);
			} else {
				ERROR("fd %d unknow epoll event %d!", fd, event);
			}
		}
	}
}

void EpollTcpServer::clientWorkerThreadFn()
{
	while (true) {
		;
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

	auto recvCall = [&](const PacketPtr& data) -> void {
		epollServer->sendData(data);
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
