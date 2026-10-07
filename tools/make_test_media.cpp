extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace {

constexpr int kWidth      = 640;
constexpr int kHeight     = 480;
constexpr int kFps        = 25;
constexpr int kSeconds    = 5;
constexpr int kSampleRate = 44100;
constexpr int kChannels   = 2;
constexpr double kPi      = 3.14159265358979323846;

constexpr int kVideoFrames = kFps * kSeconds;
constexpr int kAudioTotal  = kSampleRate * kSeconds;

void fillVideoFrame(AVFrame* f, int idx)
{
    const int shift = idx * 4;
    for (int y = 0; y < kHeight; ++y) {
        uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        for (int x = 0; x < kWidth; ++x)
            row[x] = static_cast<uint8_t>((x + shift) & 0xFF);
    }

    const int bx = (idx * 7) % (kWidth - 80);
    const int by = (idx * 5) % (kHeight - 80);
    for (int y = by; y < by + 80; ++y) {
        uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        std::memset(row + bx, 235, 80);
    }

    for (int y = 0; y < kHeight / 2; ++y) {
        std::memset(f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1], 160, kWidth / 2);
        std::memset(f->data[2] + static_cast<ptrdiff_t>(y) * f->linesize[2],  90, kWidth / 2);
    }
}

int drainEncoder(AVFormatContext* oc, AVCodecContext* c, AVStream* st)
{
    AVPacket* pkt = av_packet_alloc();
    for (;;) {
        const int ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }
        pkt->stream_index = st->index;
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->time_base = st->time_base;
        const int wret = av_interleaved_write_frame(oc, pkt);
        if (wret < 0) {
            av_packet_free(&pkt);
            return wret;
        }
    }
    av_packet_free(&pkt);
    return 0;
}

