/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
/* RECOMP_TLS, and the guest stack pointer the contention report walks. */
#include "xbox_memory_layout.h"
#include "../platform/pc_switch.h"     /* T1-1 O5 */
#include "../platform/pc_cpu_topo.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)(xbox_wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (direct 1:1 mapping)
 * ============================================================================ */

/*
 * Critical sections, shadowed onto real host locks.
 *
 * These used to be no-ops, on the stated grounds that every Xbox thread ran
 * synchronously so there was no contention. That stopped being true when
 * PsCreateSystemThreadEx began spawning real threads: a title's worker and its
 * main thread now run at the same time on different host threads, and the
 * locks the title takes to keep them apart did nothing.
 *
 * The failure that produced is worth describing, because it looks like
 * anything but a lock. Half-Life 2's level load faulted intermittently -- four
 * runs, three different functions, one clean -- and any attempt to observe it
 * made it disappear, including two plain stores to globals. Registers held
 * fragments of strings where pointers belonged, which is what a half-updated
 * structure looks like from the other thread.
 *
 * The Xbox RTL_CRITICAL_SECTION is 20 bytes of 32-bit layout and a Win32
 * CRITICAL_SECTION is 40 bytes of 64-bit layout, so the guest's own structure
 * cannot back a host lock. Shadow it instead: the guest pointer is the key,
 * the host lock lives here, and the guest's bytes are left alone. Win32
 * critical sections are recursive, which matches RTL semantics.
 *
 * Lazily created on first use rather than in RtlInitializeCriticalSection,
 * because a title can perfectly well use a statically zeroed one it never
 * announced.
 */
/* Shadow slots are allocated in chunks and never move or die: a Win32 critical
 * section must not move once initialized, and Xbox titles have no call that
 * deletes theirs. A guest-address index points at the slots.
 *
 * That index used to be one fixed 4096-slot open-addressing table. MechAssault
 * 2 creates locks for transient objects at a steady rate and filled it about
 * seven minutes into a mission; from then on every Enter/Leave of a lock that
 * did not fit scanned all 4096 slots twice (the second time under the
 * exclusive table lock) and returned no lock at all. Checkpoint saves took
 * 5 s instead of 0.2 s, returning to the menu up to 19 s, gameplay degraded,
 * and those guest locks were left unprotected. The index now doubles whenever
 * it is half full, so a lookup is one or two probes however many locks exist.
 * Readers take the index lock shared; the per-thread cache still answers
 * repeated Enter/Leave pairs without it. Host locks skip Win32 debug info:
 * there can be tens of thousands and nothing reads it. */
#define XBOX_CS_CHUNK      4096
#define XBOX_CS_MAX_CHUNKS 4096    /* 16 M guest locks */
#ifndef CRITICAL_SECTION_NO_DEBUG_INFO
#define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000
#endif

typedef struct {
    uintptr_t        key;        /* guest critical section */
    CRITICAL_SECTION cs;
    /* Who holds it, and the guest call site that took it.
     *
     * A deadlock report can already say which lock and which thread. What it
     * cannot say is where the holder took it, and that is the only part that
     * points at code. Written after the lock is held and cleared as the last
     * recursion is released, so a waiter reads a consistent pair or a stale
     * one -- never a torn address. Nothing depends on them being current;
     * they exist to be printed. */
    volatile uint32_t      owner_site;
    volatile unsigned long owner_tid;
    volatile long          depth;
} XBOX_CS_SLOT;

static XBOX_CS_SLOT*  g_cs_chunks[XBOX_CS_MAX_CHUNKS];
static size_t         g_cs_slot_count;
static XBOX_CS_SLOT** g_cs_index;          /* open addressing, power of two */
static size_t         g_cs_index_mask;
static SRWLOCK        g_cs_table_lock = SRWLOCK_INIT;
static int            g_cs_table_full;
#if defined(_MSC_VER)
static RECOMP_TLS struct {uintptr_t key;CRITICAL_SECTION *cs;} g_cs_cache[32];
#else
static __thread struct {uintptr_t key;CRITICAL_SECTION *cs;} g_cs_cache[32];
#endif

