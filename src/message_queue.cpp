#include "message_queue.h"

void MessageQueue::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = false;
        queue_.clear();
        queue_.push_back(Message{FFMsg::FLUSH, 0, 0});
        cond_.notify_all();
    }
}

void MessageQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        cond_.notify_all();
    }
}

void MessageQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void MessageQueue::postPrivate(Message m)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (abort_request_)
            return;
        queue_.push_back(m);
        cond_.notify_one();
    }
}

void MessageQueue::post(FFMsg what)
{
    postPrivate(Message{what, 0, 0});
}

void MessageQueue::post(FFMsg what, int arg1)
{
    postPrivate(Message{what, arg1, 0});
}

void MessageQueue::post(FFMsg what, int arg1, int arg2)
{
    postPrivate(Message{what, arg1, arg2});
}

int MessageQueue::get(Message& out, bool block)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (abort_request_)
            return -1;

        if (!queue_.empty()) {
            out = queue_.front();
            queue_.pop_front();
            return 1;
        }

        if (!block)
            return 0;

        cond_.wait(lock);
    }
}

void MessageQueue::remove(FFMsg what)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->what == what)
            it = queue_.erase(it);
        else
            ++it;
    }
}
