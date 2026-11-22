#define RECOMP_GENERATED_CODE
#include <recomp_types.h>
#include "check.h"

#include <fenv.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint16_t g_fp_control_word = 0x037f;

static int evaluations;
static double counted(double value) { ++evaluations; return value; }

static int memory_access(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *base = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED);
    CHECK((uintptr_t)base > UINT32_MAX);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)base;
    CHECK(XBOX_PTR(UINT64_C(0x100000005)) == (uintptr_t)base + 5);
    for (uint32_t off = 0; off < 8; ++off) {
        uint32_t addr = (uint32_t)page - 4 + off;
        MEM16(addr) = UINT16_C(0xabcd);
        CHECK(base[addr] == 0xcd && base[addr + 1] == 0xab);
        CHECK(MEM16(addr) == UINT16_C(0xabcd));
        MEM32(addr) = UINT32_C(0x89abcdef);
        CHECK(base[addr] == 0xef && base[addr + 1] == 0xcd &&
              base[addr + 2] == 0xab && base[addr + 3] == 0x89);
        CHECK(MEM32(addr) == UINT32_C(0x89abcdef));
        SMEM64(addr) = INT64_C(-0x102030405060708);
        CHECK(SMEM64(addr) == INT64_C(-0x102030405060708));
        MEMF(addr) = 1.5f;
        CHECK(MEMF(addr) == 1.5f);
        MEMD(addr) = -2.25;
        CHECK(MEMD(addr) == -2.25);
        RecompXmm vector = {{0}};
        vector.u[0] = 0x12345678;
        vector.u[1] = 0x89abcdef;
        vector.u[2] = 0x01122334;
        vector.u[3] = 0xfedcba98;
        XMM_STORE(addr, vector);
        RecompXmm loaded = XMM_MEM(addr);
        for (int i = 0; i < 4; ++i) CHECK(loaded.u[i] == vector.u[i]);
        RecompMmx mm = MMX_ZERO();
        mm.q = UINT64_C(0xfedcba9876543210);
        MMX_STORE(addr, mm);
        CHECK(MMX_MEM(addr).q == mm.q);
    }
    CHECK(munmap(base, page * 2) == 0);
    return 0;
}

static int floating_point(void)
{
    const uint16_t nearest = 0x037f, down = 0x077f, up = 0x0b7f, zero = 0x0f7f;
    CHECK(RECOMP_FCMP(-1.0, 0.0) == -1);
    CHECK(RECOMP_FCMP(1.0, 0.0) == 1);
    CHECK(RECOMP_FCMP(-0.0, 0.0) == 0);
    CHECK(RECOMP_FCMP(NAN, 1.0) == 2 && RECOMP_FCMP(1.0, NAN) == 2);
    evaluations = 0;
    CHECK(RECOMP_FCMP(counted(1.0), counted(2.0)) == -1 && evaluations == 2);
    CHECK(RECOMP_FCMP_CC(-1) == 0x0100 && RECOMP_FCMP_CC(0) == 0x4000);
    CHECK(RECOMP_FCMP_CC(1) == 0 && RECOMP_FCMP_CC(2) == 0x4500);
    CHECK(recomp_fxam(-0.0) == 0x4200);
    CHECK(recomp_fxam(-INFINITY) == 0x0700);
    CHECK((recomp_fxam(NAN) & 0x4500) == 0x0100);
    CHECK(fesetround(FE_DOWNWARD) == 0);
    CHECK(recomp_frndint(2.5, nearest) == 2.0);
    CHECK(recomp_frndint(3.5, nearest) == 4.0);
    CHECK(recomp_frndint(-2.5, down) == -3.0);
    CHECK(recomp_frndint(-2.5, up) == -2.0);
    CHECK(recomp_frndint(-2.75, zero) == -2.0);
    CHECK(signbit(recomp_frndint(-0.25, nearest)));
    CHECK(isnan(recomp_frndint(NAN, nearest)));
    CHECK(recomp_fist(1.6, down, 32) == 1);
    CHECK(recomp_fist(1.6, up, 32) == 2);
    CHECK(recomp_fist(NAN, nearest, 16) == INT16_MIN);
    CHECK(recomp_fist(2147483648.0, nearest, 32) == INT32_MIN);
    CHECK(recomp_fist(0x1p63, nearest, 64) == INT64_MIN);
    CHECK(fesetround(FE_TONEAREST) == 0);

    uint8_t memory[14] = {0};
    const double values[] = {0.0, -0.0, 1.0, -2.5, 0x1p-1074,
                             0x1.fffffffffffffp1023, INFINITY, -INFINITY, NAN};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint64_t before, after;
        recomp_fp_store80(memory + 1, values[i]);
        double result = recomp_fp_load80(memory + 1);
        memcpy(&before, &values[i], sizeof(before));
        memcpy(&after, &result, sizeof(after));
        CHECK(isnan(values[i]) ? isnan(result) : before == after);
        CHECK(memory[0] == 0 && memory[11] == 0);
    }
    /* A halfway extended value rounds to the even binary64 significand. */
    const uint8_t half_even[10] = {0x00,0x04,0,0,0,0,0,0x80,0xff,0x3f};
    const uint8_t half_odd[10] = {0x00,0x0c,0,0,0,0,0,0x80,0xff,0x3f};
    CHECK(recomp_fp_load80(half_even) == 1.0);
    CHECK(recomp_fp_load80(half_odd) == 0x1.0000000000002p0);

    CHECK(fesetround(FE_DOWNWARD) == 0);
    g_fp_control_word = 0x087f; /* 24-bit precision, round up */
    CHECK(RECOMP_FP_PC(1.0 + 0x1p-24) == 0x1.000002p0);
    CHECK(RECOMP_FP_PC(0x1.000001p-1050) == 0x1.000002p-1050);
    g_fp_control_word = 0x047f; /* 24-bit precision, round down */
    CHECK(RECOMP_FP_PC(-1.0 - 0x1p-24) == -0x1.000002p0);
    g_fp_control_word = 0x007f;
    CHECK(RECOMP_FP_PC(0x1.000001p-1050) == 0x1p-1050);
    CHECK(RECOMP_FP_PC(0x1p-1074) == 0x1p-1074);
    CHECK(isnan(RECOMP_FP_PC(NAN)) && signbit(RECOMP_FP_PC(-0.0)));
    CHECK(fesetround(FE_TONEAREST) == 0);
    return 0;
}

