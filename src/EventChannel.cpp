#include "EventChannel.h"
#include "Logger.h"

struct WorkerEvent EventChannel::pop()
{
	std::unique_lock<std::mutex> lock(lockMtx);
	eventCond_.wait(lock, [this] { return !this->eventChannel_.empty(); });
	auto value = eventChannel_.front();
	eventChannel_.pop();
	return value;
}

void EventChannel::push(struct WorkerEvent& workerEvent)
{
	std::lock_guard<std::mutex> lock(lockMtx);
	eventChannel_.push(workerEvent);
	eventCond_.notify_one();
}

void EventChannel::remove(int clientFd, TxBuffer& txBuffer)
{
	std::lock_guard<std::mutex> lock(lockMtx);
	size_t size = eventChannel_.size();
	while (size--) {
		auto workerEvent = eventChannel_.front();
		eventChannel_.pop();
		if (workerEvent.type == READ && workerEvent.msg.rxMsg.fd == clientFd) {
			continue;
		} else if (workerEvent.type == WRITE && workerEvent.msg.txMsg.fd == clientFd) {
			txBuffer.putTxMsg(workerEvent.msg.txMsg);
			continue;
		}
		eventChannel_.push(workerEvent);
	}
}
