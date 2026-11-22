/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
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

#include "../platform/host_cpu.h"
#include "apu_state.h"
#include "fpconv.h"
#include <stdarg.h>

#if defined(_MSC_VER)
#define APU_VP_TLS __declspec(thread)
#else
#define APU_VP_TLS __thread
#define _InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define _InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedExchangeAdd64(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedOr64(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedAnd64(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
static long apu_vp_compare_exchange(volatile long *p, long value, long expected)
{
    __atomic_compare_exchange_n(p, &expected, value, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
#define _InterlockedCompareExchange(p, v, c) apu_vp_compare_exchange((p), (v), (c))
#endif

/* Optional, low-frequency integration telemetry. No per-voice clocks. */
static int tail_stats_enabled = -1;
static struct {
    unsigned frames, active_reads, tail_reads, filter_visits, peak_voices;
    unsigned starts, silent_skips, finishes, guards, over_budget;
    long long ticks, max_ticks;
} tail_stats;
static LARGE_INTEGER tail_stats_frequency;
static DWORD tail_stats_last;
static long long tail_stats_begin(void)
{
    if (tail_stats_enabled < 0) {
        tail_stats_enabled = getenv("RECOMP_AUDIO_TAIL_STATS") != NULL;
        if (tail_stats_enabled) QueryPerformanceFrequency(&tail_stats_frequency);
    }
    LARGE_INTEGER t; if (!tail_stats_enabled) return 0;
    QueryPerformanceCounter(&t); return t.QuadPart;
}
static void tail_stats_end(long long start, unsigned voices)
{
    if (!start) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    long long elapsed = t.QuadPart - start;
    tail_stats.frames++; tail_stats.ticks += elapsed;
    if (elapsed > tail_stats.max_ticks) tail_stats.max_ticks = elapsed;
    if (voices > tail_stats.peak_voices) tail_stats.peak_voices = voices;
    if ((double)elapsed * 48000 > (double)tail_stats_frequency.QuadPart * NUM_SAMPLES_PER_FRAME)
        tail_stats.over_budget++;
    DWORD now = GetTickCount();
    if (now - tail_stats_last >= 5000) {
        fprintf(stderr,"[APU-TAIL] tick=%lu frames=%u active-src=%u tail-src=%u filter-only=%u peak-tails=%u starts=%u silent-skips=%u finishes=%u guards=%u avg-us=%.3f max-us=%.3f over667=%u\n",
            (unsigned long)now,tail_stats.frames,tail_stats.active_reads,tail_stats.tail_reads,
            tail_stats.filter_visits,tail_stats.peak_voices,tail_stats.starts,
            tail_stats.silent_skips,tail_stats.finishes,tail_stats.guards,
            1e6*tail_stats.ticks/tail_stats_frequency.QuadPart/tail_stats.frames,
            1e6*tail_stats.max_ticks/tail_stats_frequency.QuadPart,tail_stats.over_budget);
        memset(&tail_stats,0,sizeof(tail_stats));tail_stats_last=now;
    }
}

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters_now(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    d->vp.filters[v].src_eoi = false;
    /* A new voice on this handle replaces any tail still draining. */
    d->vp.filters[v].ended_by_data = false;
    d->vp.filters[v].drain_blocks = 0;
    d->vp.filters[v].tail_input_remaining = 0;
    d->vp.filters[v].resample_rate = 0;
    d->vp.filters[v].resample_previous_rate = 0;
}

/* VOICE_ON / OFF / RELEASE / PAUSE arrive from the title's register writes on the game thread,
 * without d->lock, while the APU thread may be inside this voice's resampler (it holds d->lock
 * for the frame). Resetting libsamplerate under it corrupted the converter: an integer divide by
 * zero in sinc_hex_vari_process, seen when sounds restart rapidly. The game thread only asks;
 * the APU thread does the reset at the start of its next frame, before it touches the voice. */
static volatile long g_reset_req[MCPX_HW_MAX_VOICES];
static volatile long g_reset_any;
static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    (void)d;
    assert(v < MCPX_HW_MAX_VOICES);
    _InterlockedExchange(&g_reset_req[v], 1);
    _InterlockedExchange(&g_reset_any, 1);
}
static void voice_apply_reset_requests(MCPXAPUState *d)
{
    if (!_InterlockedExchange(&g_reset_any, 0)) return;
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (_InterlockedExchange(&g_reset_req[v], 0)) voice_reset_filters_now(d, (uint16_t)v);
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

static bool stream_diagnostics_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_APU_STREAM_STATS") != NULL;
    return enabled != 0;
}

static struct {
    uint64_t missing_samples;
    unsigned gaps,reported_gaps;
    int last_report;
    bool empty;
} stream_counters[MCPX_HW_MAX_VOICES];

/* Output-device statistics cannot detect a source stream running dry: the
 * resampler deliberately supplies silence in that case. Observe it separately
 * without changing the guest's notifications or sample consumption. */
static void stream_diagnostics(MCPXAPUState *d, uint16_t v, int source_samples)
{
    #define counters stream_counters
    if (!stream_diagnostics_enabled()) return;
    uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, UINT32_MAX);
    if (!(fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) ||
        !(fmt & NV_PAVS_VOICE_CFG_FMT_PERSIST) ||
        !voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) ||
        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED)) return;
    bool empty = source_samples < NUM_SAMPLES_PER_FRAME;
    if (empty) {
        counters[v].missing_samples += NUM_SAMPLES_PER_FRAME - source_samples;
        if (!counters[v].empty) counters[v].gaps++;
    }
    counters[v].empty = empty;
    if ((empty && counters[v].reported_gaps < 8 &&
         counters[v].reported_gaps != counters[v].gaps) ||
        (counters[v].missing_samples && d->ep_frame_div - counters[v].last_report >= 7500)) {
        fprintf(stderr, "[APU-STREAM] frame=%d voice=%u fmt=%08X gaps=%u missing-source-samples=%llu ssl=%d segment=%d counts=%d/%d\n",
            d->ep_frame_div, v, fmt, counters[v].gaps,
            (unsigned long long)counters[v].missing_samples,
            d->vp.ssl[v].ssl_index, d->vp.ssl[v].ssl_seg,
            d->vp.ssl[v].count[0], d->vp.ssl[v].count[1]);
        counters[v].last_report = d->ep_frame_div;
        counters[v].reported_gaps = counters[v].gaps;
    }
    #undef counters
}

