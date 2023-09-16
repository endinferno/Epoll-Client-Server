#pragma once

#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>

struct RxMsg {
	int fd;
	char* payload;
	size_t len;
};

class RxBuffer {
public:
	RxBuffer();
	struct RxMsg getRxMsg();
	void putRxMsg(struct RxMsg& rxMsg);

private:
	constexpr static int FRAME_SIZE = 2048;
	constexpr static int FRAME_NUM = 2000;
	constexpr static int BUFFER_SIZE = FRAME_SIZE * FRAME_NUM;
	std::mutex lockMtx;
	std::unique_ptr<char> rxMemRegion_ = nullptr;
	std::list<struct RxMsg> rxMemRegionList_;
	std::condition_variable rxMemRegionCond_;
};
