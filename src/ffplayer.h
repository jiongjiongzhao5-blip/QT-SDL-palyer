#ifndef FFPLAYER_H
#define FFPLAYER_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audiodevice.h"
#include "av_utils.h"
#include "clock.h"
#include "decoder.h"
#include "frame_queue.h"
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

    void setPaused(bool paused);
    void setVolume(float linear01);
    void setSpeed(float ratio);

    double  positionSeconds() const;

    int64_t durationMs() const;

    MessageQueue& messages() { return msgQ_; }

    PacketQueue& audioQueue() { return audioQ_; }
    PacketQueue& videoQueue() { return videoQ_; }

    struct AudioOutInfo {
        int     targetSampleRate = 0;
        int     targetChannels   = 0;
        int     decodedFrames    = 0;
        int     swrRebuilds      = 0;
        int64_t pcmBytes         = 0;
        int     pcmPeak          = 0;
    };
    AudioOutInfo audioOutInfo() const;

private:
    void readThread();
    void handleSeekRequest();
    void audioDecodeThread();

    int decodeOneAudioFrame();

    int pullAudio(uint8_t* dst, int bytes);

    int openComponent(AVStream* stream, AVMediaType type);

    MessageQueue msgQ_;
    PacketQueue  audioQ_;
    PacketQueue  videoQ_;
    FrameQueue   sampQ_;
    FrameQueue   pictQ_;
    Decoder      audDec_;
    Decoder      vidDec_;
    Clock        audClk_;
    AudioDevice  audioDev_;

    AVFormatContextPtr ic_;
    AVStream* audioSt_ = nullptr;
    AVStream* videoSt_ = nullptr;
    int       audioIdx_ = -1;
    int       videoIdx_ = -1;
    AVRational videoTb_{};
    AVRational frameRate_{};

    std::thread readThr_;
    std::thread refreshThr_;
    std::string url_;
    std::atomic<bool>    abort_{true};
    std::atomic<bool>    paused_{false};
    std::atomic<bool>    eof_{false};
    std::atomic<int64_t> durationUs_{AV_NOPTS_VALUE};

    std::mutex ctlMtx_;
    bool       seekReq_   = false;
    int64_t    seekPosUs_ = 0;

    SwrContextPtr   swr_;
    AVChannelLayout srcLayout_{};
    AVChannelLayout tgtLayout_{};
    int             srcFreq_ = 0;
    AVSampleFormat  srcFmt_  = AV_SAMPLE_FMT_NONE;
    int             tgtFreq_ = 0;

    std::vector<uint8_t> resampleBuf_;
    const uint8_t*       audioBuf_     = nullptr;
    int                  audioBufSize_  = 0;
    int                  audioBufIndex_ = 0;
    double               audioClock_    = std::nan("");

    mutable std::mutex statMtx_;
    int                statDecodedFrames_ = 0;
    int                statSwrRebuilds_   = 0;
    int64_t            statPcmBytes_      = 0;
    int                statPcmPeak_       = 0;
};

#endif
