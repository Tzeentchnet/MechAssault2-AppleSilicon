/*
 * AArch64 POSIX fault decoder, adapted from xboxrecomp v0.18.0
 * src/platform/mmio_decode_a64.h (commit 37b61d589168adee3680fabd51805ee81427bf07).
 * The OS can report the first inaccessible byte rather than the access start
 * when an instruction crosses pages. Recover the effective address from the
 * instruction and saved registers; unsupported instructions must return zero
 * without changing the saved context or calling a device.
 */
#ifndef XBOX_MMIO_DECODE_A64_H
#define XBOX_MMIO_DECODE_A64_H

#include <stdint.h>
#include <stddef.h>

#if defined(__aarch64__) && !defined(_WIN32)
#if defined(__APPLE__)
#include <sys/ucontext.h>
#if defined(__DARWIN_OPAQUE_ARM_THREAD_STATE64) && __DARWIN_OPAQUE_ARM_THREAD_STATE64
#error "AArch64 MMIO decoding does not support opaque arm64e thread state"
#endif
#define A64_CTX_PC(uc)   ((uc)->uc_mcontext->__ss.__pc)
#define A64_CTX_X(uc, n) ((uc)->uc_mcontext->__ss.__x[(n)])
#define A64_CTX_FP(uc)   ((uc)->uc_mcontext->__ss.__fp)
#define A64_CTX_LR(uc)   ((uc)->uc_mcontext->__ss.__lr)
#define A64_CTX_SP(uc)   ((uc)->uc_mcontext->__ss.__sp)
#define A64_CTX_V_OK(uc) 1
#define A64_CTX_V(uc, n) ((uc)->uc_mcontext->__ns.__v[(n)])
#elif defined(__linux__)
#include <ucontext.h>
#include <asm/sigcontext.h>
#define A64_CTX_PC(uc)   ((uc)->uc_mcontext.pc)
#define A64_CTX_X(uc, n) ((uc)->uc_mcontext.regs[(n)])
#define A64_CTX_FP(uc)   ((uc)->uc_mcontext.regs[29])
#define A64_CTX_LR(uc)   ((uc)->uc_mcontext.regs[30])
#define A64_CTX_SP(uc)   ((uc)->uc_mcontext.sp)
static inline __uint128_t *mmio_a64_linux_vregs(ucontext_t *uc)
{
    unsigned char *p = (unsigned char *)uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (!h->magic || h->size < sizeof(*h) || h->size > (size_t)(end - p))
            break;
        if (h->magic == FPSIMD_MAGIC)
            return h->size >= sizeof(struct fpsimd_context)
                ? ((struct fpsimd_context *)p)->vregs : NULL;
        p += h->size;
    }
    return NULL;
}
#define A64_CTX_V_OK(uc) (mmio_a64_linux_vregs(uc) != NULL)
#define A64_CTX_V(uc, n) (mmio_a64_linux_vregs(uc)[(n)])
#else
#error "AArch64 MMIO decoding requires Darwin or Linux ucontext support"
#endif

