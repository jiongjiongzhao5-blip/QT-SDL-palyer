#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "av_utils.h"

class PacketQueue
{
public:
    PacketQueue() = default;
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    void start();

    void abort();

    void flush();

    void reset();

    int put(AVPacket* pkt);

    int putFlush();

    int putNullPacket(int streamIndex);

    int get(AVPacket* pkt, bool block, int* serial = nullptr, bool* isFlush = nullptr);

    bool    isAborted() const;
    int     serial() const;
    int     nbPackets() const;
    int64_t duration() const;

private:
    struct Item {
        AVPacket pkt{};
        int  serial = 0;
        bool flush  = false;
    };

    int  pushFlushLocked();
    bool overLimitLocked() const;
    void clearLocked();

    mutable std::mutex      mutex_;
    std::condition_variable cond_;
    std::deque<std::unique_ptr<Item>> queue_;

    bool    abort_request_ = true;
    int     serial_        = 0;
    int     nb_packets_    = 0;
    int64_t duration_      = 0;
    int64_t size_bytes_    = 0;

    static constexpr int64_t kMaxQueueBytes   = 16 * 1024 * 1024;
    static constexpr int     kMaxQueuePackets = 8000;
};

#endif
