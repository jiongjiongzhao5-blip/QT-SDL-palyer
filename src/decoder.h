#ifndef DECODER_H
#define DECODER_H

#include <functional>
#include <thread>

#include "av_utils.h"
#include "frame_queue.h"
#include "packet_queue.h"

class Decoder
{
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    int open(AVStream* stream);

    void start(PacketQueue& queue, std::function<void()> worker);

    void abort(FrameQueue& frameQueue);

    int decodeFrame(AVFrame* frame);

    AVCodecContext* ctx() const { return avctx_.get(); }
    AVMediaType     type() const { return type_; }
    int             pktSerial() const { return pktSerial_; }

private:
    AVCodecContextPtr avctx_;
    PacketQueue*      queue_ = nullptr;
    AVMediaType       type_  = AVMEDIA_TYPE_UNKNOWN;
    int               pktSerial_ = 0;
    std::thread       thread_;
};

#endif
