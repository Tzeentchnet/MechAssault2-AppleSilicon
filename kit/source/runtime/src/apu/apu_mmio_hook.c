/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#define _InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif

/* A0 counters (apu_core.c): g_apu_ws is set once from RECOMP_AUDIO_WAIT_STATS / RECOMP_PERF. */
extern int g_apu_ws;
static volatile long g_apu_fast_r, g_apu_fast_w;    /* untrapped guest APU register accesses (ma2_load32/ma2_store32) */

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;
extern ptrdiff_t g_xbox_mem_offset;
uint32_t apu_guest_read32(uint32_t address)
{
    if(g_apu_ws)_InterlockedIncrement(&g_apu_fast_r);
#if defined(_WIN32)
    if(g_apu_state)return (uint32_t)mcpx_apu_mmio_read(g_apu_state,address-0xFE800000u,4);
#else
    MCPXAPUState *state = __atomic_load_n(&g_apu_state, __ATOMIC_ACQUIRE);
    if(state)return (uint32_t)mcpx_apu_mmio_read(state,address-0xFE800000u,4);
#endif
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset+address);
}
void apu_guest_write32(uint32_t address,uint32_t value)
{
    if(g_apu_ws)_InterlockedIncrement(&g_apu_fast_w);
#if defined(_WIN32)
    if(g_apu_state)mcpx_apu_mmio_write(g_apu_state,address-0xFE800000u,value,4);
    else *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset+address)=value;
#else
    MCPXAPUState *state = __atomic_load_n(&g_apu_state, __ATOMIC_ACQUIRE);
    if(state)mcpx_apu_mmio_write(state,address-0xFE800000u,value,4);
    else *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset+address)=value;
#endif
}

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include <windows.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;
static volatile long g_veh_region[3];   /* [APU-VEH]: trapped accesses by region: main regs, VP, DSP/other */