static int vectors_and_flags(void)
{
    RecompXmm a = XMM_ZERO(), b = XMM_ZERO();
    a.f[0] = 3.0f; a.f[1] = -0.0f; a.f[2] = NAN; a.f[3] = 8.0f;
    b.f[0] = 4.0f; b.f[1] = 0.0f; b.f[2] = 7.0f; b.f[3] = NAN;
    RecompXmm r = XMM_MIN(a, b);
    CHECK(r.f[0] == 3.0f && r.u[1] == 0 && r.f[2] == 7.0f && isnan(r.f[3]));
    r = XMM_MAX(a, b);
    CHECK(r.f[0] == 4.0f && r.u[1] == 0 && r.f[2] == 7.0f && isnan(r.f[3]));
    r = XMM_CMP_PRED(a, b, 3);
    CHECK(r.u[0] == 0 && r.u[1] == 0 && r.u[2] == UINT32_MAX && r.u[3] == UINT32_MAX);
    r = XMM_CMP_PRED(a, b, 4);
    CHECK(r.u[0] == UINT32_MAX && r.u[1] == 0 && r.u[2] == UINT32_MAX);
    a.u[0] = 1; a.u[1] = 2; a.u[2] = 3; a.u[3] = 4;
    b.u[0] = 5; b.u[1] = 6; b.u[2] = 7; b.u[3] = 8;
    r = XMM_SHUFFLE(a, b, 0x1b);
    CHECK(r.u[0] == 4 && r.u[1] == 3 && r.u[2] == 6 && r.u[3] == 5);
    r = XMM_UNPACK_LOW(a, b);
    CHECK(r.u[0] == 1 && r.u[1] == 5 && r.u[2] == 2 && r.u[3] == 6);
    r = XMM_ANDN(a, b);
    CHECK(r.u[0] == (~1u & 5u) && r.u[3] == (~4u & 8u));

    RecompMmx x = MMX_ZERO(), y = MMX_ZERO();
    x.b[0] = 120; x.b[1] = -120; y.b[0] = 40; y.b[1] = -40;
    RecompMmx z = MMX_PADDSB(x, y);
    CHECK(z.b[0] == 127 && z.b[1] == -128);
    x.ub[0] = 250; y.ub[0] = 20;
    CHECK(MMX_PADDUSB(x, y).ub[0] == 255);
    x.q = UINT64_C(0xffff000100020003);
    CHECK(MMX_PSLLW(x, 16).q == 0);
    CHECK(MMX_PSRAD(x, 100).ud[1] == UINT32_MAX);
    x.w[0] = x.w[1] = x.w[2] = x.w[3] = INT16_MIN;
    y = x;
    z = MMX_PMADDWD(x, y);
    CHECK(z.ud[0] == 0x80000000u && z.ud[1] == 0x80000000u);
    x.d[0] = -3; x.d[1] = 12;
    a.u[2] = 0x12345678; a.u[3] = 0x87654321;
    r = XMM_FROM_PI(a, x);
    CHECK(r.f[0] == -3 && r.f[1] == 12 &&
          r.u[2] == 0x12345678 && r.u[3] == 0x87654321);
    CHECK(MMX_CVT_F2I(NAN, 0) == INT32_MIN);
    CHECK(MMX_CVT_F2I(2.9f, 1) == 2 && MMX_CVT_F2I(-2.9f, 1) == -2);
    int carry = 0;
    CHECK(ADD32_CF(UINT32_MAX, 1, &carry) == 0 && carry == 1);
    CHECK(SUB32_CF(0, 1, &carry) == UINT32_MAX && carry == 1);
    CHECK(RC_ROT(0x80, 1, &carry, 8, 1) == 1 && carry == 1);
    CHECK(CMP_L((uint8_t)0xff, (uint8_t)1) && TEST_S((uint16_t)0x8000, (uint16_t)0xffff));
    CHECK(RECOMP_PARITY8(0x03) && !RECOMP_PARITY8(0x07));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "memory") == 0)
        CHECK(memory_access() == 0);
    if (argc == 1 || strcmp(argv[1], "float") == 0)
        CHECK(floating_point() == 0);
    if (argc == 1 || strcmp(argv[1], "vectors") == 0)
        CHECK(vectors_and_flags() == 0);
    return 0;
}
