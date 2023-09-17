#include "EpollTcpClient.h"
#include "Logger.h"

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
	int cnt = 100;
	while (cnt--) {
		// while (true) {
		// INFO("input:");
		// std::getline(std::cin, msg);
		int ret = tcpClient->sendData(msg.data(), msg.size());
		INFO("sendData ret %d", ret);
	}

	tcpClient->stop();

	while (true)
		;

	return 0;
}
