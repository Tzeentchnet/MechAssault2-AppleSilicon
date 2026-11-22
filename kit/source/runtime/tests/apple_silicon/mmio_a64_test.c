/* Real fault coverage adapted from xboxrecomp v0.18.0 tests/mmio_decode_a64. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && !defined(_WIN32)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mmio_decode_a64.h"

static uint8_t *window;
static uint8_t model[0x8000];
static volatile sig_atomic_t traps, rejected;
static volatile uintptr_t last_fault;

static uint64_t read_value(void *opaque, uint32_t offset, int size)
{
    uint64_t value = 0;
    memcpy(&value, (uint8_t *)opaque + offset, (size_t)size);
    return value;
}
static void write_value(void *opaque, uint32_t offset, uint64_t value, int size)
{
    memcpy((uint8_t *)opaque + offset, &value, (size_t)size);
}
static void handler(int sig, siginfo_t *info, void *context)
{
    uintptr_t address = (uintptr_t)info->si_addr;
    if (address < (uintptr_t)window || address >= (uintptr_t)window + sizeof(model))
        _exit(90 + sig);
    ++traps;
    last_fault = address;
    if (mmio_emulate_a64((ucontext_t *)context, (uint32_t)(address - (uintptr_t)window), address,
                         model, read_value, write_value)) return;
    ++rejected;
    A64_CTX_PC((ucontext_t *)context) += 4;
}
static int decode_only(uint32_t instruction)
{
    ucontext_t uc;
    memset(&uc, 0, sizeof(uc));
#if defined(__APPLE__)
    struct __darwin_mcontext64 mc;
    memset(&mc, 0, sizeof(mc));
    uc.uc_mcontext = &mc;
#endif
    A64_CTX_PC(&uc) = (uintptr_t)&instruction;
    return mmio_emulate_a64(&uc, 0, 0, model, read_value, write_value);
}
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

int main(void)
{
    window = mmap(NULL, sizeof(model), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(window != MAP_FAILED);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGBUS, &action, NULL) == 0);
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0);

    *(volatile uint32_t *)(window + 0x40) = 0xDEADBEEFu;
    uint64_t value = UINT64_MAX;
    __asm__ volatile("ldr %w0, [%1]" : "+r"(value) : "r"(window + 0x40) : "memory");
    CHECK(value == UINT64_C(0xDEADBEEF));
    *(volatile uint16_t *)(window + 0x50) = 0xBEEF;
    *(volatile uint8_t *)(window + 0x60) = 0x80;
    CHECK(*(volatile uint16_t *)(window + 0x50) == 0xBEEF);
    CHECK((int64_t)*(volatile int8_t *)(window + 0x60) == -128);
    __asm__ volatile("ldrsw %0, [%1]" : "=r"(value) : "r"(window + 0x40) : "memory");
    CHECK(value == UINT64_C(0xFFFFFFFFDEADBEEF));

    uint8_t *p = window + 0x100;
    __asm__ volatile("str %w1, [%0], #4\n\tstr %w2, [%0], #4"
                     : "+r"(p) : "r"(1u), "r"(2u) : "memory");
    CHECK(p == window + 0x108 && *(uint32_t *)(model + 0x100) == 1 &&
          *(uint32_t *)(model + 0x104) == 2);
    p = window + 0x100;
    uint32_t word;
    __asm__ volatile("ldr %w0, [%1, #4]!" : "=r"(word), "+r"(p) : : "memory");
    CHECK(word == 2 && p == window + 0x104);
    __asm__ volatile("stur %w0, [%1, #-4]" : : "r"(3u), "r"(p) : "memory");
    __asm__ volatile("ldur %w0, [%1, #-4]" : "=r"(word) : "r"(p) : "memory");
    CHECK(word == 3);
    uint32_t index = 2;
    __asm__ volatile("ldr %w0, [%1, %w2, uxtw #2]" : "=r"(word)
                     : "r"(window + 0xf8), "r"(index) : "memory");
    CHECK(word == 3);
    index = (uint32_t)-1;
    __asm__ volatile("ldr %w0, [%1, %w2, sxtw #2]" : "=r"(word)
                     : "r"(window + 0x104), "r"(index) : "memory");
    CHECK(word == 3);

    uint64_t a, b;
    __asm__ volatile("stp %w0, %w1, [%2]" : : "r"(5u), "r"(6u), "r"(window + 0x300) : "memory");
    __asm__ volatile("ldp %w0, %w1, [%2]" : "=r"(a), "=r"(b) : "r"(window + 0x300) : "memory");
    CHECK(a == 5 && b == 6);
    p = window + 0x2f8;
    __asm__ volatile("ldp %w0, %w1, [%2, #8]!"
                     : "=r"(a), "=r"(b), "+r"(p) : : "memory");
    CHECK(a == 5 && b == 6 && p == window + 0x300);
    *(uint64_t *)(model + 0x400) = UINT64_C(0x0011223344556677);
    *(uint64_t *)(model + 0x408) = UINT64_C(0x8899AABBCCDDEEFF);
    __asm__ volatile("ldr q0, [%2]\n\tfmov %0, d0\n\tmov %1, v0.d[1]"
                     : "=r"(a), "=r"(b) : "r"(window + 0x400) : "v0", "memory");
    CHECK(a == UINT64_C(0x0011223344556677) && b == UINT64_C(0x8899AABBCCDDEEFF));
    __asm__ volatile("ldr q0, [%0]\n\tstr q0, [%1]"
                     : : "r"(model + 0x400), "r"(window + 0x420) : "v0", "memory");
    CHECK(*(uint64_t *)(model + 0x420) == a && *(uint64_t *)(model + 0x428) == b);

    CHECK(decode_only(0x69000440u) == 0);
    CHECK(decode_only(0xED400440u) == 0);
    CHECK(decode_only(0xB9400020u) == 1);
    int before = rejected;
    __asm__ volatile("ldxr %w0, [%1]" : "=r"(word) : "r"(window + 0x40) : "memory");
    CHECK(rejected == before + 1);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page <= sizeof(model) / 2 && mprotect(window, page, PROT_READ | PROT_WRITE) == 0);
    const uint64_t crossing = UINT64_C(0x8877665544332211);
    memcpy(model + page - 4, &crossing, sizeof(crossing));
    memcpy(window + page - 4, &crossing, 4);
    __asm__ volatile("ldr %0, [%1]" : "=r"(value) : "r"(window + page - 4) : "memory");
    if (value != crossing)
        fprintf(stderr, "cross-page fault at offset %#lx, expected access %#lx\n",
                (unsigned long)(last_fault - (uintptr_t)window), (unsigned long)(page - 4));
    CHECK(value == crossing);
    CHECK(traps >= 16);
    CHECK(munmap(window, sizeof(model)) == 0);
    printf("AArch64 MMIO: %d real faults, %d rejected\n", traps, rejected);
    return 0;
}
#else
int main(void) { return 77; }
#endif
