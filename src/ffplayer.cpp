#include "ffplayer.h"

FFPlayer::FFPlayer() = default;

FFPlayer::~FFPlayer()
{
    close();
}

int FFPlayer::prepare(const std::string& url)
{
    url_    = url;
    abort_  = false;
    eof_    = false;
    durationUs_ = AV_NOPTS_VALUE;

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

    if (readThr_.joinable())
        readThr_.join();

    audioQ_.flush();
    videoQ_.flush();
    ic_.reset();

    audioSt_ = videoSt_ = nullptr;
    audioIdx_ = videoIdx_ = -1;
    durationUs_ = AV_NOPTS_VALUE;
    eof_ = false;
}

void FFPlayer::seekMs(int64_t ms)
{
    std::lock_guard<std::mutex> lock(ctlMtx_);
    seekReq_   = true;
    seekPosUs_ = av_rescale(ms, AV_TIME_BASE, 1000);
}

int64_t FFPlayer::durationMs() const
{
    const int64_t us = durationUs_.load();
    return (us == AV_NOPTS_VALUE || us < 0) ? -1 : us / 1000;
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

    eof_ = false;
}

int FFPlayer::openComponent(AVStream* stream, AVMediaType type)
{
    Decoder* dec = (type == AVMEDIA_TYPE_AUDIO) ? &audDec_ : &vidDec_;

    const int ret = dec->open(stream);
    if (ret < 0)
        return ret;

    if (type == AVMEDIA_TYPE_AUDIO) {
        audioSt_  = stream;
        audioIdx_ = stream->index;
        audioQ_.start();
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
               "有一路解码器打开失败（audio=%d video=%d），将只播放另一路\n",
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