static void stream_trace(MCPXAPUState *d,uint16_t voice,hwaddr address,uint32_t descriptor,uint32_t cbo)
{
    static int selected=-2;static unsigned records;
    if(selected==-2) {const char *value=getenv("RECOMP_APU_STREAM_TRACE");selected=value?atoi(value):-1;}
    if(selected!=(int)voice || cbo || records>=512)return;
    records++;
    char bytes[33]={0};
    if(((descriptor>>16)&3)==NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
        const uint8_t *head=mcpx_apu_phys(address);
        for(unsigned i=0;i<16;i++)snprintf(bytes+i*2,3,"%02X",head[i]);
    }
    fprintf(stderr,"[APU-SSL] frame=%d voice=%u index=%d segment=%d address=%08X descriptor=%08X pitch=%04X counts=%d/%d head=%s\n",
        d->ep_frame_div,voice,d->vp.ssl[voice].ssl_index,d->vp.ssl[voice].ssl_seg,
        (unsigned)address,descriptor,
        voice_get_mask(d,voice,NV_PAVS_VOICE_TAR_PITCH_LINK,NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH),
        d->vp.ssl[voice].count[0],d->vp.ssl[voice].count[1],bytes);
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

/* RECOMP_APU_VOICE_TRACE=1 (diagnostic, default off): one [VT] line per stream-voice event (SSL A/B
 * writes, on, off with the source line, release, pause, segment flips, a persist stream running dry)
 * so a speech line that never plays can be followed from the title's PIO writes to the samples. */
static int voice_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_APU_VOICE_TRACE") != NULL;
    return enabled;
}
static void voice_trace(MCPXAPUState *d, uint16_t v, const char *event, int line)
{
    if (!voice_trace_enabled() || v >= MCPX_HW_MAX_VOICES) return;
    uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, UINT32_MAX);
    if (!(fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE)) return;
    fprintf(stderr, "[VT] f=%d v=%u %s L%d fmt=%08X act=%u idx=%d seg=%d counts=%d/%d bases=%d/%d cbo=%u ea=%u\n",
        d->ep_frame_div, v, event, line, fmt,
        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
        d->vp.ssl[v].ssl_index, d->vp.ssl[v].ssl_seg, d->vp.ssl[v].count[0], d->vp.ssl[v].count[1],
        d->vp.ssl[v].base[0], d->vp.ssl[v].base[1],
        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_EACUR));
    if (getenv("RECOMP_APU_VOICE_TRACE_RA") && (event[0] == 's' || !strcmp(event, "on"))) {   /* the guest code that asked */
        extern APU_VP_TLS uint32_t g_esp; int n = 0; fprintf(stderr, "[VT-RA] v=%u %s", v, event);
        for (uint32_t k = 0; k < 200 && n < 10 && g_esp > 0x1000u && g_esp < 0x3F00000u; k++) { uint32_t x = *(uint32_t *)(g_apu_ram_ptr + g_esp + k * 4);
            if (x >= 0x12000u && x < 0x3E9000u) { fprintf(stderr, " %08X", x); n++; } }
        fprintf(stderr, "\n");
    }
}
static bool voice_trace_dry[MCPX_HW_MAX_VOICES];

#define voice_off(d, v) voice_off_at(d, v, __LINE__)
static void voice_off_at(MCPXAPUState *d, uint16_t v, int line)
{
    voice_trace(d, v, "off", line);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* VOICE_LOCK used to take d->lock, which the APU thread holds for its whole frame: voices, the
 * GP/EP DSP programs and the output. Every lock and unlock the title writes (DirectSound wraps
 * each voice update in a pair, many per game frame) waited for that frame to finish, and a
 * critical section is not fair, so it could wait several. Where the APU keeps up with real time
 * with little to spare (an i7-1260P laptop on Mission 1, 2026-10-15) the game thread spent most
 * of its time there, and the game ran at 20-25 fps with the GPU 80% idle.
 *
 * It also bought nothing: the title's updates after the lock ran without d->lock, alongside the
 * next frame. Now the lock is a handshake on the one voice. The VP publishes the voice it is
 * about to mix (g_vp_voice) and does not start one the title holds locked (vp_enter waits a
 * little, as hardware stalls on it); the title sets the bit, then waits only while the VP is
 * inside that voice -- microseconds. RECOMP_APU_VOICE_LOCK=frame restores the old wait (A/B). */
static volatile long g_vp_voice = -1;
static DWORD g_vp_thread;
static int voice_lock_frame(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_APU_VOICE_LOCK"); v = e && !strcmp(e, "frame"); }
    return v;
}
/* RECOMP_PERF: the title's time in voice_lock, one line per 5 s. */
static struct { int on; LARGE_INTEGER f; DWORD last; long calls; long long wait, max; } vls = { -1 };
static void voice_lock_stat(LARGE_INTEGER t0)
{
    if (vls.on < 0) { vls.on = getenv("RECOMP_PERF") != NULL; QueryPerformanceFrequency(&vls.f); }
    if (!vls.on) return;
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    long long w = t1.QuadPart - t0.QuadPart;
    _InterlockedIncrement(&vls.calls); _InterlockedExchangeAdd64(&vls.wait, w);
    if (w > vls.max) vls.max = w;
    DWORD now = GetTickCount();
    if (!vls.last) vls.last = now;
    if (now - vls.last >= 5000) {
        double ms = 1000.0 / (double)vls.f.QuadPart;
        fprintf(stderr, "[APU-LOCK] %ld voice locks in %.1f s; title waited %.1f ms (max %.2f ms)%s\n",
                vls.calls, (now - vls.last) / 1000.0, vls.wait * ms, vls.max * ms, voice_lock_frame() ? " [frame lock]" : "");
        vls.last = now; vls.calls = 0; vls.wait = vls.max = 0;
    }
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    if (voice_lock_frame()) {
        qemu_mutex_lock(&d->lock);
        if (lock) {
            d->vp.voice_locked[v / 64] |= mask;
        } else {
            d->vp.voice_locked[v / 64] &= ~mask;
        }
        qemu_cond_signal(&d->cond);
        qemu_mutex_unlock(&d->lock);
        voice_lock_stat(t0);
        return;
    }
    if (lock) _InterlockedOr64((volatile int64_t *)&d->vp.voice_locked[v / 64], (int64_t)mask);
    else _InterlockedAnd64((volatile int64_t *)&d->vp.voice_locked[v / 64], (int64_t)~mask);
    /* Interlocked both sides: the VP stores g_vp_voice then reads the bit, this stores the bit
     * then reads g_vp_voice, so one of them always sees the other. */
    if (lock && GetCurrentThreadId() != g_vp_thread) {
        for (unsigned spin = 0; _InterlockedCompareExchange(&g_vp_voice, -1, -1) == (long)v; spin++) {
            if (spin < 512) xbox_host_cpu_relax(); else SwitchToThread();
        }
    }
    voice_lock_stat(t0);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* The VP's half of the voice_lock handshake, around each voice it mixes. A title holds a voice
 * locked only for a few register writes, so wait for it briefly (hardware stalls the VP there);
 * past a small per-frame budget mix it anyway, as before, so a title that holds a lock cannot
 * stop the audio. */
static unsigned g_vp_wait_budget;
static void vp_enter(MCPXAPUState *d, uint16_t v)
{
    _InterlockedExchange(&g_vp_voice, (long)v);
    if (voice_lock_frame()) return;
    while (is_voice_locked(d, v) && g_vp_wait_budget) {
        _InterlockedExchange(&g_vp_voice, -1);   /* step out so the title's lock can complete */
        for (int i = 0; i < 16; i++) xbox_host_cpu_relax();
        g_vp_wait_budget--;
        _InterlockedExchange(&g_vp_voice, (long)v);
    }
}
static void vp_leave(void) { _InterlockedExchange(&g_vp_voice, -1); }

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    /* Opt-in experiment: infer sign-magnitude from the title bytes. Read as
     * two's complement, the title's tables have smooth tails that jump from
     * small values to about -125 (0x86 is -6, not -122): 89 of 96 traced
     * ear responses are smoother as sign-magnitude. 0x80 is negative zero. */
    uint8_t raw = (uint8_t)value;
    /* Keep signed zero so the raw-byte trace can recover 0x80 exactly.
     * Sign-magnitude encoding is an experimental inference from the
     * captured title tables; absolute hardware gain is not established. */
    float tap = (raw & 0x80) ? -(float)(raw & 0x7F) : (float)raw;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = tap / 128.0f;
}

/* Exact inverse of the experimental sign-magnitude byte decode above.
 * Coefficients are copied before filter normalization; every magnitude
 * is an exact multiple of 1/128, including preserved negative zero.
 * Diagnostics retain the ORIGINAL byte rendered as signed decimal. */
static int8_t hrir_raw_byte(float value)
{
    uint8_t raw = (uint8_t)(fabsf(value) * 128.0f);
    if (signbit(value)) raw |= 0x80u;
    return (int8_t)raw;
}

/* Explicit opt-in, at most96 diagnostic lines for this process. The current
 * serialized APU PIO path owns this cache/counter. Never advances after cap. */
static bool gain_trace(void)
{
    static int enabled = -1;
    static unsigned lines;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_APU_GAIN_TRACE");
        enabled = value && !strcmp(value, "1");
    }
    if (!enabled || lines >= 96u) return false;
    ++lines;
    return true;
}

