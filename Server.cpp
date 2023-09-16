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

	EpollTcpServer(const std::string& local_ip, uint16_t local_port);

public:
	bool Start();
	bool Stop();
	int32_t SendData(const PacketPtr& data);
	void RegisterOnRecvCallback(CallbackRecv callback);
	void UnRegisterOnRecvCallback();

protected:
	void OnSocketAccept();
	void OnSocketRead(int32_t fd);
	void OnSocketWrite(int32_t fd);
	void EpollLoop();

private:
	constexpr static uint32_t EPOLL_WAIT_TIME = 10;
	constexpr static uint32_t MAX_EPOLL_EVENT = 100;
	std::string localIp_;
	uint16_t localPort_ = 0;
	int32_t listenFd_ = -1;
	int32_t epollFd_ = -1;
	std::shared_ptr<std::thread> reactorThread_ = nullptr;
	bool isShutdown_ = false;
	CallbackRecv recvCallback_ = nullptr;
};

EpollTcpServer::EpollTcpServer(const std::string& localIp, uint16_t localPort)
	: localIp_ { localIp }
	, localPort_ { localPort }
{
}

EpollTcpServer::~EpollTcpServer()
{
	Stop();
}

bool EpollTcpServer::Start()
{
	epollFd_ = epoll_create(1024);
	if (epollFd_ < 0) {
		std::cout << "epoll_create failed!" << std::endl;
		return false;
	}

	listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd_ < 0) {
		std::cout << "create socket " << localIp_ << ":" << localPort_ << " failed!" << std::endl;
		return -1;
	}

	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(localPort_);
	serverAddr.sin_addr.s_addr = inet_addr(localIp_.c_str());

	int ret = ::bind(listenFd_, (struct sockaddr*)&serverAddr, sizeof(struct sockaddr));
	if (ret != 0) {
		std::cout << "bind socket " << localIp_ << ":" << localPort_ << " failed!" << std::endl;
		::close(listenFd_);
		return -1;
	}
	std::cout << "create and bind socket " << localIp_ << ":" << localPort_ << " success!" << std::endl;

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
		std::cout << "fcntl failed!" << std::endl;
		return false;
	}
	ret = fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		std::cout << "fcntl failed!" << std::endl;
		return false;
	}

	ret = ::listen(listenFd_, SOMAXCONN);
	if (ret < 0) {
		std::cout << "listen failed!" << std::endl;
		return false;
	}
	std::cout << "EpollTcpServer Init success!" << std::endl;

	struct epoll_event evt;
	evt.events = EPOLLIN | EPOLLOUT | EPOLLET;
	evt.data.fd = listenFd_;
	fprintf(stdout, "%s fd %d events read %d write %d\n", "add", listenFd_, evt.events & EPOLLIN, evt.events & EPOLLOUT);
	ret = epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &evt);
	if (ret < 0) {
		std::cout << "epoll_ctl failed!" << std::endl;
		return false;
	}

	assert(!reactorThread_);

	reactorThread_ = std::make_shared<std::thread>(&EpollTcpServer::EpollLoop, this);
	if (!reactorThread_) {
		return false;
	}
	reactorThread_->detach();

	return true;
}

bool EpollTcpServer::Stop()
{
	isShutdown_ = true;
	::close(listenFd_);
	::close(epollFd_);
	std::cout << "stop epoll!" << std::endl;
	UnRegisterOnRecvCallback();
	return true;
}