/* RECOMP_CS_MODE=single puts every guest critical section behind one recursive
 * lock.
 *
 * Deadlock-free by construction -- there is only one lock, so there is no order
 * to invert -- and deliberately NOT the default. A title that deadlocks under
 * real locks is telling you its threads are reaching a place the console's
 * would not, and collapsing the hierarchy hides that rather than answering it.
 * What it is good for is deciding whether a lock cycle is the last thing in the
 * way: if the title still does not get further with this on, the cycle was not
 * what was stopping it.
 *
 * Safe only because guest code here never holds a lock while waiting on another
 * guest thread to make progress -- the runtime's own I/O completes
 * synchronously. That would not hold in general.
 */
static CRITICAL_SECTION g_cs_single;
static INIT_ONCE g_cs_single_once = INIT_ONCE_STATIC_INIT;
static int g_cs_single_mode = -1;

static BOOL CALLBACK xbox_cs_single_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_cs_single);
    return TRUE;
}

/* The slot a shadow lock lives in, or NULL if it is not one of ours --
 * RECOMP_CS_MODE=single hands out a lock that belongs to no slot. */
static XBOX_CS_SLOT* xbox_cs_slot_of(CRITICAL_SECTION* cs)
{
    if (!cs || cs == &g_cs_single)
        return NULL;
    return (XBOX_CS_SLOT*)((char*)cs - offsetof(XBOX_CS_SLOT, cs));
}

