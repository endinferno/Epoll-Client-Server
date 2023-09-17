#include "EpollTcpServer.h"
#include "Logger.h"

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
