#include "ffplayer.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr int kVideoQueueSize  = 3;
constexpr int kSampleQueueSize = 9;
}

FFPlayer::FFPlayer()
    : sampQ_(kSampleQueueSize)
    , pictQ_(kVideoQueueSize)
{
}

FFPlayer::~FFPlayer()
{
    close();
}

int FFPlayer::prepare(const std::string& url)
{
    url_    = url;
    abort_  = false;
    paused_ = false;
    eof_    = false;
    durationUs_ = AV_NOPTS_VALUE;

    sampQ_.setPacketQueue(&audioQ_);
    pictQ_.setPacketQueue(&videoQ_);

    audClk_.init();
    msgQ_.start();

    readThr_ = std::thread(&FFPlayer::readThread, this);

    return 0;
}

void FFPlayer::close()
{
    abort_ = true;

    msgQ_.abort();
    audioQ_.abort();
    videoQ_.abort();
    sampQ_.signal();
    pictQ_.signal();

    audioDev_.close();

    if (readThr_.joinable())    readThr_.join();
    if (refreshThr_.joinable()) refreshThr_.join();

    audDec_.abort(sampQ_);
    vidDec_.abort(pictQ_);

    audioQ_.flush();
    videoQ_.flush();
    sampQ_.flush();
    pictQ_.flush();

    ic_.reset();
    swr_.reset();
    av_channel_layout_uninit(&srcLayout_);
    av_channel_layout_uninit(&tgtLayout_);

    audioSt_ = videoSt_ = nullptr;
    audioIdx_ = videoIdx_ = -1;
    audioBuf_ = nullptr;
    audioBufSize_ = audioBufIndex_ = 0;
    audioClock_ = std::nan("");
    resampleBuf_.clear();
    eof_ = false;
}

void FFPlayer::seekMs(int64_t ms)
{
    std::lock_guard<std::mutex> lock(ctlMtx_);
    seekReq_   = true;
    seekPosUs_ = av_rescale(ms, AV_TIME_BASE, 1000);
}

void FFPlayer::setPaused(bool paused)
{
    paused_ = paused;
    audioDev_.setPaused(paused);

    audClk_.setPaused(paused);
}

void FFPlayer::setVolume(float linear01) { audioDev_.setVolume(linear01); }

void FFPlayer::setSpeed(float ratio) { audioDev_.setSpeed(ratio); }

double FFPlayer::positionSeconds() const
{
    const double c = audClk_.get();
    return std::isnan(c) ? 0.0 : std::max(0.0, c);
}

int64_t FFPlayer::durationMs() const
{
    const int64_t us = durationUs_.load();
    return (us == AV_NOPTS_VALUE || us < 0) ? -1 : us / 1000;
}

FFPlayer::AudioOutInfo FFPlayer::audioOutInfo() const
{
    std::lock_guard<std::mutex> lock(statMtx_);
    AudioOutInfo info;
    info.targetSampleRate = audioDev_.sampleRate();
    info.targetChannels   = audioDev_.channels();
    info.decodedFrames    = statDecodedFrames_;
    info.swrRebuilds      = statSwrRebuilds_;
    info.pcmBytes         = statPcmBytes_;
    info.pcmPeak          = statPcmPeak_;
    return info;
}

void FFPlayer::handleSeekRequest()
{
    {
        std::lock_guard<std::mutex> lock(ctlMtx_);
        if (!seekReq_)
            return;
        seekReq_ = false;
    }

    const int64_t target = seekPosUs_;
    if (av_seek_frame(ic_.get(), -1, target, AVSEEK_FLAG_BACKWARD) < 0)
        return;

    audioQ_.reset();
    videoQ_.reset();
    sampQ_.flush();
    pictQ_.flush();
    eof_ = false;
}

