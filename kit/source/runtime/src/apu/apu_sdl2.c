/*
 * POSIX SDL2 output for MA2's xa2_* contract.
 *
 * Adapted from xboxrecomp v0.18.0 src/apu/apu_sdl2.c. The MA2 monitor
 * supplies complete DSP/mixer blocks and retries a refused block after
 * xa2_wait_for_space(), so the SDL queue must be bounded and preserve order.
 */
#if !defined(_WIN32)

#include "apu_xaudio2.h"

#include <SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SAMPLE_RATE = 48000,
    CHANNELS = 2,
    BYTES_PER_FRAME = CHANNELS * (int)sizeof(int16_t),
    DEFAULT_FRAMES = 1024,
    DEFAULT_COUNT = 4,
    STALL_MS = 250
};

static SDL_AudioDeviceID device;
static int audio_owned;
static int initialized;
static int failed;
static int started;
static int buffer_frames = DEFAULT_FRAMES;
static int buffer_count = DEFAULT_COUNT;
static Uint32 pending_bytes;
static Uint32 stall_start;
static int stall_pending;
static unsigned submitted;

static int read_option(const char *name, int fallback, int min, int max,
                       int multiple)
{
    const char *text = getenv(name);
    char *end;
    long value;
    if (!text || !*text)
        return fallback;
    value = strtol(text, &end, 10);
    if (*end || value < min || value > max || value % multiple) {
        fprintf(stderr, "[SDL2] invalid %s=%s\n", name, text);
        return 0;
    }
    return (int)value;
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *headless;

    if (initialized)
        return 1;
    buffer_frames = read_option("RECOMP_AUDIO_BUF_SAMPLES", DEFAULT_FRAMES,
                                256, 2048, 256);
    buffer_count = read_option("RECOMP_AUDIO_BUF_COUNT", DEFAULT_COUNT, 2, 8, 1);
    if (!buffer_frames || !buffer_count)
        return 0;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    headless = getenv("RECOMP_HEADLESS");
    if (headless && *headless && *headless != '0')
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
            return 0;
        }
        audio_owned = 1;
    }
    SDL_zero(want);
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = (Uint16)buffer_frames;
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!device) {
        fprintf(stderr, "[SDL2] cannot open audio device: %s\n", SDL_GetError());
        if (audio_owned)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_owned = 0;
        return 0;
    }
    if (have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels) {
        fprintf(stderr, "[SDL2] audio device returned an incompatible format\n");
        SDL_CloseAudioDevice(device);
        device = 0;
        if (audio_owned)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_owned = 0;
        return 0;
    }
    initialized = 1;
    failed = started = stall_pending = 0;
    pending_bytes = 0;
    submitted = 0;
    fprintf(stderr, "[SDL2] audio initialized: %s, 48000 Hz stereo s16, "
                    "%d x %d frames\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
            buffer_count, buffer_frames);
    return 1;
}

void xa2_shutdown(void)
{
    if (device) {
        SDL_PauseAudioDevice(device, 1);
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
        device = 0;
    }
    if (audio_owned)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    audio_owned = initialized = failed = started = stall_pending = 0;
    pending_bytes = 0;
}

int xa2_is_active(void)
{
    return initialized;
}

int xa2_get_buffer_size(void)
{
    return buffer_frames;
}

static Uint32 queue_limit(void)
{
    return (Uint32)(buffer_frames * buffer_count * BYTES_PER_FRAME);
}

static int device_usable(void)
{
    if (!initialized || !device || failed)
        return 0;
    if (started && SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_STOPPED) {
        fprintf(stderr, "[SDL2] audio device stopped\n");
        failed = 1;
        return 0;
    }
    return 1;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    Uint32 bytes, queued;
    if (!device_usable())
        return 0;
    if (!samples || num_samples <= 0 || num_samples > buffer_frames) {
        fprintf(stderr, "[SDL2] invalid audio submission (%d frames)\n", num_samples);
        failed = 1;
        return 0;
    }
    bytes = (Uint32)num_samples * BYTES_PER_FRAME;
    pending_bytes = bytes;
    queued = SDL_GetQueuedAudioSize(device);
    if (queued > queue_limit() - bytes)
        return 0;
    if (SDL_QueueAudio(device, samples, bytes) != 0) {
        fprintf(stderr, "[SDL2] audio queue failed: %s\n", SDL_GetError());
        failed = 1;
        return 0;
    }
    pending_bytes = 0;
    stall_pending = 0;
    if (!started && ++submitted >= 2) {
        SDL_PauseAudioDevice(device, 0);
        started = 1;
    }
    return 1;
}

int xa2_wait_for_space(int milliseconds)
{
    Uint32 start, timeout;
    if (!device_usable() || milliseconds < 0)
        return -1;
    start = SDL_GetTicks();
    timeout = (Uint32)milliseconds;
    for (;;) {
        Uint32 now = SDL_GetTicks();
        if (!device_usable())
            return -1;
        if (!pending_bytes ||
            SDL_GetQueuedAudioSize(device) <= queue_limit() - pending_bytes) {
            stall_pending = 0;
            return 1;
        }
        if (!stall_pending) {
            stall_start = now;
            stall_pending = 1;
        } else if (now - stall_start >= STALL_MS) {
            fprintf(stderr, "[SDL2] audio queue stalled; dropping block\n");
            return -1;
        }
        if (now - start >= timeout)
            return 0;
        SDL_Delay(1);
    }
}

#endif
