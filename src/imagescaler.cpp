#include "imagescaler.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

bool ImageScaler::ensureContext(const AVFrame* frame)
{
    const int w   = frame->width;
    const int h   = frame->height;
    const int fmt = frame->format;

    if (sws_ && w == srcWidth_ && h == srcHeight_ && fmt == srcFormat_)
        return true;

    sws_.reset(sws_getContext(w, h, static_cast<AVPixelFormat>(fmt),
                              w, h, AV_PIX_FMT_RGB24,
                              SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws_) {
        av_log(nullptr, AV_LOG_ERROR,
               "sws_getContext failed: %s %dx%d -> RGB24\n",
               av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt)), w, h);
        return false;
    }
    srcWidth_  = w;
    srcHeight_ = h;
    srcFormat_ = fmt;
    return true;
}

int ImageScaler::toRgb24(const AVFrame* frame, std::vector<uint8_t>& out,
                         int& outWidth, int& outHeight)
{
    if (!frame || frame->width <= 0 || frame->height <= 0)
        return AVERROR(EINVAL);
    if (!ensureContext(frame))
        return AVERROR(ENOMEM);

    outWidth  = frame->width;
    outHeight = frame->height;

    out.resize(static_cast<size_t>(outWidth) * outHeight * 3);

    uint8_t* dstData[1]     = { out.data() };
    int      dstLinesize[1] = { outWidth * 3 };

    const int scaled = sws_scale(sws_.get(), frame->data, frame->linesize,
                                 0, frame->height, dstData, dstLinesize);
    return scaled > 0 ? 0 : AVERROR_EXTERNAL;
}
