#define RECOMP_GENERATED_CODE
#include <recomp_types.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcomment"
#include "kernel/kernel.h"
#pragma clang diagnostic pop
#include "kernel/xbox_memory_layout.h"
#include "check.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int release_workers;
enum { POOL_WORKERS = 16, POOL_CAPACITY = 8 };
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_changed = PTHREAD_COND_INITIALIZER;
static unsigned pool_started, pool_finished;
static int pool_go, pool_release;
static uint32_t pool_stacks[POOL_WORKERS];
static volatile int unexpected_thread_calls;
static volatile int bridge_failure, bridge_calls;
static volatile uint32_t bridge_tib, bridge_stack;
static int inline_calls;

typedef struct Worker {
    uint32_t stack_top;
    uint32_t marker;
    uint32_t tib;
    uint32_t tls_block;
    uint32_t tls_data;
    int failed;
} Worker;

static void guest_target(void)
{
    g_eax += MEM32(g_esp + 4);
    g_esp += 8;
}

static void unexpected_thread(void)
{
    __atomic_fetch_add(&unexpected_thread_calls, 1, __ATOMIC_RELAXED);
}

static void check_bridge_thread(void)
{
    uint32_t tib = g_fs_base;
    uint32_t block = MEM32(tib + 4) - 20;
    if (tib == XBOX_TIB_MAIN || MEM32(tib + 0x18) != tib ||
        MEM32(tib) != UINT32_MAX || MEM32(tib + 0x30) != 0 ||
        MEM32(block + 4) != 0x55667788 || g_fp_control_word != 0x037f ||
        g_eax != 0 || MEM32(g_esp) != 0 ||
        MEM32(g_esp + 4) != 0x1357 || MEM32(g_esp + 8) != 0x2468)
        __atomic_store_n(&bridge_failure, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&bridge_tib, tib, __ATOMIC_RELAXED);
    __atomic_store_n(&bridge_stack, g_esp + 12, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bridge_calls, 1, __ATOMIC_RELAXED);
    g_eax = 0x9876;
}

static void bridge_returns(void) { check_bridge_thread(); }

static void inline_main(void)
{
    if (g_fs_base == XBOX_TIB_MAIN && MEM32(g_esp) == 0 &&
        MEM32(g_esp + 4) == 0x1357 && MEM32(g_esp + 8) == 0x2468)
        ++inline_calls;
}

static void bridge_terminates(void)
{
    check_bridge_thread();
    PUSH32(g_esp, 0x4321);
    PUSH32(g_esp, 0x200020);
    recomp_func_t terminate = recomp_lookup_kernel(MEM32(0x20010c));
    if (terminate)
        terminate();
    __atomic_store_n(&bridge_failure, 1, __ATOMIC_RELAXED);
}

