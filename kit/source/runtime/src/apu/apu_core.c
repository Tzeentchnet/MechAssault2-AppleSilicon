/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"
#include "../platform/pc_frontend.h"
#include "../platform/pc_cpu_topo.h"   /* pc_thread_role_enter */
#include "../kernel/xbox_memory_layout.h"   /* XBOX_WORKER_STACK_TOP */
#if defined(__aarch64__) && !defined(_WIN32)
#include "../platform/posix_mmio.h"
#endif
#if defined(_WIN32)
#include <avrt.h>
#endif

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Audio wait counters (pipeline T2-0 "A0"): measurement only, no behaviour change.
 *
 * RECOMP_AUDIO_WAIT_STATS=1 or RECOMP_PERF sets g_apu_ws once, in mcpx_apu_init_standalone; with it
 * off every hook is one branch on that int. With it on, the APU thread prints one line each of
 * [AUDIO-WAITS] (guest-side wait sites), [APU-WAIT] (APU thread waits), [APU-VEH], [APU-SRC] and
 * [APU-DSP] PRAM every 5 s of wall time (apu_stats_tick). Guest wait sites call apu_ws_record from
 * any thread (Interlocked adds, no locks); the APU-thread waits use apu_wt_add (APU thread only).
 * ============================================================ */
int g_apu_ws;
static double g_ws_ms_per_tick;
double apu_ws_now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_ws_ms_per_tick;
}
enum { WS_NEWV_A, WS_NEWV_B, WS_DSPCMD, WS_XACT, WS_STOPPASS, WS_STOPREL, WS_N };
static const char *const ws_name[WS_N] = { "newvoice1", "newvoice2", "dspcmd", "xactpkt", "stoppass", "stoprel" };
typedef struct { LONG n, w, oth; LONGLONG us, it; } WsSnap;
static struct {
    volatile LONG n, w, maxus, first, last, oth;     /* entries, entries that waited, max wait (us), thread ids */
    volatile LONGLONG us, it;                        /* total wait (us), spin iterations (dspcmd) */
} g_ws[WS_N];

/* One pass through a wait site: ms > 0 if it had to wait. iters = spin iterations, if counted. */
void apu_ws_record(int site, double ms, unsigned iters)
{
    if (site < 0 || site >= WS_N) return;
    LONG tid = (LONG)GetCurrentThreadId(), us = ms >= 2147483.0 ? 0x7FFFFFFF : (LONG)(ms * 1000.0);
    InterlockedIncrement(&g_ws[site].n);
    InterlockedCompareExchange(&g_ws[site].first, tid, 0);          /* the first thread to get here */
    if (g_ws[site].first != tid) InterlockedIncrement(&g_ws[site].oth);
    g_ws[site].last = tid;
    if (iters) InterlockedExchangeAdd64(&g_ws[site].it, iters);
    if (ms > 0.0) {
        if (us < 1) us = 1;
        InterlockedIncrement(&g_ws[site].w);
        InterlockedExchangeAdd64(&g_ws[site].us, us);
        if (us > g_ws[site].maxus) g_ws[site].maxus = us;           /* racy max is fine for a report */
    }
}

/* The DSP command ack spin of guest 0x003AA7DF (recomp_0095.c), counted. The loop is the lifted one:
 * re-read the doorbell word until the GP program has cleared it; no yield, no bound, no sleep. */
void apu_ws_dspcmd_spin(volatile uint32_t *doorbell)
{
    if (*doorbell == 0) { apu_ws_record(WS_DSPCMD, 0.0, 0); return; }
    unsigned long long it = 0;
    double t0 = apu_ws_now_ms();
    do { it++; } while (*doorbell != 0);
    apu_ws_record(WS_DSPCMD, apu_ws_now_ms() - t0, it > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)it);
}