static size_t xbox_cs_hash(uintptr_t key)
{
    return (size_t)(((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 32);
}

/* Caller holds g_cs_table_lock (shared is enough). */
static XBOX_CS_SLOT* xbox_cs_find(uintptr_t key)
{
    if (!g_cs_index)
        return NULL;
    for (size_t i = xbox_cs_hash(key) & g_cs_index_mask;; i = (i + 1) & g_cs_index_mask) {
        XBOX_CS_SLOT* slot = g_cs_index[i];
        if (!slot) return NULL;
        if (slot->key == key) return slot;
    }
}

static void xbox_cs_place(XBOX_CS_SLOT** index, size_t mask, XBOX_CS_SLOT* slot)
{
    size_t i = xbox_cs_hash(slot->key) & mask;
    while (index[i]) i = (i + 1) & mask;
    index[i] = slot;
}

/* Caller holds g_cs_table_lock exclusive. Keeps the index at most half full;
 * on allocation failure the current index stays valid while it has room. */
static int xbox_cs_reserve(void)
{
    size_t capacity = g_cs_index ? g_cs_index_mask + 1 : 0;
    if ((g_cs_slot_count + 1) * 2 <= capacity)
        return 1;
    size_t grown = capacity ? capacity * 2 : 8192;
    XBOX_CS_SLOT** index = (XBOX_CS_SLOT**)calloc(grown, sizeof(*index));
    if (!index)
        return g_cs_slot_count < capacity;
    for (size_t i = 0; i < capacity; i++)
        if (g_cs_index[i]) xbox_cs_place(index, grown - 1, g_cs_index[i]);
    free(g_cs_index);
    g_cs_index = index;
    g_cs_index_mask = grown - 1;
    if (grown > 8192)
        fprintf(stderr, "  [CS] guest lock index grown to %zu (%zu locks)\n",
                grown, g_cs_slot_count);
    return 1;
}

/* Caller holds g_cs_table_lock exclusive. */
/* T1-1 O5: spin before blocking on a contended guest lock. Guest critical sections are held
 * for short stretches (the DirectSound API lock 0x3C4598, CRT heap/stdio), so a waiter that sleeps
 * pays a kernel wait and a wake-up (often a full scheduler quantum on a busy 4-thread machine)
 * where a short spin would have found the lock free. Only the contended path changes: the
 * TryEnter fast path and lock semantics are untouched.
 * Switch: RECOMP_CS_SPIN / [Cpu] CsSpin: 0 = no spin (the old path, default until the flip),
 * -1 = auto (1500 at >= 8 logical CPUs, 200 at 4-7, 0 below 4), > 0 = that spin count. */
static DWORD xbox_cs_spin_count(void)
{
    static volatile LONG spin = -1;
    if (spin < 0) {
        int sw = pc_switch("RECOMP_CS_SPIN", "Cpu", "CsSpin", 0);
        unsigned n = pc_cpu_logical();
        LONG c;
        if (sw < 0) c = n >= 8 ? 1500 : n >= 4 ? 200 : 0;
        else c = sw > 100000 ? 100000 : sw;
        if (InterlockedCompareExchange(&spin, c, -1) == -1)
            fprintf(stderr, "[CS-SPIN] spin=%ld (switch %d, %u logical)\n", (long)c, sw, n);
    }
    return (DWORD)spin;
}

static XBOX_CS_SLOT* xbox_cs_create(uintptr_t key)
{
    size_t chunk = g_cs_slot_count / XBOX_CS_CHUNK;
    if (chunk >= XBOX_CS_MAX_CHUNKS || !xbox_cs_reserve())
        return NULL;
    if (!g_cs_chunks[chunk]) {
        g_cs_chunks[chunk] = (XBOX_CS_SLOT*)calloc(XBOX_CS_CHUNK, sizeof(XBOX_CS_SLOT));
        if (!g_cs_chunks[chunk])
            return NULL;
    }
    XBOX_CS_SLOT* slot = &g_cs_chunks[chunk][g_cs_slot_count % XBOX_CS_CHUNK];
    if (!InitializeCriticalSectionEx(&slot->cs, xbox_cs_spin_count(), CRITICAL_SECTION_NO_DEBUG_INFO))
        InitializeCriticalSectionAndSpinCount(&slot->cs, xbox_cs_spin_count());
    slot->key = key;
    g_cs_slot_count++;
    xbox_cs_place(g_cs_index, g_cs_index_mask, slot);
    return slot;
}

static CRITICAL_SECTION* xbox_cs_shadow(PRTL_CRITICAL_SECTION guest)
{
    if (g_cs_single_mode < 0) {
        const char* mode = getenv("RECOMP_CS_MODE");
        g_cs_single_mode = (mode && !strcmp(mode, "single")) ? 1 : 0;
        if (g_cs_single_mode)
            fprintf(stderr, "  [CS] RECOMP_CS_MODE=single: every guest lock "
                            "shares one recursive lock\n");
    }
    if (g_cs_single_mode) {
        if (!guest)
            return NULL;
        InitOnceExecuteOnce(&g_cs_single_once, xbox_cs_single_init, NULL, NULL);
        return &g_cs_single;
    }
    uintptr_t key = (uintptr_t)guest;
    XBOX_CS_SLOT* slot;

    if (!key)
        return NULL;
    size_t cached = xbox_cs_hash(key) & 31;
    if (g_cs_cache[cached].key == key) return g_cs_cache[cached].cs;

    AcquireSRWLockShared(&g_cs_table_lock);
    slot = xbox_cs_find(key);
    ReleaseSRWLockShared(&g_cs_table_lock);
    if (!slot) {
        AcquireSRWLockExclusive(&g_cs_table_lock);
        slot = xbox_cs_find(key);
        if (!slot)
            slot = xbox_cs_create(key);
        ReleaseSRWLockExclusive(&g_cs_table_lock);
    }
    if (!slot) {
        if (!g_cs_table_full) {
            g_cs_table_full = 1;
            xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL,
                     "critical-section shadow allocation failed at %zu locks; "
                     "further locks are unprotected", g_cs_slot_count);
        }
        return NULL;
    }
    g_cs_cache[cached].key = key; g_cs_cache[cached].cs = &slot->cs;
    return &slot->cs;
}

/* Contention is worth saying out loud.
 *
 * Now that these locks are real, a title that leaks one -- or a thread of ours
 * that holds one somewhere the console's would not -- deadlocks instead of
 * sailing through. That looks like a hang with no output at all, so the first
 * time a lock is actually contended, say which one and who is waiting.
 * TryEnter first, so the common uncontended path costs nothing extra.
 */
/* The guest VA is what cross-references against a disassembly; the native
 * pointer does not. */
extern ptrdiff_t g_xbox_mem_offset;

extern RECOMP_TLS uint32_t g_esp;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

/* A guest backtrace, printed where the guest is standing.
 *
 * This runs on the guest thread -- g_esp is thread-local and therefore real
 * here, which it is not in the watchdog or any sampling thread. Lifted calls
 * push their guest return address before jumping, so scanning the stack for
 * words that land in the code range recovers the chain. Approximate by
 * construction: a stale value from an earlier call can look like a frame. It
 * is still the difference between knowing two threads are deadlocked and
 * knowing which two paths did it. */
static void xbox_guest_backtrace(int depth)
{
    uint32_t sp = g_esp;
    int shown = 0;
    int i;

    if (!sp || !g_xbox_code_hi)
        return;
    for (i = 0; i < 256 && shown < depth; i++) {
        uint32_t word = *(const uint32_t *)((uintptr_t)(sp + i * 4)
                                            + g_xbox_mem_offset);
        if (word >= g_xbox_code_lo && word < g_xbox_code_hi) {
            fprintf(stderr, "  [CS]     guest 0x%08X\n", word);
            shown++;
        }
    }
}

static LONG g_cs_contention_reports;
static LONG g_cs_enters, g_cs_leaves;

/* The enter/leave totals only feed the "[CS] enters= leaves=" line of the
 * contention report. Two interlocked increments per lock pair on two shared
 * cache lines, from every game/APU/GPU thread, is real cost (~100M pairs in a
 * 25 minute run), so count only with RECOMP_CS_COUNT=1 (read once). */
static int cs_counting(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_CS_COUNT") != NULL;
    return on;
}

/* Which CRT lock is this?
 *
 * MSVC keeps its internal locks in a table of {CRITICAL_SECTION*, refcount}
 * pairs, so a contended address can be named by its index rather than left as
 * a bare pointer. The index is what the CRT's own headers document: 1 is the
 * stdio scan lock, 4 the heap, and 16 upwards are the per-stream locks. The
 * table's address is per-title, so it is supplied rather than assumed.
 */
static uint32_t g_crt_lock_table, g_crt_lock_count;

void xbox_SetCrtLockTable(uint32_t table_va, uint32_t count)
{
    g_crt_lock_table = table_va;
    g_crt_lock_count = count;
}

static int xbox_crt_lock_index(uint32_t guest_va)
{
    uint32_t i;

    if (!g_crt_lock_table)
        return -1;
    for (i = 0; i < g_crt_lock_count; i++) {
        uint32_t slot = *(const uint32_t *)((uintptr_t)(g_crt_lock_table + i * 8)
                                            + g_xbox_mem_offset);
        if (slot == guest_va)
            return (int)i;
    }
    return -1;
}

/* Every acquisition and release of a lock the CRT names, in order.
 *
 * A deadlock report says which two locks are crossed but not how they got that
 * way, and the two candidate explanations need opposite fixes: the title really
 * does take them in two orders (its problem, and it shipped, so unlikely), or
 * this runtime dropped a release somewhere and a lock that should be free is
 * still held (our problem). The order in this log tells them apart.
 *
 * Gated on RECOMP_CS_TRACE_CRT because it is one table scan per lock operation
 * and the CRT locks are hot. */
static void crt_lock_trace(const char *what, PRTL_CRITICAL_SECTION guest)
{
    static int enabled = -1;
    int idx;

    static int all;
    uint32_t va;

    if (enabled < 0) {
        const char *v = getenv("RECOMP_CS_TRACE_CRT");
        enabled = v != NULL;
        all = v && !strcmp(v, "all");
    }
    if (!enabled)
        return;
    va = (uint32_t)((uintptr_t)guest - (uintptr_t)g_xbox_mem_offset);
    idx = xbox_crt_lock_index(va);
    /* "all" logs by address as well as index. Whether a lock's release is
     * missing or merely unrecognised by the index lookup are different bugs,
     * and only the raw address separates them. */
    /* One lock, both sides, with the guest stack that got there.
     *
     * A take at one address and a release 0x78 lower is not a lock protocol
     * this runtime can reason about from addresses alone -- it needs the two
     * call sites, because the question is whether the guest computed different
     * addresses or we did. */
    {
        static int watch = -1;
        static uint32_t watch_va;
        if (watch < 0) {
            const char *w = getenv("RECOMP_CS_WATCH");
            watch = w != NULL;
            watch_va = w ? (uint32_t)strtoul(w, NULL, 0) : 0;
        }
        if (watch && va == watch_va) {
            fprintf(stderr, "  [CSWATCH] t%-6lu %s 0x%08X\n",
                    (unsigned long)GetCurrentThreadId(), what, va);
            xbox_guest_backtrace(8);
            fflush(stderr);
        }
    }

    if (all) {
        fprintf(stderr, "  [CSTRACE] t%-6lu %-4s 0x%08X idx %d\n",
                (unsigned long)GetCurrentThreadId(), what, va, idx);
        fflush(stderr);
        return;
    }
    if (idx < 0)
        return;
    fprintf(stderr, "  [CSTRACE] t%-6lu %-4s lock %d\n",
            (unsigned long)GetCurrentThreadId(), what, idx);
    fflush(stderr);
}

extern RECOMP_TLS uint32_t g_xbox_kernel_caller;

static void xbox_cs_note_owner(CRITICAL_SECTION* cs)
{
    XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
    if (!slot)
        return;
    slot->owner_site = g_xbox_kernel_caller;
    slot->owner_tid  = GetCurrentThreadId();
    slot->depth++;
}

static void xbox_cs_clear_owner(CRITICAL_SECTION* cs)
{
    XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
    if (!slot)
        return;
    if (--slot->depth <= 0) {
        slot->depth = 0;
        slot->owner_tid = 0;
    }
}

/* T1-0 TH-4 ([PERF-RATE], main.c, RECOMP_PERF_CPU=1): contended enters and the
 * time spent blocked in them, in total and for the DirectSound API lock 0x3C4598,
 * plus the share the game thread waited. Counted on the contended path only, so
 * the uncontended TryEnter path is unchanged. [0..2] all locks: contended, wait
 * QPC ticks, game-thread wait ticks; [3..5] the same for 0x3C4598. */
static volatile LONG64 g_cs_rate[6];
static DWORD g_cs_rate_game_tid;
static volatile LONG g_cs_rate_on;
void xbox_rtl_cs_rate_enable(DWORD game_tid)
{
    g_cs_rate_game_tid = game_tid;
    InterlockedExchange(&g_cs_rate_on, 1);
}
void xbox_rtl_cs_rate_counts(long long out[6])
{
    int i;
    for (i = 0; i < 6; i++) out[i] = (long long)g_cs_rate[i];
}

/* Who is blocked on which guest lock right now (2026-11-09, Not It freeze hunt).
 * The contention report above stops after 16 lines, so a deadlock an hour into a
 * match printed nothing. A contended enter publishes itself here for the length of
 * the wait (one store before and one after the native wait; the uncontended path is
 * untouched), and xbox_cs_waiters_dump lets a hang sampler say: thread T waits on
 * lock L at guest site S, L is held by thread H which took it at guest site S2,
 * plus the waiter's guest backtrace -- the two halves of a lock-order deadlock. */
#define XBOX_CS_WAITERS 32
static struct {
    volatile LONG     tid;    /* 0 = free */
    volatile uint32_t lock_va, site, esp;
    XBOX_CS_SLOT     *slot;
} g_cs_waiters[XBOX_CS_WAITERS];

static int xbox_cs_waiter_add(PRTL_CRITICAL_SECTION guest, CRITICAL_SECTION *cs)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;
    for (i = 0; i < XBOX_CS_WAITERS; i++) {
        if (InterlockedCompareExchange(&g_cs_waiters[i].tid, me, 0) == 0) {
            g_cs_waiters[i].lock_va = (uint32_t)((uintptr_t)guest - (uintptr_t)g_xbox_mem_offset);
            g_cs_waiters[i].site = g_xbox_kernel_caller;
            g_cs_waiters[i].esp = g_esp;
            g_cs_waiters[i].slot = xbox_cs_slot_of(cs);
            return i;
        }
    }
    return -1;
}

