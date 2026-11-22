/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xbox/xboxrecomp.h>
#include "recomp/gen/recomp_bringup.h"
#include "usb/usb_gamepad.h"
#include "kernel/kernel.h"
#include "pc_settings.h"
#include "platform/pc_frontend.h"

/* getenv() once per diagnostic switch: the hooks below run on every input read
 * and audio callback. The values never change after startup. */
enum { DIAG_INPUT, DIAG_BOT, DIAG_AUDIO, DIAG_LOAD, DIAG_COUNT };
static int ma2_diag(int which)
{
    static const char *const names[DIAG_COUNT] = { "RECOMP_INPUT_DIAG", "RECOMP_BOT_DIAG", "RECOMP_AUDIO_STATS", "RECOMP_LOAD_TRACE" };
    static signed char cache[DIAG_COUNT] = { -1, -1, -1, -1 };
    if (cache[which] < 0)
        cache[which] = getenv(names[which]) != NULL;
    return cache[which];
}

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

#define GUEST32(va) (*(uint32_t *)(g_xbox_mem_offset + (uint32_t)(va)))

/* Audio API lock 0x3C4598, APU ISR try-enter and stop assist: moved verbatim to
 * apu/ma2_audio_guest.inc (pipeline M0; track T2 owns it). */
#define MA2_AUDIO_GUEST_PART 1
#include "apu/ma2_audio_guest.inc"
#undef MA2_AUDIO_GUEST_PART

void ma2_audio_callback_trace(uint32_t object,uint32_t stack,uint32_t site)
{
    static LONG count;
    if (!ma2_diag(DIAG_AUDIO)) return;
    if(object<0x10000 || object>0x03FFFF00) return;
    uint32_t table=GUEST32(object);
    if (table!=0x453448 && InterlockedIncrement(&count)>100) return;
    fprintf(stderr,"[AUDIO-CALLBACK] site=%08X object=%08X table=%08X target=%08X esp=%08X ebp=%08X seh=%08X args=",
        site,object,table,(table>=0x10000 && table<0x03FFFF00)?GUEST32(table+(site==0x3AAD56?0x18:0x1C)):0,
        stack,g_ebp,g_seh_ebp);
    for(unsigned i=0;i<8;i++) fprintf(stderr," %08X",GUEST32(stack+i*4));
    fprintf(stderr,"\n");
}

void ma2_audio_retire_trace(uint32_t object)
{
    if(!ma2_diag(DIAG_AUDIO) || object<0x10000 || object>0x03FFFF00) return;
    if(GUEST32(object+0x54)==object+0x54) return;
    fprintf(stderr,"[AUDIO-RETIRE] tid=%lu object=%08X table=%08X flags=%04X main=%08X irql=%u voice=%u "
        "active-links=%08X,%08X deferred-links=%08X,%08X\n",GetCurrentThreadId(),object,GUEST32(object),
        *(uint16_t *)(g_xbox_mem_offset+object+0x12),GUEST32(object+8),xbox_KeGetCurrentIrql(),
        *(uint16_t *)(g_xbox_mem_offset+object+0xC),GUEST32(object+0x4C),GUEST32(object+0x50),
        GUEST32(object+0x54),GUEST32(object+0x58));
}

/* Native movie frames use Bink's guest clock while the renderer is uncapped. */
void ma2_movie_yield(void)
{
    Sleep(1);
}

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern void mcpx_apu_dsp_set_doorbell(MCPXAPUState *, uint32_t);
void ma2_register_dsp_doorbell(uint32_t va)
{
    /* 0x003AA7DF waits for the DSP to clear the command it just submitted.
     * Register the actual allocation, which changes when caches are present.
     * The interpreter executes the title's original GP and EP effects. */
    mcpx_apu_dsp_set_doorbell(g_apu_state, va);
}