recomp_func_t recomp_lookup(uint32_t address)
{
    return address == 0x200010 ? guest_target :
           address == 0x200018 ? unexpected_thread :
           address == 0x20001c ? bridge_returns :
           address == 0x200024 ? bridge_terminates :
           address == 0x200028 ? inline_main : NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

void ma2_xact_on_free(uint32_t address, uint32_t size)
{
    (void)address; (void)size;
}

void recomp_icall_fail_log(uint32_t address) { (void)address; abort(); }
void recomp_icall_not_code_log(uint32_t address) { (void)address; abort(); }

static void put32(uint8_t *where, uint32_t value)
{
    memcpy(where, &value, sizeof(value));
}

static void make_image(uint8_t *image, int has_tls)
{
    memset(image, 0, 0x1000);
    put32(image + 0x104, XBOX_BASE_ADDRESS);
    put32(image + 0x108, 0x1000);
    put32(image + 0x11c, 1);
    put32(image + 0x120, XBOX_BASE_ADDRESS + 0x180);
    put32(image + 0x12c, has_tls ? 0x200000 : 0);
    put32(image + 0x180, 4);
    put32(image + 0x184, 0x200000);
    put32(image + 0x188, 0x1000);
    put32(image + 0x18c, 0x300);
    put32(image + 0x190, 0x80);
    put32(image + 0x300, 0x200040);
    put32(image + 0x304, 0x200048);
    put32(image + 0x310, 4);
    put32(image + 0x340, 0x11223344);
    put32(image + 0x344, 0x55667788);
}

static void *worker_main(void *opaque)
{
    Worker *worker = opaque;
    if (g_esp != 0 || g_eax != 0 || g_fp_control_word != 0x037f ||
        g_fp_top != 0 || g_xmm0.q[0] != 0 || g_mm0.q != 0)
        worker->failed = 1;
    worker->tib = xbox_AllocThreadTib();
    if (!worker->tib) {
        worker->failed = 1;
    } else {
        g_fs_base = worker->tib;
        g_esp = worker->stack_top;
        worker->tls_block = MEM32(g_fs_base + 4) - 20;
        worker->tls_data = MEM32(worker->tls_block);
        if (MEM32(g_fs_base + 0x18) != g_fs_base ||
            MEM32(g_fs_base) != UINT32_MAX ||
            MEM32(g_fs_base + 0x30) != 0 ||
            MEM32(worker->tls_block + 4) != 0x55667788 ||
            MEM32(worker->tls_block + 8) != 0 ||
            MEM32(worker->tls_data + 4) != 0)
            worker->failed = 1;
        MEM32(g_fs_base) = worker->marker;
        MEM32(worker->tls_block + 4) = worker->marker;
        MEM32(worker->tls_data + 4) = worker->marker;
        g_eax = worker->marker;
        g_fp_control_word = (uint16_t)(0x037f + worker->marker);
        g_fp_stack[3] = (double)worker->marker;
        g_xmm0.u[2] = worker->marker;
        g_mm0.ud[1] = worker->marker;
        g_df = (int)worker->marker;
    }

    pthread_mutex_lock(&mutex);
    if (worker->tib) {
        PUSH32(g_esp, 7);
        PUSH32(g_esp, 0x10203);
        RECOMP_ICALL_SAFE_AT(0x200010, worker->stack_top, 0x200020);
        if (g_esp != worker->stack_top || g_eax != worker->marker + 7)
            worker->failed = 1;
    }
    ++ready;
    pthread_cond_broadcast(&changed);
    while (!release_workers)
        pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);

    if (worker->tib &&
        (g_fs_base != worker->tib || MEM32(g_fs_base) != worker->marker ||
         MEM32(worker->tls_block + 4) != worker->marker ||
         MEM32(worker->tls_data + 4) != worker->marker ||
         g_eax != worker->marker + 7 ||
         g_fp_control_word != (uint16_t)(0x037f + worker->marker) ||
         g_fp_stack[3] != (double)worker->marker ||
         g_xmm0.u[2] != worker->marker || g_mm0.ud[1] != worker->marker ||
         g_df != (int)worker->marker))
        worker->failed = 1;
    return NULL;
}

static void *pool_worker(void *opaque)
{
    unsigned index = (unsigned)(uintptr_t)opaque;
    pthread_mutex_lock(&pool_mutex);
    ++pool_started;
    pthread_cond_broadcast(&pool_changed);
    while (!pool_go)
        pthread_cond_wait(&pool_changed, &pool_mutex);
    pthread_mutex_unlock(&pool_mutex);
    pool_stacks[index] = xbox_AllocThreadStack();
    pthread_mutex_lock(&pool_mutex);
    ++pool_finished;
    pthread_cond_broadcast(&pool_changed);
    while (!pool_release)
        pthread_cond_wait(&pool_changed, &pool_mutex);
    pthread_mutex_unlock(&pool_mutex);
    xbox_FreeThreadStack(pool_stacks[index]);
    return NULL;
}