void EpollTcpServer::OnSocketAccept()
{
	while (true) {
		struct sockaddr_in clientAddr;
		socklen_t clientAddrLen = sizeof(clientAddr);

		int clientFd = accept(listenFd_, (struct sockaddr*)&clientAddr, &clientAddrLen);
		if (clientFd == -1) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				std::cout << "accept all coming connections!" << std::endl;
				break;
			} else {
				std::cout << "accept error!" << std::endl;
				continue;
			}
		}
		std::cout << "accpet connection from " << inet_ntoa(clientAddr.sin_addr) << ":" << ntohs(clientAddr.sin_port) << std::endl;

		int flags = fcntl(clientFd, F_GETFL, 0);
		if (flags < 0) {
			std::cout << "fcntl failed!" << std::endl;
			::close(clientFd);
			continue;
		}
		int ret = fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
		if (ret < 0) {
			std::cout << "fcntl failed!" << std::endl;
			::close(clientFd);
			continue;
		}

		struct epoll_event evt;
		evt.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
		evt.data.fd = clientFd;
		fprintf(stdout, "%s fd %d events read %d write %d\n", "add", clientFd, evt.events & EPOLLIN, evt.events & EPOLLOUT);
		ret = epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &evt);
		if (ret < 0) {
			std::cout << "epoll_ctl failed!" << std::endl;
			::close(clientFd);
			continue;
		}
	}
}

void EpollTcpServer::RegisterOnRecvCallback(CallbackRecv callback)
{
	assert(!recvCallback_);
	recvCallback_ = callback;
}

void EpollTcpServer::UnRegisterOnRecvCallback()
{
	assert(recvCallback_);
	recvCallback_ = nullptr;
}

void EpollTcpServer::OnSocketRead(int32_t fd)
{
	char read_buf[4096];
	bzero(read_buf, sizeof(read_buf));
	int n = -1;
	while ((n = ::read(fd, read_buf, sizeof(read_buf))) > 0) {
		std::cout << "fd: " << fd << " recv: " << read_buf << std::endl;
		std::string msg(read_buf, n);
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

void EpollTcpServer::OnSocketWrite(int32_t fd)
{
	// TODO(smaugx) not care for now
	std::cout << "fd: " << fd << " writeable!" << std::endl;
}

int32_t EpollTcpServer::SendData(const PacketPtr& data)
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
		std::cout << "fd: " << data->fd << " write error, close it!" << std::endl;
		return -1;
	}
	std::cout << "fd: " << data->fd << " write size: " << r << " ok!" << std::endl;
	return r;
}

void EpollTcpServer::EpollLoop()
{
	struct epoll_event* alive_events = static_cast<epoll_event*>(calloc(MAX_EPOLL_EVENT, sizeof(epoll_event)));
	if (!alive_events) {
		std::cout << "calloc memory failed for epoll_events!" << std::endl;
		return;
	}
	while (!isShutdown_) {
		int num = epoll_wait(epollFd_, alive_events, MAX_EPOLL_EVENT, EPOLL_WAIT_TIME);

		for (int i = 0; i < num; ++i) {
			int fd = alive_events[i].data.fd;
			int events = alive_events[i].events;

			if ((events & EPOLLERR) || (events & EPOLLHUP)) {
				std::cout << "epoll_wait error!" << std::endl;
				::close(fd);
			} else if (events & EPOLLRDHUP) {
				std::cout << "fd:" << fd << " closed EPOLLRDHUP!" << std::endl;
				::close(fd);
			} else if (events & EPOLLIN) {
				std::cout << "epollin" << std::endl;
				if (fd == listenFd_) {
					OnSocketAccept();
				} else {
					OnSocketRead(fd);
				}
			} else if (events & EPOLLOUT) {
				std::cout << "epollout" << std::endl;
				OnSocketWrite(fd);
			} else {
				std::cout << "unknow epoll event!" << std::endl;
			}
		}
	}

	free(alive_events);
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
	auto epollServer = std::make_shared<EpollTcpServer>(localIp, localPort);
	if (!epollServer) {
		std::cout << "tcp_server create faield!" << std::endl;
		exit(-1);
	}

	auto recvCall = [&](const PacketPtr& data) -> void {
		epollServer->SendData(data);
		return;
	};

	epollServer->RegisterOnRecvCallback(recvCall);

	if (!epollServer->Start()) {
		std::cout << "tcp_server start failed!" << std::endl;
		exit(1);
	}
	std::cout << "############tcp_server started!################" << std::endl;

	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	epollServer->Stop();

	return 0;
}