/* APU-thread waits ([APU-WAIT]); only the APU thread calls apu_wt_add and reads these. */
enum { WT_THROTTLE, WT_BACKPRESSURE, WT_FETRAP, WT_DSPJOIN, WT_ISR, WT_N };
static const char *const wt_name[WT_N] = { "throttle", "backpressure", "fetrap", "dspjoin", "isr" };
static struct { unsigned n, w; double ms, max; } g_wt[WT_N];
static void wt_add(int id, double ms)
{
    g_wt[id].n++;
    if (ms > 0.0) { g_wt[id].w++; g_wt[id].ms += ms; if (ms > g_wt[id].max) g_wt[id].max = ms; }
}
void apu_wt_dspjoin(double ms) { wt_add(WT_DSPJOIN, ms); }

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* Physical addresses, resolved the way every other bus master here does it
 * (dma_resolve in nv2a_pb_exec.c, bus_resolve in usb/ohci.c).
 *
 * DirectSound builds its voice, SGE and notifier structures in
 * MmAllocateContiguousMemory and hands the APU their physical addresses.
 * Physical P and the contiguous window's 0x80000000 + P are the same bytes on
 * hardware; here the window is separate storage. Reading every address as low
 * RAM meant the voice processor walked zeroes and wrote each "voice done"
 * notification into ordinary RAM, where DirectSound never looked -- so a
 * buffer never reported that it had stopped, and Burnout 3's frontend waits
 * on exactly that (IDirectSoundBuffer::GetStatus, polled forever). */
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;
extern uint32_t xbox_ContiguousAllocatedBytes(void);

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    uint32_t a = (uint32_t)addr & 0x0FFFFFFFu;
    if(xbox_DmaIsMain(a))return g_apu_ram_ptr+(a-XBOX_DMA_MAIN_BASE);
    if (a >= g_xbox_image_lo && a < g_xbox_image_hi)
        return g_apu_ram_ptr + a;
    if (a < xbox_ContiguousAllocatedBytes())
        return g_apu_ram_ptr + 0x80000000u + a;
    return g_apu_ram_ptr + (a & 0x03FFFFFFu);
}

/* The interrupt line, as the frame thread sees it. update_irq used to call
 * pci_irq_assert, which is an empty stub here, so DirectSound's service
 * routine never ran and no voice completion ever reached it. */
static volatile LONG s_irq_line;

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 1);
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 0);
    }
}

/* ---- delivering it ---------------------------------------------------- */

/* HalGetInterruptVector(5) -- the APU's IRQ -- is what DirectSound connects
 * its service routine to (0x002F8E81 in Burnout 3). */
#define APU_VECTOR 5

typedef void (*apu_guest_fn)(void);
extern apu_guest_fn recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern uint32_t xbox_AllocThreadTib(void);
extern int xbox_IrqlBlocksInterrupt(int level);
extern int xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
extern int xbox_InterruptEnter(unsigned bus_irq, int level);
extern void xbox_InterruptLeave(unsigned bus_irq, int saved);
#if defined(_MSC_VER)
#  define APU_TLS __declspec(thread)
#else
#  define APU_TLS __thread
#endif
extern APU_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern APU_TLS uint32_t g_fs_base;

/* Call the connected service routine while the line is up, from the frame
 * thread, the way the OHCI model delivers USB interrupts (ohci_call_isr):
 * a worker stack for the call, a TIB of this thread's own, and a hold-off
 * while a guest thread sits at raised IRQL, which is when the single-CPU
 * console could not have taken the interrupt. The routine acknowledges by
 * writing ISTS, which drops the line through update_irq. */