AVCodecContext* openStream(AVFormatContext* oc, AVStream** outStream,
                           AVCodecID wantId, const char* fallbackName)
{
    const AVCodec* codec = avcodec_find_encoder(wantId);
    if (!codec)
        codec = avcodec_find_encoder_by_name(fallbackName);
    if (!codec) {
        std::fprintf(stderr, "找不到编码器 %s\n", fallbackName);
        return nullptr;
    }
    std::printf("  使用编码器: %s\n", codec->name);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st)
        return nullptr;

    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c)
        return nullptr;

    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    *outStream = st;
    return c;
}

}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    const std::string outPath = (argc > 1) ? argv[1] : "test_media.mp4";

    std::printf("==================================================\n");
    std::printf(" 测试素材生成器\n");
    std::printf(" 目标: %s\n", outPath.c_str());
    std::printf(" 规格: %dx%d @ %dfps, %d 秒 | %dHz %d 声道\n",
                kWidth, kHeight, kFps, kSeconds, kSampleRate, kChannels);
    std::printf("==================================================\n");

    AVFormatContext* oc = nullptr;
    int ret = avformat_alloc_output_context2(&oc, nullptr, nullptr, outPath.c_str());
    if (ret < 0 || !oc) {
        std::fprintf(stderr, "无法根据扩展名推断封装格式\n");
        return 1;
    }
    std::printf("  封装格式: %s\n", oc->oformat->name);

    AVStream* vst = nullptr;
    AVCodecContext* vc = openStream(oc, &vst, AV_CODEC_ID_H264, "mpeg4");
    if (!vc)
        return 1;
    if (vc->codec_id == AV_CODEC_ID_NONE) {
        std::fprintf(stderr, "视频编码器上下文未正确初始化\n");
        return 1;
    }
    vc->width     = kWidth;
    vc->height    = kHeight;
    vc->time_base = AVRational{1, kFps};
    vc->framerate = AVRational{kFps, 1};
    vc->pix_fmt   = AV_PIX_FMT_YUV420P;
    vc->gop_size  = 12;
    vc->bit_rate  = 800000;
    if (vc->codec_id == AV_CODEC_ID_H264)
        av_opt_set(vc->priv_data, "preset", "veryfast", 0);

    ret = avcodec_open2(vc, nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开视频编码器失败\n");
        return 1;
    }
    avcodec_parameters_from_context(vst->codecpar, vc);
    vst->time_base = vc->time_base;

    AVStream* ast = nullptr;
    AVCodecContext* ac = openStream(oc, &ast, AV_CODEC_ID_AAC, "aac");
    if (!ac)
        return 1;
    ac->sample_rate = kSampleRate;
    av_channel_layout_default(&ac->ch_layout, kChannels);
    ac->time_base   = AVRational{1, kSampleRate};
    ac->bit_rate    = 128000;

    {
        ac->sample_fmt = AV_SAMPLE_FMT_NONE;
        const void* cfgList = nullptr;
        int cfgCount = 0;
        const int qret = avcodec_get_supported_config(ac, nullptr,
                                                     AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                     0, &cfgList, &cfgCount);
        if (qret >= 0 && cfgList && cfgCount > 0) {
            const AVSampleFormat* fmts = static_cast<const AVSampleFormat*>(cfgList);
            ac->sample_fmt = fmts[0];
            std::printf("  编码器支持 %d 种采样格式，选用: %s\n",
                        cfgCount, av_get_sample_fmt_name(ac->sample_fmt));
        } else {
            ac->sample_fmt = AV_SAMPLE_FMT_FLTP;
            std::printf("  查询采样格式失败，回退为 FLTP\n");
        }
    }

    ret = avcodec_open2(ac, nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开音频编码器失败\n");
        return 1;
    }
    if (ac->frame_size <= 0) {
        std::fprintf(stderr, "音频编码器未给出 frame_size，无法分包\n");
        return 1;
    }
    std::printf("  音频采样格式: %s, 每帧 %d 个采样\n",
                av_get_sample_fmt_name(ac->sample_fmt), ac->frame_size);
    avcodec_parameters_from_context(ast->codecpar, ac);
    ast->time_base = ac->time_base;

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, outPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "无法创建输出文件: %s\n", outPath.c_str());
            return 1;
        }
    }
    ret = avformat_write_header(oc, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "写文件头失败\n");
        return 1;
    }

    {
        AVFrame* frame = av_frame_alloc();
        frame->format = vc->pix_fmt;
        frame->width  = vc->width;
        frame->height = vc->height;
        av_frame_get_buffer(frame, 0);

        for (int i = 0; i < kVideoFrames; ++i) {
            av_frame_make_writable(frame);
            fillVideoFrame(frame, i);
            frame->pts = i;

            ret = avcodec_send_frame(vc, frame);
            if (ret < 0) {
                std::fprintf(stderr, "发送视频帧 %d 失败\n", i);
                break;
            }
            if (drainEncoder(oc, vc, vst) < 0) {
                std::fprintf(stderr, "写视频包失败\n");
                break;
            }
        }
        avcodec_send_frame(vc, nullptr);
        drainEncoder(oc, vc, vst);
        av_frame_free(&frame);
        std::printf("  已编码视频 %d 帧\n", kVideoFrames);
    }

    {
        AVFrame* frame = av_frame_alloc();
        frame->format      = ac->sample_fmt;
        frame->sample_rate = ac->sample_rate;
        frame->nb_samples  = ac->frame_size;
        av_channel_layout_copy(&frame->ch_layout, &ac->ch_layout);
        av_frame_get_buffer(frame, 0);

        int64_t written = 0;
        while (written < kAudioTotal) {
            av_frame_make_writable(frame);

            for (int ch = 0; ch < kChannels; ++ch) {
                float* p = reinterpret_cast<float*>(frame->data[ch]);
                const double freq = 440.0 + 220.0 * ch;
                for (int n = 0; n < ac->frame_size; ++n) {
                    const int64_t idx = written + n;
                    if (idx >= kAudioTotal) {
                        p[n] = 0.0f;
                        continue;
                    }
                    const double t = idx / double(kSampleRate);
                    p[n] = static_cast<float>(0.3 * std::sin(2.0 * kPi * freq * t));
                }
            }
            frame->pts = written;

            ret = avcodec_send_frame(ac, frame);
            if (ret < 0) {
                std::fprintf(stderr, "发送音频帧失败\n");
                break;
            }
            if (drainEncoder(oc, ac, ast) < 0) {
                std::fprintf(stderr, "写音频包失败\n");
                break;
            }
            written += ac->frame_size;
        }
        avcodec_send_frame(ac, nullptr);
        drainEncoder(oc, ac, ast);
        av_frame_free(&frame);
        std::printf("  已编码音频 %lld 个采样（约 %.2f 秒）\n",
                    static_cast<long long>(written), written / double(kSampleRate));
    }

    av_write_trailer(oc);

    if (!(oc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&oc->pb);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_free_context(oc);

    std::printf("\n  回读验证（确认文件合法可解）：\n");

    AVFormatContext* ic = nullptr;
    ret = avformat_open_input(&ic, outPath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "  ✗ 回读失败！文件可能已损坏\n");
        return 1;
    }
    avformat_find_stream_info(ic, nullptr);

    std::printf("    封装格式  : %s\n", ic->iformat->name);
    std::printf("    总时长    : %.3f 秒\n", ic->duration / double(AV_TIME_BASE));
    std::printf("    流数量    : %u\n", ic->nb_streams);

    for (unsigned i = 0; i < ic->nb_streams; ++i) {
        AVStream* st = ic->streams[i];
        const bool isVideo = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        if (isVideo) {
            std::printf("      流 %u [视频] %s  %dx%d  time_base=%d/%d\n",
                        i, avcodec_get_name(st->codecpar->codec_id),
                        st->codecpar->width, st->codecpar->height,
                        st->time_base.num, st->time_base.den);
        } else {
            std::printf("      流 %u [音频] %s  %dHz %d声道  time_base=%d/%d\n",
                        i, avcodec_get_name(st->codecpar->codec_id),
                        st->codecpar->sample_rate,
                        st->codecpar->ch_layout.nb_channels,
                        st->time_base.num, st->time_base.den);
        }
    }
    avformat_close_input(&ic);

    std::printf("\n完成。文件已就绪，可以给 Decoder 测试用了。\n");
    return 0;
}
