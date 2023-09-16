#include "RxMsg.h"

RxBuffer::RxBuffer()
{
	rxMemRegion_ = std::make_unique<char[]>(BUFFER_SIZE);
	for (int i = 0; i < FRAME_NUM; i++) {
		struct RxMsg msg;
		msg.fd = 0;
		msg.len = FRAME_SIZE;
		msg.payload = rxMemRegion_.get() + i * FRAME_SIZE;
		rxMemRegionList_.emplace_back(msg);
	}
}

struct RxMsg RxBuffer::getRxMsg()
{
	std::unique_lock<std::mutex> lock(lockMtx);
	rxMemRegionCond_.wait(lock, [this] { return !this->rxMemRegionList_.empty(); });
	struct RxMsg msg = rxMemRegionList_.front();
	rxMemRegionList_.pop_front();
	return msg;
}

void RxBuffer::putRxMsg(struct RxMsg& rxMsg)
{
	std::lock_guard<std::mutex> lock(lockMtx);
	rxMemRegionList_.push_back(rxMsg);
	if (rxMemRegionList_.size() == 1) {
		rxMemRegionCond_.notify_one();
	}
}
