#include "audiodevice.h"
#include "av_utils.h"

#include <algorithm>
#include <cstring>

#include <SDL3/SDL.h>

AudioDevice::~AudioDevice()
{
    close();
}

void SDLCALL AudioDevice::onPull(void* userdata, SDL_AudioStream* stream,
                                 int additionalAmount, int  )
{
    auto* self = static_cast<AudioDevice*>(userdata);

    uint8_t chunk[4096];

    while (additionalAmount > 0) {
        const int want = std::min(additionalAmount, static_cast<int>(sizeof(chunk)));

        int got = -1;
        if (self->pull_)
            got = self->pull_(chunk, want);

        if (got <= 0) {
            std::memset(chunk, 0, want);
            got = want;
        }

        if (got > 0)
            SDL_PutAudioStreamData(stream, chunk, got);

        additionalAmount -= got;
    }
}

bool AudioDevice::open(int sampleRate, int channels, PullCallback pull)
{
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            av_log(nullptr, AV_LOG_FATAL, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    spec_.format   = SDL_AUDIO_S16;
    spec_.channels = channels;
    spec_.freq     = sampleRate;
    pull_          = std::move(pull);

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec_, &AudioDevice::onPull, this);
    if (!stream_) {
        av_log(nullptr, AV_LOG_FATAL,
               "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioDevice::close()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    pull_ = nullptr;
}

void AudioDevice::setPaused(bool paused)
{
    if (!stream_)
        return;
    if (paused)
        SDL_PauseAudioStreamDevice(stream_);
    else
        SDL_ResumeAudioStreamDevice(stream_);
}

void AudioDevice::setVolume(float linear01)
{
    if (stream_)
        SDL_SetAudioStreamGain(stream_, std::clamp(linear01, 0.0f, 1.0f));
}

void AudioDevice::setSpeed(float ratio)
{
    if (stream_)
        SDL_SetAudioStreamFrequencyRatio(stream_, ratio);
}

int AudioDevice::frameBytes() const
{
    return SDL_AUDIO_FRAMESIZE(spec_);
}

int AudioDevice::bytesPerSecond() const
{
    return spec_.freq * frameBytes();
}
