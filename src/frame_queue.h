#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <condition_variable>
#include <mutex>
#include <vector>

#include "av_utils.h"

class PacketQueue;

struct Frame
{
    AVFramePtr frame;
    double pts      = 0.0;
    double duration = 0.0;
    int    width    = 0;
    int    height   = 0;
    int    format   = 0;
};

class FrameQueue
{
public:
    explicit FrameQueue(int maxSize);
    ~FrameQueue() = default;

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    void setPacketQueue(PacketQueue* pktq) { pktq_ = pktq; }

    void signal();

    Frame* peek();
    Frame* peekNext();
    Frame* peekLast();

    Frame* peekWritable();
    void   push();

    Frame* peekReadable();
    void   next();

    int  nbRemaining() const;
    void flush();

private:
    std::vector<Frame> queue_;
    int rindex_  = 0;
    int windex_  = 0;
    int size_    = 0;
    int maxSize_ = 0;

    PacketQueue* pktq_ = nullptr;
    mutable std::mutex      mutex_;
    std::condition_variable cond_;
};

#endif