static void apu_deliver_irq(MCPXAPUState *d)
{
    static int tib_ready;
    uint32_t kint, routine, context;
    apu_guest_fn fn;
    int slot;

    if (!InterlockedCompareExchange(&s_irq_line, 0, 0))
        return;
    if (xbox_IrqlBlocksInterrupt(27 - APU_VECTOR))
        return;
    kint = xbox_GetConnectedInterrupt(APU_VECTOR);
    if (!kint)
        return;
    routine = *(uint32_t *)(g_apu_ram_ptr + kint + 0);
    context = *(uint32_t *)(g_apu_ram_ptr + kint + 4);
    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn)
        return;
    if (!tib_ready) {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib)
            return;
        g_fs_base = tib;
        tib_ready = 1;
    }
    slot = xbox_worker_stack_alloc();
    if (slot < 0)
        return;

#if defined(__aarch64__) && !defined(_WIN32)
    if (!xbox_PosixMmioRegisterThread()) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[APU] cannot register IRQ thread for POSIX MMIO\n");
        }
        xbox_worker_stack_free(slot);
        return;
    }
#endif
    double ws_t0 = g_apu_ws ? apu_ws_now_ms() : 0.0;     /* [APU-WAIT] isr: time in the guest ISR */
    qemu_mutex_unlock(&d->lock);
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(g_apu_ram_ptr + g_esp) = 0xDEADBEEFu;
    {
        int saved = xbox_InterruptEnter(APU_VECTOR, *(uint32_t *)(g_apu_ram_ptr + kint + 12));
        fn();
        xbox_InterruptLeave(APU_VECTOR, saved);
    }
#if defined(__aarch64__) && !defined(_WIN32)
    if (!xbox_PosixMmioUnregisterThread()) abort();
#endif
    xbox_worker_stack_free(slot);
    if (g_apu_ws) wt_add(WT_ISR, apu_ws_now_ms() - ws_t0);
    qemu_mutex_lock(&d->lock);

    {
        static unsigned n;
        if (n++ < 3) {
            fprintf(stderr, "[APU] interrupt delivered to 0x%08X -> %s\n",
                    routine, (g_eax & 1) ? "claimed" : "declined");
            fflush(stderr);
        }
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* This is the output sample counter, not a Windows timer. DirectSound
         * uses it for stream cursors and refill deadlines. */
        r = (uint64_t)d->ep_frame_div * NUM_SAMPLES_PER_FRAME;
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* W-10: 1 while another window has focus and [Audio] MuteInBackground is on; the output ramps to 0. */
static volatile LONG g_apu_background;
void mcpx_apu_set_background(int background)
{
    if (InterlockedExchange(&g_apu_background, background ? 1 : 0) != (background ? 1 : 0))
        fprintf(stderr, "[AUDIO] %s\n", background ? "window in background: muted" : "window has focus: sound on");
}
/* 1 while mcpx_apu_hold holds the frame thread (the hidden game while its in-game editor is on screen). */
static volatile LONG g_apu_hold;
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };
static int16_t g_monitor_pending[WAVEOUT_BUF_SAMPLES][2];
static int g_monitor_filled;
static int g_output_silent;
static FILE *g_output_capture;
static uint32_t g_capture_bytes;

/* Capture the same PCM sent to the device, while optionally feeding the
 * device silence. Update the WAV header regularly: bounded diagnostic runs
 * can terminate without calling the APU finalizer. */
static void audio_capture_header(void)
{
    uint32_t header[11] = {0x46464952, g_capture_bytes + 36, 0x45564157,
        0x20746d66, 16, 0x00020001, 48000, 192000, 0x00100004,
        0x61746164, g_capture_bytes};
    if (!g_output_capture) return;
    fseek(g_output_capture, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), g_output_capture);
    fseek(g_output_capture, 0, SEEK_END);
    fflush(g_output_capture);
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;
    g_monitor_filled = 0;
    const char *silent = getenv("RECOMP_AUDIO_SILENT");
    const char *capture = getenv("RECOMP_AUDIO_CAPTURE");
    g_output_silent = silent && atoi(silent) != 0;
    g_capture_bytes = 0;
    if (capture && *capture) {
        g_output_capture = fopen(capture, "wb+");
        if (g_output_capture) {
            audio_capture_header();
            fprintf(stderr, "[AUDIO] Capturing 48 kHz PCM to %s%s\n", capture,
                g_output_silent ? " (device output silent)" : "");
        } else fprintf(stderr, "[AUDIO] Cannot open PCM capture: %s\n", capture);
    }

    /* The host backend receives the already mixed VP/DSP output. */
    if (xa2_init()) {
#if defined(_WIN32)
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
#else
        fprintf(stderr, "[APU] Using SDL2 audio backend\n");
#endif
        return;
    }