int xbox_cs_waiters_dump(char *out, int cap)
{
    int o = 0, i;
    for (i = 0; i < XBOX_CS_WAITERS && o < cap - 400; i++) {
        LONG tid = g_cs_waiters[i].tid;
        uint32_t sp, k, shown;
        XBOX_CS_SLOT *slot;
        if (!tid) continue;
        slot = g_cs_waiters[i].slot;
        o += snprintf(out + o, (size_t)(cap - o),
                      "[CS-WAIT] tid=%ld waits on 0x%08X at guest %08X (esp %08X); held by tid=%lu taken at guest %08X depth %ld; guest ret:",
                      (long)tid, g_cs_waiters[i].lock_va, g_cs_waiters[i].site, g_cs_waiters[i].esp,
                      slot ? slot->owner_tid : 0ul, slot ? slot->owner_site : 0u, slot ? slot->depth : 0l);
        sp = g_cs_waiters[i].esp;
        for (k = 0, shown = 0; sp && g_xbox_code_hi && k < 512 && shown < 20 && o < cap - 32; k++) {
            uint32_t a = sp + k * 4, w;
            if (a < 0x10000u || a >= 0x4000000u - 4) break;
            w = *(const uint32_t *)((uintptr_t)a + g_xbox_mem_offset);
            if (w >= g_xbox_code_lo && w < g_xbox_code_hi) {
                o += snprintf(out + o, (size_t)(cap - o), " %06X", w);
                shown++;
            }
        }
        o += snprintf(out + o, (size_t)(cap - o), "\n");
    }
    return o;
}

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    int waiter;
    LARGE_INTEGER rate_t0;
    rate_t0.QuadPart = 0;
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    if (!cs)
        return;
    if (cs_counting())
        InterlockedIncrement(&g_cs_enters);
    crt_lock_trace("take", CriticalSection);
    if (TryEnterCriticalSection(cs)) {
        xbox_cs_note_owner(cs);
        return;
    }
    if (InterlockedIncrement(&g_cs_contention_reports) <= 16) {
        fprintf(stderr, "  [CS] thread %lu waiting on guest lock 0x%08X"
                        " (held by thread %lu)\n",
                (unsigned long)GetCurrentThreadId(),
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset),
                (unsigned long)(uintptr_t)cs->OwningThread);
        {
            int idx = xbox_crt_lock_index(
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset));
            if (idx >= 0)
                fprintf(stderr, "  [CS]   that is CRT lock %d\n", idx);
        }
        {
            /* Where the holder took it. The waiter's own backtrace below
             * says who is blocked; this is the half that says who to look
             * at. */
            XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
            if (slot && slot->owner_tid)
                fprintf(stderr, "  [CS]   holder thread %lu took it at guest"
                                " 0x%08X (depth %ld)\n",
                        (unsigned long)slot->owner_tid, slot->owner_site, (long)slot->depth);
        }
        if (cs_counting())
            fprintf(stderr, "  [CS] enters=%ld leaves=%ld (outstanding %ld)\n",
                    (long)g_cs_enters, (long)g_cs_leaves, (long)(g_cs_enters - g_cs_leaves));
        xbox_guest_backtrace(10);
        fflush(stderr);
    }
    if (g_cs_rate_on) QueryPerformanceCounter(&rate_t0);
    waiter = xbox_cs_waiter_add(CriticalSection, cs);
    EnterCriticalSection(cs);
    if (waiter >= 0) InterlockedExchange(&g_cs_waiters[waiter].tid, 0);
    if (rate_t0.QuadPart) {
        LARGE_INTEGER rate_t1;
        LONG64 d;
        int a = (uint32_t)((uintptr_t)CriticalSection - (uintptr_t)g_xbox_mem_offset) == 0x003C4598u ? 3 : 0;
        QueryPerformanceCounter(&rate_t1);
        d = rate_t1.QuadPart - rate_t0.QuadPart;
        InterlockedIncrement64(&g_cs_rate[0]);
        InterlockedAdd64(&g_cs_rate[1], d);
        if (GetCurrentThreadId() == g_cs_rate_game_tid) InterlockedAdd64(&g_cs_rate[2], d);
        if (a) {
            InterlockedIncrement64(&g_cs_rate[3]);
            InterlockedAdd64(&g_cs_rate[4], d);
            if (GetCurrentThreadId() == g_cs_rate_game_tid) InterlockedAdd64(&g_cs_rate[5], d);
        }
    }
    xbox_cs_note_owner(cs);
    if (g_cs_contention_reports <= 16) {
        fprintf(stderr, "  [CS] thread %lu acquired 0x%08X\n",
                (unsigned long)GetCurrentThreadId(),
                (uint32_t)((uintptr_t)CriticalSection
                           - (uintptr_t)g_xbox_mem_offset));
        fflush(stderr);
    }
}