/* RECOMP_APU_MIX_TRACE=1 observes active post-filter voice blocks only,
 * before voice_mix_block. It does not measure decoder output, tail blocks,
 * the actual mix waveform, GP/EP output, or speaker clipping. Peak sums are
 * conservative upper bounds for the observed active contributions.
 * Caps per process: 8 submix, 32 routing-topology, 48 distinct raw HRIR,
 * 120 summaries (7500 APU frames each, nominally5s). No new audio math.
 * PIO and frame callbacks can overlap: logger state uses a nonblocking
 * gate; contended diagnostic events are omitted rather than waiting. */
static volatile LONG mix_trace_busy, mix_trace_done;
static bool mix_trace(void)
{
#if defined(_MSC_VER)
    static APU_VP_TLS int enabled = -1;
#else
    static __thread int enabled = -1;
#endif
    if (enabled < 0) {
        const char *value = getenv("RECOMP_APU_MIX_TRACE");
        enabled = value && !strcmp(value, "1");
    }
    return enabled && !InterlockedCompareExchange(&mix_trace_done,0,0);
}
static bool mix_trace_enter(void)
{
    if (InterlockedCompareExchange(&mix_trace_busy,1,0)!=0) return false;
    if (InterlockedCompareExchange(&mix_trace_done,0,0)) {
        InterlockedExchange(&mix_trace_busy,0); return false;
    }
    return true;
}
static void mix_trace_leave(void)
{
    InterlockedExchange(&mix_trace_busy,0);
}

/* A single stdio call keeps each bounded line together with other traces. */
static void mix_trace_append(char *line,size_t cap,size_t *used,const char *fmt,...)
{
    if (*used>=cap-1) return;
    va_list args; va_start(args,fmt);
    int n=vsnprintf(line+*used,cap-*used,fmt,args); va_end(args);
    if(n>0) *used+=(size_t)n<cap-*used ? (size_t)n : cap-*used-1;
}

static const uint8_t mix_trace_bin[] = {2, 3, 6, 7, 10};
#define MIX_TRACE_BINS ((int)(sizeof(mix_trace_bin) / sizeof(mix_trace_bin[0])))

static struct {
    unsigned frames, src_over;
    float src_peak;
    unsigned n[MIX_TRACE_BINS], n3d[MIX_TRACE_BINS], nmp[MIX_TRACE_BINS], dup[MIX_TRACE_BINS];
    unsigned fvoices[MIX_TRACE_BINS], max_fvoices[MIX_TRACE_BINS];
    float gmax[MIX_TRACE_BINS], vmax[MIX_TRACE_BINS], fsum[MIX_TRACE_BINS], max_fsum[MIX_TRACE_BINS];
    uint8_t sig[32][22]; unsigned sigs, submix_lines, hrir_lines, summaries;
    int8_t hrir[48][2][HRTF_NUM_TAPS]; float itd[48];
} mt;

static void mix_trace_voice(uint16_t v, bool multipass, int mp_bin,
                            bool clear_mix, int voice_list, unsigned channels,
                            const uint8_t bins[8], const float gains[8],
                            const uint16_t vol[8], float samples[][2])
{
    if (channels<1 || channels>2 || !mix_trace_enter()) return;
    float peak = 0;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        for (unsigned ch = 0; ch < channels; ch++)
            if (fabsf(samples[i][ch]) > peak) peak = fabsf(samples[i][ch]);
    if (!multipass) {
        if (peak > mt.src_peak) mt.src_peak = peak;
        if (peak > 1.0f) mt.src_over++;
    }
    bool watched = false;
    for (int k = 0; k < MIX_TRACE_BINS; k++) {
        unsigned slots = 0; float level = 0;
        for (int b = 0; b < 8; b++) {
            if (bins[b] != mix_trace_bin[k] || gains[b] == 0) continue;
            slots++; level += gains[b] * peak;
            if (gains[b] > mt.gmax[k]) mt.gmax[k] = gains[b];
        }
        if (!slots) continue;
        watched = true;
        mt.n[k] += slots;
        if (v < MCPX_HW_MAX_3D_VOICES) mt.n3d[k] += slots;
        if (multipass) mt.nmp[k] += slots;
        if (slots > 1) mt.dup[k]++;
        if (level > mt.vmax[k]) mt.vmax[k] = level;
        mt.fsum[k] += level; mt.fvoices[k]++;
    }
    if (!watched || mt.sigs >= 32) { mix_trace_leave(); return; }
    uint8_t sig[22]={(uint8_t)(v<MCPX_HW_MAX_3D_VOICES),(uint8_t)multipass,(uint8_t)channels};
    for (int b=0;b<8;b++) { sig[3+b]=bins[b]; sig[11+b]=(uint8_t)(gains[b]!=0); }
    sig[19]=(uint8_t)(multipass ? mp_bin : 0xFF);
    sig[20]=(uint8_t)clear_mix; sig[21]=(uint8_t)voice_list;
    for (unsigned i=0;i<mt.sigs;i++) if (!memcmp(mt.sig[i],sig,sizeof(sig))) { mix_trace_leave(); return; }
    memcpy(mt.sig[mt.sigs++],sig,sizeof(sig));
    char line[1280]={0}; size_t used=0;
    mix_trace_append(line,sizeof(line),&used, "[APU-MIX] route voice=%u %s%s ch=%u mp_bin=%d clear_mix=%u list=%d postfilter_peak=%.3f", v,
            v < MCPX_HW_MAX_3D_VOICES ? "3D" : "2D", multipass ? " multipass" : "", channels, mp_bin, (unsigned)clear_mix, voice_list, peak);
    for (int b = 0; b < 8; b++)
        mix_trace_append(line,sizeof(line),&used, " %d:bin%u/%.1fdB/g%.3f", b, bins[b], (vol[b] & 0xFFF) / -64.0f, gains[b]);
    fprintf(stderr,"%s\n",line);
    mix_trace_leave();
}