int FFPlayer::openComponent(AVStream* stream, AVMediaType type)
{
    Decoder* dec = (type == AVMEDIA_TYPE_AUDIO) ? &audDec_ : &vidDec_;

    const int ret = dec->open(stream);
    if (ret < 0)
        return ret;

    if (type == AVMEDIA_TYPE_AUDIO) {
        AVCodecContext* c = audDec_.ctx();

        tgtFreq_ = c->sample_rate;
        av_channel_layout_uninit(&tgtLayout_);
        av_channel_layout_copy(&tgtLayout_, &c->ch_layout);

        if (!audioDev_.open(tgtFreq_, tgtLayout_.nb_channels,
                            [this](uint8_t* d, int b) { return pullAudio(d, b); }))
            return -1;

        audioSt_  = stream;
        audioIdx_ = stream->index;
        audioQ_.start();

        audDec_.start(audioQ_, [this] { audioDecodeThread(); });

    } else {
        videoSt_   = stream;
        videoIdx_  = stream->index;
        videoTb_   = stream->time_base;
        frameRate_ = av_guess_frame_rate(ic_.get(), stream, nullptr);
        videoQ_.start();
    }
    return 0;
}

void FFPlayer::readThread()
{
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, url_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "open input failed: %s\n",
               av_err_string(ret).c_str());
        msgQ_.post(FFMsg::Error, ret);
        return;
    }
    ic_.reset(raw);
    msgQ_.post(FFMsg::OpenInput);

    ret = avformat_find_stream_info(ic_.get(), nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_WARNING, "find_stream_info: %s\n",
               av_err_string(ret).c_str());
        msgQ_.post(FFMsg::Error, ret);
        return;
    }
    msgQ_.post(FFMsg::FindStreamInfo);

    if (ic_->duration != AV_NOPTS_VALUE)
        durationUs_.store(ic_->duration);

    const int videoIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_VIDEO,
                                             -1, -1, nullptr, 0);
    const int audioIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_AUDIO,
                                             -1, videoIdx, nullptr, 0);

    int audioOpenErr = 0, videoOpenErr = 0;
    if (audioIdx >= 0)
        audioOpenErr = openComponent(ic_->streams[audioIdx], AVMEDIA_TYPE_AUDIO);
    if (videoIdx >= 0)
        videoOpenErr = openComponent(ic_->streams[videoIdx], AVMEDIA_TYPE_VIDEO);
    msgQ_.post(FFMsg::ComponentOpen);

    if (audioIdx < 0 && videoIdx < 0) {
        msgQ_.post(FFMsg::Error, AVERROR_STREAM_NOT_FOUND);
        return;
    }
    const bool audioUsable = (audioIdx >= 0) && (audioOpenErr == 0);
    const bool videoUsable = (videoIdx >= 0) && (videoOpenErr == 0);
    if (!audioUsable && !videoUsable) {
        msgQ_.post(FFMsg::Error, audioOpenErr != 0 ? audioOpenErr : videoOpenErr);
        return;
    }
    if (audioOpenErr != 0 || videoOpenErr != 0) {
        av_log(nullptr, AV_LOG_WARNING,
               "有一路打不开（audio=%d video=%d），将只播放另一路\n",
               audioOpenErr, videoOpenErr);
    }
    msgQ_.post(FFMsg::Prepared);

    AVPacketPtr pkt = make_packet();
    while (!abort_.load()) {
        handleSeekRequest();

        ret = av_read_frame(ic_.get(), pkt.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic_->pb)) {
                if (!eof_.exchange(true)) {
                    if (audioIdx_ >= 0) audioQ_.putNullPacket(audioIdx_);
                    if (videoIdx_ >= 0) videoQ_.putNullPacket(videoIdx_);
                    msgQ_.post(FFMsg::Completed);
                }
            }
            if (ic_->pb && ic_->pb->error)
                break;
            av_usleep(10 * 1000);
            continue;
        }
        eof_ = false;

        if (pkt->stream_index == audioIdx_)
            audioQ_.put(pkt.get());
        else if (pkt->stream_index == videoIdx_)
            videoQ_.put(pkt.get());
        else
            av_packet_unref(pkt.get());
    }
}

void FFPlayer::audioDecodeThread()
{
    AVFramePtr frame = make_frame();

    while (!abort_.load()) {
        const int got = audDec_.decodeFrame(frame.get());
        if (got < 0) break;
        if (!got)  continue;

        Frame* slot = sampQ_.peekWritable();
        if (!slot) break;

        const AVRational tb{1, frame->sample_rate};
        slot->pts = (frame->pts == AV_NOPTS_VALUE)
                        ? std::nan("")
                        : frame->pts * av_q2d(tb);

        slot->duration = static_cast<double>(frame->nb_samples) / frame->sample_rate;
        slot->format   = frame->format;

        av_frame_move_ref(slot->frame.get(), frame.get());
        sampQ_.push();

        std::lock_guard<std::mutex> lock(statMtx_);
        ++statDecodedFrames_;
    }
}

