#include "apu/apu_state.h"
#include "apu/apu.h"
#include "apu/apu_xaudio2.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void mcpx_apu_monitor_init(MCPXAPUState *, Error **);
void mcpx_apu_monitor_finalize(MCPXAPUState *);
void mcpx_apu_monitor_frame(MCPXAPUState *);

static int active, buffer_frames, submissions, submitted_frames;
static int16_t submitted[512][2];

int xa2_init(void) { active = 1; return 1; }
void xa2_shutdown(void) { active = 0; }
int xa2_is_active(void) { return active; }
int xa2_get_buffer_size(void) { return buffer_frames; }
int xa2_wait_for_space(int ms) { (void)ms; return -1; }
int xa2_submit_samples(const int16_t *samples, int frames)
{
    if (frames > 512) return 0;
    ++submissions;
    submitted_frames = frames;
    for (int i = 0; i < frames; ++i) {
        submitted[i][0] = samples[2 * i];
        submitted[i][1] = samples[2 * i + 1];
    }
    return 1;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static MCPXAPUState apu;
static void emit(int16_t left, int16_t right)
{
    for (int i = 0; i < 256; ++i) {
        apu.monitor.frame_buf[i][0] = left;
        apu.monitor.frame_buf[i][1] = right;
    }
    apu.ep_frame_div = 7;
    mcpx_apu_monitor_frame(&apu);
}

int main(void)
{
    unsetenv("RECOMP_AUDIO_CAPTURE");
    unsetenv("RECOMP_AUDIO_SILENT");
    CHECK(!mcpx_apu_init_standalone((uint8_t *)(uintptr_t)1));
    buffer_frames = 512;
    mcpx_apu_monitor_init(&apu, NULL);
    emit(1111, -2222);
    CHECK(submissions == 0);
    mcpx_apu_monitor_finalize(&apu);

    buffer_frames = 256;
    mcpx_apu_monitor_init(&apu, NULL);
    emit(3333, -4444);
    CHECK(submissions == 1 && submitted_frames == 256);
    CHECK(submitted[0][0] == 3333 && submitted[0][1] == -4444);
    CHECK(submitted[255][0] == 3333 && submitted[255][1] == -4444);
    mcpx_apu_monitor_finalize(&apu);

    buffer_frames = 512;
    mcpx_apu_monitor_init(&apu, NULL);
    emit(1234, -1234);
    emit(5678, -5678);
    CHECK(submissions == 2 && submitted_frames == 512);
    CHECK(submitted[0][0] == 1234 && submitted[255][1] == -1234);
    CHECK(submitted[256][0] == 5678 && submitted[511][1] == -5678);
    mcpx_apu_monitor_finalize(&apu);
    puts("APU monitor: DSP blocks retain order and restart discards partial output");
    return 0;
}
