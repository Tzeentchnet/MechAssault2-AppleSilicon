#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

static Uint32 queued, peak, ticks, max_expected = 2048;
static int initializations, quits, open_fails, init_fails, bad_format;
static int queue_failures, calls, delays, drain = 1024, subsystem, playing, stopped;
static Uint8 zero_block[256 * 32 * 4];

int SDLCALL SDL_InitSubSystem(Uint32 flags)
{ (void)flags; ++initializations; if (init_fails) return -1; subsystem = 1; return 0; }
void SDLCALL SDL_QuitSubSystem(Uint32 flags) { (void)flags; ++quits; subsystem = 0; }
Uint32 SDLCALL SDL_WasInit(Uint32 flags) { return subsystem ? flags : 0; }
const char * SDLCALL SDL_GetError(void) { return "synthetic SDL failure"; }
const char * SDLCALL SDL_GetCurrentAudioDriver(void) { return "synthetic"; }
SDL_AudioDeviceID SDLCALL SDL_OpenAudioDevice(const char *name, int capture,
    const SDL_AudioSpec *want, SDL_AudioSpec *got, int changes)
{
    (void)name; (void)capture; (void)changes;
    if (open_fails) return 0;
    *got = *want;
    if (bad_format) got->channels = 1;
    return 17;
}
void SDLCALL SDL_CloseAudioDevice(SDL_AudioDeviceID id) { (void)id; }
void SDLCALL SDL_PauseAudioDevice(SDL_AudioDeviceID id, int pause) { (void)id; playing = !pause; }
SDL_AudioStatus SDLCALL SDL_GetAudioDeviceStatus(SDL_AudioDeviceID id)
{ (void)id; return stopped ? SDL_AUDIO_STOPPED : playing ? SDL_AUDIO_PLAYING : SDL_AUDIO_PAUSED; }
void SDLCALL SDL_ClearQueuedAudio(SDL_AudioDeviceID id) { (void)id; queued = 0; }
Uint32 SDLCALL SDL_GetQueuedAudioSize(SDL_AudioDeviceID id) { (void)id; return queued; }
int SDLCALL SDL_QueueAudio(SDL_AudioDeviceID id, const void *data, Uint32 bytes)
{
    (void)id; (void)data;
    ++calls;
    if (queue_failures) { --queue_failures; return -1; }
    queued += bytes;
    if (queued > peak) peak = queued;
    return 0;
}
void SDLCALL SDL_Delay(Uint32 ms)
{
    ticks += ms; ++delays;
    if (drain > 0) queued = queued > (Uint32)drain ? queued - (Uint32)drain : 0;
}
Uint32 SDLCALL SDL_GetTicks(void) { return ticks; }
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

int main(void)
{
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "257", 1);
    CHECK(!xa2_init());
    CHECK(initializations == 0);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "256", 1);
    setenv("RECOMP_AUDIO_BUF_COUNT", "2", 1);
    init_fails = 1;
    CHECK(!xa2_init());
    init_fails = 0; open_fails = 1;
    CHECK(!xa2_init() && quits == 1);
    open_fails = 0; bad_format = 1;
    CHECK(!xa2_init() && quits == 2);
    bad_format = 0;

    CHECK(xa2_init() && xa2_is_active() && xa2_get_buffer_size() == 256);
    CHECK(queued == 0 && !playing);
    CHECK(xa2_submit_samples((const int16_t *)zero_block, 256) && !playing);
    CHECK(xa2_submit_samples((const int16_t *)zero_block, 256) && playing);
    CHECK(queued == max_expected && calls == 2);
    CHECK(!xa2_submit_samples((const int16_t *)zero_block, 256));
    CHECK(xa2_wait_for_space(0) == 0);
    CHECK(xa2_wait_for_space(10) == 1 && delays > 0);
    CHECK(xa2_submit_samples((const int16_t *)zero_block, 256));
    CHECK(peak <= max_expected);

    drain = 0;
    int before = calls;
    CHECK(!xa2_submit_samples((const int16_t *)zero_block, 256));
    for (int i = 0; i < 30 && xa2_wait_for_space(10) == 0; ++i) {}
    CHECK(xa2_wait_for_space(10) == -1 && calls == before);
    xa2_shutdown();
    drain = 1024;
    CHECK(xa2_init());
    queue_failures = 1;
    CHECK(!xa2_submit_samples((const int16_t *)zero_block, 256));
    CHECK(xa2_wait_for_space(10) == -1);
    xa2_shutdown();
    CHECK(xa2_init());
    CHECK(xa2_submit_samples((const int16_t *)zero_block, 256));
    CHECK(xa2_submit_samples((const int16_t *)zero_block, 256));
    stopped = 1;
    CHECK(!xa2_submit_samples((const int16_t *)zero_block, 256));
    CHECK(xa2_wait_for_space(10) == -1);
    xa2_shutdown();
    puts("SDL audio: prefill, retry, bounds, stall, and reinitialization passed");
    return 0;
}
