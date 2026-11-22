/* pc_cpu_topo.h -- CPU topology and thread roles for the native-pipeline branch (pipeline M0, 2026-10-31).
 *
 * What the process may run on, measured once at startup by pc_cpu_topo_init() (pc_cpu_topo.c, which
 * main.c #includes): the logical CPUs under the process affinity mask and default CPU Sets, their
 * cores, the P/E split (EfficiencyClass: the highest class present is "P"), last-level caches, the
 * power source and a tier for the 4-thread floor. Hardware rule (PIPELINE-BRIEF): thread counts scale
 * with these numbers, never with this PC; RECOMP_AFFINITY=<hex> (test only) simulates 4- and
 * 8-thread machines, and everything reads the masked numbers.
 *
 * pc_thread_role_enter(role) registers the calling thread under a role, for the per-thread CPU report
 * (TH-1, T1-0), and runs the placement policy for that role. The policy is a no-op until T1-5
 * ([Cpu] ThreadPlacement): today it changes nothing about any thread.
 *
 * Header-only use from library code: the API is a __declspec(selectany) table filled by main.c. In
 * lib-only tests (no main.c) the table is empty, role calls do nothing and pc_cpu_logical() counts
 * the process affinity mask itself.
 *
 * Ownership (PIPELINE-SPEC section 3): track T1 owns this file and pc_cpu_topo.c. The interface is
 * frozen after M0: add roles, fields (at the end) and functions; never renumber or change these.
 * Include as "platform/pc_cpu_topo.h" from src\*.c, "../platform/pc_cpu_topo.h" from src\kernel and
 * src\apu (those land in the kit's runtime tree). */
#pragma once
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Thread roles. Values are stable within a build; add new roles before PC_ROLE_COUNT. */
typedef enum PcThreadRole {
    PC_ROLE_GAME = 0,   /* the guest game thread (runs inline on the main thread) */
    PC_ROLE_GUEST,      /* other guest threads (PsCreateSystemThreadEx, e.g. the event worker) */
    PC_ROLE_TIMER,      /* kernel timer thread: timers, DPCs and vblank today */
    PC_ROLE_VBLANK,     /* a split-off vblank thread (T1-5) */
    PC_ROLE_DPC,        /* a split-off serial DPC thread (T1-5) */
    PC_ROLE_CLOCK,      /* kernel HAL clock worker */
    PC_ROLE_EXECUTOR,   /* NV2A pushbuffer executor / ack thread */
    PC_ROLE_RASTER,     /* executor helper workers */
    PC_ROLE_PRESENT,    /* frame-buffer present / window thread */
    PC_ROLE_SHADER,     /* shader compile workers */
    PC_ROLE_PRELOAD,    /* shader preload worker */
    PC_ROLE_PACK,       /* texture-pack reader and fingerprint workers */
    PC_ROLE_APU,        /* APU frame thread (VP, mix) */
    PC_ROLE_DSP,        /* GP/EP DSP thread */
    PC_ROLE_VP,         /* VP helper workers */
    PC_ROLE_AUDIO_OUT,  /* host audio output (XAudio2 / waveOut feeder) */
    PC_ROLE_BOT,        /* native bot job worker */
    PC_ROLE_IO,         /* async file I/O */
    PC_ROLE_USB,        /* OHCI / input */
    PC_ROLE_WATCHDOG,   /* watchdog and stall monitors */
    PC_ROLE_DIAG,       /* profilers and other test-only threads */
    PC_ROLE_OTHER,
    PC_ROLE_COUNT
} PcThreadRole;

static __inline const char *pc_thread_role_name(int role)
{
    static const char *const names[PC_ROLE_COUNT] = {
        "game", "guest", "timer", "vblank", "dpc", "clock", "executor", "raster", "present", "shader",
        "preload", "pack", "apu", "dsp", "vp", "audio_out", "bot", "io", "usb", "watchdog", "diag", "other" };
    return role >= 0 && role < PC_ROLE_COUNT ? names[role] : "other";
}

/* Tiers of PIPELINE-B section 4.3 (the policy that uses them is T1-5's). */
typedef enum PcCpuTier {
    PC_CPU_TIER_FLOOR = 0,  /* fewer than 4 logical CPUs: below the 4-thread floor; placement stays off */
    PC_CPU_TIER_SMALL = 1,  /* 4 or more logical, fewer than 6 cores: no CPU Sets; EcoQoS only on hybrid */
    PC_CPU_TIER_FULL  = 2   /* 6 or more cores: P-core CPU Sets allowed for the game and executor */
} PcCpuTier;

enum { PC_CPU_SRC_MASK = 0, PC_CPU_SRC_GLPI = 1, PC_CPU_SRC_CPUSETS = 2 };   /* PcCpuTopo.source */
enum { PC_POWER_UNKNOWN = -1, PC_POWER_AC = 0, PC_POWER_BATTERY = 1 };       /* PcCpuTopo.power */

#define PC_CPU_TOPO_MAX_SETS 256

