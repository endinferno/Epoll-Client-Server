#include "EpollTcpClient.h"

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
	timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (timerFd_ < 0) {
		ERROR("create_timer failed");
		return false;
	}
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
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(serverPort_);
	serverAddr.sin_addr.s_addr = inet_addr(serverIp_.c_str());

	int ret = ::connect(connFd_, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	if (ret < 0) {
		ERROR("connect failed! ret=%d errno:%d", ret, errno);
		return false;
	}

	if (!setSocketNonBlock(connFd_)) {
		return false;
	}

	INFO("EpollTcpClient Init success!");

	uint32_t event = EPOLLIN | EPOLLET;
	if (!setEpollCtl(epollFd_, EPOLL_CTL_ADD, timerFd_, event)) {
		ERROR("epoll_ctl failed!");
		::close(timerFd_);
		return false;
	}

	event = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
	if (!setEpollCtl(epollFd_, EPOLL_CTL_ADD, connFd_, event)) {
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
	size_t size = txMsg.len;
	const void* data = txMsg.payload;
	int writeBytes = ::write(connFd_, data, size);
	INFO("fd: %d writeable! len %zu writeBytes %d errno %d", txMsg.fd, txMsg.len, writeBytes, errno);
	if (writeBytes == (int)size) {
		txBuffer_.putTxMsg(txMsg);
		return 0;
	} else if (writeBytes == -1) {
		return errno;
	}
	// Send part of data successfully, change the TxMsg to unsend part
	memmove(txMsg.payload, txMsg.payload + writeBytes, size - writeBytes);
	txMsg.len -= writeBytes;
	DEBUG("fd: %d errno: %d write size: %d ok!", connFd_, errno, writeBytes);
	return EAGAIN;
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
				INFO("fd: %d event: %d epoll_wait error!", fd, event);
				::close(fd);
				setTimer(clientReconnectTimeout_);
			} else if (event & EPOLLRDHUP) {
				INFO("fd: %d closed EPOLLRDHUP!", fd);
				::close(fd);
				setTimer(clientReconnectTimeout_);
			} else if (event & EPOLLIN) {
				INFO("EPOLLIN event");
				if (fd == timerFd_) {
					handleTimeout();
				} else {
					struct RxMsg rxMsg;
					rxMsg.fd = fd;
					struct WorkerEvent workerEvent;
					workerEvent.type = READ;
					workerEvent.msg.rxMsg = rxMsg;
					readEventChannel_.push(workerEvent);
				}
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
	while (!isShutdown_) {
		auto workerEventOption = readEventChannel_.pop();
		if (!workerEventOption.has_value()) {
			continue;
		}
		auto workerEvent = workerEventOption.value();
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
	while (!isShutdown_) {
		auto workerEventOption = writeEventChannel_.pop();
		if (!workerEventOption.has_value()) {
			continue;
		}
		auto workerEvent = workerEventOption.value();
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
			int ret = onWriteEvent(workerEvent.msg.txMsg);
			if (ret == 0) {
				// Means write successfully
				continue;
			} else {
				// Push the event back
				// Will send when send buffer available
				// including 2 situations
				// 1. kernel send buffer full
				// 2. server side is lost
				// When in the first situation, we will set kernelSendBufferFull
				// And will never call write function until we get EPOLLOUT
				// When in the second situation, we will set kernelSendBufferFull also
				// And will never call write function until we get EPOLLOUT
				// It is meaningful, because when we connect to server again
				// We will receive EPOLLOUT event, and write can be called again
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

void EpollTcpClient::setAutoConnect(int milliseconds)
{
	clientReconnectTimeout_ = milliseconds;
}

bool EpollTcpClient::reconnectServer()
{
	connFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (connFd_ < 0) {
		ERROR("create socket failed!");
		return false;
	}

	struct sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(serverPort_);
	serverAddr.sin_addr.s_addr = inet_addr(serverIp_.c_str());

	int ret = ::connect(connFd_, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	if (ret < 0) {
		ERROR("connect failed! ret=%d errno:%d", ret, errno);
		::close(connFd_);
		return false;
	}
	if (!setSocketNonBlock(connFd_)) {
		ERROR("set socket nonblock failed");
		::close(connFd_);
		return false;
	}
	uint32_t event = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
	if (!setEpollCtl(epollFd_, EPOLL_CTL_ADD, connFd_, event)) {
		ERROR("epoll_ctl failed!");
		::close(connFd_);
		return false;
	}
	return true;
}

void EpollTcpClient::handleTimeout()
{
	uint64_t timerVal;
	int readBytes = read(timerFd_, &timerVal, sizeof(uint64_t));
	if (readBytes != sizeof(uint64_t)) {
		return;
	}
	if (reconnectServer()) {
		setTimer(0);
	}
}

void EpollTcpClient::setTimer(int milliseconds)
{
	struct itimerspec timerSpec;
	long long sec = milliseconds / 1000;
	long long nsec = (milliseconds - sec * 1000) * 1000000;

	timerSpec.it_value.tv_sec = sec;
	timerSpec.it_value.tv_nsec = nsec;
	timerSpec.it_interval.tv_sec = sec;
	timerSpec.it_interval.tv_nsec = nsec;

	int ret = timerfd_settime(timerFd_, 0, &timerSpec, nullptr);
	if (ret == -1) {
		ERROR("timer_settime failed!");
	}
}