static void mix_trace_frame_end(void)
{
    if (!mix_trace_enter()) return;
    for (int k = 0; k < MIX_TRACE_BINS; k++) {
        if (mt.fsum[k] > mt.max_fsum[k]) mt.max_fsum[k] = mt.fsum[k];
        if (mt.fvoices[k] > mt.max_fvoices[k]) mt.max_fvoices[k] = mt.fvoices[k];
        mt.fsum[k] = 0; mt.fvoices[k] = 0;
    }
    if (++mt.frames < 7500) { mix_trace_leave(); return; }
    char line[1280]={0}; size_t used=0;
    mix_trace_append(line,sizeof(line),&used, "[APU-MIX] scope=active_postfilter_no_tails bounds_not_waveform frames=%u postfilter_peak=%.3f blocks_gt1=%u", mt.frames, mt.src_peak, mt.src_over);
    for (int k = 0; k < MIX_TRACE_BINS; k++)
        mix_trace_append(line,sizeof(line),&used, " | bin%u n=%u 3d=%u mp=%u dup=%u gmax=%.3f voice_peak_bound=%.3f frame_peak_sum_bound=%.3f max_frame_voice_visits=%u",
                mix_trace_bin[k], mt.n[k], mt.n3d[k], mt.nmp[k], mt.dup[k], mt.gmax[k],
                mt.vmax[k], mt.max_fsum[k], mt.max_fvoices[k]);
    fprintf(stderr,"%s\n",line);
    mt.frames = mt.src_over = 0; mt.src_peak = 0;
    memset(mt.n, 0, sizeof(mt.n)); memset(mt.n3d, 0, sizeof(mt.n3d));
    memset(mt.nmp, 0, sizeof(mt.nmp)); memset(mt.dup, 0, sizeof(mt.dup));
    memset(mt.gmax, 0, sizeof(mt.gmax)); memset(mt.vmax, 0, sizeof(mt.vmax));
    memset(mt.max_fsum, 0, sizeof(mt.max_fsum)); memset(mt.max_fvoices, 0, sizeof(mt.max_fvoices));
    if (++mt.summaries>=120u) InterlockedExchange(&mix_trace_done,1);
    mix_trace_leave();
}