#if !defined(_WIN32)
    fprintf(stderr, "[APU] SDL2 audio unavailable; output is disabled\n");
    return;
#else
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
#endif
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    g_monitor_filled = 0;
    if (g_output_capture) {
        audio_capture_header();
        fclose(g_output_capture);
        g_output_capture = NULL;
    }
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* Preserve the 256 samples the VP/DSP just rendered. The software mixer
 * adds host DirectSound voices to them; it must not replace guest audio. */
void mcpx_apu_prepare_output_frame(MCPXAPUState *d, int16_t out[256][2])
{
    static int volume = -1;
    if (volume < 0) {
        const char *setting = getenv("RECOMP_AUDIO_VOLUME");
        volume = setting ? atoi(setting) : 100;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
    }
    memcpy(out, d->monitor.frame_buf, sizeof(d->monitor.frame_buf));
    if (g_audio_muted) {
        memset(out, 0, sizeof(d->monitor.frame_buf));
        return;
    }
    if (g_test_tone.active) {
        for (int i = 0; i < 256; i++) {
            int16_t sample = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
            out[i][0] = out[i][1] = sample;
            g_test_tone.phase += g_test_tone.phase_inc;
            if (g_test_tone.phase >= 2.0 * M_PI) g_test_tone.phase -= 2.0 * M_PI;
        }
    }
    mixer_render(out, 256);
    /* A live volume change ramps over 27 ms to avoid a discontinuity. */
    static float gain=-1.0f;
    float target=(g_apu_background?0:pc_setting_get(PC_VOLUME,volume))/100.0f;   /* W-10 mute in background */
    if(gain<0)gain=target;
    for(int i=0;i<256;i++) {
        float difference=target-gain;
        float step=1.0f/1280.0f;
        gain+=difference>step?step:difference < -step?-step:difference;
        if(gain!=1.0f)for(int c=0;c<2;c++)out[i][c]=(int16_t)(out[i][c]*gain);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    int16_t (*pending)[2] = g_monitor_pending;
    int16_t frame[256][2];
    int target;
    if ((d->ep_frame_div + 1) % 8) return;
    if (!xa2_is_active() && !g_waveout.initialized) return;

    mcpx_apu_prepare_output_frame(d, frame);
    if (g_output_capture) {
        uint32_t previous = g_capture_bytes;
        g_capture_bytes += (uint32_t)fwrite(frame, 1, sizeof(frame), g_output_capture);
        if (previous / 192000 != g_capture_bytes / 192000)
            audio_capture_header();
    }
    /* One DSP block is 256 samples, regardless of the backend's buffer size.
     * Collect consecutive blocks to keep the original 48 kHz timing. */
    target = xa2_is_active() ? xa2_get_buffer_size() : WAVEOUT_BUF_SAMPLES;
    if (target < 256 || target > WAVEOUT_BUF_SAMPLES || target % 256) return;
    memcpy(pending + g_monitor_filled, frame, sizeof(frame));
    g_monitor_filled += 256;
    if (g_monitor_filled < target) return;

    static int stats=-1;
    if(stats<0)stats=getenv("RECOMP_AUDIO_STATS")!=NULL;
    if(stats) {
        static DWORD last;
        static unsigned blocks, peak;
        DWORD now = GetTickCount();
        blocks++;
        for (int i = 0; i < target; i++)
            for (int channel = 0; channel < 2; channel++) {
                unsigned magnitude = (unsigned)abs((int)pending[i][channel]);
                if (magnitude > peak) peak = magnitude;
            }
        if (now - last >= 5000) {
            fprintf(stderr, "[AUDIO] %u output blocks, peak=%u/32768\n", blocks, peak);
            fflush(stderr);
            last = now; blocks = peak = 0;
        }
    }
    if (g_output_silent) memset(pending, 0, (size_t)target * sizeof(pending[0]));
    if (xa2_is_active()) {
        while (!xa2_submit_samples((const int16_t *)pending, target)) {
            /* Preserve every sample under backpressure; the guest audio
             * worker still needs the APU lock to refill streaming voices. */
            double ws_t0 = g_apu_ws ? apu_ws_now_ms() : 0.0;
            qemu_mutex_unlock(&d->lock);
            int ready = xa2_wait_for_space(10);
            qemu_mutex_lock(&d->lock);
            if (g_apu_ws) wt_add(WT_BACKPRESSURE, apu_ws_now_ms() - ws_t0);
            if (ready < 0 || qatomic_read(&d->exiting)) break;
        }
    } else {
        int idx = g_waveout.next_buf;
        WAVEHDR *hdr = &g_waveout.hdrs[idx];
        /* FT-RT: the wait for the oldest buffer is bounded (it was unbounded, so a removed or hung output
         * device froze the APU thread and, through the guest audio worker, the game). After
         * WAVEOUT_STALL_MS without a free buffer the block is dropped, like the XAudio2 path does, and
         * while the device stays stuck later blocks are dropped without waiting (one buffer check each);
         * the first free buffer ends the stall. RECOMP_TEST_WAVEOUT_STALL_S=<s> (test only) treats
         * every buffer as busy for 4 s starting <s> seconds after the first block. */
        static int wo_stalled;
        static unsigned wo_dropped, wo_stalls;
        static DWORD wo_first, wo_test_stall_s = (DWORD)-1;
        if (wo_test_stall_s == (DWORD)-1) {
            const char *hook = getenv("RECOMP_TEST_WAVEOUT_STALL_S");
            wo_test_stall_s = hook && *hook ? (DWORD)atoi(hook) : 0;
            wo_first = GetTickCount();
        }
        #define WAVEOUT_STALL_MS 250
        #define WAVEOUT_BUSY(h) (((!((h)->dwFlags & WHDR_DONE) && ((h)->dwFlags & WHDR_INQUEUE))) ||             (wo_test_stall_s && GetTickCount() - wo_first >= wo_test_stall_s * 1000u &&              GetTickCount() - wo_first < wo_test_stall_s * 1000u + 4000u))
        if (WAVEOUT_BUSY(hdr) && !wo_stalled) {
            DWORD wait_start = GetTickCount();
            while (WAVEOUT_BUSY(hdr)) {
                double ws_t0 = g_apu_ws ? apu_ws_now_ms() : 0.0;
                qemu_mutex_unlock(&d->lock);
                Sleep(1);
                qemu_mutex_lock(&d->lock);
                if (g_apu_ws) wt_add(WT_BACKPRESSURE, apu_ws_now_ms() - ws_t0);
                if (qatomic_read(&d->exiting)) { g_monitor_filled = 0; return; }
                if (GetTickCount() - wait_start >= WAVEOUT_STALL_MS) {
                    wo_stalled = 1; wo_stalls++;
                    fprintf(stderr, "[APU] waveOut stalled %d ms, dropping blocks (stall #%u)\n", WAVEOUT_STALL_MS, wo_stalls);
                    break;
                }
            }
        }
        if (WAVEOUT_BUSY(hdr)) {
            wo_dropped++;
            g_monitor_filled = 0;
            return;
        }
        if (wo_stalled) {
            wo_stalled = 0;
            fprintf(stderr, "[APU] waveOut recovered (%u blocks dropped so far)\n", wo_dropped);
        }
        #undef WAVEOUT_BUSY
        #undef WAVEOUT_STALL_MS
        memcpy(g_waveout.bufs[idx], pending, (size_t)target * 2 * sizeof(int16_t));
        hdr->dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));
        g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
        g_waveout.frames_written++;
    }
    g_monitor_filled = 0;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

