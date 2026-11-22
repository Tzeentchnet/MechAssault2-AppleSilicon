#define RECOMP_GENERATED_CODE
#include <recomp_types.h>
#include "check.h"

#include <pthread.h>
#include <stdlib.h>

enum { WORKERS = 4, CALLS = 100000 };
RECOMP_TLS uint32_t g_eax, g_esp, g_ebx, g_esi, g_edi;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;

static void target(void) { ++g_eax; g_esp += 4; }
recomp_func_t recomp_lookup(uint32_t address)
{
    return address == 0x11000 ? target : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t address) { (void)address; return NULL; }
void recomp_icall_fail_log(uint32_t address) { (void)address; abort(); }
void recomp_icall_not_code_log(uint32_t address) { (void)address; abort(); }

static void *worker(void *opaque)
{
    (void)opaque;
    for (unsigned i = 0; i < CALLS; ++i) {
        g_esp = 0xffc;
        RECOMP_ICALL_SAFE_AT(0x11000, 0x1000, 0x10100);
        if (g_esp != 0x1000 || g_eax != i + 1)
            return (void *)(uintptr_t)1;
    }
    return NULL;
}

int main(void)
{
    pthread_t workers[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        CHECK(pthread_create(&workers[i], NULL, worker, NULL) == 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        void *result;
        CHECK(pthread_join(workers[i], &result) == 0 && result == NULL);
    }
    CHECK(g_icall_count == WORKERS * CALLS);
    CHECK(g_icall_trace_idx == WORKERS * CALLS);
    return 0;
}
