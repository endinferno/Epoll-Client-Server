#include "EventChannel.h"
#include "Logger.h"

std::optional<struct WorkerEvent> EventChannel::pop()
{
	std::unique_lock<std::mutex> lock(lockMtx);
	if (!eventCond_.wait_for(lock, eventChannelPopTimeout_, [this] { return !this->eventChannel_.empty(); })) {
		return std::nullopt;
	}
	if (eventChannel_.empty()) {
		return std::nullopt;
	}
	auto workerEvent = eventChannel_.front();
	eventChannel_.pop();
	return workerEvent;
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
