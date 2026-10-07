#include "decoder.h"

Decoder::~Decoder()
{
    if (thread_.joinable())
        thread_.join();
}

int Decoder::open(AVStream* stream)
{
    avctx_.reset(avcodec_alloc_context3(nullptr));
    if (!avctx_)
        return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(avctx_.get(), stream->codecpar);
    if (ret < 0)
        return ret;

    avctx_->pkt_timebase = stream->time_base;

    const AVCodec* codec = avcodec_find_decoder(avctx_->codec_id);
    if (!codec) {
        av_log(nullptr, AV_LOG_WARNING, "No decoder for %s\n",
               avcodec_get_name(avctx_->codec_id));
        return AVERROR(EINVAL);
    }

    ret = avcodec_open2(avctx_.get(), codec, nullptr);
    if (ret < 0)
        return ret;

    type_ = avctx_->codec_type;
    return 0;
}

void Decoder::start(PacketQueue& queue, std::function<void()> worker)
{
    queue_ = &queue;
    queue_->start();
    thread_ = std::thread(std::move(worker));
}

void Decoder::abort(FrameQueue& frameQueue)
{
    if (queue_)
        queue_->abort();
    frameQueue.signal();
    if (thread_.joinable())
        thread_.join();
    if (queue_)
        queue_->flush();
}

int Decoder::decodeFrame(AVFrame* frame)
{
    AVPacket pkt{};

    for (;;) {
        if (queue_ && queue_->isAborted())
            return -1;

        int ret = avcodec_receive_frame(avctx_.get(), frame);
        if (ret >= 0) {
            if (type_ == AVMEDIA_TYPE_AUDIO) {
                const AVRational tb{1, frame->sample_rate};
                if (frame->pts != AV_NOPTS_VALUE)
                    frame->pts = av_rescale_q(frame->pts, avctx_->pkt_timebase, tb);
            }
            return 1;
        }

        if (ret == AVERROR_EOF) {
            avcodec_flush_buffers(avctx_.get());
            return 0;
        }
        if (ret != AVERROR(EAGAIN)) {
            av_log(avctx_.get(), AV_LOG_ERROR, "receive_frame: %s\n",
                   av_err_string(ret).c_str());
        }

        bool isFlush = false;
        if (queue_->get(&pkt, true, &pktSerial_, &isFlush) < 0)
            return -1;

        if (isFlush) {
            avcodec_flush_buffers(avctx_.get());
            continue;
        }

        ret = avcodec_send_packet(avctx_.get(), &pkt);
        if (ret == AVERROR(EAGAIN)) {
            av_log(avctx_.get(), AV_LOG_ERROR,
                   "send/receive both returned EAGAIN (API violation)\n");
        }

        av_packet_unref(&pkt);
    }
}
