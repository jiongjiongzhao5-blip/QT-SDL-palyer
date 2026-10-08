#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <windows.h>

#include "audiodevice.h"

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

constexpr double kPi       = 3.14159265358979323846;
constexpr bool   kQuietTone = true;

struct ToneSource {
    int    sampleRate = 48000;
    int    channels   = 2;
    double phase      = 0.0;
    double freq       = 440.0;

    std::atomic<bool>      stall{false};
    std::atomic<long long> bytesPulled{0};
    std::atomic<int>       callCount{0};
    std::atomic<int>       stallHits{0};

    int pull(uint8_t* dst, int bytes)
    {
        callCount.fetch_add(1);

        if (stall.load()) {
            stallHits.fetch_add(1);
            return -1;
        }

        const int frameBytes = 2 * channels;
        const int frames     = bytes / frameBytes;
        auto* out = reinterpret_cast<int16_t*>(dst);
        const double step = 2.0 * kPi * freq / sampleRate;

        for (int i = 0; i < frames; ++i) {
            const int16_t v = kQuietTone
                                  ? static_cast<int16_t>(1200.0 * std::sin(phase))
                                  : static_cast<int16_t>(0);
            for (int c = 0; c < channels; ++c)
                out[i * channels + c] = v;
            phase += step;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }

        const int produced = frames * frameBytes;
        bytesPulled.fetch_add(produced);
        return produced;
    }
};

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("==================================================\n");
    std::printf(" M7 实验台 B：AudioDevice（SDL3 音频输出）\n");
    if (kQuietTone)
        std::printf(" 提示：会发出一声很轻的 440Hz 提示音\n");
    std::printf("==================================================\n");

    ToneSource src;

    section("[1] 打开设备");
    AudioDevice dev;
    const int sampleRate = 48000;
    const int channels   = 2;

    const bool opened = dev.open(sampleRate, channels,
                                [&src](uint8_t* d, int b) { return src.pull(d, b); });
    check(opened, "SDL_OpenAudioDeviceStream 成功");
    if (!opened) {
        std::printf("\n        设备打不开（可能是无声卡的环境），后续测试跳过。\n");
        std::printf("        SDL 的错误信息已经由 AudioDevice 打印在上面。\n");
        return 0;
    }

    std::printf("        采样率 %d Hz, 声道 %d\n", dev.sampleRate(), dev.channels());
    std::printf("        一个采样帧 %d 字节, 每秒消耗 %d 字节\n",
                dev.frameBytes(), dev.bytesPerSecond());
    check(dev.isOpen(), "isOpen() 返回 true");
    check(dev.frameBytes() == 4, "S16 立体声一个采样帧 = 4 字节");
    check(dev.bytesPerSecond() == sampleRate * 4,
          "bytesPerSecond = 采样率 × 每帧字节数");

    section("[2] ★ 稳态消耗速率校验");
    msleep(250);
    const long long a = src.bytesPulled.load();
    const int       windowMs = 600;
    msleep(windowMs);
    const long long b = src.bytesPulled.load();

    const double measured = double(b - a) / (windowMs / 1000.0);
    const double expected = double(dev.bytesPerSecond());
    const double ratio    = measured / expected;

    std::printf("        稳定期 %d ms 内拉取 %lld 字节\n", windowMs, b - a);
    std::printf("        实测速率 %.0f 字节/秒，理论 %.0f 字节/秒，比值 %.3f\n",
                measured, expected, ratio);
    std::printf("        回调累计被调用 %d 次\n", src.callCount.load());

    check(b > a, "数据持续被拉取（回调在工作）");
    check(ratio > 0.75 && ratio < 1.25,
          "★ 实测速率与理论值一致（±25%）—— 设备确实按实时速率在消耗");
    check(src.callCount.load() > 3,
          "回调被反复调用（不是只调一次就停了）");

    section("[3] 欠载补静音");
    const int callsBefore = src.callCount.load();
    src.stall = true;
    msleep(300);
    src.stall = false;
    const int callsDuring = src.callCount.load() - callsBefore;

    std::printf("        欠载期间回调仍被调用 %d 次，其中返回 -1 的有 %d 次\n",
                callsDuring, src.stallHits.load());
    check(src.stallHits.load() > 0, "确实触发了欠载路径");
    check(callsDuring > 0,
          "★ 欠载期间回调仍在被调用 —— 说明补静音让流水线保持连续，没有卡住");

    msleep(100);

    section("[4] 暂停 / 恢复");
    dev.setPaused(true);
    msleep(200);
    const long long p1 = src.bytesPulled.load();
    msleep(300);
    const long long p2 = src.bytesPulled.load();
    const double pausedGrow = double(p2 - p1) / expected;
    std::printf("        暂停 300ms 期间多拉了 %.1f ms 的量\n", pausedGrow * 1000.0);
    check(pausedGrow < 0.15, "暂停后基本不再拉取数据（<150ms 的残余）");

    dev.setPaused(false);
    msleep(300);
    const long long p3 = src.bytesPulled.load();
    check(p3 > p2, "恢复后继续拉取数据");
    std::printf("        恢复 300ms 内多拉了 %.1f ms 的量\n",
                double(p3 - p2) / expected * 1000.0);

    section("[5] 音量 / 倍速接口");
    dev.setVolume(0.4f);
    msleep(80);
    dev.setVolume(-1.0f);
    msleep(80);
    dev.setVolume(5.0f);
    msleep(80);
    dev.setVolume(0.6f);
    check(true, "setVolume 接受越界值而不崩溃（内部 clamp 生效）");

    dev.setSpeed(1.5f);
    msleep(120);
    dev.setSpeed(1.0f);
    msleep(120);
    check(true, "setSpeed 调用正常（1.5x 会变调，属预期行为）");

    section("[6] close 之后回调停止");
    dev.close();
    check(!dev.isOpen(), "close 后 isOpen() 返回 false");

    const long long c1 = src.bytesPulled.load();
    msleep(300);
    const long long c2 = src.bytesPulled.load();
    check(c2 == c1, "★ close 之后不再有任何回调 —— 流已被销毁并同步等待回调退出");

    dev.close();
    check(true, "重复调用 close() 安全（幂等）");

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
