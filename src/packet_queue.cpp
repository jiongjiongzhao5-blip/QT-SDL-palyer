#include "packet_queue.h"

PacketQueue::~PacketQueue()
{
    flush();
}

bool PacketQueue::overLimitLocked() const
{
    return size_bytes_ > kMaxQueueBytes || nb_packets_ > kMaxQueuePackets;
}

void PacketQueue::clearLocked()
{
    while (!queue_.empty()) {
        auto& front = queue_.front();
        if (!front->flush)
            av_packet_unref(&front->pkt);
        queue_.pop_front();
    }
    nb_packets_ = 0;
    duration_   = 0;
    size_bytes_ = 0;
}

int PacketQueue::pushFlushLocked()
{
    ++serial_;
    auto item    = std::make_unique<Item>();
    item->serial = serial_;
    item->flush  = true;
    queue_.push_back(std::move(item));
    ++nb_packets_;
    cond_.notify_all();
    return 0;
}

void PacketQueue::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = false;
        pushFlushLocked();
    }
}

void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        cond_.notify_all();
    }
}

void PacketQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    cond_.notify_all();
}

void PacketQueue::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    pushFlushLocked();
}

int PacketQueue::put(AVPacket* pkt)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (abort_request_) {
            av_packet_unref(pkt);
            return -1;
        }

        cond_.wait(lock, [this] {
            return abort_request_ || !overLimitLocked();
        });
        if (abort_request_) {
            av_packet_unref(pkt);
            return -1;
        }

        auto item    = std::make_unique<Item>();
        item->serial = serial_;
        item->flush  = false;
        size_bytes_ += pkt->size;
        duration_   += pkt->duration;
        av_packet_move_ref(&item->pkt, pkt);
        ++nb_packets_;
        queue_.push_back(std::move(item));
    }
    cond_.notify_one();
    return 0;
}

int PacketQueue::putFlush()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (abort_request_)
            return -1;
        pushFlushLocked();
    }
    cond_.notify_one();
    return 0;
}

int PacketQueue::putNullPacket(int streamIndex)
{
    AVPacket pkt{};
    pkt.stream_index = streamIndex;
    return put(&pkt);
}

int PacketQueue::get(AVPacket* pkt, bool block, int* serial, bool* isFlush)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (abort_request_)
            return -1;

        if (!queue_.empty()) {
            auto item = std::move(queue_.front());
            queue_.pop_front();
            --nb_packets_;
            duration_   -= item->pkt.duration;
            size_bytes_ -= item->pkt.size;

            if (serial)  *serial  = item->serial;
            if (isFlush) *isFlush = item->flush;

            if (item->flush) {
                av_packet_unref(pkt);
            } else {
                av_packet_move_ref(pkt, &item->pkt);
            }

            cond_.notify_all();
            return 1;
        }

        if (!block)
            return 0;

        cond_.wait(lock);
    }
}

bool PacketQueue::isAborted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return abort_request_;
}

int PacketQueue::serial() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

int PacketQueue::nbPackets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nb_packets_;
}

int64_t PacketQueue::duration() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_;
}