/* Deduplicate against all48 recorded raw HRIR/ITD pairs; capped sample. */
static void mix_trace_hrir(unsigned entry, const float h[2][HRTF_NUM_TAPS], float itd)
{
    if (!mix_trace_enter()) return;
    int8_t taps[2][HRTF_NUM_TAPS];
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < HRTF_NUM_TAPS; k++) taps[ch][k] = hrir_raw_byte(h[ch][k]);
    for(unsigned i=0;i<mt.hrir_lines;i++)
        if(itd==mt.itd[i] && !memcmp(taps,mt.hrir[i],sizeof(taps))) { mix_trace_leave(); return; }
    if(mt.hrir_lines>=48u) { mix_trace_leave(); return; }
    mt.itd[mt.hrir_lines]=itd;
    memcpy(mt.hrir[mt.hrir_lines++],taps,sizeof(taps));
    char line[1280]={0}; size_t used=0;
    mix_trace_append(line,sizeof(line),&used, "[APU-MIX] hrir entry=%u itd=%.3f", entry, itd);
    for (int ch = 0; ch < 2; ch++) {
        mix_trace_append(line,sizeof(line),&used, " %c=", ch ? 'R' : 'L');
        for (int k = 0; k < HRTF_NUM_TAPS; k++) mix_trace_append(line,sizeof(line),&used, "%s%d", k ? "," : "", taps[ch][k]);
    }
    fprintf(stderr,"%s\n",line);
    mix_trace_leave();
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    /* push-y E3/E3b (R4-AUDIO-THREADS P2-1): while the front end is TRAPPED on an idle-voice notification the DirectSound ISR is about to read
     * FEDECMETH/FEDECPARAM; a game-thread PIO write that overwrote the pair first made it release a late, or the WRONG, voice. Real hardware stops
     * decoding while trapped, so the pair stays. E3: count the clobbers ([APU-FE] clobber n=...). E3b (RECOMP_APU_FE_TRAP_FIX, default on;
     * =0 measures only): keep the trapped pair; the method itself still executes. */
    {
        static volatile long clobber; static int fix = -1;
        if (fix < 0) { const char *e = getenv("RECOMP_APU_FE_TRAP_FIX"); fix = !(e && *e == '0'); }
        if (method != SE2FE_IDLE_VOICE && (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
            long n = _InterlockedIncrement(&clobber);
            if ((n & (n - 1)) == 0) fprintf(stderr, "[APU-FE] clobber n=%ld method=%08X param=%08X (trapped pair %08X/%08X) fix=%d\n", n, method, argument,
                                            d->regs[NV_PAPU_FEDECMETH], d->regs[NV_PAPU_FEDECPARAM], fix);
            if (fix) goto fe_dispatch;
        }
        d->regs[NV_PAPU_FEDECMETH] = method;
        d->regs[NV_PAPU_FEDECPARAM] = argument;
    }
fe_dispatch: ;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;
        voice_trace_dry[selected_handle] = false;
        voice_trace(d, (uint16_t)selected_handle, "on", __LINE__);

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        if (stream_diagnostics_enabled()) {
            memset(&stream_counters[selected_handle],0,sizeof(stream_counters[selected_handle]));
            fprintf(stderr, "[APU-VOICE] frame=%d on=%u fmt=%08X ba=%08X end=%u\n",
                d->ep_frame_div, selected_handle,
                voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CFG_FMT, UINT32_MAX),
                voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA),
                voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO));
        }
        {   /* RECOMP_APU_STACK_BA=<hex buffer>: who starts that sound -- the game thread's guest stack */
            static long want = -2; static int shown;
            if (want == -2) { const char *e = getenv("RECOMP_APU_STACK_BA"); want = e ? strtol(e, NULL, 16) : -1; }
            uint32_t ba = voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
            if (want >= 0 && ba == (uint32_t)want && shown < 40) {
                extern ptrdiff_t g_xbox_mem_offset; extern APU_VP_TLS uint32_t g_esp;
                char line[600]; int o = 0; uint32_t sp = g_esp;
                shown++;
                o += snprintf(line + o, sizeof line - (size_t)o, "[APU-STACK] ba=%08X esp=%08X:", ba, sp);
                for (int k = 0; k < 900 && o < 560 && sp > 0x10000 && sp < 0x04000000; k++) {
                    uint32_t val = *(volatile uint32_t *)((uintptr_t)(sp + 4u * (uint32_t)k) + g_xbox_mem_offset);
                    if (val >= 0x00011000u && val < 0x003D0000u) o += snprintf(line + o, sizeof line - (size_t)o, " %08X", val);
                }
                fprintf(stderr, "%s\n", line);
            }
        }
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        assert(selected_handle < MCPX_HW_MAX_VOICES);
        voice_trace(d, (uint16_t)selected_handle, "release", __LINE__);
        if (d->vp.filters[selected_handle].drain_blocks)
            voice_reset_filters(d, (uint16_t)selected_handle);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        voice_reset_filters(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        selected_handle = argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE;
        assert(selected_handle < MCPX_HW_MAX_VOICES);
        voice_trace(d, (uint16_t)selected_handle, (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) ? "pause" : "unpause", __LINE__);
        if ((argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) &&
            d->vp.filters[selected_handle].drain_blocks)
            voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        voice_trace_dry[current_voice] = false;
        voice_trace(d, (uint16_t)current_voice, "ssla", __LINE__);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        voice_trace_dry[current_voice] = false;
        voice_trace(d, (uint16_t)current_voice, "sslb", __LINE__);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        if (mix_trace() && mix_trace_enter()) {
            if(mt.submix_lines<8u) {
                ++mt.submix_lines;
                fprintf(stderr, "[APU-MIX] hrtf_submix=%u,%u,%u,%u\n", d->vp.hrtf_submix[0],
                        d->vp.hrtf_submix[1], d->vp.hrtf_submix[2], d->vp.hrtf_submix[3]);
            }
            mix_trace_leave();
        }
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        if (gain_trace())
            fprintf(stderr, "[APU-GAIN] hrtf_headroom=%u\n", d->vp.hrtf_headroom);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            d->set_irq = true;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
            if (mix_trace())
                mix_trace_hrir(d->vp.hrtf.current_entry,
                               d->vp.hrtf.entries[d->vp.hrtf.current_entry].hrir,
                               s6p9_to_float(itd));
            if (gain_trace()) {
                const float (*h)[HRTF_NUM_TAPS] = d->vp.hrtf.entries[d->vp.hrtf.current_entry].hrir;
                fprintf(stderr, "[APU-GAIN] hrir entry=%u itd=%.3f", d->vp.hrtf.current_entry,
                        s6p9_to_float(itd));
                /* Recover ORIGINAL encoded bytes before normalization;
                 * preserve the existing signed-decimal raw trace format. */
                for (int ch = 0; ch < 2; ch++) {
                    fprintf(stderr, " %c=", ch ? 'R' : 'L');
                    for (int k = 0; k < HRTF_NUM_TAPS; k++)
                        fprintf(stderr, "%s%d", k ? "," : "", (int)hrir_raw_byte(h[ch][k]));
                }
                fprintf(stderr, "\n");
            }
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
            if (gain_trace())
                fprintf(stderr, "[APU-GAIN] submix_headroom bin=%d value=%u\n",
                        slot, d->vp.submix_headroom[slot]);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                d->vp.filters[v].ended_by_data = true;
                d->vp.filters[v].tail_input_remaining = (int)ceilf(
                    (2464.0f / 128.0f) / fminf(1.0f, fminf(d->vp.filters[v].resample_rate, d->vp.filters[v].resample_previous_rate))) + 2;
                voice_off(d, (uint16_t)v);
            } else {
                if (!voice_trace_dry[v]) { voice_trace_dry[v] = true; voice_trace(d, (uint16_t)v, "dry", __LINE__); }
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        stream_trace(d,(uint16_t)v,segment_offset,segment_length,cbo);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    /* EBO is inclusive. A callback that stops exactly before the final sample
     * must consume it on its next call before completing or wrapping. */
    if (cbo > ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                voice_trace_dry[v] = false;
                voice_trace(d, (uint16_t)v, "flip", __LINE__);
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                d->vp.filters[v].ended_by_data = true;
                d->vp.filters[v].tail_input_remaining = (int)ceilf(
                    (2464.0f / 128.0f) / fminf(1.0f, fminf(d->vp.filters[v].resample_rate, d->vp.filters[v].resample_previous_rate))) + 2;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling at the pitch programmed by DirectSound.
 *
 * Reuse libsamplerate's sinc converter, as in xemu's voice processor.
 * ============================================================ */

static long voice_resample_callback(void *opaque, float **data)
{
    MCPXAPUVoiceFilter *filter = opaque;
    MCPXAPUState *d = filter->owner;
    uint16_t v = filter->voice;
    int sample_count = 0;
    while (sample_count < NUM_SAMPLES_PER_FRAME) {
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        if (!active) break;

        int count = voice_get_samples(d, v,
            (float (*)[2])(filter->resample_buf + sample_count * 2),
            NUM_SAMPLES_PER_FRAME - sample_count);
        if (count <= 0) break;
        sample_count += count;
    }
    stream_diagnostics(d, v, sample_count);
    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    int returned = NUM_SAMPLES_PER_FRAME;
    if (!active) {
        /* End the converter after finite sinc support padding. Returning zero
         * invokes libsamplerate's end_of_input path, rather than guessing that
         * a quiet output block implies no authored samples remain buffered. */
        int zeros = NUM_SAMPLES_PER_FRAME - sample_count;
        if (zeros > filter->tail_input_remaining) zeros = filter->tail_input_remaining;
        filter->tail_input_remaining -= zeros;
        returned = sample_count + zeros;
    }
    if (returned == 0) filter->src_eoi = true;
    memset(filter->resample_buf + sample_count * 2, 0,
        (NUM_SAMPLES_PER_FRAME - sample_count) * 2 * sizeof(float));
    *data = filter->resample_buf;
    return returned;
}

/* [APU-SRC] (A0, RECOMP_AUDIO_WAIT_STATS / RECOMP_PERF -> g_apu_ws from apu_core.c): per 5 s, the
 * resampling ratio of each voice-frame: ==1, >1, <1, and ramping (ratio differs from the previous
 * frame's). Counted on the APU thread in voice_resample; reported by apu_src_report. */
extern int g_apu_ws;
static struct { unsigned long long n, eq, gt, lt, ramp; } src_stats;
extern void apu_sinc_verify_report(void);   /* libsamplerate/src_sinc.c (overlay, T2-6) */
void apu_src_report(double secs)
{
    fprintf(stderr, "[APU-SRC] %.1f s: voice-frames=%llu ratio==1: %llu, >1: %llu, <1: %llu, ramping: %llu\n",
            secs, src_stats.n, src_stats.eq, src_stats.gt, src_stats.lt, src_stats.ramp);
    memset(&src_stats, 0, sizeof(src_stats));
    apu_sinc_verify_report();   /* T2-6: [APU-SRC-VERIFY] totals, only with RECOMP_APU_SRC=verify */
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];
    if (!filter->resampler) {
        int error;
        filter->owner = d;
        filter->voice = v;
        filter->resampler = src_callback_new(voice_resample_callback,
            SRC_SINC_FASTEST, 2, &error, filter);
        if (!filter->resampler) {
            fprintf(stderr, "[APU] Voice resampler: %s\n", src_strerror(error));
            return -1;
        }
    }
    /* libsamplerate's sinc step is lrint(index_inc * ratio): a NaN or tiny ratio
     * makes it zero and the converter divides by it. Keep the ratio in range.
     * Below 1/8 (a voice pitched up more than three octaves) every output sample
     * runs the sinc filter over 1/ratio times as many taps: at 1/64 one voice cost
     * the APU frame thread seconds while it held d->lock, and the title, waiting
     * on that lock inside DirectSound, hitched for 3 s or froze (2026-10-14).
     * No real sound sits up there; play it at the cap. */
    if (rate > 0.0f && rate < 1.0f / 8.0f) {
        static int logged;
        if (logged++ < 20) fprintf(stderr, "[APU-RATE] voice %u ratio %.5f held at 1/8\n", (unsigned)v, rate);
        rate = 1.0f / 8.0f;
    }
    /* Once the callback has ended its input, libsamplerate's sinc converter takes no more: fed again
     * (the voice restarted within the frame, before its deferred reset), it consumes nothing,
     * makes nothing, and src_callback_read loops for ever -- here, on the APU thread, holding
     * d->lock, with the title stuck in voice_lock behind it (a bot match froze on Killing Field,
     * 2026-10-14). This is the converter's own thread, so resetting it here is safe. */
    if (filter->src_eoi) { src_reset(filter->resampler); filter->src_eoi = false; }
    if (!(rate >= 1.0f / 64.0f)) rate = rate > 0.0f ? 1.0f / 64.0f : 1.0f;
    else if (!(rate <= 64.0f)) rate = 64.0f;
    filter->resample_previous_rate = filter->resample_rate > 0 ? filter->resample_rate : rate;
    filter->resample_rate = rate;
    if (g_apu_ws) {
        src_stats.n++;
        if (rate == 1.0f) src_stats.eq++; else if (rate > 1.0f) src_stats.gt++; else src_stats.lt++;
        if (rate != filter->resample_previous_rate) src_stats.ramp++;
    }
    if (tail_stats_enabled > 0) {
        if (filter->drain_blocks) tail_stats.tail_reads++;
        else tail_stats.active_reads++;
    }
    return (int)src_callback_read(filter->resampler, rate, requested_num,
        (float *)samples);
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

/* Apply the same per-sample processing to active blocks and owned tails. */
static void voice_filter_block(sv_filter svf[2], HrtfFilter *hrtf,
                               bool lpf, bool use_hrtf, float samples[][2])
{
    if (lpf) for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            samples[i][ch] = clampf(run_svf(&svf[ch], samples[i][ch]), -1.0f, 1.0f);
    if (use_hrtf) hrtf_filter_process(hrtf, samples, samples);
}

static void voice_mix_block(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                            float monitor[][2], float samples[][2],
                            const uint8_t bins[8], const float gains[8],
                            unsigned channels, bool hrtf, float monitor_gain)
{
    if (channels < 1 || channels > 2) return;
    for (int b = 0; b < 8; b++) {
        if (bins[b] >= NUM_MIXBINS || gains[b] == 0) continue;
        /* HRTF slots 0-3 are ear-major: the title programs SET_HRTF_SUBMIXES
         * as 6,8,7,9 (front-left, back-left, front-right, back-right), so
         * slots 0/1 carry the left ear and 2/3 the right. b % 2 sent the
         * left ear to both bins 6 and 7. */
        int ch = (hrtf && b < 4) ? (b >> 1) : (int)(b % channels);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            mixbins[bins[b]][i] += gains[b] * samples[i][ch];
    }
    if (monitor) for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        monitor[i][0] += monitor_gain * samples[i][0];
        monitor[i][1] += monitor_gain * samples[i][1];
    }
}

static float voice_tail_max_abs(float peak, float sample)
{
    float magnitude = fabsf(sample);
    return magnitude > peak ? magnitude : peak;
}

static float voice_tail_peak(MCPXAPUVoiceFilter *f, float samples[][2])
{
    float peak = 0;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        for (int ch = 0; ch < 2; ch++) peak = voice_tail_max_abs(peak, samples[i][ch]);
    if (f->drain_lpf) for (int ch = 0; ch < 2; ch++) {
        peak = voice_tail_max_abs(peak, f->drain_svf[ch].b);
        peak = voice_tail_max_abs(peak, f->drain_svf[ch].l);
    }
    if (f->drain_hrtf) for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < HRTF_BUFLEN; i++)
            peak = voice_tail_max_abs(peak, f->drain_hrtf_state.ch[ch].buf[i]);
    return peak;
}

