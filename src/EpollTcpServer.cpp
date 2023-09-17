#include "EpollTcpServer.h"

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

	if (!setSocketReUse(listenFd_)) {
		ERROR("set socket reuse %s:%u failed!", localIp_.c_str(), localPort_);
		::close(listenFd_);
		return false;
	}

	if (!setSocketNonBlock(listenFd_)) {
		::close(listenFd_);
		return false;
	}

	ret = ::listen(listenFd_, SOMAXCONN);
	if (ret < 0) {
		ERROR("listen failed!");
		::close(listenFd_);
		return false;
	}
	INFO("EpollTcpServer Init success!");

	uint32_t event = EPOLLIN | EPOLLET;
	if (!setEpollCtl(acceptEpollFd_, EPOLL_CTL_ADD, listenFd_, event)) {
		ERROR("epoll_ctl failed!");
		::close(listenFd_);
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

		if (!setSocketNonBlock(clientFd)) {
			::close(clientFd);
			continue;
		}

		int32_t clientEpollFd = clientEpollFd_[clientFd % clientReactorThreadNum];
		uint32_t event = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
		if (!setEpollCtl(clientEpollFd, EPOLL_CTL_ADD, clientFd, event)) {
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
	submitWriteEvent(fd, data, size, txMsgOption.value());
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

void EpollTcpServer::submitReadEvent(int clientFd)
{
	int readWorkerIdx = clientFd % clientReadWorkerThreadNum;
	struct WorkerEvent workerEvent;
	workerEvent.type = READ;
	workerEvent.msg.rxMsg = RxMsg {
		.fd = clientFd,
		.payload = rxBuffer_[readWorkerIdx].get(),
		.len = BUFFER_SIZE,
	};
	readEventChannel_[readWorkerIdx]->push(workerEvent);
}

void EpollTcpServer::submitWriteEvent(int clientFd, const void* data, size_t size, struct TxMsg& txMsg)
{
	int writeWorkerIdx = clientFd % clientWriteWorkerThreadNum;
	txMsg.fd = clientFd;
	txMsg.len = size;
	memcpy(txMsg.payload, data, size);
	struct WorkerEvent workerEvent;
	workerEvent.type = WRITE;
	workerEvent.msg.txMsg = txMsg;
	writeEventChannel_[writeWorkerIdx]->push(workerEvent);
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
				DEBUG("fd: %d epoll_wait error EPOLLERR!", fd);
				disconnectClient(fd);
			} else if (event & EPOLLRDHUP) {
				DEBUG("fd: %d closed EPOLLRDHUP!", fd);
				disconnectClient(fd);
			} else if (event & EPOLLIN) {
				DEBUG("epollin fd %d", fd);
				submitReadEvent(fd);
			} else if (event & EPOLLOUT) {
				DEBUG("epollout fd %d", fd);
				setSendBufferFull(fd, false);
			} else {
				ERROR("fd %d unknow epoll event %d!", fd, event);
			}
		}
	}
}

void EpollTcpServer::disconnectClient(int clientFd)
{
	int32_t clientEpollFd = clientEpollFd_[clientFd % clientReactorThreadNum];
	setEpollCtl(clientEpollFd, EPOLL_CTL_DEL, clientFd, 0);
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

void EpollTcpServer::setSendBufferFull(int clientFd, bool isSendBufFull)
{
	isKernelSendBufferFullMapMtx_.lock();
	isKernelSendBufferFullMap_[clientFd] = isSendBufFull;
	isKernelSendBufferFullMapMtx_.unlock();
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
				setSendBufferFull(fd, true);
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
