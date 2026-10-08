#ifndef IMAGESCALER_H
#define IMAGESCALER_H

#include <cstdint>
#include <vector>

#include "av_utils.h"

class ImageScaler
{
public:
    ImageScaler() = default;
    ~ImageScaler() = default;

    int toRgb24(const AVFrame* frame, std::vector<uint8_t>& out,
                int& outWidth, int& outHeight);

private:
    bool ensureContext(const AVFrame* frame);

    SwsContextPtr sws_;
    int srcWidth_  = 0;
    int srcHeight_ = 0;
    int srcFormat_ = AV_PIX_FMT_NONE;
};

#endif