/* Conservative guard derived from sinc support and the slowest fixed SVF
 * pole, not a fixed number of output blocks. Normal completion uses EOF and
 * filter-state silence. Guard expiry is an explicit fail-safe diagnostic,
 * not successful completion. The guard covers the tested legal ratio/filter
 * range; any expiry in an integration run blocks acceptance. */
static int voice_tail_budget(MCPXAPUVoiceFilter *f)
{
    double rate = f->resample_rate;
    double previous = f->resample_previous_rate;
    double small = fmin(1.0, fmin(rate, previous));
    double support = ceil((2464.0 / 128.0) / small) + 2;
    /* One callback block can remain saved_data; one can be in the sinc FIFO. */
    double frames = ceil((2 * NUM_SAMPLES_PER_FRAME + 2 * support) * fmax(rate, previous));
    double settle = HRTF_BUFLEN + NUM_SAMPLES_PER_FRAME;
    if (f->drain_lpf) for (int ch = 0; ch < 2; ch++) {
        double a = f->drain_svf[ch].f, q = f->drain_svf[ch].q;
        double trace = 2 - a*q - a*a, det = 1 - a*q;
        double disc = trace*trace - 4*det;
        double radius = disc <= 0 ? sqrt(det) :
            fmax(fabs((trace + sqrt(disc))/2), fabs((trace - sqrt(disc))/2));
        double state = fmax(32.0, 32 * fmax(fabs(f->drain_svf[ch].b), fabs(f->drain_svf[ch].l)));
        if (radius > 0 && radius < 1)
            settle = fmax(settle, 2 * ceil(log(state / 1e-7) / -log(radius)) + HRTF_BUFLEN);
    }
    return (int)ceil((frames + settle) / NUM_SAMPLES_PER_FRAME) + 1;
}