typedef struct PcCpuSet {
    unsigned long id;          /* CPU Set id (SetThreadSelectedCpuSets) */
    unsigned short group;      /* processor group */
    unsigned char logical;     /* group-relative logical processor index */
    unsigned char core;        /* group-relative core index */
    unsigned char eff_class;   /* EfficiencyClass: higher = faster */
    unsigned char llc;         /* last-level cache index */
    unsigned char numa;        /* NUMA node */
    unsigned char flags;       /* SYSTEM_CPU_SET_INFORMATION AllFlags (1 parked, 2 allocated, 4 ours, 8 real-time) */
} PcCpuSet;

typedef struct PcCpuTopo {
    unsigned logical;          /* logical CPUs this process may run on (affinity mask, default CPU Sets); >= 1 */
    unsigned physical;         /* distinct cores among them */
    unsigned classes;          /* distinct EfficiencyClass values among them (1 = uniform) */
    unsigned p_class;          /* highest EfficiencyClass among them: the performance cores */
    unsigned p_logical;        /* logical CPUs in p_class */
    unsigned p_physical;       /* cores in p_class */
    unsigned e_logical;        /* logical CPUs in every lower class (E-cores, LP E-cores) */
    unsigned e_physical;       /* cores in every lower class */
    unsigned llc;              /* distinct last-level caches among them (0 = unknown) */
    unsigned system_logical;   /* every logical CPU on the system, mask ignored */
    unsigned system_classes;   /* distinct EfficiencyClass values on the system (2+ = hybrid CPU) */
    unsigned system_p_class;   /* highest EfficiencyClass on the system (p_class below it = only E-cores usable) */
    unsigned groups;           /* processor groups on the system */
    unsigned long long mask;   /* process affinity mask of the primary group at detection (0 = several groups) */
    int source;                /* PC_CPU_SRC_* */
    int tier;                  /* PcCpuTier */
    int power;                 /* PC_POWER_* at detection */
    int has_battery;           /* 1 = a system battery, 0 = none, -1 = unknown */
    int saver;                 /* 1 = battery saver on at detection */
    unsigned set_count;        /* entries in set[] (source == PC_CPU_SRC_CPUSETS) */
    PcCpuSet set[PC_CPU_TOPO_MAX_SETS];  /* the usable CPU Sets, in system order */
} PcCpuTopo;

/* Filled by pc_cpu_topo_init() (main.c). Library code reads it through the functions below. */
typedef struct PcCpuTopoApi {
    const PcCpuTopo *topo;
    void (*role_enter)(int role);
    void (*role_leave)(void);
    void (*cpu_note)(void);    /* T1-0 TH-1: set only while RECOMP_PERF_CPU=1 reports run */
} PcCpuTopoApi;
#if defined(_MSC_VER)
__declspec(selectany) PcCpuTopoApi pc_cpu_topo_api = { 0, 0, 0, 0 };
#else
__attribute__((weak)) PcCpuTopoApi pc_cpu_topo_api = { 0, 0, 0, 0 };
#endif

/* The startup topology, or NULL (lib-only tests, or before main.c's init). */
static __inline const PcCpuTopo *pc_cpu_topo(void) { return pc_cpu_topo_api.topo; }

/* Logical CPUs this process may use; >= 1. Without the table: the process affinity mask's count. */
static __inline unsigned pc_cpu_logical(void)
{
    const PcCpuTopo *t = pc_cpu_topo_api.topo;
#if defined(_WIN32)
    DWORD_PTR process = 0, system = 0;
    unsigned n = 0;
    if (t) return t->logical;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
        for (; process; process &= process - 1) n++;
    else
        n = (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? n : 1;
#else
    long n;
    if (t) return t->logical;
    n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#endif
}

static __inline int pc_cpu_tier(void)
{
    const PcCpuTopo *t = pc_cpu_topo_api.topo;
    if (t) return t->tier;
    return pc_cpu_logical() < 4 ? PC_CPU_TIER_FLOOR : PC_CPU_TIER_SMALL;
}

/* Live power source (the topology holds the startup snapshot): PC_POWER_*. */
static __inline int pc_power_source(void)
{
#if defined(_WIN32)
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return PC_POWER_UNKNOWN;
    return ps.ACLineStatus == 0 ? PC_POWER_BATTERY : ps.ACLineStatus == 1 ? PC_POWER_AC : PC_POWER_UNKNOWN;
#else
    return PC_POWER_UNKNOWN;
#endif
}

/* Register the calling thread under a role (PcThreadRole). Call once near the top of the thread;
 * calling again changes its role. pc_thread_role_leave() before a short-lived thread exits. */
static __inline void pc_thread_role_enter(int role)
{
    if (pc_cpu_topo_api.role_enter) pc_cpu_topo_api.role_enter(role);
}

static __inline void pc_thread_role_leave(void)
{
    if (pc_cpu_topo_api.role_leave) pc_cpu_topo_api.role_leave();
}

/* T1-0 TH-1: a registered thread may call this once per loop or frame. With RECOMP_PERF_CPU=1 it
 * stamps the processor it runs on for main.c's [PERF-CPU] placement line; otherwise it does nothing. */
static __inline void pc_thread_cpu_note(void)
{
    if (pc_cpu_topo_api.cpu_note) pc_cpu_topo_api.cpu_note();
}

#ifdef __cplusplus
}
#endif
