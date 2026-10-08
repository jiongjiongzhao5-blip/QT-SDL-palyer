#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "decoder.h"
#include "imagescaler.h"

extern "C" {
#include <libavutil/imgutils.h>
}

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc)
{
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", desc); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", desc); }
}

static void section(const char* title)
{
    std::printf("\n==================================================\n");
    std::printf(" %s\n", title);
    std::printf("==================================================\n");
}

static void msleep(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static bool saveJpeg(const std::string& path, const std::vector<uint8_t>& rgb,
                     int w, int h)
{
    SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_RGB24,
                                     w, h, AV_PIX_FMT_YUVJ420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) { std::printf("        建转换上下文失败\n"); return false; }

    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUVJ420P;
    yuv->width  = w;
    yuv->height = h;
    if (av_frame_get_buffer(yuv, 0) < 0) {
        sws_freeContext(sws); av_frame_free(&yuv); return false;
    }
    const uint8_t* srcData[1] = { rgb.data() };
    int            srcStride[1] = { w * 3 };
    sws_scale(sws, srcData, srcStride, 0, h, yuv->data, yuv->linesize);
    sws_freeContext(sws);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) { std::printf("        找不到 MJPEG 编码器\n"); av_frame_free(&yuv); return false; }

    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width       = w;
    c->height      = h;
    c->pix_fmt     = AV_PIX_FMT_YUVJ420P;
    c->time_base   = AVRational{1, 25};
    c->color_range = AVCOL_RANGE_JPEG;
    int ret = avcodec_open2(c, codec, nullptr);
    if (ret < 0) {
        std::printf("        avcodec_open2 失败: %s\n", av_err_string(ret).c_str());
        avcodec_free_context(&c); av_frame_free(&yuv); return false;
    }
    yuv->pts = 0;

    bool ok = false;
    AVPacket* p = av_packet_alloc();
    ret = avcodec_send_frame(c, yuv);
    if (ret < 0)
        std::printf("        send_frame 失败: %s\n", av_err_string(ret).c_str());

    ret = avcodec_receive_packet(c, p);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        avcodec_send_frame(c, nullptr);
        ret = avcodec_receive_packet(c, p);
    }
    if (ret < 0) {
        std::printf("        receive_packet 失败: %s\n", av_err_string(ret).c_str());
    } else {
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) {
            std::printf("        文件打不开: %s\n", path.c_str());
        } else {
            std::fwrite(p->data, 1, static_cast<size_t>(p->size), fp);
            std::fclose(fp);
            std::printf("        JPEG 大小 %d 字节\n", p->size);
            ok = true;
        }
    }

    av_packet_free(&p);
    av_frame_free(&yuv);
    avcodec_free_context(&c);
    return ok;
}

struct RgbStats {
    double avgR = 0, avgG = 0, avgB = 0;
};