static int parallel_stack_pool(void)
{
    pthread_t workers[POOL_WORKERS];
    for (unsigned i = 0; i < POOL_WORKERS; ++i)
        CHECK(pthread_create(&workers[i], NULL, pool_worker, (void *)(uintptr_t)i) == 0);
    pthread_mutex_lock(&pool_mutex);
    while (pool_started != POOL_WORKERS)
        pthread_cond_wait(&pool_changed, &pool_mutex);
    pool_go = 1;
    pthread_cond_broadcast(&pool_changed);
    while (pool_finished != POOL_WORKERS)
        pthread_cond_wait(&pool_changed, &pool_mutex);
    unsigned allocated = 0;
    int unique = 1;
    for (unsigned i = 0; i < POOL_WORKERS; ++i) {
        if (!pool_stacks[i]) continue;
        ++allocated;
        for (unsigned j = 0; j < i; ++j)
            if (pool_stacks[i] == pool_stacks[j]) unique = 0;
    }
    pool_release = 1;
    pthread_cond_broadcast(&pool_changed);
    pthread_mutex_unlock(&pool_mutex);
    for (unsigned i = 0; i < POOL_WORKERS; ++i)
        CHECK(pthread_join(workers[i], NULL) == 0);
    CHECK(allocated == POOL_CAPACITY && unique);

    uint32_t stacks[POOL_CAPACITY];
    for (unsigned i = 0; i < POOL_CAPACITY; ++i) {
        stacks[i] = xbox_AllocThreadStack();
        CHECK(stacks[i] != 0);
    }
    CHECK(xbox_AllocThreadStack() == 0);
    for (unsigned i = 0; i < POOL_CAPACITY; ++i)
        xbox_FreeThreadStack(stacks[i]);
    return 0;
}

static void setup_thread_thunks(void)
{
    MEM32(0x200100) = 0x800000ffu;
    MEM32(0x200104) = 0x800000feu;
    MEM32(0x200108) = 0x800000bbu;
    MEM32(0x20010c) = 0x80000102u;
    xbox_kernel_set_thunk_address(0x200100, 4);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
}

static int rejected_thread(unsigned slot, uint32_t routine, uint32_t expected_status)
{
    uint32_t args[10] = {0};
    uint32_t saved_esp = g_esp;
    unsigned count = slot ? 7 : 10;
    args[0] = 0x200200;
    args[slot ? 3 : 4] = 0x200204;
    args[slot ? 6 : 9] = routine;
    MEM32(0x200200) = MEM32(0x200204) = 0xabcdef01;
    for (unsigned i = count; i > 0; --i)
        PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, 0x200020);
    recomp_func_t thunk = recomp_lookup_kernel(MEM32(0x200100 + 4 * slot));
    CHECK(thunk != NULL);
    thunk();
    CHECK(g_esp == saved_esp);
    CHECK(g_eax == expected_status);
    CHECK(MEM32(0x200200) == 0 && MEM32(0x200204) == 0);
    CHECK(__atomic_load_n(&unexpected_thread_calls, __ATOMIC_RELAXED) == 0);
    return 0;
}

static int successful_thread(unsigned slot, uint32_t routine, uint32_t expected_exit)
{
    uint32_t args[10] = {0};
    uint32_t saved_esp = g_esp;
    unsigned count = slot ? 7 : 10;
    args[0] = 0x200200;
    args[slot ? 3 : 4] = 0x200204;
    args[slot ? 4 : 5] = 0x1357;
    args[slot ? 5 : 6] = 0x2468;
    args[slot ? 6 : 9] = routine;
    MEM32(0x200200) = MEM32(0x200204) = 0;
    __atomic_store_n(&bridge_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&bridge_failure, 0, __ATOMIC_RELAXED);
    for (unsigned i = count; i > 0; --i)
        PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, 0x200020);
    recomp_func_t thunk = recomp_lookup_kernel(MEM32(0x200100 + 4 * slot));
    CHECK(thunk != NULL);
    thunk();
    CHECK(g_esp == saved_esp && g_eax == 0);
    uint32_t token = MEM32(0x200200);
    CHECK(token != 0 && MEM32(0x200204) != 0);
    HANDLE thread = (HANDLE)xbox_thread_debug_handle();
    CHECK(thread != NULL && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    CHECK(GetExitCodeThread(thread, &exit_code) && exit_code == expected_exit);
    CHECK(__atomic_load_n(&bridge_calls, __ATOMIC_RELAXED) == 1);
    CHECK(!__atomic_load_n(&bridge_failure, __ATOMIC_RELAXED));
    uint32_t tib = __atomic_load_n(&bridge_tib, __ATOMIC_RELAXED);
    uint32_t stack = __atomic_load_n(&bridge_stack, __ATOMIC_RELAXED);
    CHECK(xbox_HeapBlockSize(tib) == 0);
    CHECK(xbox_HeapBlockSize(stack + 16 - 512u * 1024u) == 0);
    CHECK(g_eax == 0 && g_esp == saved_esp);

    PUSH32(g_esp, token);
    PUSH32(g_esp, 0x200020);
    thunk = recomp_lookup_kernel(MEM32(0x200108));
    CHECK(thunk != NULL);
    thunk();
    CHECK(g_esp == saved_esp && g_eax == 0);
    return 0;
}