/* RECOMP_APU_MP_TRACE=1: does each multipass read of bin 31 take exactly the
 * sound written by the voice processed just before it (a source/3D pair), or
 * several sounds, or nothing? Observes only; one line per 5 s. */
static struct {
    int enabled; unsigned long long seq, last_write_seq;
    unsigned writers, frames, reads, empty, one, adjacent, multi, max_writers;
    unsigned writes_by_list[3], tail_writes, leftover_frames, nonclear_reads;
    unsigned idle_sent, idle_deferred, idle_max_wait;
    float leftover_peak;
} mp;
static bool mp_trace(void)
{
    if (!mp.enabled) {
        const char *value = getenv("RECOMP_APU_MP_TRACE");
        mp.enabled = value && !strcmp(value, "1") ? 1 : 2;
    }
    return mp.enabled == 1;
}
static void mp_trace_write(const uint8_t bins[8], const float gains[8], int list, bool tail)
{
    for (int b = 0; b < 8; b++) {
        if (bins[b] != MULTIPASS_BIN || gains[b] == 0) continue;
        mp.writers++; mp.last_write_seq = mp.seq;
        if (tail) mp.tail_writes++; else if (list >= 0 && list < 3) mp.writes_by_list[list]++;
        return;
    }
}
static void mp_trace_read(int bin, bool clear)
{
    if (bin != MULTIPASS_BIN) return;
    mp.reads++;
    if (!mp.writers) mp.empty++;
    else if (mp.writers == 1) { mp.one++; if (mp.seq == mp.last_write_seq + 1) mp.adjacent++; }
    else mp.multi++;
    if (mp.writers > mp.max_writers) mp.max_writers = mp.writers;
    if (clear) mp.writers = 0; else mp.nonclear_reads++;
}
static void mp_trace_frame_end(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    float peak = 0;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) peak = fmaxf(peak, fabsf(mixbins[MULTIPASS_BIN][i]));
    if (peak > 0) { mp.leftover_frames++; mp.leftover_peak = fmaxf(mp.leftover_peak, peak); }
    if (++mp.frames < 7500) return;
    fprintf(stderr, "[APU-MP] frames=%u reads=%u empty=%u one_writer=%u adjacent_pair=%u multi_writer=%u max_writers=%u nonclear=%u writes_by_list=%u/%u/%u tail_writes=%u leftover_frames=%u leftover_peak=%.3f idle_sent=%u idle_deferred=%u idle_max_wait_frames=%u\n",
            mp.frames, mp.reads, mp.empty, mp.one, mp.adjacent, mp.multi, mp.max_writers, mp.nonclear_reads,
            mp.writes_by_list[0], mp.writes_by_list[1], mp.writes_by_list[2], mp.tail_writes, mp.leftover_frames, mp.leftover_peak,
            mp.idle_sent, mp.idle_deferred, mp.idle_max_wait);
    unsigned long long seq = mp.seq, last = mp.last_write_seq; unsigned writers = mp.writers; int enabled = mp.enabled;
    memset(&mp, 0, sizeof(mp)); mp.seq = seq; mp.last_write_seq = last; mp.writers = writers; mp.enabled = enabled;
}

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    int mp_bin = -1;
    bool clear_mix = false;
    if (multipass) {
        /* Read from multipass bin */
        mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (mp_trace()) mp_trace_read(mp_bin, clear_mix);
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        int empty = 0;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            /* After the data ends, the callback supplies silence and the
             * converter emits the samples it has already read ahead. */
            if (!active && !d->vp.filters[v].ended_by_data) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    bool ended = !active && d->vp.filters[v].ended_by_data;
    d->vp.filters[v].ended_by_data = false;
    if (!active && !ended) return;

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);

        }
    }

    bool use_hrtf = v < MCPX_HW_MAX_3D_VOICES && apu_hrtf_enabled() &&
        voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE) != HRTF_NULL_HANDLE;
    voice_filter_block(d->vp.filters[v].svf, &d->vp.filters[v].hrtf,
                       lpf, use_hrtf, samples);
    /* HRTF turns a mono source into two ear signals. Preserve that output
     * channel count through active mixing and the frozen tail snapshot. */
    if (use_hrtf) channels = 2;

    uint8_t bins[8]; float gains[8]; float monitor_gain = 0;
    for (int b = 0; b < 8; b++) {
        bins[b] = (uint8_t)bin[b];
        if (bin[b] < 0 || bin[b] >= NUM_MIXBINS) { gains[b] = 0; continue; }
        float hr = (float)(1 << ((v < MCPX_HW_MAX_3D_VOICES && b < 4) ?
            d->vp.hrtf_headroom : d->vp.submix_headroom[bin[b]]));
        bins[b] = (uint8_t)bin[b];
        gains[b] = ea_value * (attenuate(vol[b]) / hr);
        float mg = attenuate(vol[b]) / (float)(1 << d->vp.submix_headroom[bin[b]]);
        if (mg > monitor_gain) monitor_gain = mg;
    }
    monitor_gain *= ea_value;
    if (mix_trace()) mix_trace_voice(v, multipass, mp_bin, clear_mix, voice_list, channels, bins, gains, vol, samples);
    if (mp_trace()) mp_trace_write(bins, gains, voice_list, false);
    voice_mix_block(mixbins, d->monitor.point == MCPX_APU_DEBUG_MON_VP ? sample_buf : NULL,
                    samples, bins, gains, channels, use_hrtf, monitor_gain);
    if (ended && d->vp.filters[v].resampler) {
        MCPXAPUVoiceFilter *f = &d->vp.filters[v];
        /* Frozen zero gains cannot become audible during this owned tail.
         * VOICE_ON resets the converter before reusing the handle. */
        bool audible_tail = monitor_gain != 0;
        for (int b = 0; b < 8; b++)
            audible_tail |= bins[b] < NUM_MIXBINS && gains[b] != 0;
        if (!audible_tail) {
            if (tail_stats_enabled > 0) tail_stats.silent_skips++;
            f->drain_blocks = 0; f->tail_input_remaining = 0;
            return;
        }
        memcpy(f->drain_gain, gains, sizeof(gains));
        memcpy(f->drain_bin, bins, sizeof(bins));
        f->drain_rate = rate; f->drain_channels = (uint8_t)channels;
        f->drain_lpf = lpf; f->drain_hrtf = use_hrtf; f->drain_eof = false;
        memcpy(f->drain_svf, f->svf, sizeof(f->drain_svf));
        for (int ch = 0; ch < 2; ch++) if (lpf)
            setup_svf(&f->drain_svf[ch], f->svf[ch].f, f->svf[ch].q, F_LP);
        f->drain_hrtf_state = f->hrtf; f->drain_monitor_gain = monitor_gain;
        f->drain_blocks = voice_tail_budget(f);
        if (tail_stats_enabled > 0) tail_stats.starts++;
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* [APU-TIME] (apu_dsp_runtime.c prints it): QPC ticks spent here and active voices mixed. */
long long g_apu_vp_ticks;
unsigned long long g_apu_vp_voices;
unsigned g_apu_vp_voices_max;

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    LARGE_INTEGER vp_t0; QueryPerformanceCounter(&vp_t0);
    unsigned vp_voices = 0;
    long long stats_start = tail_stats_begin(); unsigned stats_voices = 0;
    g_vp_thread = GetCurrentThreadId();
    g_vp_wait_budget = 64;    /* vp_enter: a few tens of microseconds per frame at most -- a title's
                                 lock-update-unlock takes a few; one held longer costs no more */
    voice_apply_reset_requests(d);
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    /* Tails own frozen gains, routing and filter parameters until EOF/decay.
     * VOICE_ON, explicit OFF/RELEASE/PAUSE and reset cancel this ownership. */
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        MCPXAPUVoiceFilter *f = &d->vp.filters[v];
        if (!f->drain_blocks) continue;
        if (tail_stats_enabled > 0) { stats_voices++; if (f->drain_eof) tail_stats.filter_visits++; }
        if (voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                           NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
            f->drain_blocks = 0; continue;
        }
        float tail[NUM_SAMPLES_PER_FRAME][2] = {{0}};
        if (!f->drain_eof) {
            vp_enter(d, (uint16_t)v);
            int count = voice_resample(d, (uint16_t)v, tail, NUM_SAMPLES_PER_FRAME, f->drain_rate);
            vp_leave();
            if (count < 0 || src_error(f->resampler)) {
                voice_reset_filters_now(d, (uint16_t)v); continue;
            }
            if (count < NUM_SAMPLES_PER_FRAME) f->drain_eof = true;
        }
        voice_filter_block(f->drain_svf, &f->drain_hrtf_state,
                           f->drain_lpf, f->drain_hrtf, tail);
        if (mp_trace() && !voice_should_mute((uint16_t)v)) mp_trace_write(f->drain_bin, f->drain_gain, -1, true);
        if (!voice_should_mute((uint16_t)v))
            voice_mix_block(mixbins, d->monitor.point == MCPX_APU_DEBUG_MON_VP ? d->vp.sample_buf : NULL,
                            tail, f->drain_bin, f->drain_gain, f->drain_channels, f->drain_hrtf,
                            f->drain_monitor_gain);
        if (f->drain_eof && voice_tail_peak(f, tail) < 1e-7f) {
            /* Retain the ended converter allocation/state until VOICE_ON resets
             * it. Clearing every SRC FIFO on a simultaneous burst adds work
             * without affecting silence or ownership. */
            f->drain_blocks = 0; f->tail_input_remaining = 0;
            f->ended_by_data = false;
            if (tail_stats_enabled > 0) tail_stats.finishes++;
        } else if (--f->drain_blocks == 0) {
            if (tail_stats_enabled > 0) tail_stats.guards++;
            fprintf(stderr, "[APU] Tail guard exceeded voice=%d rate=%g eof=%d\n", v, f->drain_rate, f->drain_eof);
            voice_reset_filters_now(d, (uint16_t)v);
        }
    }

    /* Idle-voice notification. Every inactive voice in a list used to raise
     * SE2FE_IDLE_VOICE in the same walk, each overwriting FEDECPARAM before
     * DirectSound's ISR read it. Only the last voice was ever reported, and a
     * voice the title keeps listed while inactive won every frame, so other
     * stopped voices were never acknowledged: DirectSound spun 0.5 s per voice
     * in its stop wait (KeQuerySystemTime loop), freezing the game at the
     * end-of-gate fight on 2026-10-11. Hardware halts the walk at a trap. Report
     * one voice per frame, only when no trap is pending, choosing the one
     * waiting longest; the rest stay listed and are reported on later frames. */
    static uint32_t idle_frame, idle_since[MCPX_HW_MAX_VOICES];
    int idle_voice = -1; uint32_t idle_oldest = 0;
    idle_frame++;

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                if (!idle_since[v]) idle_since[v] = idle_frame;
                uint32_t age = idle_frame - idle_since[v] + 1;
                if (age > idle_oldest) { idle_oldest = age; idle_voice = v; }
            } else {
                idle_since[v] = 0;
                /* Process voice directly (single-threaded) */
                mp.seq++;
                vp_voices++;
                vp_enter(d, v);
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
                vp_leave();
            }
            d->regs[current] = d->regs[next];
        }
    }
    if (idle_voice >= 0) {
        if (!(d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)) {
            fe_method(d, SE2FE_IDLE_VOICE, (uint32_t)idle_voice);
            idle_since[idle_voice] = 0;
            mp.idle_sent++;
        } else mp.idle_deferred++;
        if (idle_oldest > mp.idle_max_wait) mp.idle_max_wait = idle_oldest;
    }
    if (mix_trace()) mix_trace_frame_end();
    if (mp_trace()) mp_trace_frame_end(mixbins);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] = isamp[2 * i];
            d->monitor.frame_buf[off + i][1] = isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
    tail_stats_end(stats_start,stats_voices);
    {
        LARGE_INTEGER vp_t1; QueryPerformanceCounter(&vp_t1);
        g_apu_vp_ticks += vp_t1.QuadPart - vp_t0.QuadPart;
        g_apu_vp_voices += vp_voices;
        if (vp_voices > g_apu_vp_voices_max) g_apu_vp_voices_max = vp_voices;
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++)
        d->vp.filters[v].resampler = src_delete(d->vp.filters[v].resampler);
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        _InterlockedExchange(&g_reset_req[v], 0);
        voice_reset_filters_now(d, (uint16_t)v);
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