/* Host-side try: the same shadow lock Enter/Leave use (the guest's own
 * RtlTryEnterCriticalSection bridge keeps a separate table). Leave with
 * xbox_RtlLeaveCriticalSection. For runtime code that must not block on a
 * guest lock -- an interrupt that skips work while the lock is held. */
BOOLEAN __stdcall xbox_RtlTryEnterCriticalSectionShadow(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    if (!cs || !TryEnterCriticalSection(cs))
        return FALSE;
    if (cs_counting())
        InterlockedIncrement(&g_cs_enters);
    xbox_cs_note_owner(cs);
    return TRUE;
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    if (cs) {
        if (cs_counting())
            InterlockedIncrement(&g_cs_leaves);
        crt_lock_trace("drop", CriticalSection);
        xbox_cs_clear_owner(cs);
        LeaveCriticalSection(cs);
    }
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    /* Creating the shadow here is not required -- Enter does it -- but doing
     * it now keeps the first Enter off the exclusive path. */
    CRITICAL_SECTION* cs = xbox_cs_shadow(CriticalSection);
    /* B4.5 (T1-1): the shadow keeps its state across a guest re-init, so re-initialising a lock
     * another thread holds would have reset it on the console. Say so (16 lines at most); a hit
     * is a lead, the behaviour is unchanged. */
    if (cs) {
        XBOX_CS_SLOT* slot = xbox_cs_slot_of(cs);
        static LONG reinit_reports;
        if (slot && slot->owner_tid && InterlockedIncrement(&reinit_reports) <= 16)
            fprintf(stderr, "[CS] re-init of held lock 0x%08X (owner %lu)\n",
                    (uint32_t)((uintptr_t)CriticalSection - (uintptr_t)g_xbox_mem_offset),
                    (unsigned long)slot->owner_tid);
    }
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_SHARING_VIOLATION:          return ERROR_SHARING_VIOLATION;
        case STATUS_DISK_FULL:                  return ERROR_DISK_FULL;
        case STATUS_NOT_A_DIRECTORY:            return ERROR_DIRECTORY;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        case STATUS_CONFLICTING_ADDRESSES:      return ERROR_INVALID_ADDRESS;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