int FFPlayer::decodeOneAudioFrame()
{
    if (eof_.load() && sampQ_.nbRemaining() == 0)
        return -1;

    Frame* af = sampQ_.peekReadable();
    if (!af)
        return -1;

    AVFrame* f = af->frame.get();
    const auto inFmt = static_cast<AVSampleFormat>(f->format);

    if (!swr_ || inFmt != srcFmt_ || f->sample_rate != srcFreq_ ||
        av_channel_layout_compare(&f->ch_layout, &srcLayout_) != 0) {

        swr_.reset();
        SwrContext* rawSwr = nullptr;

        const int r = swr_alloc_set_opts2(&rawSwr,
                                          &tgtLayout_, AV_SAMPLE_FMT_S16, tgtFreq_,
                                          &f->ch_layout, inFmt, f->sample_rate,
                                          0, nullptr);
        if (r < 0 || !rawSwr || swr_init(rawSwr) < 0) {
            av_log(nullptr, AV_LOG_ERROR, "swr init failed: %s\n",
                   av_err_string(r).c_str());
            if (rawSwr) swr_free(&rawSwr);
            sampQ_.next();
            return -1;
        }
        swr_.reset(rawSwr);
        av_channel_layout_uninit(&srcLayout_);
        av_channel_layout_copy(&srcLayout_, &f->ch_layout);
        srcFreq_ = f->sample_rate;
        srcFmt_  = inFmt;

        std::lock_guard<std::mutex> lock(statMtx_);
        ++statSwrRebuilds_;
    }

    const int outCount = static_cast<int>(av_rescale_rnd(
        f->nb_samples, tgtFreq_, f->sample_rate, AV_ROUND_UP)) + 256;

    const int outBytes = av_samples_get_buffer_size(
        nullptr, tgtLayout_.nb_channels, outCount, AV_SAMPLE_FMT_S16, 1);
    if (outBytes <= 0) { sampQ_.next(); return -1; }

    resampleBuf_.resize(outBytes);
    uint8_t* outPlanes[1] = { resampleBuf_.data() };

    const int converted = swr_convert(swr_.get(), outPlanes, outCount,
                                      const_cast<const uint8_t**>(f->extended_data),
                                      f->nb_samples);
    if (converted < 0) { sampQ_.next(); return -1; }

    const int dataBytes = converted * tgtLayout_.nb_channels
                          * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

    audioBuf_     = resampleBuf_.data();
    audioBufSize_ = dataBytes;
    audioClock_   = af->pts + af->duration;

    sampQ_.next();
    return dataBytes;
}

int FFPlayer::pullAudio(uint8_t* dst, int bytes)
{
    int copied = 0;

    while (copied < bytes) {
        if (audioBufIndex_ >= audioBufSize_) {
            if (decodeOneAudioFrame() < 0)
                break;
            audioBufIndex_ = 0;
        }

        const int avail = audioBufSize_ - audioBufIndex_;
        const int chunk = std::min(avail, bytes - copied);
        if (audioBuf_)
            std::memcpy(dst + copied, audioBuf_ + audioBufIndex_, chunk);
        else
            std::memset(dst + copied, 0, chunk);
        audioBufIndex_ += chunk;
        copied += chunk;
    }

    if (!std::isnan(audioClock_) && audioDev_.bytesPerSecond() > 0) {
        const double remain = static_cast<double>(audioBufSize_ - audioBufIndex_)
                              / audioDev_.bytesPerSecond();
        audClk_.set(audioClock_ - remain);
    }

    if (copied == 0)
        return -1;

    {
        const auto* s = reinterpret_cast<const int16_t*>(dst);
        const int n  = copied / 2;
        int peak = statPcmPeak_;
        for (int i = 0; i < n; ++i) {
            const int v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak) peak = v;
        }
        std::lock_guard<std::mutex> lock(statMtx_);
        statPcmBytes_ += copied;
        statPcmPeak_   = peak;
    }

    return copied;
}