typedef uint64_t (*mmio_a64_read_fn)(void *dev, uint32_t off, int size);
typedef void (*mmio_a64_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_a64_xreg(ucontext_t *uc, int r)
{
    if (r < 29) return (uint64_t *)&A64_CTX_X(uc, r);
    if (r == 29) return (uint64_t *)&A64_CTX_FP(uc);
    if (r == 30) return (uint64_t *)&A64_CTX_LR(uc);
    return (uint64_t *)&A64_CTX_SP(uc);
}

static inline int mmio_a64_access_offset(uint64_t address, uintptr_t fault,
                                         uint32_t fault_off, unsigned bytes,
                                         uint32_t *off)
{
    if ((uint64_t)fault < address || (uint64_t)fault - address >= bytes)
        return 0;
    uint64_t delta = (uint64_t)fault - address;
    if (delta > fault_off || (uint64_t)fault_off - delta + bytes > UINT64_C(0x100000000))
        return 0;
    *off = fault_off - (uint32_t)delta;
    return 1;
}

static inline int mmio_emulate_a64(ucontext_t *uc, uint32_t off, uintptr_t fault,
                                   void *dev,
                                   mmio_a64_read_fn rd, mmio_a64_write_fn wr)
{
    uint32_t insn = *(const uint32_t *)(uintptr_t)A64_CTX_PC(uc);
    unsigned group = (insn >> 27) & 7;
    int vector = (insn >> 26) & 1;
    if (!rd || !wr || (vector && !A64_CTX_V_OK(uc))) return 0;

    if (group == 7) {
        int size = (insn >> 30) & 3, opc = (insn >> 22) & 3;
        int rt = insn & 31, rn = (insn >> 5) & 31;
        int form = (insn >> 24) & 3, writeback = 0, bit21 = 0, sub = 0;
        int bytes, load, sign64 = 0, sign32 = 0;
        int64_t imm = 0;
        uint64_t value;
        if (form == 0) {
            bit21 = (insn >> 21) & 1;
            sub = (insn >> 10) & 3;
            if (!bit21) {
                if (sub == 2) return 0;
                imm = (int64_t)(int32_t)(((insn >> 12) & 0x1ffu) << 23) >> 23;
                if (sub == 1 || sub == 3) {
                    writeback = 1;
                }
            } else if (sub != 2) return 0;
        } else if (form != 1) return 0;

        if (vector) {
            if (opc & 2) { if (size != 0) return 0; bytes = 16; }
            else bytes = 1 << size;
            load = opc & 1;
        } else {
            bytes = 1 << size;
            switch (opc) {
            case 0: load = 0; break;
            case 1: load = 1; break;
            case 2:
                if (size == 3) return 0;
                load = 1; sign64 = 1; break;
            default:
                if (size >= 2) return 0;
                load = 1; sign32 = 1; break;
            }
        }

        uint64_t address = *mmio_a64_xreg(uc, rn);
        if (form == 1) {
            address += ((insn >> 10) & 0xfffu) * (uint64_t)bytes;
        } else if (!bit21) {
            if (sub != 1) address += (uint64_t)imm;
        } else {
            unsigned rm = (insn >> 16) & 31u, option = (insn >> 13) & 7u;
            uint64_t index = rm == 31 ? 0 : *mmio_a64_xreg(uc, (int)rm);
            if (option == 2) index = (uint32_t)index;
            else if (option == 6) index = (uint64_t)(int64_t)(int32_t)index;
            else if (option != 3 && option != 7) return 0;
            if ((insn >> 12) & 1u) index *= (uint64_t)bytes;
            address += index;
        }
        if (!mmio_a64_access_offset(address, fault, off, (unsigned)bytes, &off))
            return 0;

        if (vector) {
            if (load) {
                if (bytes == 16) {
                    __uint128_t lo = rd(dev, off, 8), hi = rd(dev, off + 8, 8);
                    A64_CTX_V(uc, rt) = lo | (hi << 64);
                } else A64_CTX_V(uc, rt) = rd(dev, off, bytes);
            } else {
                __uint128_t q = A64_CTX_V(uc, rt);
                wr(dev, off, (uint64_t)q, bytes == 16 ? 8 : bytes);
                if (bytes == 16) wr(dev, off + 8, (uint64_t)(q >> 64), 8);
            }
        } else {
            if (load) {
                value = rd(dev, off, bytes);
                if (sign64) {
                    int shift = 64 - bytes * 8;
                    value = (uint64_t)(((int64_t)(value << shift)) >> shift);
                } else if (sign32) {
                    int shift = 32 - bytes * 8;
                    value = (uint32_t)(((int32_t)((uint32_t)value << shift)) >> shift);
                } else if (size != 3) value &= (UINT64_C(1) << (bytes * 8)) - 1;
                if (rt != 31) *mmio_a64_xreg(uc, rt) = value;
            } else {
                value = rt == 31 ? 0 : *mmio_a64_xreg(uc, rt);
                wr(dev, off, value, bytes);
            }
        }
        if (writeback) *mmio_a64_xreg(uc, rn) += (uint64_t)imm;
        A64_CTX_PC(uc) += 4;
        return 1;
    }

    if (group == 5 && ((insn >> 23) & 7) >= 1 && ((insn >> 23) & 7) <= 3) {
        int opc = (insn >> 30) & 3, load = (insn >> 22) & 1;
        int mode = (insn >> 23) & 3, rt = insn & 31, rt2 = (insn >> 10) & 31;
        int rn = (insn >> 5) & 31, bytes, sign = 0;
        int64_t imm = (int64_t)(int32_t)(((insn >> 15) & 0x7fu) << 25) >> 25;
        if (vector) {
            bytes = 4 << opc;
            if (bytes > 8) return 0;
        } else if (opc == 0) bytes = 4;
        else if (opc == 1) { if (!load) return 0; bytes = 4; sign = 1; }
        else if (opc == 2) bytes = 8;
        else return 0;
        uint64_t address = *mmio_a64_xreg(uc, rn);
        if (mode != 1) address += (uint64_t)(imm * bytes);
        if (!mmio_a64_access_offset(address, fault, off, (unsigned)(2 * bytes), &off))
            return 0;
        if (load) {
            uint64_t a = rd(dev, off, bytes), b = rd(dev, off + bytes, bytes);
            if (sign) { a = (uint64_t)(int64_t)(int32_t)a; b = (uint64_t)(int64_t)(int32_t)b; }
            if (vector) { A64_CTX_V(uc, rt) = a; A64_CTX_V(uc, rt2) = b; }
            else {
                if (rt != 31) *mmio_a64_xreg(uc, rt) = a;
                if (rt2 != 31) *mmio_a64_xreg(uc, rt2) = b;
            }
        } else {
            uint64_t a = vector ? (uint64_t)A64_CTX_V(uc, rt) :
                rt == 31 ? 0 : *mmio_a64_xreg(uc, rt);
            uint64_t b = vector ? (uint64_t)A64_CTX_V(uc, rt2) :
                rt2 == 31 ? 0 : *mmio_a64_xreg(uc, rt2);
            wr(dev, off, a, bytes);
            wr(dev, off + bytes, b, bytes);
        }
        if (mode == 1 || mode == 3) *mmio_a64_xreg(uc, rn) += (uint64_t)(imm * bytes);
        A64_CTX_PC(uc) += 4;
        return 1;
    }
    return 0;
}
#endif
#endif
