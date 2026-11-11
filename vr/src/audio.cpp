// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "audio.h"

#include "log.h"

#include <SDL3/SDL.h>
#include <opus/opus_multistream.h>

#include <vector>

namespace {

OpusMSDecoder *g_dec = nullptr;
SDL_AudioStream *g_stream = nullptr;
int g_channels = 2, g_frame = 240, g_rate = 48000;
int g_max_queued = 0;  // bytes; more than this queued and we drop a packet
std::vector<int16_t> g_pcm;
unsigned long g_drops = 0;

int init(int audio_config, const POPUS_MULTISTREAM_CONFIGURATION c, void *, int) {
    (void)audio_config;
    int err = 0;
    g_dec = opus_multistream_decoder_create(c->sampleRate, c->channelCount, c->streams, c->coupledStreams,
                                            c->mapping, &err);
    if (!g_dec) { fwlog("audio: opus decoder: %s", opus_strerror(err)); return -1; }
    g_channels = c->channelCount;
    g_frame = c->samplesPerFrame;
    g_rate = c->sampleRate;
    g_pcm.assign(size_t(g_frame) * g_channels, 0);
    // About 60 ms: enough to ride out Wi-Fi jitter without audible lag.
    g_max_queued = g_rate * g_channels * int(sizeof(int16_t)) * 60 / 1000;

    const SDL_AudioSpec spec{SDL_AUDIO_S16, g_channels, g_rate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_stream) {
        // No audio output is not fatal: keep streaming video.
        fwlog("audio: no output device (%s); continuing without sound", SDL_GetError());
        return 0;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    fwlog("audio: %d ch, %d Hz, %d samples per packet", g_channels, g_rate, g_frame);
    return 0;
}

void cleanup() {
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    if (g_dec) opus_multistream_decoder_destroy(g_dec);
    if (g_drops) fwlog("audio: dropped %lu packets to keep latency down", g_drops);
    g_stream = nullptr;
    g_dec = nullptr;
}

void play(char *data, int len) {
    if (!g_dec || !g_stream) return;
    // data == NULL: a lost packet; opus conceals it.
    const int n = opus_multistream_decode(g_dec, reinterpret_cast<const unsigned char *>(data), data ? len : 0,
                                          g_pcm.data(), g_frame, 0);
    if (n <= 0) return;
    if (SDL_GetAudioStreamQueued(g_stream) > g_max_queued) { g_drops++; return; }
    SDL_PutAudioStreamData(g_stream, g_pcm.data(), n * g_channels * int(sizeof(int16_t)));
}

}  // namespace

AUDIO_RENDERER_CALLBACKS audio_callbacks() {
    AUDIO_RENDERER_CALLBACKS cb;
    LiInitializeAudioCallbacks(&cb);
    cb.init = init;
    cb.cleanup = cleanup;
    cb.decodeAndPlaySample = play;
    cb.capabilities = 0;
    return cb;
}
