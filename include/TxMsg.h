#pragma once

#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>

struct TxMsg {
	int fd;
	char* payload;
	size_t len;
};

class TxBuffer {
public:
	TxBuffer();
	struct TxMsg getTxMsg();
	void putTxMsg(struct TxMsg& txMsg);

private:
	constexpr static int FRAME_SIZE = 2048;
	constexpr static int FRAME_NUM = 2000;
	constexpr static int BUFFER_SIZE = FRAME_SIZE * FRAME_NUM;
	std::mutex lockMtx;
	std::unique_ptr<char[]> txMemRegion_ = nullptr;
	std::list<struct TxMsg> txMemRegionList_;
	std::condition_variable txMemRegionCond_;
};
