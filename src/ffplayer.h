#ifndef FFPLAYER_H
#define FFPLAYER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "av_utils.h"
#include "decoder.h"
#include "message_queue.h"
#include "packet_queue.h"

class FFPlayer
{
public:
    FFPlayer();
    ~FFPlayer();

    FFPlayer(const FFPlayer&) = delete;
    FFPlayer& operator=(const FFPlayer&) = delete;

    int prepare(const std::string& url);

    void close();

    void seekMs(int64_t ms);

    int64_t durationMs() const;

    MessageQueue& messages() { return msgQ_; }

    PacketQueue& audioQueue() { return audioQ_; }
    PacketQueue& videoQueue() { return videoQ_; }

private:
    void readThread();
    void handleSeekRequest();

    int openComponent(AVStream* stream, AVMediaType type);

    MessageQueue msgQ_;
    PacketQueue  audioQ_;
    PacketQueue  videoQ_;
    Decoder      audDec_;
    Decoder      vidDec_;

    AVFormatContextPtr ic_;
    AVStream* audioSt_ = nullptr;
    AVStream* videoSt_ = nullptr;
    int       audioIdx_ = -1;
    int       videoIdx_ = -1;
    AVRational videoTb_{};
    AVRational frameRate_{};

    std::thread readThr_;
    std::string url_;

    std::atomic<bool>    abort_{true};
    std::atomic<bool>    eof_{false};
    std::atomic<int64_t> durationUs_{AV_NOPTS_VALUE};

    std::mutex ctlMtx_;
    bool       seekReq_   = false;
    int64_t    seekPosUs_ = 0;
};

#endif
