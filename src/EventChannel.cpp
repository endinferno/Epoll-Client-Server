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
