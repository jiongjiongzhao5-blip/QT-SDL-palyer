#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <condition_variable>
#include <deque>
#include <mutex>

#include "ffmsg.h"

struct Message
{
    FFMsg what = FFMsg::FLUSH;
    int   arg1 = 0;
    int   arg2 = 0;
};

class MessageQueue
{
public:
    MessageQueue() = default;

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    void start();

    void abort();

    void flush();

    void post(FFMsg what);
    void post(FFMsg what, int arg1);
    void post(FFMsg what, int arg1, int arg2);

    int get(Message& out, bool block);

    void remove(FFMsg what);

private:
    void postPrivate(Message m);

    mutable std::mutex      mutex_;
    std::condition_variable cond_;
    std::deque<Message>     queue_;
    bool abort_request_ = true;
};

#endif
