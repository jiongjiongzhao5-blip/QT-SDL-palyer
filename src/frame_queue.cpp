#include "frame_queue.h"
#include "packet_queue.h"

FrameQueue::FrameQueue(int maxSize)
    : maxSize_(maxSize)
{
    queue_.resize(maxSize_);
    for (auto& f : queue_)
        f.frame = make_frame();
}

void FrameQueue::signal()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cond_.notify_all();
}

Frame* FrameQueue::peek()
{
    return &queue_[rindex_ % maxSize_];
}

Frame* FrameQueue::peekNext()
{
    return &queue_[(rindex_ + 1) % maxSize_];
}

Frame* FrameQueue::peekLast()
{
    return &queue_[rindex_];
}

Frame* FrameQueue::peekWritable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] {
            return size_ < maxSize_ || (pktq_ && pktq_->isAborted());
        });
    }
    if (pktq_ && pktq_->isAborted())
        return nullptr;
    return &queue_[windex_];
}

void FrameQueue::push()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++windex_ == maxSize_)
            windex_ = 0;
        ++size_;
        cond_.notify_all();
    }
}

Frame* FrameQueue::peekReadable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] {
            return size_ > 0 || (pktq_ && pktq_->isAborted());
        });
    }
    if (pktq_ && pktq_->isAborted())
        return nullptr;
    return &queue_[rindex_ % maxSize_];
}

void FrameQueue::next()
{
    av_frame_unref(queue_[rindex_].frame.get());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++rindex_ == maxSize_)
            rindex_ = 0;
        --size_;
        cond_.notify_all();
    }
}

int FrameQueue::nbRemaining() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void FrameQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& f : queue_)
        av_frame_unref(f.frame.get());
    rindex_ = windex_ = size_ = 0;
    cond_.notify_all();
}
