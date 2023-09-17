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

	uint32_t event = EPOLLIN | EPOLLOUT | EPOLLET;
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