static void throttle(MCPXAPUState *d, HANDLE timer)
{
    if (d->ep_frame_div % 8) {
        return;
    }

    double ws_t0 = g_apu_ws ? apu_ws_now_ms() : 0.0;     /* [APU-WAIT] throttle: whole call, per 8th frame */
    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > 250000) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_us = d->next_frame_time_us - now_us;
        if (remaining_us > 0) {
#if defined(_WIN32)
            if(timer) {
                LARGE_INTEGER due;
                due.QuadPart=-remaining_us*10;
                if(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE)) {
                    qemu_mutex_unlock(&d->lock);
                    WaitForSingleObject(timer,10);
                    qemu_mutex_lock(&d->lock);
                    if(qatomic_read(&d->exiting))break;
                    continue;
                }
            }
#endif
            int remaining_ms=(int)((remaining_us+999)/1000);
            qemu_cond_timedwait(&d->cond, &d->lock, remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
    if (g_apu_ws) wt_add(WT_THROTTLE, apu_ws_now_ms() - ws_t0);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

/* Every 5 s of wall time, on the APU thread, with RECOMP_AUDIO_WAIT_STATS / RECOMP_PERF: the A0 lines.
 * Counts are deltas since the last line; each module reports its own through apu_*_report. */
extern void apu_veh_report(double secs);        /* apu_mmio_hook.c */
extern void apu_src_report(double secs);        /* apu_vp.c */
extern void apu_dsp_pram_report(double secs);   /* apu_dsp_runtime.c */
static void apu_stats_tick(void)
{
    static double last;
    static WsSnap prev[WS_N];
    double now = apu_ws_now_ms();
    if (last == 0.0) { last = now; return; }
    if (now - last < 5000.0) return;
    double secs = (now - last) / 1000.0;
    last = now;

    char line[1024];
    int len = snprintf(line, sizeof(line), "[AUDIO-WAITS] %.1f s:", secs);
    for (int i = 0; i < WS_N && len < (int)sizeof(line) - 160; i++) {
        LONG n = g_ws[i].n, w = g_ws[i].w, oth = g_ws[i].oth;
        LONGLONG us = g_ws[i].us, it = g_ws[i].it;
        LONG maxus = InterlockedExchange(&g_ws[i].maxus, 0);
        len += snprintf(line + len, sizeof(line) - len, "%s %s n=%ld w=%ld %.2f ms max %.2f",
                        i ? " |" : "", ws_name[i], (long)(n - prev[i].n), (long)(w - prev[i].w),
                        (double)(us - prev[i].us) / 1000.0, maxus / 1000.0);
        if (i == WS_DSPCMD) len += snprintf(line + len, sizeof(line) - len, " iters=%lld", it - prev[i].it);
        len += snprintf(line + len, sizeof(line) - len, " tid=%ld oth=%ld",
                        (long)g_ws[i].last, (long)(oth - prev[i].oth));
        prev[i].n = n; prev[i].w = w; prev[i].oth = oth; prev[i].us = us; prev[i].it = it;
    }
    fprintf(stderr, "%s\n", line);

    len = snprintf(line, sizeof(line), "[APU-WAIT] %.1f s:", secs);
    for (int i = 0; i < WT_N; i++) {
        len += snprintf(line + len, sizeof(line) - len, "%s %s n=%u w=%u %.2f ms max %.2f", i ? " |" : "", wt_name[i],
                        g_wt[i].n, g_wt[i].w, g_wt[i].ms, g_wt[i].max);
        memset(&g_wt[i], 0, sizeof(g_wt[i]));
    }
    fprintf(stderr, "%s\n", line);
    apu_veh_report(secs);
    apu_src_report(secs);
    apu_dsp_pram_report(secs);
    fflush(stderr);
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    HANDLE timer=NULL;
    pc_thread_role_enter(PC_ROLE_APU);
#if defined(_WIN32)
    DWORD task_index=0;
    HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task_index);
    if(mmcss)AvSetMmThreadPriority(mmcss,AVRT_PRIORITY_HIGH);
    /* A millisecond condition-variable timeout drifts at a 5.333ms audio
     * cadence. A high-resolution timer preserves the absolute deadline. */
    timer=CreateWaitableTimerExW(NULL,NULL,0x00000002,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(NULL,FALSE,NULL);
#endif
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        /* mcpx_apu_hold: parked between two frames (never inside the ISR or an output wait), so the guest
         * finds the APU where it left it when the hold ends. A GP/EP frame still running on the DSP thread
         * finishes on its own and is joined by the next frame as usual. */
        if (InterlockedCompareExchange(&g_apu_hold, 0, 0)) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        if (g_apu_ws) apu_stats_tick();

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d,timer);

        /* The doorbell ack stands in for the GP DSP, which on hardware runs
         * whatever the front end is doing. Tying it to se_frame stopped it
         * whenever FECTL was trapped or halted, and DirectSound then waits
         * forever to post its next command: Burnout 3 stalls in
         * sub_002F805E polling the same doorbell it was acked on at init. */
        mcpx_apu_dsp_ack_poll(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        if(xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF &&
           (fectl & NV_PAPU_FECTL_FEMETHMODE)) {
            /* An idle-voice trap or a parameter update suspends the front
             * end until DirectSound services it. Do not advance the voice
             * clock by feeding zeros through the DSP: that inserted sharp
             * gaps into every active sound while the interrupt was pending.
             * Service it now, then wait with the lock released if necessary,
             * as xemu's sequencer does. Existing queued PCM bridges the wait. */
            update_irq(d);apu_deliver_irq(d);
            fectl=qatomic_read(&d->regs[NV_PAPU_FECTL]);
            if(fectl & NV_PAPU_FECTL_FEMETHMODE) {
                double ws_t0 = g_apu_ws ? apu_ws_now_ms() : 0.0;
                qemu_cond_timedwait(&d->cond,&d->lock,1);
                if (g_apu_ws) wt_add(WT_FETRAP, apu_ws_now_ms() - ws_t0);
                continue;
            }
        }
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            /* Before the sequencer starts, initialization DSP commands and
             * software DirectSound voices still need the output clock. */
            if(mcpx_apu_dsp_enabled()) {
                float silence[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]={0};
                mcpx_apu_dsp_native_frame(d,silence);
            }
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }

        /* What xemu's frame thread does after each frame: turn a pending
         * notification (set by the voice processor or a trapped method) into
         * the interrupt line. Nothing here ever did, so the line never rose
         * even before there was anything to deliver it to. */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        apu_deliver_irq(d);

        /* Let the guest in once per frame.
         *
         * The thread holds d->lock for its whole loop and only drops it inside
         * throttle()'s wait. Once voices really play, processing can run
         * behind real time, throttle never waits, and the lock is never
         * released -- while every VOICE_ON/OFF/RELEASE the title writes needs
         * it (voice_lock). A critical section is not fair, so the title's
         * thread starved there indefinitely: Burnout 3 froze on its vehicle
         * select, blocked in voice_lock at raised IRQL, which in turn held
         * off every USB interrupt. */
        qemu_mutex_unlock(&d->lock);
        SwitchToThread();
        qemu_mutex_lock(&d->lock);
    }

    qemu_mutex_unlock(&d->lock);
