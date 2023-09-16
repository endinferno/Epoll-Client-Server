#include "TxMsg.h"
#include "Logger.h"

TxBuffer::TxBuffer()
{
	txMemRegion_ = std::make_unique<char[]>(BUFFER_SIZE);
	for (int i = 0; i < FRAME_NUM; i++) {
		struct TxMsg msg;
		msg.fd = 0;
		msg.len = FRAME_SIZE;
		msg.payload = txMemRegion_.get() + i * FRAME_SIZE;
		txMemRegionList_.push_back(msg);
	}
}

std::optional<struct TxMsg> TxBuffer::getTxMsg()
{
	std::unique_lock<std::mutex> lock(lockMtx);
	if (!txMemRegionCond_.wait_for(lock, txMemGetTimeout_, [this] { return !this->txMemRegionList_.empty(); })) {
		return std::nullopt;
	}
	if (txMemRegionList_.empty()) {
		return std::nullopt;
	}
	struct TxMsg msg = txMemRegionList_.front();
	txMemRegionList_.pop_front();
	return msg;
}

void TxBuffer::putTxMsg(struct TxMsg& txMsg)
{
	std::lock_guard<std::mutex> lock(lockMtx);
	txMemRegionList_.push_back(txMsg);
	if (txMemRegionList_.size() == 1) {
		txMemRegionCond_.notify_one();
	}
}
