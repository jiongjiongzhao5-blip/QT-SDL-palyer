#ifndef AV_UTILS_H
#define AV_UTILS_H

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if LIBAVCODEC_VERSION_MAJOR < 61
#error "本项目要求 FFmpeg 7 及以上版本（推荐 FFmpeg 9），请升级 FFmpeg 开发包。"
#endif

struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); }
};

struct AVFrameDeleter {
    void operator()(AVFrame* p) const { if (p) av_frame_free(&p); }
};

struct AVPacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};

struct SwsContextDeleter {
    void operator()(SwsContext* p) const { if (p) sws_freeContext(p); }
};

struct SwrContextDeleter {
    void operator()(SwrContext* p) const { if (p) swr_free(&p); }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr  = std::unique_ptr<AVCodecContext,  AVCodecContextDeleter>;
using AVFramePtr         = std::unique_ptr<AVFrame,         AVFrameDeleter>;
using AVPacketPtr        = std::unique_ptr<AVPacket,        AVPacketDeleter>;
using SwsContextPtr      = std::unique_ptr<SwsContext,      SwsContextDeleter>;
using SwrContextPtr      = std::unique_ptr<SwrContext,      SwrContextDeleter>;

inline AVFramePtr  make_frame()  { return AVFramePtr(av_frame_alloc()); }
inline AVPacketPtr make_packet() { return AVPacketPtr(av_packet_alloc()); }

inline std::string av_err_string(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

inline void print_av_error(const char* where, int err)
{
    av_log(nullptr, AV_LOG_ERROR, "%s: %s\n", where, av_err_string(err).c_str());
}

#endif