#if defined(_WIN32)
    if(timer)CloseHandle(timer);
    if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
#endif
    pc_thread_role_leave();
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* Hold (1) or release (0) the whole audio emulation: the frame thread parks at the top of its loop, so the
 * voice processor, the GP/EP DSP frames (their thread only runs when the frame thread hands it one), the
 * doorbell ack, the output and the interrupt delivery all stop; nothing is reset or dropped. Used by the
 * scenario code only while the game is hidden and idle behind its in-game editor (ma2_scenario.c live
 * session), where it cut the hidden game from ~55% of a core to almost nothing. Not the title's own
 * pause_requested (FECTL/SECTL and the mixer clear that one). Any thread; returns at once. */
void mcpx_apu_hold(int hold)
{
    MCPXAPUState *d = g_state;
    const LONG want = hold ? 1 : 0;
    if (InterlockedExchange(&g_apu_hold, want) == want || !d) return;
    if (!want) {
        /* under the lock: the frame thread checks the flag and waits under it, so this wake cannot be lost */
        qemu_mutex_lock(&d->lock);
        qemu_cond_broadcast(&d->cond);
        qemu_mutex_unlock(&d->lock);
    }
    fprintf(stderr, "[APU] audio emulation %s\n", want ? "held (hidden game behind the editor)" : "running again");
    fflush(stderr);
}