/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    if (!g_apu_state) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);
    if (g_apu_ws) _InterlockedIncrement(&g_veh_region[mmio_offset < 0x20000 ? 0 : mmio_offset < 0x30000 ? 1 : 2]);

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    static int trace = -1;      /* getenv once: this runs on every trapped access */
    if (trace < 0) trace = getenv("RECOMP_APU_TRACE") != NULL;
    if (trace) {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. */
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

/* [APU-VEH] every 5 s (called by the APU thread's apu_stats_tick): trapped accesses (VEH, an exception and a
 * decode each) and untrapped ones (the lifted DirectSound code calls apu_guest_*), as deltas. */
void apu_veh_report(double secs)
{
    static int pr, pw, pf; static long p0, p1, p2, pfr, pfw;
    int r = g_apu_mmio_read_count, w = g_apu_mmio_write_count, f = g_apu_mmio_decode_fail;
    long a = g_veh_region[0], b = g_veh_region[1], c = g_veh_region[2], fr = g_apu_fast_r, fw = g_apu_fast_w;
    fprintf(stderr, "[APU-VEH] %.1f s: trapped reads=%d writes=%d decode_fail=%d (main=%ld vp=%ld dsp=%ld); untrapped reads=%ld writes=%ld\n",
            secs, r - pr, w - pw, f - pf, a - p0, b - p1, c - p2, fr - pfr, fw - pfw);
    pr = r; pw = w; pf = f; p0 = a; p1 = b; p2 = c; pfr = fr; pfw = fw;
}

#elif defined(__aarch64__)
#include "../platform/posix_mmio.h"
#include "../kernel/xbox_memory_layout.h"
#include "../usb/ohci.h"

static unsigned long g_apu_posix_reads, g_apu_posix_writes;

#if defined(XBOXRECOMP_TEST_FAILURES)
__attribute__((weak, noinline))
void xbox_TestObservePosixDevice(int device) { (void)device; }
#define OBSERVE_DEVICE(device) xbox_TestObservePosixDevice(device)
#else
#define OBSERVE_DEVICE(device) ((void)0)
#endif

static int apu_is_device(uint32_t off)
{
    return off < 0x30000u ||
        (off >= 0x3F000u && off < 0x40000u) ||
        (off >= 0x5F000u && off < 0x60000u);
}

static uint64_t apu_posix_read(void *dev, uint32_t off, int size)
{
    uint64_t result = 0;
    for (int i = 0; i < size;) {
        uint32_t at = off + (uint32_t)i;
        int n = size - i, boundary = 4 - (int)(at & 3u);
        if (n > boundary) n = boundary;
        uint64_t part = 0;
        if (apu_is_device(at)) {
            uint32_t word = (uint32_t)mcpx_apu_mmio_read(dev, at & ~3u, 4);
            part = (word >> ((at & 3u) * 8u)) &
                ((UINT64_C(1) << (n * 8)) - 1);
        } else {
            volatile uint8_t *p = xbox_McpxMemoryAlias(at, (size_t)n);
            if (p) for (int j = 0; j < n; ++j) part |= (uint64_t)p[j] << (8 * j);
        }
        result |= part << (8 * i);
        i += n;
    }
    __atomic_add_fetch(&g_apu_posix_reads, 1, __ATOMIC_RELAXED);
    return result;
}

static void apu_posix_write(void *dev, uint32_t off, uint64_t value, int size)
{
    for (int i = 0; i < size;) {
        uint32_t at = off + (uint32_t)i;
        int n = size - i, boundary = 4 - (int)(at & 3u);
        if (n > boundary) n = boundary;
        uint64_t part = value >> (8 * i);
        if (apu_is_device(at)) {
            uint32_t aligned = at & ~3u;
            uint32_t word = (uint32_t)part;
            if (n != 4) {
                unsigned shift = (at & 3u) * 8u;
                uint32_t mask = (uint32_t)((UINT64_C(1) << (n * 8)) - 1) << shift;
                uint32_t old = (uint32_t)mcpx_apu_mmio_read(dev, aligned, 4);
                word = (old & ~mask) | ((word << shift) & mask);
            }
            mcpx_apu_mmio_write(dev, aligned, word, 4);
        } else {
            volatile uint8_t *p = xbox_McpxMemoryAlias(at, (size_t)n);
            if (p) for (int j = 0; j < n; ++j) p[j] = (uint8_t)(part >> (8 * j));
        }
        i += n;
    }
    __atomic_add_fetch(&g_apu_posix_writes, 1, __ATOMIC_RELAXED);
}

enum PosixMmioOwner { OWNER_ALIAS, OWNER_APU, OWNER_AC97, OWNER_OHCI };

static enum PosixMmioOwner apu_posix_owner(uint32_t va)
{
    if (xbox_ApuMmioOwnsAddress(va)) return OWNER_APU;
    if (xbox_Ac97OwnsAddress(va)) return OWNER_AC97;
    if (xbox_OhciOwnsAddress(va)) return OWNER_OHCI;
    return OWNER_ALIAS;
}

/* Runs exclusively on the broker thread. Validate the whole instruction
 * before any device or alias operation, so a rejection has no side effects. */
static int apu_posix_backend(void *dev, XboxPosixMmioRequest *request)
{
    uint32_t va = 0xFE800000u + request->fault_offset;
    enum PosixMmioOwner owner = apu_posix_owner(va);
    if (owner == OWNER_ALIAS) return 0;
    for (unsigned i = 0; i < request->count; ++i) {
        XboxPosixMmioOp *op = &request->op[i];
        if (!xbox_McpxMemoryAlias(op->offset, op->size)) return 0;
        for (unsigned j = 0; j < op->size; ++j) {
            uint32_t at = 0xFE800000u + op->offset + j;
            enum PosixMmioOwner current = apu_posix_owner(at);
            if (current != OWNER_ALIAS &&
                (current != owner ||
                 (current == OWNER_OHCI && !xbox_OhciPosixSameOwner(va, at))))
                return 0;
        }
    }

    if (owner == OWNER_APU) {
        if (!dev) return 0;
        OBSERVE_DEVICE(1);
        for (unsigned i = 0; i < request->count; ++i) {
            XboxPosixMmioOp *op = &request->op[i];
            if (op->write) apu_posix_write(dev, op->offset, op->value, (int)op->size);
            else op->value = apu_posix_read(dev, op->offset, (int)op->size);
        }
        return 1;
    }
    if (owner == OWNER_AC97) {
        OBSERVE_DEVICE(2);
        for (unsigned i = 0; i < request->count; ++i) {
            XboxPosixMmioOp *op = &request->op[i];
            xbox_Ac97PosixAccess(op->offset, &op->value, op->size, op->write);
        }
        return 1;
    }
    if (owner == OWNER_OHCI) {
        if (!xbox_OhciIsEnabled()) return 0;
        OBSERVE_DEVICE(3);
        return xbox_OhciPosixRequest(va, request);
    }
    return 0;
}

static void apu_posix_publish(void *dev)
{
    __atomic_store_n(&g_apu_state, dev, __ATOMIC_RELEASE);
}

static void apu_posix_unpublish(void *dev)
{
    (void)dev;
    __atomic_store_n(&g_apu_state, NULL, __ATOMIC_RELEASE);
}

int xbox_PosixMmioStart(void)
{
    return xbox_PosixMmioBrokerStart(apu_posix_backend, apu_posix_publish,
                                     apu_posix_unpublish, UINT64_C(20000000000));
}

int xbox_PosixMmioAttach(MCPXAPUState *apu)
{
    if (!xbox_GetMemoryBase() || !xbox_McpxMemoryAlias(0, 0x800000u))
        return 0;
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset() + UINT32_C(0xFE800000);
    return xbox_PosixMmioBrokerBind(base, 0x800000u, apu);
}

void apu_veh_report(double secs)
{
    static unsigned long previous_r, previous_w;
    unsigned long r = __atomic_load_n(&g_apu_posix_reads, __ATOMIC_RELAXED);
    unsigned long w = __atomic_load_n(&g_apu_posix_writes, __ATOMIC_RELAXED);
    fprintf(stderr, "[APU-MMIO] %.1f s: trapped reads=%lu writes=%lu\n",
            secs, r - previous_r, w - previous_w);
    previous_r = r; previous_w = w;
}
#else
int xbox_PosixMmioStart(void) { return 0; }
int xbox_PosixMmioAttach(MCPXAPUState *apu) { (void)apu; return 0; }
void apu_veh_report(double secs) { (void)secs; }
#endif /* _WIN32 / POSIX AArch64 */