static RgbStats analyze(const std::vector<uint8_t>& rgb)
{
    RgbStats s;
    if (rgb.empty()) return s;
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    const size_t n = rgb.size() / 3;
    for (size_t i = 0; i < n; ++i) {
        sumR += rgb[i * 3 + 0];
        sumG += rgb[i * 3 + 1];
        sumB += rgb[i * 3 + 2];
    }
    s.avgR = double(sumR) / n;
    s.avgG = double(sumG) / n;
    s.avgB = double(sumB) / n;
    return s;
}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string mediaPath = (argc > 1) ? argv[1] : "../_media/test_media.mp4";
    const std::string pngPath   = (argc > 2) ? argv[2] : "../_media/frame_preview.jpg";

    std::printf("==================================================\n");
    std::printf(" M7 实验台 A：ImageScaler（YUV -> RGB24）\n");
    std::printf(" 素材: %s\n", mediaPath.c_str());
    std::printf("==================================================\n");

    section("[1] 用 Decoder 解出若干视频帧");
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, mediaPath.c_str(), nullptr, nullptr) < 0) {
        std::printf("  打不开素材\n"); return 1;
    }
    avformat_find_stream_info(ic, nullptr);
    const int vIdx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) { std::printf("  没有视频流\n"); return 1; }

    PacketQueue vq;
    Decoder dec;
    dec.open(ic->streams[vIdx]);
    dec.start(vq, [] {});

    AVPacket* pkt = av_packet_alloc();
    int pushed = 0;
    while (pushed < 60 && av_read_frame(ic, pkt) >= 0) {
        if (pkt->stream_index == vIdx) { vq.put(pkt); ++pushed; }
        else av_packet_unref(pkt);
    }
    std::printf("        喂入 %d 个视频包\n", pushed);
    check(pushed > 0, "拿到视频包");

    vq.putNullPacket(vIdx);

    section("[2] 逐帧转换：尺寸 / 大小 / 缓冲复用");
    ImageScaler scaler;
    std::vector<uint8_t> rgb;
    AVFrame* frame = av_frame_alloc();
    int  frames = 0;
    int  expectW = 0, expectH = 0;
    bool sizeOk = true, reuseOk = true;
    uint8_t* firstBuf = nullptr;
    std::vector<uint8_t> keepForPng;

    while (true) {
        const int r = dec.decodeFrame(frame);
        if (r <= 0) break;

        int w = 0, h = 0;
        if (scaler.toRgb24(frame, rgb, w, h) < 0) {
            std::printf("        toRgb24 失败\n");
            sizeOk = false;
            break;
        }

        if (frames == 0) {
            expectW = w; expectH = h;
            firstBuf = rgb.data();
            std::printf("        第一帧: %dx%d -> RGB24 %zu 字节（期望 %d）\n",
                        w, h, rgb.size(), w * h * 3);
        } else {
            if (w != expectW || h != expectH) sizeOk = false;
            if (rgb.data() != firstBuf) reuseOk = false;
        }
        if (rgb.size() != static_cast<size_t>(w) * h * 3) sizeOk = false;

        if (frames == 40)
            keepForPng = rgb;

        ++frames;
    }

    std::printf("        共转换 %d 帧\n", frames);
    check(frames > 40, "转换了 40 帧以上");
    check(sizeOk, "所有帧的输出大小都恰好等于 w * h * 3（紧凑无填充）");
    check(reuseOk,
          "★ 输出缓冲地址全程不变 —— vector 被复用，没有反复分配");

    section("[3] ★ 通道顺序验证：结果应该整体偏蓝");
    const RgbStats st = analyze(keepForPng);
    std::printf("        第 41 帧的三通道均值: R=%.1f  G=%.1f  B=%.1f\n",
                st.avgR, st.avgG, st.avgB);
    check(!keepForPng.empty(), "拿到了用于分析的一帧");
    check(st.avgB > st.avgR + 20.0,
          "★ B 通道明显高于 R 通道 —— 通道顺序正确（没把 RGB 写成 BGR）");
    check(st.avgG > st.avgR && st.avgB > st.avgG,
          "三通道大小关系符合 YUV(U=160,V=90) 的理论预期 B > G > R");

    section("[4] 导出快照（JPEG）");
    if (!keepForPng.empty()) {
        if (saveJpeg(pngPath, keepForPng, expectW, expectH)) {
            std::printf("        已写出: %s (%dx%d)\n", pngPath.c_str(), expectW, expectH);
            check(true, "JPEG 导出成功 —— 可以打开看看颜色和移动方块的位置对不对");
        } else {
            check(false, "JPEG 导出失败");
        }
    }

    section("[5] 错误路径");
    {
        int w = 0, h = 0;
        std::vector<uint8_t> tmp;
        check(scaler.toRgb24(nullptr, tmp, w, h) < 0, "传 nullptr 帧被拒绝（返回负错误码）");

        AVFrame* bad = av_frame_alloc();
        const int r = scaler.toRgb24(bad, tmp, w, h);
        std::printf("        尺寸为 0 的帧 -> 返回 %d (%s)\n",
                    r, av_err_string(r).c_str());
        check(r == AVERROR(EINVAL), "尺寸非法的帧返回 EINVAL");
        av_frame_free(&bad);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    FrameQueue dummy(3);
    dec.abort(dummy);
    avformat_close_input(&ic);

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