void ma2_trace_loading(uint32_t request, uint32_t result, uint32_t completed)
{
    static unsigned count;
    uint32_t entry_count;
    if (!ma2_diag(DIAG_LOAD) || count++ % 100 != 0)
        return;
    fprintf(stderr, "[LOAD] request=%08X result=%08X completed=%08X "
                    "path=%.128s\n", request, result, completed,
                    (const char *)(g_xbox_mem_offset + 0x50B800));
    if (request >= 0x10000 && request < 0x03FFFFD0) {
        fprintf(stderr, "  request words:");
        for (int i = 0; i < 28; i++)
            fprintf(stderr, " %08X", GUEST32(request + i * 4));
        fprintf(stderr, "\n");
    }
    if (result >= 0x10000 && result < 0x03FFFFD0) {
        fprintf(stderr, "  result words:");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %08X", GUEST32(result + i * 4));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  scheduler: active=%08X file=%08X ios=%08X "
                    "bytes=%u expected=%u list=%08X\n",
            GUEST32(0x4E4D48), GUEST32(0x4A1950),
            GUEST32(0x4E4D4C), GUEST32(0x4E4D50),
            GUEST32(0x4E4D60), GUEST32(0x50DC34));
    entry_count = request >= 0x10000 && request < 0x03FFF000
        ? GUEST32(request + 0xC) : 0;
    if (entry_count > 8)
        entry_count = 8;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t entry = request + 0x10 + i * 0x20;
        uint32_t job = GUEST32(entry + 0x18);
        fprintf(stderr, "  entry[%u]=%08X flags=%08X completed=%08X "
                        "hash=%08X job=%08X\n",
                i, entry, GUEST32(entry), GUEST32(entry + 4),
                GUEST32(entry + 8), job);
        if (job >= 0x10000 && job < 0x03FFFF80) {
            fprintf(stderr, "    job words:");
            for (int word = 0; word < 16; word++)
                fprintf(stderr, " %08X", GUEST32(job + word * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

void ma2_trace_worker(uint32_t site, uint32_t wait_result, uint32_t busy)
{
    static RECOMP_TLS uint32_t last_site, last_request, last_flags, last_wait;
    static RECOMP_TLS unsigned count;
    uint32_t request, flags;
    if (!ma2_diag(DIAG_LOAD))
        return;
    request = GUEST32(0x4E4D48);
    flags = request >= 0x10000 && request < 0x03FFFFD0 ? GUEST32(request) : 0;
    if (count++ >= 100 && last_site == site && last_request == request &&
        last_flags == flags && last_wait == wait_result)
        return;
    last_site = site; last_request = request; last_flags = flags; last_wait = wait_result;
    fprintf(stderr, "[WORK] site=%08X wait=%08X busy=%u request=%08X flags=%08X "
                    "file=%08X ios=%08X bytes=%u expected=%u callbacks=%08X/%08X\n",
            site, wait_result, busy, request, flags, GUEST32(0x4A1950),
            GUEST32(0x4E4D4C), GUEST32(0x4E4D50), GUEST32(0x4E4D60),
            GUEST32(0x50C6EC), GUEST32(0x50C6F0));
    if (request >= 0x10000 && request < 0x03FFFFD0) {
        fprintf(stderr, "  work words:");
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %08X", GUEST32(request + i * 4));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

void recomp_unresolved_stub(uint32_t va)
{
    static RECOMP_TLS uint32_t seen[512];
    static RECOMP_TLS unsigned count;
    for (unsigned i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count < 512)
        seen[count++] = va;
    fprintf(stderr, "[STUB] reached untranslated call 0x%08X esp=%08X "
                    "ebp=%08X seh_ebp=%08X eax=%08X ecx=%08X\n",
            va, g_esp, g_ebp, g_seh_ebp, g_eax, g_ecx);
    if (g_esp >= 0x10000 && g_esp < 0x03FFFFD0) {
        fprintf(stderr, "  guest stack:");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %08X", GUEST32(g_esp + i * 4));
        fprintf(stderr, "\n");
    }
}

/* The Xbox CPU has CPUID and MMX. The original routine toggles EFLAGS.ID,
 * queries CPUID leaf 1 bit 23, caches the answer and honours the caller's
 * 0x40000 "disable MMX" flag. pushfd/popfd/cpuid are untranslated by the
 * lifter, so that probe both corrupts its stack and reports MMX absent.
 * Preserve the original observable result and cdecl stack convention. */
void sub_0031C900(void)
{
    uint32_t options = GUEST32(g_esp + 4);
    if ((int32_t)GUEST32(0x004A0C68) < 0)
        GUEST32(0x004A0C68) = 1;
    g_eax = GUEST32(0x004A0C68);
    GUEST32(0x00534E2C) = g_eax && !(options & 0x00040000u);
    g_esp += 4;
}

/* ── B11 (R10 E2/E3/E4): the other CPU probes ────────────────────────────
 * Same class as sub_0031C900: pushfd/popfd/cpuid lift to no-ops while the
 * pops between them still run, so the lifted bodies skew the guest stack.
 * The lifts are kept in the gen files as *_unused_lift. */

/* MMX probe on the render-init path (0x32FC6C -> 0x334C40). The lift returned
 * eax = 0 and, through the one-slot skew, also handed ebx = 0 back to its
 * caller. Keep the observed result (MMX off, so the render path is unchanged)
 * and leave ebx/ecx alone, as the original does. */
void sub_00334C40(void)
{
    g_eax = 0;
    g_edx = 0;
    g_esp += 4;
}

/* CPU-detect behind HasSSE() (sub_002E0FBA, 16 callers). On its first call the
 * lift popped the return-address slot into ebx and left esp 4 bytes high.
 * Keep the observed result: leaf-0 max = 0, feature words 0, so HasSSE() = 0.
 * Registers are preserved, as the original pushes and pops all four. */
void sub_002E0F4C(void)
{
    if (!GUEST32(0x004CDD90)) {
        for (uint32_t va = 0x004CDD94; va <= 0x004CDDAC; va += 4)
            GUEST32(va) = 0;
        GUEST32(0x004CDD90) = 1;
    }
    g_esp += 4;
}

/* The two probes with no pushfd (0x26D650, 0x2E4180) read whatever edx the
 * caller held as the CPUID feature word. The gen code now pins it to 0; this
 * notes once per site when the stale value would have said MMX/SSE/SSE2. */
void ma2_cpuid_pin_note(uint32_t site, uint32_t stale_edx)
{
    static volatile LONG noted[2];
    unsigned slot = site == 0x0026D68Bu ? 0 : 1;
    uint32_t bits = stale_edx & 0x06800000u;
    if (!bits || InterlockedExchange(&noted[slot], 1)) return;
    fprintf(stderr, "[CPUID-PIN] site=%08X stale edx=%08X would have read MMX=%u SSE=%u SSE2=%u; pinned to 0\n",
            site, stale_edx, !!(bits & 0x00800000u), !!(bits & 0x02000000u), !!(bits & 0x04000000u));
}

/* ── B11.2 frame clock: carry the fractional millisecond ─────────────────
 * The clock producers sub_0001DD80/DDB4/DE70 (recomp_0001.c) store the frame
 * step as (int)(dt*1000) in 0x4E2424, the integer-ms step that about 20
 * consumers read (timers, turn accumulators, the actor-dispatch loop).
 * Truncating every frame loses the fraction: game time runs 14 % slow at
 * 144 Hz, 28 % at 360 Hz, and the step is 0 above 1000 fps, so integer-ms
 * timers stop. Carry the remainder instead: the sum of the steps tracks the
 * double game clock at any frame rate, with no fps cap. It is deterministic
 * (the same dt sequence gives the same steps), each step is the truncated
 * value or one more, and at 60 fps it only changes 16,16,16 into 16,17,17.
 * Switch: [Display] FrameClockCarry=1 (default); RECOMP_MS_CARRY=0/1 overrides.
 * RECOMP_MS_CARRY_STATS=1 logs truncated vs carried ms against wall time. */
static double g_ma2_ms_rem;
static int ma2_ms_carry_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MS_CARRY");
        on = (e && *e) ? *e != '0' : ma2_pc_ini_get_int("Display", "FrameClockCarry", 1) != 0;
        fprintf(stderr, "[MS-CARRY] frame clock fractional-ms carry %s\n", on ? "on" : "off");
    }
    return on;
}
void ma2_ms_step_reset(void)
{
    g_ma2_ms_rem = 0.0;
}
static void ma2_crt_special_test(void);
static void ma2_ms_stats(double dt, uint32_t truncated, uint32_t carried)
{
    static int on = -1;
    static uint64_t freq, t0, next;
    static uint64_t frames, sum_trunc, sum_carry, zero_trunc, zero_carry;
    static double sum_dt;
    if (on < 0) on = getenv("RECOMP_MS_CARRY_STATS") != NULL;
    if (!on) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
        t0 = (uint64_t)now.QuadPart;
        next = t0 + freq * 10;
    }
    frames++; sum_dt += dt; sum_trunc += truncated; sum_carry += carried;
    zero_trunc += truncated == 0; zero_carry += carried == 0;
    if ((uint64_t)now.QuadPart >= next) {
        double wall = (double)((uint64_t)now.QuadPart - t0) / (double)freq;
        next += freq * 10;
        fprintf(stderr, "[MS-CARRY] wall=%.1fs dt_sum=%.3fs trunc=%.3fs (%.1f%%) carry=%.3fs (%.1f%%) frames=%llu fps=%.0f zero_steps trunc=%llu carry=%llu\n",
                wall, sum_dt, sum_trunc / 1000.0, sum_dt > 0 ? 100.0 * (sum_trunc / 1000.0) / sum_dt : 0.0,
                sum_carry / 1000.0, sum_dt > 0 ? 100.0 * (sum_carry / 1000.0) / sum_dt : 0.0,
                (unsigned long long)frames, wall > 0 ? frames / wall : 0.0,
                (unsigned long long)zero_trunc, (unsigned long long)zero_carry);
    }
}
uint32_t ma2_ms_step_carry(uint32_t truncated)
{
    static int tested;
    double dt = *(volatile double *)(g_xbox_mem_offset + 0x004E2428u);
    double scale = *(volatile double *)(g_xbox_mem_offset + 0x00493B58u);
    double x = dt * scale;
    uint32_t carried = truncated;
    if (!tested) { tested = 1; ma2_crt_special_test(); }
    /* Only carry when the step is the plain truncation of dt*1000 the producers
     * compute; anything else (negative, huge, NaN) keeps the original value. */
    if (x >= 0.0 && x < 1.0e6 && (uint32_t)x == truncated) {
        x += g_ma2_ms_rem;
        carried = (uint32_t)x;
        g_ma2_ms_rem = x - (double)carried;
    } else {
        g_ma2_ms_rem = 0.0;
    }
    ma2_ms_stats(dt, truncated, carried);
    return ma2_ms_carry_enabled() ? carried : truncated;
}

/* ── T1-0 TH-2: fixed frame step + game-state hash (test only, OFF by default) ──
 * The three clock producers (sub_0001DE70 = 0, sub_0001DD80 = 1, sub_0001DDB4 = 2,
 * recomp_0001.c) pass the raw elapsed seconds through ma2_frame_elapsed() before
 * it lands in 0x4E2418 (DE70: before the 0.2 s clamp and the 0x50BA70 override).
 * With neither variable set it returns its argument unchanged, bit for bit.
 *   RECOMP_FIXED_DT=<seconds>  (0 < s <= 0.2): every game-thread producer call
 *       sees that elapsed value, so game time no longer follows the wall clock.
 *   RECOMP_STATE_HASH=<N>: every N game frames (frame counter 0x4E2420), on the
 *       game thread before the producer updates the clock, log
 *       [STATE-HASH] frame= core= data= ram=, where core = the clock globals
 *       0x4E2420..0x4E2447, data = the XBE .data section in 4 KB blocks (the
 *       wall-clock fps ring 0x4E2200..0x4E240F left out), ram = all 64 MB of
 *       guest RAM in 64 KB blocks. Word-wise FNV-1a 64 (a single changed word
 *       always changes the hash).
 *   RECOMP_STATE_HASH_FILE=<path>: also write every block hash (binary,
 *       "MA2STH01" header, see pipeline\sthcmp.py) so two runs can be bisected
 *       to the first diverging block offline.
 *   RECOMP_PERF_CPU=1: the game thread stamps its processor once per frame for
 *       main.c's [PERF-CPU] report (pc_thread_cpu_note). */
#include "platform/pc_cpu_topo.h"
#define TH2_RAM_BYTES   (64u << 20)
#define TH2_RAM_BLOCK   (64u << 10)
#define TH2_DATA_BLOCK  4096u
#define TH2_MAX_DATA    256u
static int th2_mode = -1;            /* -1 unread; bit 0 fixed dt, bit 1 hash, bit 2 cpu note */
static double th2_dt;
static uint32_t th2_every, th2_last = 0xFFFFFFFFu;
static DWORD th2_tid;
static uint64_t th2_calls[4];        /* de70, dd80, ddb4, other threads */
static uint32_t th2_data_lo, th2_data_blocks;
static FILE *th2_file;
static uint64_t th2_fnv(uint64_t h, const uint8_t *p, size_t n)
{
    const uint64_t prime = 0x100000001B3ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * prime; }
    for (; i < n; i++) h = (h ^ p[i]) * prime;
    return h;
}
/* Hash guest [lo, hi) leaving out [x0, x1). */
static uint64_t th2_range(uint32_t lo, uint32_t hi, uint32_t x0, uint32_t x1)
{
    const uint8_t *b = (const uint8_t *)(g_xbox_mem_offset);
    uint64_t h = 0xCBF29CE484222325ull;
    if (x1 <= lo || x0 >= hi) return th2_fnv(h, b + lo, hi - lo);
    if (x0 > lo) h = th2_fnv(h, b + lo, x0 - lo);
    if (x1 < hi) h = th2_fnv(h, b + x1, hi - x1);
    return h;
}
static void th2_init(void)
{
    const char *e = getenv("RECOMP_FIXED_DT");
    int mode = 0;
    th2_tid = GetCurrentThreadId();
    if (e && *e) {
        double v = strtod(e, NULL);
        if (v > 0.0 && v <= 0.2) { th2_dt = v; mode |= 1; }
    }
    e = getenv("RECOMP_STATE_HASH");
    if (e && *e) {
        long n = strtol(e, NULL, 0);
        if (n > 0) { th2_every = (uint32_t)n; mode |= 2; }
    }
    e = getenv("RECOMP_PERF_CPU");
    if (e && *e && *e != '0') mode |= 4;
    if (mode & 2) {
        /* The .data section from the XBE header at 0x10000 (section headers: count
         * at +0x11C, VA at +0x120, 56 bytes each: flags, VA, size, ..., name VA at +0x14). */
        uint32_t count = GUEST32(0x0001011Cu), hdr = GUEST32(0x00010120u), i;
        th2_data_lo = 0x00495000u; th2_data_blocks = 0xA5u;   /* 0x495D40 + 670452, rounded out */
        if (count && count < 128 && hdr >= 0x00010000u && hdr < 0x00012000u) {
            for (i = 0; i < count; i++) {
                uint32_t s = hdr + i * 56u, va = GUEST32(s + 4), size = GUEST32(s + 8), name = GUEST32(s + 0x14);
                if (name >= 0x00010000u && name < 0x00012000u &&
                    !memcmp((const char *)(g_xbox_mem_offset + name), ".data", 6) && size) {
                    uint32_t lo = va & ~(TH2_DATA_BLOCK - 1u), hi = (va + size + TH2_DATA_BLOCK - 1u) & ~(TH2_DATA_BLOCK - 1u);
                    th2_data_lo = lo; th2_data_blocks = (hi - lo) / TH2_DATA_BLOCK;
                    break;
                }
            }
        }
        if (th2_data_blocks > TH2_MAX_DATA) th2_data_blocks = TH2_MAX_DATA;
        e = getenv("RECOMP_STATE_HASH_FILE");
        if (e && *e && (th2_file = fopen(e, "wb")) != NULL) {
            uint32_t h[6] = { th2_data_lo, th2_data_blocks, TH2_RAM_BYTES / TH2_RAM_BLOCK, TH2_RAM_BLOCK, th2_every, 0 };
            fwrite("MA2STH01", 1, 8, th2_file);
            fwrite(h, sizeof(h), 1, th2_file);
        }
    }
    if (mode & 3)
        fprintf(stderr, "[STATE-HASH] init fixed_dt=%.9f every=%u data=%08X+%u blocks file=%s game_tid=%lu\n",
                (mode & 1) ? th2_dt : 0.0, th2_every, th2_data_lo, th2_data_blocks,
                th2_file ? "yes" : "no", (unsigned long)th2_tid);
    th2_mode = mode;
}
static void th2_hash(uint32_t frame)
{
    static uint64_t data[TH2_MAX_DATA], ram[TH2_RAM_BYTES / TH2_RAM_BLOCK];
    LARGE_INTEGER t0, t1, f;
    uint64_t core, dall = 0xCBF29CE484222325ull, rall = 0xCBF29CE484222325ull;
    uint32_t i, nram = TH2_RAM_BYTES / TH2_RAM_BLOCK;
    QueryPerformanceCounter(&t0);
    core = th2_range(0x004E2420u, 0x004E2448u, 0, 0);
    for (i = 0; i < th2_data_blocks; i++) {
        uint32_t lo = th2_data_lo + i * TH2_DATA_BLOCK;
        data[i] = th2_range(lo, lo + TH2_DATA_BLOCK, 0x004E2200u, 0x004E2410u);
        dall = th2_fnv(dall, (const uint8_t *)&data[i], 8);
    }
    for (i = 0; i < nram; i++) {
        uint32_t lo = i * TH2_RAM_BLOCK;
        ram[i] = th2_range(lo, lo + TH2_RAM_BLOCK, 0x004E2200u, 0x004E2410u);
        rall = th2_fnv(rall, (const uint8_t *)&ram[i], 8);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    if (th2_file) {
        uint32_t h[2] = { frame, 0 };
        fwrite(h, sizeof(h), 1, th2_file);
        fwrite(&core, 8, 1, th2_file); fwrite(&dall, 8, 1, th2_file); fwrite(&rall, 8, 1, th2_file);
        fwrite(data, 8, th2_data_blocks, th2_file);
        fwrite(ram, 8, nram, th2_file);
        fflush(th2_file);
    }
    {   /* T1-2 gate: the 48 raw clock-block bytes (0x4E2420..0x4E244F) as 6 LE qwords */
        const uint64_t *q = (const uint64_t *)(g_xbox_mem_offset + 0x004E2420u);
        fprintf(stderr, "[STATE-RAW] frame=%u %016llX %016llX %016llX %016llX %016llX %016llX\n", frame,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
    }
    fprintf(stderr, "[STATE-HASH] frame=%u core=%016llX data=%016llX ram=%016llX clock=%.6f calls=%llu/%llu/%llu/%llu hash_ms=%.1f\n",
            frame, (unsigned long long)core, (unsigned long long)dall, (unsigned long long)rall,
            *(volatile double *)(g_xbox_mem_offset + 0x004E2440u),
            (unsigned long long)th2_calls[0], (unsigned long long)th2_calls[1],
            (unsigned long long)th2_calls[2], (unsigned long long)th2_calls[3],
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
}
double ma2_frame_elapsed(int producer, double elapsed)
{
    if (th2_mode < 0) th2_init();
    if (!th2_mode) return elapsed;
    if (GetCurrentThreadId() != th2_tid) { th2_calls[3]++; return elapsed; }
    th2_calls[producer & 3]++;
    if (th2_mode & 4) pc_thread_cpu_note();
    if (th2_mode & 2) {
        uint32_t frame = GUEST32(0x004E2420u);
        if (frame % th2_every == 0 && frame != th2_last) { th2_last = frame; th2_hash(frame); }
    }
    return (th2_mode & 1) ? th2_dt : elapsed;
}

/* B11.1 check (RECOMP_CRT_SPECIAL_TEST=1, off by default): call the lifted CRT
 * cos/sin (double on the stack) and _CIacos/_CIasin (st0) with +-Inf, NaN and
 * 1.0 once on the game thread and log the result bits. Every guest register
 * and the FPU state are saved and restored around it. */
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
void sub_0030C650(void);
void sub_0030C700(void);
void sub_0030A420(void);
void sub_0030C4B8(void);
static void ma2_crt_special_test(void)
{
    if (!getenv("RECOMP_CRT_SPECIAL_TEST")) return;
    static const uint64_t in[] = { 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull,
                                   0xFFF8000000000000ull, 0x3FF0000000000000ull };
    static const struct { const char *name; void (*fn)(void); int stack_arg; } f[] = {
        { "cos", sub_0030C650, 1 }, { "sin", sub_0030C700, 1 }, { "acos", sub_0030A420, 0 }, { "asin", sub_0030C4B8, 0 } };
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_ebx = g_ebx, r_esi = g_esi, r_edi = g_edi;
    uint32_t r_esp = g_esp, r_ebp = g_ebp, r_seh = g_seh_ebp;
    int r_top = g_fp_top, r_cmp = g_fp_cmp;
    uint16_t r_cw = g_fp_control_word;
    double r_stack[8];
    memcpy(r_stack, g_fp_stack, sizeof(r_stack));
    g_esp = (r_esp - 0x100u) & ~15u;   /* below the live frame */
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        for (unsigned k = 0; k < sizeof(in) / sizeof(in[0]); k++) {
            double x, r;
            uint64_t bits;
            uint32_t esp0 = g_esp;
            int top0 = g_fp_top;
            memcpy(&x, &in[k], 8);
            if (f[i].stack_arg) { g_esp -= 8; memcpy((void *)(g_xbox_mem_offset + g_esp), &x, 8); }
            else { g_fp_top = (g_fp_top + 7) & 7; g_fp_stack[g_fp_top] = x; }
            g_esp -= 4; GUEST32(g_esp) = 0;   /* return address; the callee's ret pops it */
            g_seh_ebp = g_ebp = esp0;
            f[i].fn();
            if (f[i].stack_arg) g_esp += 8;
            r = g_fp_stack[g_fp_top & 7];
            g_fp_top = (g_fp_top + 1) & 7;
            memcpy(&bits, &r, 8);
            fprintf(stderr, "[CRT-TEST] %s(%016llX) = %016llX (%g) eax=%08X esp_ok=%d fp_ok=%d\n", f[i].name,
                    (unsigned long long)in[k], (unsigned long long)bits, r, g_eax, g_esp == esp0, g_fp_top == top0);
            g_esp = esp0; g_fp_top = top0; g_fp_control_word = r_cw;
        }
    }
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx; g_esi = r_esi; g_edi = r_edi;
    g_esp = r_esp; g_ebp = r_ebp; g_seh_ebp = r_seh;
    memcpy(g_fp_stack, r_stack, sizeof(r_stack));
    g_fp_top = r_top; g_fp_cmp = r_cmp; g_fp_control_word = r_cw;
}

/* B11.3/B11.4 bounded audio spin-waits (ma2_voice_new_wait, ma2_xact_packet_wait): moved verbatim to
 * apu/ma2_audio_guest.inc (pipeline M0; track T2 owns it). */
#define MA2_AUDIO_GUEST_PART 2
#include "apu/ma2_audio_guest.inc"
#undef MA2_AUDIO_GUEST_PART

/* XDK's write-combined memory flush, verified at 0040F980 in ma2.xbe.
 * PFB.WBC_FLUSH drains CPU writes; it does not wait for a rendered frame.
 * The host has coherent RAM. Complete the memory barrier here instead of
 * waiting for the rendering thread's next unrelated poll to clear its bit.
 * NV2A's existing PFB model also reads WBC with FLUSH immediately cleared. */
void sub_0040F980(void)
{
    g_eax=GUEST32(0x0041BC68);
    g_eax=GUEST32(g_eax+0x1C28);
    volatile LONG *wbc=(volatile LONG *)(g_xbox_mem_offset+g_eax+0x100410);
    g_ecx=(uint32_t)*wbc|0x10000u;
    MemoryBarrier();
    InterlockedAnd(wbc,~0x10000L);
    g_esp+=4;
}

/* NV2A interrupt registers use write-one-to-clear semantics. Plain mapped
 * RAM used to retain the bit and make the XDK ISR spin until another thread
 * cleared it. Acknowledge the requested PCRTC bits synchronously. */
void ma2_gpu_ack_vblank(uint32_t register_base,uint32_t bits)
{
    volatile LONG *pcrtc=(volatile LONG *)(g_xbox_mem_offset+register_base+0x600100);
    volatile LONG *pmc=(volatile LONG *)(g_xbox_mem_offset+register_base+0x100);
    InterlockedAnd(pcrtc,~(LONG)bits);
    if(!*pcrtc)InterlockedAnd(pmc,~0x01000000L);
}

/* Deliver NV097 software methods at the point they leave the FIFO. XDK's
 * original miniport handler implements all fourteen method types, including
 * BlockOnTime's event and the title's render callbacks. Calling that handler
 * directly avoids emulating its MMIO interrupt acknowledgement prologue. */
#include <kernel/xbox_memory_layout.h>
#include <kernel/nv2a_pb_objects.h>
extern RECOMP_TLS uint32_t g_fs_base;
void sub_004152E0(void);
void ma2_gpu_software_method(uint32_t value)
{
    static RECOMP_TLS uint32_t stack_top;
    if (!stack_top) {
        stack_top = xbox_AllocThreadStack();
        g_fs_base = xbox_AllocThreadTib();
        if (!stack_top || !g_fs_base) {
            fprintf(stderr,"[GPU] Cannot allocate the guest callback context\n");
            return;
        }
    }
    uint32_t device = GUEST32(0x41BC68);
    if (!device) return;
    /* XDK type 1 is QueueFlip: the high 27 bits contain surface.Data with
     * the low four bits reserved for presentation flags (0040EA20/415304).
     * It names the actual scanout allocation even when the last or largest
     * draw was a bloom/shadow target. Keep the original handler below for
     * its vblank queue, fences and callbacks. */
    if ((value & 0x1F) == 1) {
        uint32_t physical = (value >> 5) & 0x07FFFFF0u, pitch = 0;
        xbox_GetDisplayFramebuffer(&pitch);
        if (physical < XBOX_CONTIG_SIZE)
            nv2a_pb_set_scanout(XBOX_CONTIG_BASE + physical, pitch);
    }
    g_esp = stack_top - 4;
    GUEST32(g_esp) = 0;
    g_ecx = device + 0x1C28;
    g_eax = value;
    sub_004152E0();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
extern void sub_0015CED6(void);
extern uint64_t xbox_save_probe_callback_begin(void);
extern void xbox_save_probe_callback_end(uint64_t started);
static void ma2_save_progress_probe(void)
{
    uint64_t started=xbox_save_probe_callback_begin();
    sub_0015CED6();
    xbox_save_probe_callback_end(started);
}

extern void ma2_scn_msg_execute(void);   /* scenario/ma2_scenario.c: mission message queue (custom levels) */
extern void ma2_scn_gamecond_execute(void), ma2_scn_player_update(void);   /* scenario/ma2_scenario.c: result screen */
extern void ma2_scn_corporeal_build(void);   /* scenario/ma2_scenario.c: generated water / wall models (custom levels) */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if(xbox_va==0x0015CED6u)return ma2_save_progress_probe;
    if(xbox_va==0x001A0991u)return ma2_scn_msg_execute;   /* MAActionBroadcast/PopupMenu Execute */
    if(xbox_va==0x0014E259u)return ma2_scn_gamecond_execute;   /* MAActionGameCondition Execute */
    if(xbox_va==0x000C80DBu)return ma2_scn_player_update;      /* player entity update (death -> lose) */
    if(xbox_va==0x000D75C7u)return ma2_scn_corporeal_build;    /* MACorporeal slot-113 model build (resized water, cut walls) */
    recomp_func_t recovered = recomp_lookup_bringup(xbox_va);
    if (recovered)
        return recovered;
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    enum { SLOTS = 64 };
    static RECOMP_TLS uint32_t seen[SLOTS];
    static RECOMP_TLS uint64_t hits[SLOTS];
    static RECOMP_TLS int count;
    int slot;
    uint64_t n;

    for (slot = 0; slot < count; slot++)
        if (seen[slot] == va)
            break;
    if (slot == count) {
        if (count == SLOTS)
            return;
        seen[count++] = va;
    }
    n = ++hits[slot];
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    if (n != 1)
        return;
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    fprintf(stderr, "  hits=%llu esp=%08X eax=%08X ecx=%08X edx=%08X\n",
            (unsigned long long)hits[slot], g_esp, g_eax, g_ecx, g_edx);
    if (g_esp >= 0x10000 && g_esp < 0x03FFFFD0) {
        const uint32_t *stack = (const uint32_t *)(g_xbox_mem_offset + g_esp);
        fprintf(stderr, "  guest stack:");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %08X", stack[i]);
        fprintf(stderr, "\n");
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* The call site: the generated code pushed the return VA before the check,
     * so [esp] names the caller (map it with the .map / function index); ecx is
     * `this` for a vtable call. Without it the line says only that some loop
     * somewhere is calling through a dead object. */
    if (g_esp >= 0x10000 && g_esp < 0x03FFFFD0) {
        fprintf(stderr, " ret=%08X ecx=%08X tid=%lu stack:", GUEST32(g_esp), g_ecx,
                (unsigned long)GetCurrentThreadId());
        for (int k = 1; k < 9; k++)
            fprintf(stderr, " %08X", GUEST32(g_esp + k * 4));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

static uint32_t ma2_primary_input_slot=UINT32_MAX;

void ma2_input_read(uint32_t state_va, uint32_t handle_va, uint32_t status)
{
    XBOX_INPUT_STATE state = {0};
    static RECOMP_TLS uint16_t last_buttons;
    static RECOMP_TLS uint32_t last_packet;
    if (status || !state_va) return;
    /* Xbox input is a 4-byte packet followed by an 18-byte gamepad. */
    memcpy(&state, (const void *)((uintptr_t)state_va + g_xbox_mem_offset), 22);
    /* Retail XInputOpen's handle points to the XID entry, which points to
     * the USB device. +0x14 is the guest's port (sub_004203DC); +5 is its
     * assigned USB address, used by sub_004203E0 to submit transfers. Match
     * that address to the enumerated host pad, since Xbox port numbering
     * depends on the physical controller/hub wiring. */
    uint32_t device=*(const uint32_t *)((uintptr_t)handle_va+g_xbox_mem_offset);
    uint32_t usb=device?*(const uint32_t *)((uintptr_t)device+g_xbox_mem_offset):0;
    uint32_t port=usb?*(const uint32_t *)((uintptr_t)usb+0x14+g_xbox_mem_offset):0xFFFFFFFFu;
    uint8_t address=usb?*(const uint8_t *)((uintptr_t)usb+5+g_xbox_mem_offset):0;
    int host_pad=-1;
    if(address)for(int i=0;i<XBOX_MAX_CONTROLLERS;i++)if(usb_gamepad_address(i)==address){host_pad=i;break;}
    if(host_pad==0 && port<4)ma2_primary_input_slot=port+1;
    if(host_pad==0 && pc_display_movie_active()) {
        int pressed=state.Gamepad.wButtons!=0;
        for(unsigned i=0;i<8;i++)if(state.Gamepad.bAnalogButtons[i]>32)pressed=1;
        if(pressed)pc_input_movie_skip();
    }
    if (host_pad==0) {   /* test harness: RECOMP_TEST_HUMAN_AUTOPILOT (ma2_bots_core.c) */
        extern int ma2_bot_test_human_pad(float *v);
        float tv[20];
        if (ma2_bot_test_human_pad(tv)) {
            static const uint16_t dig[8]={0x1,0x2,0x4,0x8,0x10,0x20,0x40,0x80};
            uint16_t b=0; for(int i=0;i<8;i++) if(tv[i]>0.5f) b|=dig[i];
            state.Gamepad.wButtons=b;
            static const int an[8]={8,9,10,11,12,13,14,15};
            for(int i=0;i<8;i++){ float f=tv[an[i]]; state.Gamepad.bAnalogButtons[i]=(uint8_t)(f<0.f?0:f>1.f?255:(int)(f*255.f)); }
            #define STK(x) ((SHORT)((x)<-1.f?-32767:(x)>1.f?32767:(int)((x)*32767.f)))
            state.Gamepad.sThumbLX=STK(tv[16]); state.Gamepad.sThumbLY=STK(tv[17]);
            state.Gamepad.sThumbRX=STK(tv[18]); state.Gamepad.sThumbRY=STK(tv[19]);
            { static uint32_t pk=0x40000000u; state.dwPacketNumber=++pk; }
            memcpy((void *)((uintptr_t)state_va + g_xbox_mem_offset), &state, 22);
        }
    }
    xbox_InputAcknowledge(&state);
    if (ma2_diag(DIAG_INPUT) &&
        (state.Gamepad.wButtons != last_buttons || state.dwPacketNumber != last_packet)) {
        fprintf(stderr, "[PAD-READ] handle=%08X port=%u host=%d address=%u packet=%u buttons=%04X A=%u sticks=%d,%d,%d,%d\n",
                handle_va,port,host_pad,address,state.dwPacketNumber, state.Gamepad.wButtons,
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_A],
                state.Gamepad.sThumbLX, state.Gamepad.sThumbLY,
                state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);
        last_buttons = state.Gamepad.wButtons;
        last_packet = state.dwPacketNumber;
    }
}

void ma2_input_converted(uint32_t port)
{
    static RECOMP_TLS unsigned printed;
    static RECOMP_TLS unsigned calls;
    static RECOMP_TLS int16_t previous_x, previous_y;
    if (!ma2_diag(DIAG_INPUT) || port >= 4 || printed >= 500) return;
    const int16_t *sticks = (const int16_t *)((uintptr_t)(0x4E4BD2 + port * 0x60) + g_xbox_mem_offset);
    if (!sticks[2] && !sticks[3]) return;
    if (sticks[2] == previous_x && sticks[3] == previous_y && (++calls % 30)) return;
    previous_x = sticks[2]; previous_y = sticks[3];
    const float *actions = (const float *)((uintptr_t)(0x4E37B8 + (port + 1) * 0xF0) + g_xbox_mem_offset);
    fprintf(stderr, "[PAD-CONVERT] port=%u sticks=%d,%d gate=%u,%u mask=%08X axes=%.5f,%.5f,%.5f,%.5f\n",
            port, sticks[2], sticks[3],
            *(const uint8_t *)((uintptr_t)0x4A192C + g_xbox_mem_offset),
            *(const uint8_t *)((uintptr_t)0x4A192D + g_xbox_mem_offset),
            *(const uint32_t *)((uintptr_t)0x50BA9C + g_xbox_mem_offset),
            actions[0], actions[3], actions[6], actions[9]);
    printed++;
}

void ma2_input_mapping(uint32_t object)
{
    static unsigned printed;
    if (!ma2_diag(DIAG_INPUT) || !object || printed++ >= 8) return;
    fprintf(stderr, "[PAD-MAP] object=%08X port=%u layout=%u buttons=%u move-lists=%08X,%08X look-lists=%08X,%08X\n",
            object, GUEST32(object), GUEST32(object+4), GUEST32(object+8),
            GUEST32(object+0x120),GUEST32(object+0x230),GUEST32(object+0x164),GUEST32(object+0x274));
}

static void ma2_mouse_commit(void);
static RECOMP_TLS float ma2_frame_mouse_x,ma2_frame_mouse_y;
static RECOMP_TLS uint32_t ma2_mouse_component;
static RECOMP_TLS unsigned ma2_mouse_primary_context,ma2_mouse_used_x,ma2_mouse_used_y;

/* Private, opt-in read-only input-routing recorder. Never consumes mouse data. */
static int ma2_aim_span(uint32_t p, uint32_t n)
{
    return p >= 0x10000 && n <= 0x04000000 && p <= 0x04000000 - n;
}
static int ma2_aim_vtable(uint32_t p, uint32_t n)
{
    return p >= 0x4258A0 && n <= 0x495D40 - 0x4258A0 && p <= 0x495D40 - n;
}
static int ma2_aim_enabled(void)
{
    static RECOMP_TLS int enabled;
    static RECOMP_TLS unsigned initialized;
    if (!initialized) {
        const char *option = getenv("RECOMP_TANK_AIM_DIAG");
        enabled = option && !strcmp(option, "1");
        initialized = 1;
    }
    return enabled;
}

/* Additive, process-bounded transition recorder. The original 240-row TLS
 * recorder remains intact. Host-only bookkeeping never resets input state. */
static void ma2_aim_transition_trace(const char *where,uint32_t component,unsigned action)
{
    struct Context { uint32_t component,actor,uid,layout; unsigned rows,last; };
    static struct Context seen[128];
    static volatile LONG busy;
    static unsigned count,total,started,first_tick,expired;
    if(!ma2_aim_enabled())return; /* OFF: no guest or clock reads. */
    if(InterlockedCompareExchange(&busy,1,0))return; /* Never wait on the title. */
    if(total>=512 || expired)goto done;
    unsigned now=GetTickCount();
    if(!started){started=1;first_tick=now;}
    if(now-first_tick>10800000u){expired=1;goto done;} /* Permanent three-hour window. */
    if(component!=ma2_mouse_component || !ma2_aim_span(component,0x704))goto done;
    uint32_t table=GUEST32(component);
    if(!ma2_aim_vtable(table,0x154))goto done;
    uint32_t actor=GUEST32(component+0xC);
    if(!ma2_aim_span(actor,0x90))goto done;
    uint32_t actor_table=GUEST32(actor);
    if(!ma2_aim_vtable(actor_table,4))goto done;
    uint32_t uid=GUEST32(actor+0x2C),layout=GUEST32(0x4B296C);
    unsigned index;
    for(index=0;index<count;index++)
        if(seen[index].component==component && seen[index].actor==actor &&
           seen[index].uid==uid && seen[index].layout==layout)break;
    if(index==count){
        if(count>=128)goto done;
        seen[index].component=component;seen[index].actor=actor;
        seen[index].uid=uid;seen[index].layout=layout;count++;
    }
    if(seen[index].rows>=4 || (seen[index].rows && now-seen[index].last<250))goto done;
    uint32_t parent=GUEST32(actor+4),parent_table=0,parent_class=0,parent_uid=0;
    if(ma2_aim_span(parent,0x30)){
        uint32_t t=GUEST32(parent);
        if(ma2_aim_vtable(t,4)){parent_table=t;parent_class=GUEST32(parent+0x28);parent_uid=GUEST32(parent+0x2C);}
    }
    uint32_t root=GUEST32(actor+0x8C),root_table=0,root_flags=0;
    if(ma2_aim_span(root,0x148)){
        uint32_t t=GUEST32(root);
        if(ma2_aim_vtable(t,4)){root_table=t;root_flags=GUEST32(root+0x144);}
    }
    uint32_t tank_root=0,tank_table=0,tank_flags=0;
    /* Original MATank update1757FE uses inline physics actor+488, distinct
     * from actor+8C. Expose identity/flags only for the exact original class. */
    if(actor_table==0x46AE48u && ma2_aim_span(actor,0x5D0)){
        uint32_t t=GUEST32(actor+0x488);
        if(ma2_aim_vtable(t,4)){tank_root=actor+0x488;tank_table=t;tank_flags=GUEST32(tank_root+0x144);}
    }
    uint32_t camera=GUEST32(component+8),camera_table=0;
    if(ma2_aim_span(camera,4)){uint32_t t=GUEST32(camera);if(ma2_aim_vtable(t,4))camera_table=t;}
    fprintf(stderr,"[AIM-TRANSITION] context=%u sample=%u elapsed_ms=%u where=%s action=%u selected=%08X component=%08X cvt=%08X actor=%08X avt=%08X class=%08X uid=%08X parent=%08X pvt=%08X pclass=%08X puid=%08X root=%08X rvt=%08X rflags=%08X tank_inline_root=%08X tivt=%08X tiflags=%08X camera=%08X camvt=%08X mode=%u layout=%u raw=%.6f,%.6f used=%u,%u targets=%08X,%08X\n",
        index,seen[index].rows+1,now-first_tick,where,action,ma2_mouse_component,component,table,
        actor,actor_table,GUEST32(actor+0x28),uid,parent,parent_table,parent_class,parent_uid,
        root,root_table,root_flags,tank_root,tank_table,tank_flags,camera,camera_table,
        *(const uint8_t *)((uintptr_t)component+0x6E4+g_xbox_mem_offset),layout,
        ma2_frame_mouse_x,ma2_frame_mouse_y,ma2_mouse_used_x,ma2_mouse_used_y,
        GUEST32(table+0x88),GUEST32(table+0x98));
    seen[index].rows++;seen[index].last=now;total++;
done:
    InterlockedExchange(&busy,0);
}

static void ma2_aim_trace(const char *where, uint32_t component, unsigned action)
{
    ma2_aim_transition_trace(where,component,action);
    static RECOMP_TLS unsigned printed, last_tick;
    /* Disabled tracing performs zero extra guest reads. */
    if (!ma2_aim_enabled() || printed >= 240) return;
    unsigned now = GetTickCount();
    if (printed && now - last_tick < 100) return;
    if (!ma2_aim_span(component, 0x704)) return;
    uint32_t table = GUEST32(component);
    if (!ma2_aim_vtable(table, 0x154)) return;
    uint32_t actor = GUEST32(component + 0xC), camera = GUEST32(component + 8);
    uint32_t owner = GUEST32(component + 4), owner_flags = 0, owner_class = 0;
    if (ma2_aim_span(owner, 0x2F4) && ma2_aim_vtable(GUEST32(owner), 4)) {
        owner_flags = GUEST32(owner + 0x244); owner_class = GUEST32(owner + 0x28);
    }
    uint32_t actor_table = 0, actor_class = 0, root = 0, root_table = 0;
    uint32_t camera_table = 0;
    float heading = 0, pitch = 0;
    if (ma2_aim_span(actor, 0x90)) {
        uint32_t candidate = GUEST32(actor);
        if (ma2_aim_vtable(candidate, 4)) {
            actor_table = candidate;
            actor_class = GUEST32(actor + 0x28);
            root = GUEST32(actor + 0x8C);
            if (ma2_aim_span(root, 4)) {
                candidate = GUEST32(root);
                if (ma2_aim_vtable(candidate, 4)) root_table = candidate;
            }
        }
    }
    if (ma2_aim_span(camera, 0x148)) {
        uint32_t candidate = GUEST32(camera);
        if (ma2_aim_vtable(candidate, 4)) {
            camera_table = candidate;
            heading = *(const float *)((uintptr_t)camera + 0x11C + g_xbox_mem_offset);
            pitch = *(const float *)((uintptr_t)camera + 0x144 + g_xbox_mem_offset);
        }
    }
    fprintf(stderr, "[TANK-AIM-ROUTE] where=%s action=%u primary=%u selected=%08X component=%08X cvt=%08X actor=%08X avt=%08X class=%08X root=%08X rvt=%08X camera=%08X camvt=%08X yaw=%.6f pitch=%.6f mode=%u inputmode=%u raw=%.6f,%.6f used=%u,%u targets=%08X,%08X values=%.6f,%.6f,%.6f,%.6f flags=%u,%u,%u,%u owner=%08X owner_class=%08X owner_flags=%08X\n",
        where, action, ma2_mouse_primary_context, ma2_mouse_component, component, table,
        actor, actor_table, actor_class, root, root_table, camera, camera_table, heading, pitch,
        *(const uint8_t *)((uintptr_t)component + 0x6E4 + g_xbox_mem_offset), GUEST32(0x4B296C),
        ma2_frame_mouse_x, ma2_frame_mouse_y, ma2_mouse_used_x, ma2_mouse_used_y,
        GUEST32(table + 0x88), GUEST32(table + 0x98),
        *(const float *)((uintptr_t)component + 0x8C + g_xbox_mem_offset),
        *(const float *)((uintptr_t)component + 0xA8 + g_xbox_mem_offset),
        *(const float *)((uintptr_t)component + 0xFC + g_xbox_mem_offset),
        *(const float *)((uintptr_t)component + 0x118 + g_xbox_mem_offset),
        *(const uint8_t *)((uintptr_t)component + 0x6AC + g_xbox_mem_offset),
        *(const uint8_t *)((uintptr_t)component + 0x6AD + g_xbox_mem_offset),
        *(const uint8_t *)((uintptr_t)component + 0x6B0 + g_xbox_mem_offset),
        *(const uint8_t *)((uintptr_t)component + 0x6B1 + g_xbox_mem_offset), owner, owner_class, owner_flags);
    ++printed; last_tick = now;
}
static void ma2_aim_command_trace(uint32_t object, uint32_t history)
{
    if (!ma2_aim_enabled()) return;
    if (!ma2_aim_span(object, 0x2F4) || !ma2_aim_span(history, 4)) return;
    if (!ma2_aim_vtable(GUEST32(object), 4)) return;
    ma2_aim_trace("command-any", GUEST32(object + 0x2F0), GUEST32(history));
}

void ma2_input_context(uint32_t controller)
{
    ma2_mouse_primary_context=controller && GUEST32(controller)==ma2_primary_input_slot;
    if(ma2_diag(DIAG_BOT)&&controller){
        static DWORD last;DWORD now=GetTickCount();
        if(now-last>=1000){last=now;
            fprintf(stderr,"[MOUSE-DIAG] context mapping=%08X slot=%u primary_slot=%u primary=%d component=%08X\n",
                    controller,GUEST32(controller),ma2_primary_input_slot,ma2_mouse_primary_context,ma2_mouse_component);}
    }
}

static float ma2_mouse_look_value(uint32_t component,unsigned axis,float controller_value);
/* Final look value for one axis. Invert flight controls flips the vertical
 * axis while the VTOL (component vtable 0x477570) is the controlled vehicle. */
float ma2_mouse_look(uint32_t component,unsigned axis,float controller_value)
{
    float value=ma2_mouse_look_value(component,axis,controller_value);
    if(!axis || component<0x10000 || component>=0x04000000-4)return value;
    int flight=GUEST32(component)==0x477570u,invert=flight && pc_setting_get(PC_INVERT_FLIGHT,0);
    static unsigned printed;
    if(value && ma2_diag(DIAG_INPUT) && printed++<60)
        fprintf(stderr,"[LOOK-Y] component=%08X vtable=%08X value=%.5f flight=%d inverted=%d\n",
                component,GUEST32(component),value,flight,invert);
    return invert?-value:value;
}

static float ma2_mouse_look_value(uint32_t component,unsigned axis,float controller_value)
{
    ma2_aim_trace("look-before", component, axis ? 5 : 9);
    if(ma2_diag(DIAG_BOT)){
        static DWORD last;DWORD now=GetTickCount();
        if(now-last>=1000){last=now;
            fprintf(stderr,"[MOUSE-DIAG] look component=%08X mouse_component=%08X locked=%u frame_mouse=%.4f,%.4f\n",
                    component,ma2_mouse_component,component?*(uint8_t *)((uintptr_t)component+0x6E4+g_xbox_mem_offset):0,
                    ma2_frame_mouse_x,ma2_frame_mouse_y);}
    }
    if(!component || component!=ma2_mouse_component || *(uint8_t *)((uintptr_t)component+0x6E4+g_xbox_mem_offset))
        return controller_value;
    /* Physics and camera integrate using the title's simulation step. Divide
     * pixel displacement by that same step, so caps and slow frames cannot
     * change degrees per mouse count. The controller path stays unchanged. */
    double step=*(const double *)((uintptr_t)0x4E2428+g_xbox_mem_offset);
    if(!(step>0 && step<=1))return controller_value;
    float delta=0;
    if(!axis && !ma2_mouse_used_x) { delta=ma2_frame_mouse_x;ma2_mouse_used_x=1; }
    if(axis && !ma2_mouse_used_y) { delta=-ma2_frame_mouse_y;ma2_mouse_used_y=1; }
    delta/=(float)step;
    static unsigned printed[2];
    if(delta && ma2_diag(DIAG_INPUT) && printed[axis?1:0]++<80) {
        fprintf(stderr,"[MOUSE-LOOK] component=%08X axis=%u pad=%.5f mouse=%.5f dt=%.6f time=%u\n",
                component,axis,controller_value,delta,step,GUEST32(0x4E2430));
        if(printed[axis?1:0]==1) {
            uint32_t actor=GUEST32(component+0xC),table=actor?GUEST32(actor):0;
            fprintf(stderr,"[MOUSE-CONTEXT] actor=%08X table=%08X yaw260=%08X inputmode=%u\n",
                    actor,table,table?GUEST32(table+0x260):0,GUEST32(0x4B296C));
        }
    }
    return controller_value+delta;
}


/* The original POV pitch tail call bypasses manual dispatch. Add host mouse
 * displacement only for the primary component already selected by input.
 * Controller inversion, authored gain, integer units and clamps stay in the
 * original translated route. ma2_mouse_look divides displacement by dt once. */
/* Tank action9's actor callback is an authored RET8. Its control pass
 * 1753E6 instead sends the final yaw argument through 109FF4 to the inline
 * turret physics actor+488. Add mouse displacement after the pad curve,
 * leaving the three hull movement/heading arguments unchanged. */
float ma2_mouse_tank_yaw(uint32_t component,float controller_value)
{
    if(component!=ma2_mouse_component || component<0x10000u ||
       component>0x04000000u-0x708u || GUEST32(component)!=0x477850u)
        return controller_value;
    uint32_t actor=GUEST32(component+0xC);
    if(actor<0x10000u || actor>0x04000000u-0x5D0u ||
       GUEST32(actor)!=0x46AE48u || GUEST32(actor+0x488)!=0x4930B8u ||
       (int32_t)GUEST32(actor+0x184)<=0 ||
       (*(uint8_t *)((uintptr_t)actor+0x5CC+g_xbox_mem_offset)&1u))
        return controller_value;
    return ma2_mouse_look(component,0,controller_value);
}

float ma2_mouse_pov_pitch(uint32_t actor,float controller_value)
{
    uint32_t component=ma2_mouse_component;
    if(component<0x10000u || component>0x04000000u-0x708u || !actor ||
       GUEST32(component)!=0x4772A0u || GUEST32(component+0xC)!=actor ||
       *(uint8_t *)((uintptr_t)component+0x6E4+g_xbox_mem_offset))
        return controller_value;
    return ma2_mouse_look(component,1,controller_value);
}


/* Primary = a local human player whose own input component (MAInput child,
 * class MEM32(0x4EAFA4), +0x7C clear) reads the primary controller slot.
 * Bots never qualify. Replaces "the last input context", which bots break. */
int ma2_bot_is_player(uint32_t player);
static int ma2_object_primary(uint32_t object)
{
    if(object<0x10000u || object>0x04000000u-0x300u) return 0;
    if(!(GUEST32(object+0x244)&4u) || ma2_bot_is_player(object)) return 0;
    uint32_t cls=GUEST32(0x4EAFA4),e=GUEST32(object+0xC);
    for(int n=0;n<64 && e>=0x10000u && e<0x04000000u-0x90u;n++,e=GUEST32(e+8))
        if(GUEST32(e+0x28)==cls && !*(uint8_t *)((uintptr_t)e+0x7C+g_xbox_mem_offset))
            return GUEST32(e+0x80)==ma2_primary_input_slot;
    return 0;
}
extern void ma2_tank_fixed_input_select(uint32_t object);
void ma2_input_command(uint32_t object,uint32_t history,uint32_t handler)
{
    int primary=ma2_object_primary(object);
    if(primary)ma2_tank_fixed_input_select(object);
    ma2_aim_command_trace(object, history);
    static unsigned printed;
    if(!history)return;
    uint32_t action=GUEST32(history);
    if(action!=5 && action!=9)return;
    if(primary)ma2_mouse_component=GUEST32(object+0x2F0);
    if(!ma2_diag(DIAG_INPUT))return;
    if(primary && ma2_mouse_component>=0x10000 && ma2_mouse_component<0x04000000-16) {
        /* The detailed command trace is intentionally bounded, but a new
         * player after death/reload must remain observable in long probes.
         * This reads the component referenced by the current input command. */
        static RECOMP_TLS uint32_t last_actor,last_report;
        uint32_t actor=GUEST32(ma2_mouse_component+0xC),now=GetTickCount();
        if(actor>=0x10000 && actor<0x04000000-0x4CC &&
           (actor!=last_actor || now-last_report>=1000)) {
            uint32_t table=GUEST32(actor);
            if(table>=0x4258A0 && table<0x495D40) {
                fprintf(stderr,"[PLAYER-CONTEXT] actor=%08X table=%08X component=%08X time=%u\n",
                        actor,table,ma2_mouse_component,GUEST32(0x4E2430));
                last_actor=actor;last_report=now;
            }
        }
    }
    if(printed>=120)return;
    const float *h=(const float *)((uintptr_t)history+g_xbox_mem_offset);
    if(!h[1])return;
    uint32_t component=GUEST32(object+0x2F0),vtable=GUEST32(component);
    fprintf(stderr,"[CAMERA-COMMAND] object=%08X action=%u handler=%08X target=%08X component=%08X vtable=%08X value=%.5f held=%.5f mouse=%.5f,%.5f time=%u\n",
            object,action,handler,GUEST32(vtable+(action==9?0x98:0x88)),component,vtable,h[1],h[3],ma2_frame_mouse_x,ma2_frame_mouse_y,GUEST32(0x4E2430));
    printed++;
}

void ma2_input_committed(uint32_t caller)
{
    ma2_pc_input_context_update();
    ma2_mouse_commit();
    static unsigned calls, printed;
    static unsigned loop_calls,loop_tick;
    if(ma2_diag(DIAG_INPUT)) {
        unsigned tick=GetTickCount();
        ++loop_calls;
        if(tick-loop_tick>=1000) {
            if(loop_calls>1000)fprintf(stderr,"[PAD-LOOP] calls=%u caller=%08X confirm=%.3f,%.3f time=%u\n",
                loop_calls,caller,*(float *)((uintptr_t)0x4E32F0+g_xbox_mem_offset),
                *(float *)((uintptr_t)0x4E32FC+g_xbox_mem_offset),GUEST32(0x4E2430));
            loop_calls=0;loop_tick=tick;
        }
    }
    if (!ma2_diag(DIAG_INPUT) || (++calls % 30) || printed >= 400) return;
    const float *all = (const float *)((uintptr_t)0x4E3248+g_xbox_mem_offset);
    if (!(all[48] || all[51] || all[54] || all[57] || all[228] || all[231] || all[234] || all[237])) return;
    fprintf(stderr, "[PAD-COMMIT] axes=%.4f,%.4f,%.4f,%.4f port2=%.4f,%.4f,%.4f,%.4f time=%u\n",
            all[48],all[51],all[54],all[57],all[228],all[231],all[234],all[237],
            GUEST32(0x4E2430));
    printed++;
}

static void ma2_mouse_commit(void)
{
    static RECOMP_TLS unsigned printed;
    ma2_aim_trace("commit-previous", ma2_mouse_component, UINT32_MAX);
    ma2_frame_mouse_x=ma2_frame_mouse_y=0;
    ma2_mouse_used_x=ma2_mouse_used_y=0;
    if(ma2_primary_input_slot>4)return;
    /* Convert once to displacement; the final look hook supplies the actual
     * simulation duration. This avoids the deadzone and nonlinear stick curve. */
    const double elapsed=.01;
    float mouse_x,mouse_y;
    if(!xbox_InputReadMouseAxes(&mouse_x,&mouse_y,elapsed))return;
    ma2_frame_mouse_x=mouse_x*(float)elapsed;ma2_frame_mouse_y=mouse_y*(float)elapsed;
    if(ma2_diag(DIAG_INPUT) && printed++<100)
        fprintf(stderr,"[MOUSE-CAMERA] slot=%u dt=%.6f axes=%.4f,%.4f\n",ma2_primary_input_slot,elapsed,mouse_x,mouse_y);
}

void ma2_input_evaluated(uint32_t object)
{
    static unsigned calls,printed;
    if(!ma2_diag(DIAG_INPUT) || (++calls%16) || printed>=300 || !object)return;
    const float *p=(const float *)((uintptr_t)object+g_xbox_mem_offset);
    float values[4]={p[0x14C/4],p[0x190/4],p[0x25C/4],p[0x2A0/4]};
    if(!(values[0] || values[1] || values[2] || values[3]))return;
    fprintf(stderr,"[PAD-EVALUATE] object=%08X time=%u action4/5/8/9=%.4f,%.4f,%.4f,%.4f active=%X,%X,%X,%X\n",
        object,GUEST32(0x4E2430),values[0],values[1],values[2],values[3],
        GUEST32(object+0x144),GUEST32(object+0x188),GUEST32(object+0x254),GUEST32(object+0x298));
    printed++;
}

/* Campaign coverage overrides require a bounded probe and a dedicated save
 * folder. They never alter the normal player's progress or startup behavior. */
static int ma2_campaign_probe_enabled(void)
{
    const char *save=getenv("RECOMP_SAVE_DIR"),*watch=getenv("RECOMP_WATCHDOG_SECS");
    return save && watch && atoi(watch)>0 &&
        (strstr(save,"bringup\\logs\\") || strstr(save,"bringup/logs/"));
}

void ma2_asset_error(uint32_t record,uint32_t file,uint32_t error)
{
    fprintf(stderr,"[ASSET-ERROR] archive=%.255s resource=%08X file=%08X error=%u status=%08X file_error=%u length=%u\n",
        (const char *)((uintptr_t)0x50B908+g_xbox_mem_offset),GUEST32(record+8),file,error,
        GUEST32(file),GUEST32(file+4),GUEST32(file+0x20));
}

void ma2_asset_status(uint32_t manager,uint32_t record)
{
    if(!ma2_diag(DIAG_LOAD) || (record && (GUEST32(record)&0x20000)))return;
    fprintf(stderr,"[ASSET-STATUS] archive=%.255s manager=%08X entries=%u record=%08X flags=%08X error=%u file=%08X\n",
        (const char *)((uintptr_t)0x50B908+g_xbox_mem_offset),manager,GUEST32(manager+0xC),record,
        record?GUEST32(record):0,record?GUEST32(record+4):0,record?GUEST32(record+0x18):0);
}

void ma2_disc_error(uint32_t stack)
{
    fprintf(stderr,"[DISC-ERROR] archive=%.255s esp=%08X caller=%08X frame=%08X\n",
        (const char *)((uintptr_t)0x50B908+g_xbox_mem_offset),stack,GUEST32(stack),g_ebp);
    for(unsigned i=0;i<24;i++)fprintf(stderr,"[DISC-STACK] %08X %08X\n",stack+i*4,GUEST32(stack+i*4));
}

uint32_t ma2_stream_read_size(uint32_t stream,uint32_t requested)
{
    /* A buffered refill can start on a sector boundary near EOF. Limit the
     * request to real file bytes; the title's asynchronous completion checks
     * the requested count exactly and interprets a short read as disc failure.
     * Keep buffer capacity separate from the amount valid in this refill. */
    uint32_t size=GUEST32(stream+0xC),offset=GUEST32(stream+0x24);
    uint32_t remaining=offset<size?size-offset:0;
    if(requested>remaining) {
        if(ma2_diag(DIAG_LOAD))fprintf(stderr,"[STREAM-TAIL] stream=%08X offset=%u size=%u requested=%u actual=%u\n",
            stream,offset,size,requested,remaining);
        return remaining;
    }
    return requested;
}

extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *,uint64_t,unsigned int);
extern uint8_t *mcpx_apu_phys(uint64_t address);
void ma2_audio_wait_timeout(uint32_t object)
{
    static unsigned printed;
    if(!ma2_diag(DIAG_AUDIO) || printed++>=80)return;
    uint32_t state_base=(uint32_t)mcpx_apu_mmio_read(g_apu_state,0x202C,4);
    uint16_t voice=*(uint16_t *)((uintptr_t)object+0xC+g_xbox_mem_offset);
    fprintf(stderr,"[VOICE-TIMEOUT] object=%08X flags=%04X count=%u voice=%u hwstate=%08X env=%08X irql=%u fectl=%08X ists=%08X ien=%08X feav=%08X\n",
        object,*(uint16_t *)((uintptr_t)object+0x12+g_xbox_mem_offset),
        *(uint8_t *)((uintptr_t)object+0x64+g_xbox_mem_offset),voice,
        voice<256?*(uint32_t *)mcpx_apu_phys((uint64_t)state_base+voice*128+0x54):0,
        voice<256?*(uint32_t *)mcpx_apu_phys((uint64_t)state_base+voice*128+0x34):0,xbox_KeGetCurrentIrql(),
        (uint32_t)mcpx_apu_mmio_read(g_apu_state,0x1100,4),
        (uint32_t)mcpx_apu_mmio_read(g_apu_state,0x1000,4),
        (uint32_t)mcpx_apu_mmio_read(g_apu_state,0x1004,4),
        (uint32_t)mcpx_apu_mmio_read(g_apu_state,0x1118,4));
}

void ma2_probe_campaign(uint32_t manager,uint32_t campaign)
{
    const char *test=getenv("RECOMP_TEST_MISSION_INDEX");
    if((!test && !getenv("RECOMP_MISSION_DIAG")) || !manager || campaign>=2)return;
    uint32_t records=GUEST32(manager+0x24),record=records+campaign*0x11C;
    if(!records || record>0x03FFFE00)return;
    unsigned difficulty=GUEST32(record+0x90);
    if(difficulty>2)return;
    fprintf(stderr,"[CAMPAIGN-PROFILE] campaign=%u difficulty=%u unlocked=%u,%u,%u checkpoint=%u\n",
        campaign,difficulty,GUEST32(record+0x64),GUEST32(record+0x70),
        GUEST32(record+0x7C),GUEST32(record+0x98));
    if(test && ma2_campaign_probe_enabled()) {
        int index=atoi(test);
        if(index>=0 && index<41) {
            GUEST32(record+0x64+difficulty*12)=(uint32_t)index;
            fprintf(stderr,"[CAMPAIGN-PROBE] set isolated campaign destination to index=%d\n",index);
        }
    }
    /* Test saves only: put campaign 0 back to a fully unlocked profile (all three difficulties, checkpoint 1). */
    if(test && campaign==0 && getenv("RECOMP_TEST_PROFILE_FIX") && ma2_campaign_probe_enabled()) {
        for(unsigned k=0;k<3;k++)GUEST32(record+0x64+k*12)=40;
        GUEST32(record+0x98)=1;
        fprintf(stderr,"[CAMPAIGN-PROBE] restored test profile: unlocked=40,40,40 checkpoint=1\n");
    }
}

/* Inspect the retail mission picker; optionally select a test mission before
 * the owned probe presses A. The retail loader keeps every mission's metadata. */
void ma2_scn_mission_picker(uint32_t object);   /* scenario/ma2_scenario.c */
void ma2_probe_missions(uint32_t object,uint32_t selected)
{
    ma2_scn_mission_picker(object);   /* custom scenarios: the editor Test file starts selected */
    static uint32_t last_object,last_count,last_selected=UINT32_MAX;
    static uint32_t chosen_object;
    const char *test=getenv("RECOMP_TEST_MISSION_INDEX");
    if((!test && !getenv("RECOMP_MISSION_DIAG")) || !object || object>0x03FFFF00)return;
    uint32_t first=GUEST32(object+0xAC),end=GUEST32(object+0xB0);
    if(!first || end<first || end>0x04000000 || (end-first)%0x1C)return;
    unsigned count=(end-first)/0x1C;
    if(!count || count>128)return;
    if(test && chosen_object!=object && ma2_campaign_probe_enabled()) {
        int index=atoi(test);
        if(index>=0 && (unsigned)index<count) {
            GUEST32(object+0x154)=(uint32_t)index;
            chosen_object=object;
            fprintf(stderr,"[MISSION-PROBE] selected index=%d in retail mission picker\n",index);
        }
    }
    if(object==last_object && count==last_count && selected==last_selected)return;
    last_object=object;last_count=count;last_selected=selected;
    fprintf(stderr,"[MISSION-LIST] object=%08X selected=%d current=%d count=%u\n",
            object,(int)selected,(int)GUEST32(object+0x154),count);
    for(unsigned i=0;i<count;i++) {
        uint32_t entry=first+i*0x1C,name=GUEST32(entry);
        char label[128]={0};
        if(name && name<0x03FFFE00)for(unsigned n=0;n<127;n++) {
            uint16_t c=*(const uint16_t *)((uintptr_t)name+n*2+g_xbox_mem_offset);
            if(!c)break;label[n]=c<128?(char)c:'?';
        }
        fprintf(stderr,"[MISSION-ENTRY] %u name=%s id=%08X level=%u available=%u\n",
                i,label,GUEST32(entry+0xC),GUEST32(entry+0x14),
                *(const uint8_t *)((uintptr_t)entry+0x18+g_xbox_mem_offset));
    }
}

/* VtolSnapFix (RECOMP_VTOL_SNAP_FIX, default on; 0 = original). The flyer's horizontal drag sub_0019236B zeroes the
 * horizontal velocity whenever vx^2+vz^2 < 0.5 (0x4BB180); the thrust step sub_00192724 runs after it and adds a*dt
 * (~95 m/s^2). At 60 fps one step adds ~1.6 m/s and clears the snap; at 144+ fps (0.66 m/s, 240 fps 0.40 m/s) every
 * step was zeroed again, so a craft reversing or starting from a hover sat at a*dt until a contact freed it (playtest,
 * 11-04 recording t=9.5-13.4 s). While the craft's commanded velocity (target state +0x7C, vel at body+0x8C/+0x94)
 * passes the thrust step's own gate (0x4BB174, 0.05) the snap is skipped; with no thrust it stops the craft as before. */
int ma2_vtol_snap_keep(uint32_t body)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VTOL_SNAP_FIX"); on = !(e && *e == '0'); }
    if (!on || body < 0x10000u || body > 0x3FFF000u) return 0;
    float tx = *(float *)(g_xbox_mem_offset + body + 0x8Cu), tz = *(float *)(g_xbox_mem_offset + body + 0x94u);
    float gate = *(float *)(g_xbox_mem_offset + 0x4BB174u);
    return tx * tx + tz * tz >= gate;
}

/* RECOMP_LENS_TRACE=1 (diagnostic, off by default): MAModSubObjectLensFilter updates (sub_0013FC88) and lens
 * filter draws (sub_000AFCB0) with the game clock, to see whether a drawn screen flash is still being faded. */
static int lens_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_LENS_TRACE"); on = v && atoi(v) != 0; }
    return on;
}
#define LT_U32(a) (*(volatile uint32_t *)(g_xbox_mem_offset + (uint32_t)(a)))
#define LT_U8(a)  (*(volatile uint8_t *)(g_xbox_mem_offset + (uint32_t)(a)))
#define LT_F(a)   (*(volatile float *)(g_xbox_mem_offset + (uint32_t)(a)))
#define LT_D(a)   (*(volatile double *)(g_xbox_mem_offset + (uint32_t)(a)))
static struct { uint32_t filt, lf; double t; int logged; } s_lens_seen[64]; static unsigned s_lens_next;
void ma2_lens_trace_update(uint32_t lf)
{
    if (!lens_trace_on() || !lf) return;
    fprintf(stderr, "[LENS] upd t=%.3f dt=%.4f lf=%08X filt=%08X cur=%.3f delta=%.3f first=%u single=%u", LT_D(0x4E2440), LT_D(0x4E2428), lf,
            LT_U32(lf + 0xCC), LT_F(lf + 0xDC), LT_F(lf + 0xD8), LT_U8(lf + 0xE0), LT_U8(0x50C85C));
    {   /* the per-view list the filters are made for (sub_00018F30()+0x1C) and this sub-object's {view, filter} pairs */
        uint32_t g = LT_U32(0x524A5C);
        if (g) { fprintf(stderr, " views:"); for (uint32_t p = LT_U32(g + 0x1C), e = LT_U32(g + 0x20), n = 0; p && p < e && n < 8; p += 4, n++) fprintf(stderr, " %08X", LT_U32(p)); }
        fprintf(stderr, " pairs:");
        for (uint32_t p = LT_U32(lf + 0xFC), e = LT_U32(lf + 0x100), n = 0; p && p < e && n < 8; p += 8, n++)
            fprintf(stderr, " %08X>%08X(v%u a%.3f)", LT_U32(p), LT_U32(p + 4), LT_U32(p + 4) ? LT_U8(LT_U32(p + 4) + 8) : 0, LT_U32(p + 4) ? LT_F(LT_U32(p + 4) + 0x18) : 0.f);
        fputc('\n', stderr);
    }
    /* single-view filter at +0xCC; per-player filters as {player, filter} pairs in [+0xFC, +0x100) */
    uint32_t list[9]; int nf = 0;
    if (LT_U32(lf + 0xCC)) list[nf++] = LT_U32(lf + 0xCC);
    for (uint32_t p = LT_U32(lf + 0xFC), e = LT_U32(lf + 0x100); p && p < e && nf < 9; p += 8) if (LT_U32(p + 4)) list[nf++] = LT_U32(p + 4);
    for (int k = 0; k < nf; k++) {
        uint32_t f = list[k];
        int slot = -1;
        for (int i = 0; i < 64; i++) if (s_lens_seen[i].filt == f) { slot = i; break; }
        if (slot < 0) { slot = (int)(s_lens_next++ & 63u); s_lens_seen[slot].filt = f; }
        s_lens_seen[slot].lf = lf; s_lens_seen[slot].t = LT_D(0x4E2440); s_lens_seen[slot].logged = 0;
    }
}
void ma2_lens_trace_draw(uint32_t f)
{
    if (!lens_trace_on() || !f || !LT_U8(f + 8) || !(LT_F(f + 0x18) > 0.001f)) return;
    fprintf(stderr, "[LENS] draw t=%.3f filt=%08X a=%.3f rgb=%.2f,%.2f,%.2f tex=%08X\n", LT_D(0x4E2440), f, LT_F(f + 0x18),
            LT_F(f + 0xC), LT_F(f + 0x10), LT_F(f + 0x14), LT_U32(f + 0x1C));
    for (int i = 0; i < 64; i++) {
        if (s_lens_seen[i].filt != f) continue;
        uint32_t lf = s_lens_seen[i].lf, a4 = LT_U32(lf + 0xA4), own = LT_U32(lf + 0x80);
        double age = LT_D(0x4E2440) - s_lens_seen[i].t;
        if (age > 0.15 && s_lens_seen[i].logged < 3) {
            s_lens_seen[i].logged++;
            fprintf(stderr, "[LENS] STALE filt=%08X lf=%08X age=%.3f +70=%08X +74=%u +6C=%u +64=%u +68=%u now=%u +78=%u +84=%u +88=%u "
                    "+A4=%08X(+34=%08X) +AC=%08X +B8=%08X +60=%08X +80=%08X(own+80=%08X own+1C=%08X)\n",
                    f, lf, age, LT_U32(lf + 0x70), LT_U32(lf + 0x74), LT_U32(lf + 0x6C), LT_U32(lf + 0x64), LT_U32(lf + 0x68), LT_U32(0x4E2430),
                    LT_U8(lf + 0x78), LT_U32(lf + 0x84), LT_U8(lf + 0x88), a4, a4 ? LT_U32(a4 + 0x34) : 0, LT_U32(lf + 0xAC), LT_U32(lf + 0xB8),
                    LT_U32(lf + 0x60), own, own ? LT_U32(own + 0x80) : 0, own ? LT_U32(own + 0x1C) : 0);
        }
        break;
    }
}

/* Screen-tint fix (R26 tint, 2026-11-10): frozen explosion lens flash.
 * MAModSubObjectLensFilter (explosion/hit effects, e.g. Expl_Mech_01: StartAlpha 0.2, DeltaAlpha 0.6/s, white) shows
 * a full-screen colour quad (filter object 0x463D74, drawn by sub_000AFCB0 after the 3D scene, before the HUD)
 * when its effect comes into view, and fades it. The quad's visible flag (+8) and alpha (+0x18) are only written
 * by the sub-object's own per-frame update (sub_0013FC88): visible=1 with alpha -= dt*delta while in view, 0 when
 * out of view. Any frame where that update does not run (the modifier gate sub_002B1270 -> sub_00107DA3, the
 * sub-object's timing window, or no camera for the view) leaves the last state on the view's overlay list, so a
 * flash caught mid-fade stays on screen at constant strength until the effect is destroyed (seconds): the uniform
 * grey-white haze. Fix: a lens quad is drawn only if its update showed it in this frame or the one before;
 * otherwise it is hidden, exactly as the out-of-view branch would. Other filter users (screen fades) are untouched.
 * RECOMP_LENS_FREEZE_FIX=0 restores the old behaviour (A/B). RECOMP_TEST_LENS_FREEZE=<s> (test only) skips a lens
 * sub-object's update for <s> seconds right after its flash first shows, reproducing the freeze on demand. */
#define LENS_VIS_N 256u
static struct { uint32_t filt, frame; } s_lens_vis[LENS_VIS_N];
static int lens_fix_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_LENS_FREEZE_FIX"); on = !(v && *v == '0'); fprintf(stderr, "[LENS] freeze fix %s\n", on ? "on" : "off"); }
    return on;
}
static unsigned lens_vis_find(uint32_t f, int insert)
{
    unsigned h = (f >> 4) * 2654435761u >> 24, empty = LENS_VIS_N;
    for (unsigned i = 0; i < LENS_VIS_N; i++) {
        unsigned k = (h + i) & (LENS_VIS_N - 1);
        if (s_lens_vis[k].filt == f) return k;
        if (!s_lens_vis[k].filt) { if (empty == LENS_VIS_N) empty = k; break; }
    }
    return insert ? empty : LENS_VIS_N;
}
static void lens_vis_erase(unsigned k)
{   /* linear-probing delete: re-insert the rest of the cluster */
    s_lens_vis[k].filt = 0;
    for (unsigned i = (k + 1) & (LENS_VIS_N - 1); s_lens_vis[i].filt; i = (i + 1) & (LENS_VIS_N - 1)) {
        uint32_t f = s_lens_vis[i].filt, fr = s_lens_vis[i].frame;
        s_lens_vis[i].filt = 0;
        unsigned j = lens_vis_find(f, 1);
        if (j < LENS_VIS_N) { s_lens_vis[j].filt = f; s_lens_vis[j].frame = fr; }
    }
}
/* sub_0013FC88 / sub_0013FD69 just set filter+8 = 1 for this frame */
void ma2_lens_shown(uint32_t filt)
{
    if (!filt || !lens_fix_on()) return;
    unsigned k = lens_vis_find(filt, 1);
    if (k < LENS_VIS_N) { s_lens_vis[k].filt = filt; s_lens_vis[k].frame = LT_U32(0x4E2420); }
}
/* sub_000AFCB0 entry: hide a lens quad whose update has not shown it since the frame before */
void ma2_lens_draw_check(uint32_t filt)
{
    if (!filt || !lens_fix_on()) return;
    unsigned k = lens_vis_find(filt, 0);
    if (k >= LENS_VIS_N) return;
    if ((uint32_t)(LT_U32(0x4E2420) - s_lens_vis[k].frame) <= 1u) return;
    if (LT_U8(filt + 8)) {
        if (lens_trace_on())
            fprintf(stderr, "[LENS] hide frozen t=%.3f filt=%08X a=%.3f last_shown_frame=%u frame=%u\n", LT_D(0x4E2440), filt,
                    LT_F(filt + 0x18), s_lens_vis[k].frame, LT_U32(0x4E2420));
        LT_U8(filt + 8) = 0;
    }
    lens_vis_erase(k);
}
/* RECOMP_OVERLAY_TRACE=1 (diagnostic, off by default; R27 tint2): one 'D' line per visible full-screen filter quad
 * (class 0x463D74) drawn by sub_000AFCB0, into the overlay trace file (nv2a_overlay_trace_file, default
 * overlay_trace.txt in the working directory). Fields: guest frame (0x4E2420), game time, scaled dt (0x4E2428), filter,
 * creation site (return address of the sub_000AFBA0 ctor call: 0x11BCCF/0x11BDD1/0x11BE6A camera +D4/+CC/+C8 fades,
 * 0x13F653/0x13F750 lens filter single/per-view, 0x1C55E6/0x1C57ED class 0x47EC10), alpha, RGB, texture, and for lens
 * quads the frame its update last showed it (ma2_lens_shown; -1 = never). */
extern FILE *nv2a_overlay_trace_file(void);
#define OVL_SITE_N 1024u
static struct { uint32_t filt, site; } s_ovl_site[OVL_SITE_N];
void ma2_overlay_note_ctor(uint32_t filt, uint32_t ret)
{
    if (!filt || !nv2a_overlay_trace_file()) return;
    unsigned h = (filt >> 4) * 2654435761u >> 22;
    for (unsigned i = 0; i < OVL_SITE_N; i++) {
        unsigned k = (h + i) & (OVL_SITE_N - 1);
        if (!s_ovl_site[k].filt || s_ovl_site[k].filt == filt) { s_ovl_site[k].filt = filt; s_ovl_site[k].site = ret; return; }
    }
}
static uint32_t ovl_site(uint32_t filt)
{
    unsigned h = (filt >> 4) * 2654435761u >> 22;
    for (unsigned i = 0; i < OVL_SITE_N; i++) {
        unsigned k = (h + i) & (OVL_SITE_N - 1);
        if (s_ovl_site[k].filt == filt) return s_ovl_site[k].site;
        if (!s_ovl_site[k].filt) return 0;
    }
    return 0;
}
void ma2_overlay_trace_draw(uint32_t filt)
{
    FILE *f = nv2a_overlay_trace_file();
    if (!f || !filt || !LT_U8(filt + 8) || !(LT_F(filt + 0x18) > 0.002f)) return;
    unsigned k = lens_vis_find(filt, 0);
    long last = k < LENS_VIS_N ? (long)s_lens_vis[k].frame : -1;
    fprintf(f, "D %u %.4f %.5f %08X %06X %.4f %.3f %.3f %.3f %08X %ld\n", LT_U32(0x4E2420), LT_D(0x4E2440), LT_D(0x4E2428),
            filt, ovl_site(filt), LT_F(filt + 0x18), LT_F(filt + 0xC), LT_F(filt + 0x10), LT_F(filt + 0x14), LT_U32(filt + 0x1C), last);
}

/* sub_0013F832 (lens sub-object release): its filters are freed, forget them */
void ma2_lens_forget(uint32_t lf)
{
    if (!lf) return;
    uint32_t f = LT_U32(lf + 0xCC);
    unsigned k;
    if (f && (k = lens_vis_find(f, 0)) < LENS_VIS_N) lens_vis_erase(k);
    for (uint32_t p = LT_U32(lf + 0xFC), e = LT_U32(lf + 0x100), n = 0; p && p < e && n < 16; p += 8, n++)
        if ((f = LT_U32(p + 4)) && (k = lens_vis_find(f, 0)) < LENS_VIS_N) lens_vis_erase(k);
}
/* RECOMP_TEST_LENS_FREEZE=<seconds>: nonzero when sub_0013FC88 should skip this lens sub-object's update */
int ma2_lens_test_skip(uint32_t lf)
{
    static double secs = -1.0;
    static uint32_t frozen_lf; static double until;
    if (secs < 0.0) { const char *v = getenv("RECOMP_TEST_LENS_FREEZE"); secs = v ? atof(v) : 0.0; }
    if (secs <= 0.0 || !lf) return 0;
    double now = LT_D(0x4E2440);
    if (frozen_lf == lf) {
        if (now < until) return 1;
        frozen_lf = 0; return 0;
    }
    if (frozen_lf) return 0;
    /* freeze the first lens sub-object seen with a visible, still-bright flash */
    uint32_t f = LT_U32(lf + 0xCC);
    int bright = f && LT_U8(f + 8) && LT_F(f + 0x18) > 0.05f;
    for (uint32_t p = LT_U32(lf + 0xFC), e = LT_U32(lf + 0x100), n = 0; !bright && p && p < e && n < 16; p += 8, n++)
        if ((f = LT_U32(p + 4)) && LT_U8(f + 8) && LT_F(f + 0x18) > 0.05f) bright = 1;
    if (!bright) return 0;
    frozen_lf = lf; until = now + secs;
    fprintf(stderr, "[LENS] TEST freeze lf=%08X filt=%08X a=%.3f for %.1fs at t=%.3f\n", lf, f, LT_F(f + 0x18), secs, now);
    return 1;
}
