#pragma once

#include "RxMsg.h"
#include "TxMsg.h"
#include <condition_variable>
#include <mutex>
#include <queue>

union WokerMsg {
	struct RxMsg rxMsg;
	struct TxMsg txMsg;
};

enum WorkerType {
	READ,
	WRITE,
};

struct WorkerEvent {
	enum WorkerType type;
	union WokerMsg msg;
};

class EventChannel {
public:
	EventChannel() = default;
	struct WorkerEvent pop();
	void push(struct WorkerEvent& workerEvent);

private:
	std::queue<WorkerEvent> eventChannel_;
	std::condition_variable eventCond_;
	std::mutex lockMtx;
};
