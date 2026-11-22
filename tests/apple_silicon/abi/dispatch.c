#define RECOMP_GENERATED_CODE
#define RECOMP_ABI_CHECK
#include <recomp_types.h>
#include "check.h"

#include <string.h>

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_esp, g_ebx, g_esi, g_edi;
uint32_t g_xbox_code_lo = 0x10000, g_xbox_code_hi = 0x20000;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;

static int manual_calls, generated_calls, kernel_calls, failed, rejected, violations;
static int argument_ok;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    (void)va; (void)ebx0; (void)esi0; (void)edi0; (void)esp0;
    ++violations;
}

void recomp_icall_fail_log(uint32_t va) { (void)va; ++failed; }
void recomp_icall_not_code_log(uint32_t va) { (void)va; ++rejected; }

static void callee_cleanup(void)
{
    argument_ok = MEM32(g_esp) == 0x34567 && MEM32(g_esp + 4) == 0x42;
    g_eax = MEM32(g_esp + 4) + 1;
    g_esp += 8;
}

static void caller_cleanup(void)
{
    argument_ok = MEM32(g_esp) == 0x34567 && MEM32(g_esp + 4) == 0x42;
    g_eax = MEM32(g_esp + 4) + 2;
    g_esp += 4;
}

static void manual_target(void) { ++manual_calls; callee_cleanup(); }
static void generated_target(void) { ++generated_calls; caller_cleanup(); }
static void kernel_target(void) { ++kernel_calls; callee_cleanup(); }
static void wrapped_return(void) { g_esp += 4; }
static void missing_return(void) { }
static void corrupt_callee_saved(void) { g_ebx++; g_esp += 4; }

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    return va == 0x11000 ? manual_target : NULL;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x11000 || va == 0x12000 ? generated_target : NULL;
}

recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    return va == 0xfe000004u ? kernel_target : NULL;
}

static void prepare(uint32_t top)
{
    g_esp = top;
    PUSH32(g_esp, 0x42);
    PUSH32(g_esp, 0x34567);
}

int main(void)
{
    _Alignas(16) uint8_t memory[4096] = {0};
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    g_ebx = 11; g_esi = 22; g_edi = 33;

    prepare(2048);
    RECOMP_ABI_CALL(0x11111, callee_cleanup);
    CHECK(argument_ok && g_eax == 0x43 && g_esp == 2048 && violations == 0);

    prepare(2048);
    RECOMP_ICALL_SAFE_AT(0x11000, 2048, 0x10100);
    CHECK(manual_calls == 1 && generated_calls == 0 && g_esp == 2048);
    CHECK(argument_ok && g_eax == 0x43);

    prepare(2048);
    RECOMP_ICALL_SAFE_AT_CC(0x12000, 2048, 0x10100);
    CHECK(generated_calls == 1 && argument_ok && g_eax == 0x44 && g_esp == 2044);
    g_esp += 4;
    CHECK(g_esp == 2048);

    prepare(2048);
    RECOMP_ICALL_SAFE_AT(0xfe000004u, 2048, 0x10100);
    CHECK(kernel_calls == 1 && argument_ok && g_esp == 2048);

    prepare(2048);
    RECOMP_ICALL_SAFE_AT(0x13000, 2048, 0x10100);
    CHECK(failed == 1 && g_esp == 2048 && g_eax == 0);

    prepare(2048);
    RECOMP_ICALL_SAFE_AT_CC(0x13000, 2048, 0x10100);
    CHECK(failed == 2 && g_esp == 2044 && g_eax == 0);
    g_esp += 4;

    prepare(2048);
    RECOMP_ICALL_SAFE_AT(0x30000, 2048, 0x10100);
    CHECK(rejected == 1 && g_esp == 2048 && g_eax == 0);

    prepare(2048);
    RECOMP_ITAIL(0xfe000004u);
    CHECK(kernel_calls == 2 && argument_ok && g_esp == 2048);
    prepare(2048);
    RECOMP_ITAIL(0x13000);
    CHECK(failed == 3 && g_esp == 2044 && g_eax == 0);

    g_esp = 2000;
    MEM32(g_esp) = 0x55667788;
    PUSH32(g_esp, MEM32(g_esp));
    CHECK(g_esp == 1996 && MEM32(g_esp) == 0x55667788);
    uint32_t popped = 0;
    POP32(g_esp, popped);
    CHECK(popped == 0x55667788 && g_esp == 2000);

    g_esp = 0xfffffffcu;
    RECOMP_ABI_CALL(0x11111, wrapped_return);
    CHECK(g_esp == 0 && violations == 0);
    g_esp = 0xfffffffcu;
    RECOMP_ABI_CALL(0x11111, missing_return);
    CHECK(violations == 1);
    g_esp = 0x1000;
    RECOMP_ABI_CALL(0x11111, corrupt_callee_saved);
    CHECK(violations == 2);
    return 0;
}