static int successful_inline_main(void)
{
    uint32_t args[10] = {0}, saved_esp = g_esp;
    args[0] = 0x200200;
    args[4] = 0x200204;
    args[5] = 0x1357;
    args[6] = 0x2468;
    args[9] = 0x200028;
    for (unsigned i = 10; i > 0; --i)
        PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, 0x200020);
    recomp_func_t thunk = recomp_lookup_kernel(MEM32(0x200100));
    CHECK(thunk != NULL);
    thunk();
    CHECK(g_esp == saved_esp && g_eax == 0 && inline_calls == 1);
    CHECK(MEM32(0x200200) != 0 && MEM32(0x200204) == 0);
    return 0;
}

static int run_guest(void)
{
    _Alignas(16) uint8_t image[0x1000];
    make_image(image, 1);
    CHECK(g_esp == 0 && g_eax == 0 && g_fs_base == XBOX_TIB_MAIN);
    CHECK(xbox_MemoryLayoutInit(image, sizeof(image)));
    CHECK(g_esp == XBOX_STACK_TOP && g_fs_base == XBOX_TIB_MAIN);
    g_eax = 0x1234;
    MEM32(XBOX_TIB_MAIN) = 0x5678;
    MEM32(XBOX_TIB_MAIN + 0x30) = 0xabcdef01;
    uint32_t main_tls_block = MEM32(XBOX_TIB_MAIN + 4) - 20;
    CHECK(MEM32(main_tls_block + 4) == 0x55667788);
    MEM32(main_tls_block + 4) = 0x87654321;
    Worker workers[2] = {{0, 1, 0, 0, 0, 0}, {0, 2, 0, 0, 0, 0}};
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        workers[i].stack_top = xbox_AllocThreadStack();
        CHECK(workers[i].stack_top != 0);
        CHECK(pthread_create(&threads[i], NULL, worker_main, &workers[i]) == 0);
    }
    pthread_mutex_lock(&mutex);
    while (ready != 2)
        pthread_cond_wait(&changed, &mutex);
    release_workers = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(!workers[0].failed && !workers[1].failed);
    CHECK(workers[0].tib != workers[1].tib);
    CHECK(workers[0].tls_block != workers[1].tls_block);
    CHECK(workers[0].tls_data != workers[1].tls_data);
    CHECK(g_eax == 0x1234 && g_esp == XBOX_STACK_TOP &&
          g_fs_base == XBOX_TIB_MAIN && MEM32(XBOX_TIB_MAIN) == 0x5678);
    CHECK(MEM32(XBOX_TIB_MAIN + 0x30) == 0xabcdef01);
    CHECK(MEM32(main_tls_block + 4) == 0x87654321);
    for (unsigned i = 0; i < 2; ++i)
        xbox_FreeThreadStack(workers[i].stack_top);
    CHECK(parallel_stack_pool() == 0);
    setup_thread_thunks();
    xbox_SetThreadMode(XBOX_THREAD_MODE_INLINE);
    CHECK(rejected_thread(0, 0, (uint32_t)STATUS_INVALID_PARAMETER) == 0);
    CHECK(rejected_thread(1, 0x200030, (uint32_t)STATUS_INVALID_PARAMETER) == 0);
    CHECK(successful_inline_main() == 0);
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    uint32_t full_pool[POOL_CAPACITY];
    for (unsigned i = 0; i < POOL_CAPACITY; ++i) {
        full_pool[i] = xbox_AllocThreadStack();
        CHECK(full_pool[i] != 0);
    }
    CHECK(rejected_thread(0, 0x200018, (uint32_t)STATUS_INSUFFICIENT_RESOURCES) == 0);
    CHECK(rejected_thread(1, 0x200018, (uint32_t)STATUS_INSUFFICIENT_RESOURCES) == 0);
    for (unsigned i = 0; i < POOL_CAPACITY; ++i)
        xbox_FreeThreadStack(full_pool[i]);
    CHECK(rejected_thread(0, 0x200030, (uint32_t)STATUS_INVALID_PARAMETER) == 0);
    CHECK(rejected_thread(1, 0x200030, (uint32_t)STATUS_INVALID_PARAMETER) == 0);
    CHECK(successful_thread(0, 0x20001c, 0) == 0);
    CHECK(successful_thread(1, 0x200024, 0x4321) == 0);
    CHECK(xbox_MemoryLayoutShutdownChecked());

    make_image(image, 0);
    CHECK(xbox_MemoryLayoutInit(image, sizeof(image)));
    uint32_t tib_without_tls = xbox_AllocThreadTib();
    CHECK(tib_without_tls != 0);
    CHECK(MEM32(tib_without_tls + 0x18) == tib_without_tls);
    CHECK(MEM32(tib_without_tls) == UINT32_MAX);
    CHECK(xbox_MemoryLayoutShutdownChecked());

    make_image(image, 1);
    put32(image + 0x310, 0x400);
    CHECK(xbox_MemoryLayoutInit(image, sizeof(image)));
    uint32_t large_total = ((8u + 0x400u + 15u) & ~15u) + 4u;
    uint32_t large_block = MEM32(XBOX_TIB_MAIN + 4) - large_total;
    uint32_t large_thread_data = MEM32(large_block);
    CHECK(large_thread_data >= large_block + large_total);
    CHECK(large_thread_data + 64u <= XBOX_STACK_BASE);
    MEM32(large_thread_data + 4) = 0x13572468;
    CHECK(MEM32(large_block + 0x204) == 0);
    CHECK(xbox_MemoryLayoutShutdownChecked());

    make_image(image, 1);
    put32(image + 0x310, UINT32_MAX - 7u);
    int malformed_initialized = xbox_MemoryLayoutInit(image, sizeof(image));
    if (malformed_initialized)
        CHECK(xbox_MemoryLayoutShutdownChecked());
    CHECK(!malformed_initialized);

    make_image(image, 0);
    CHECK(xbox_MemoryLayoutInit(image, sizeof(image)));
    setup_thread_thunks();
    uint32_t reserved_stack = xbox_AllocThreadStack();
    CHECK(reserved_stack != 0);
    unsigned fills = 0;
    while (fills < 256 && xbox_HeapAlloc(512u * 1024u, 4096)) ++fills;
    CHECK(fills < 256);
    fills = 0;
    while (fills < 256 && xbox_HeapAlloc(4096, 16)) ++fills;
    CHECK(fills < 256);
    fills = 0;
    while (fills < 256 && xbox_HeapAlloc(16, 16)) ++fills;
    CHECK(fills < 256);
    xbox_FreeThreadStack(reserved_stack);
    uint32_t probe_stack = xbox_AllocThreadStack();
    CHECK(probe_stack == reserved_stack);
    CHECK(xbox_AllocThreadTib() == 0);
    xbox_FreeThreadStack(probe_stack);
    CHECK(rejected_thread(0, 0x200018, (uint32_t)STATUS_INSUFFICIENT_RESOURCES) == 0);
    CHECK(rejected_thread(1, 0x200018, (uint32_t)STATUS_INSUFFICIENT_RESOURCES) == 0);
    CHECK(xbox_MemoryLayoutShutdownChecked());
    return 0;
}

static void *guest_main(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)run_guest();
}

int main(void)
{
    pthread_t guest;
    void *result;
    g_eax = 0x9876;
    g_esp = 0x5432;
    CHECK(setenv("RECOMP_HEADLESS", "1", 1) == 0);
    CHECK(pthread_create(&guest, NULL, guest_main, NULL) == 0);
    CHECK(pthread_join(guest, &result) == 0);
    CHECK(result == NULL && g_eax == 0x9876 && g_esp == 0x5432);
    return 0;
}