#if defined(__aarch64__) && !defined(_WIN32)
int mcpx_apu_quiesce(MCPXAPUState *d)
{
    if (!d || d != g_state) return 0;
    InterlockedExchange(&g_apu_hold, 1);
    unsigned attempts = 0;
    while (!TryEnterCriticalSection(&d->lock.cs)) {
        if (++attempts == 5000) return 0;
        Sleep(1);
    }
    qemu_cond_broadcast(&d->cond);
    for (unsigned i = 0; !d->is_idle && i < 50; ++i)
        qemu_cond_timedwait(&d->idle_cond, &d->lock, 100);
    int idle = d->is_idle;
    qemu_mutex_unlock(&d->lock);
    return idle;
}
#endif

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
#if !defined(_WIN32)
    if (ram_ptr && (ram_ptr != xbox_GetMemoryBase() ||
                    !xbox_McpxMemoryAlias(0x30000u, 0x30000u))) {
        fprintf(stderr, "[APU] guest RAM and MCPX alias must be initialized first\n");
        return NULL;
    }
#endif
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;
    {   /* A0 counters: read the switches once, before any thread uses them */
        LARGE_INTEGER f;
        const char *e = getenv("RECOMP_AUDIO_WAIT_STATS");
        QueryPerformanceFrequency(&f);
        g_ws_ms_per_tick = 1000.0 / (double)f.QuadPart;
        g_apu_ws = (e && atoi(e) != 0) || getenv("RECOMP_PERF") != NULL;
    }

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init programmable audio processors. */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

#if defined(__aarch64__) && !defined(_WIN32)
    if (xbox_PosixMmioIsBound()) {
        fprintf(stderr, "[APU] POSIX MMIO remains attached; APU retained\n");
        return;
    }
#endif
    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
#if defined(__aarch64__) && !defined(_WIN32)
    InterlockedExchange(&g_apu_hold, 0);
#endif
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);
    mcpx_apu_dsp_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

#if defined(__aarch64__) && !defined(_WIN32)
int mcpx_apu_shutdown_checked(MCPXAPUState *d)
{
    if (d && xbox_PosixMmioIsBound()) return 0;
    mcpx_apu_shutdown(d);
    return 1;
}
#endif

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if ((addr>=0x3F000 && addr<0x40000) ||
               (addr>=0x5F000 && addr<0x60000)) {
        if(is_write)mcpx_apu_dsp_mmio_write(d,(uint32_t)addr,(uint32_t)val);
    }
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    } else if ((addr>=0x3F000 && addr<0x40000) ||
               (addr>=0x5F000 && addr<0x60000)) {
        return mcpx_apu_dsp_mmio_read(d,(uint32_t)addr);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
