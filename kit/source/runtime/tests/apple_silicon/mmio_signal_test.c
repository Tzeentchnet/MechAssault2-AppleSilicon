#include "posix_mmio.h"

#if defined(__aarch64__) && !defined(_WIN32)
#include "mmio_decode_a64.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

enum { UNAVAILABLE = 0x180, NESTED = 0x200, WORKER_FAULT = 0x240,
       BLOCKED = 0x280, THREAD_BASE = 0x1000 };
static uint8_t *window;
static size_t window_size;
static _Thread_local int producer_marker;
static atomic_uint signal_count, handled_count, chained_count, context_errors;
static atomic_uint sigbus_count, sigsegv_count, nested_rejected;
static atomic_int allow_chain;

typedef struct FakeDevice {
    uint8_t *bytes;
    atomic_uint callbacks, wrong_thread, gate_entered, gate_release;
    atomic_uint nested_sent, worker_fault_rejected, publications, unpublications;
    pthread_t nested_target;
    int stall_fd;
} FakeDevice;

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
#if defined(__APPLE__)
    volatile unsigned char saved[sizeof(*uc->uc_mcontext)];
    volatile unsigned char *machine = (volatile unsigned char *)uc->uc_mcontext;
#else
    volatile unsigned char saved[sizeof(uc->uc_mcontext)];
    volatile unsigned char *machine = (volatile unsigned char *)&uc->uc_mcontext;
#endif
    for (size_t i = 0; i < sizeof(saved); ++i) saved[i] = machine[i];
    atomic_fetch_add_explicit(&signal_count, 1, memory_order_relaxed);
    if (signal_number == SIGBUS)
        atomic_fetch_add_explicit(&sigbus_count, 1, memory_order_relaxed);
    if (signal_number == SIGSEGV)
        atomic_fetch_add_explicit(&sigsegv_count, 1, memory_order_relaxed);
    if (xbox_PosixMmioFault(context, info->si_addr)) {
        atomic_fetch_add_explicit(&handled_count, 1, memory_order_relaxed);
        return;
    }
    for (size_t i = 0; i < sizeof(saved); ++i) {
        if (saved[i] != machine[i]) {
            atomic_fetch_add_explicit(&context_errors, 1, memory_order_relaxed);
            break;
        }
    }
    atomic_fetch_add_explicit(&chained_count, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&allow_chain, memory_order_relaxed)) {
        const char owned[] = "unexpected unhandled test-window fault\n";
        const char outside[] = "unexpected fault outside test window\n";
        uintptr_t address = (uintptr_t)info->si_addr;
        if (address >= (uintptr_t)window && address - (uintptr_t)window < window_size)
            (void)write(STDERR_FILENO, owned, sizeof(owned) - 1);
        else
            (void)write(STDERR_FILENO, outside, sizeof(outside) - 1);
        _exit(90 + signal_number);
    }
    /* This is the test's outer chain stub, not runtime fault handling. */
    A64_CTX_PC(uc) += 4;
}

static void nested_handler(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number; (void)info;
    if (xbox_PosixMmioFault(context, window + NESTED)) _exit(88);
    atomic_fetch_add_explicit(&nested_rejected, 1, memory_order_release);
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&delay, NULL);
}

static int wait_at_least(atomic_uint *value, unsigned wanted)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) >= wanted) return 1;
        sleep_ms(1);
    }
    return 0;
}

static void publish(void *opaque)
{
    FakeDevice *device = opaque;
    atomic_fetch_add_explicit(&device->publications, 1, memory_order_relaxed);
}

static void unpublish(void *opaque)
{
    FakeDevice *device = opaque;
    atomic_fetch_add_explicit(&device->unpublications, 1, memory_order_relaxed);
}

static int fake_device(void *opaque, XboxPosixMmioRequest *request)
{
    FakeDevice *device = opaque;
    if (producer_marker)
        atomic_fetch_add_explicit(&device->wrong_thread, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&device->callbacks, 1, memory_order_relaxed);
    if (device->stall_fd >= 0) {
        const char byte = 1;
        (void)write(device->stall_fd, &byte, 1);
        for (;;) sleep_ms(1000);
    }
    if (request->fault_offset == UNAVAILABLE) return 0;
    if (request->fault_offset == BLOCKED) {
        atomic_store_explicit(&device->gate_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&device->gate_release, memory_order_acquire))
            sleep_ms(1);
    }
    if (request->fault_offset == NESTED) {
        unsigned previous = atomic_load_explicit(&nested_rejected, memory_order_acquire);
        if (pthread_kill(device->nested_target, SIGUSR1) != 0 ||
            !wait_at_least(&nested_rejected, previous + 1))
            return 0;
        atomic_fetch_add_explicit(&device->nested_sent, 1, memory_order_release);
    }
    if (request->fault_offset == WORKER_FAULT) {
        uint32_t value = 0x13572468u;
        __asm__ volatile("ldr %w0, [%1]" : "+r"(value) : "r"(window + 0x80) : "memory");
        if (value != 0x13572468u) return 0;
        atomic_store_explicit(&device->worker_fault_rejected, 1, memory_order_release);
    }
    for (unsigned i = 0; i < request->count; ++i) {
        XboxPosixMmioOp *op = &request->op[i];
        if (op->write) {
            for (unsigned j = 0; j < op->size; ++j)
                device->bytes[op->offset + j] = (uint8_t)(op->value >> (8 * j));
        } else {
            uint64_t value = 0;
            for (unsigned j = 0; j < op->size; ++j)
                value |= (uint64_t)device->bytes[op->offset + j] << (8 * j);
            op->value = value;
        }
    }
    return 1;
}

static uint32_t fault_load32(uintptr_t address, uint32_t initial)
{
    __asm__ volatile("ldr %w0, [%1]" : "+r"(initial) : "r"(address) : "memory");
    return initial;
}

static void fault_store32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(address) : "memory");
}

typedef struct Group {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    int go;
    atomic_uint failures;
    unsigned loops;
    uint32_t special_offset;
    int write_only;
} Group;

typedef struct Producer {
    Group *group;
    unsigned index;
    int registered;
} Producer;

static void *producer_thread(void *opaque)
{
    Producer *producer = opaque;
    Group *group = producer->group;
    producer_marker = 1;
    producer->registered = xbox_PosixMmioRegisterThread();
    if (!producer->registered)
        atomic_fetch_add_explicit(&group->failures, 1, memory_order_relaxed);
    pthread_mutex_lock(&group->lock);
    ++group->ready;
    pthread_cond_broadcast(&group->cond);
    while (!group->go) pthread_cond_wait(&group->cond, &group->lock);
    pthread_mutex_unlock(&group->lock);
    if (producer->registered) {
        uint32_t offset = group->special_offset ? group->special_offset :
            THREAD_BASE + producer->index * 16;
        for (unsigned i = 0; i < group->loops; ++i) {
            uint32_t expected = 0xa0000000u | (producer->index << 16) | i;
            fault_store32((uintptr_t)window + offset, expected);
            if (!group->write_only &&
                fault_load32((uintptr_t)window + offset, 0) != expected)
                atomic_fetch_add_explicit(&group->failures, 1, memory_order_relaxed);
        }
        if (!xbox_PosixMmioUnregisterThread())
            atomic_fetch_add_explicit(&group->failures, 1, memory_order_relaxed);
    }
    return NULL;
}

static void group_wait_ready(Group *group, unsigned wanted)
{
    pthread_mutex_lock(&group->lock);
    while (group->ready < wanted) pthread_cond_wait(&group->cond, &group->lock);
    pthread_mutex_unlock(&group->lock);
}

static void group_release(Group *group)
{
    pthread_mutex_lock(&group->lock);
    group->go = 1;
    pthread_cond_broadcast(&group->cond);
    pthread_mutex_unlock(&group->lock);
}

typedef struct OverflowResult {
    int registered;
    uint32_t value;
} OverflowResult;

static void *overflow_thread(void *opaque)
{
    OverflowResult *result = opaque;
    producer_marker = 1;
    result->registered = xbox_PosixMmioRegisterThread();
    if (result->registered) xbox_PosixMmioUnregisterThread();
    else result->value = fault_load32((uintptr_t)window + 0x40, 0x12344321u);
    return NULL;
}

typedef struct OverlapResult {
    void *stack;
    size_t size;
    int configured;
    int registered;
} OverlapResult;

static void *overlap_thread(void *opaque)
{
    OverlapResult *result = opaque;
    stack_t alternate = { .ss_sp = result->stack, .ss_size = result->size, .ss_flags = 0 };
    result->configured = sigaltstack(&alternate, NULL) == 0;
    if (result->configured) {
        result->registered = xbox_PosixMmioRegisterThread();
        if (result->registered) xbox_PosixMmioUnregisterThread();
        alternate.ss_flags = SS_DISABLE;
        if (sigaltstack(&alternate, NULL) != 0) result->configured = 0;
    }
    return NULL;
}

static void *exit_without_unregister(void *opaque)
{
    int *registered = opaque;
    *registered = xbox_PosixMmioRegisterThread();
    return NULL;
}

static void *reused_stack_fault(void *opaque)
{
    uint32_t *value = opaque;
    producer_marker = 1;
    *value = fault_load32((uintptr_t)window + 0x40, 0x87654321u);
    return NULL;
}

typedef struct Cancellation {
    atomic_uint ready, proceed, completed, failed;
} Cancellation;

static void *cancelled_producer(void *opaque)
{
    Cancellation *test = opaque;
    producer_marker = 1;
    if (!xbox_PosixMmioRegisterThread()) {
        atomic_store(&test->failed, 1);
        atomic_store(&test->ready, 1);
        return NULL;
    }
    if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != 0)
        atomic_store(&test->failed, 1);
    atomic_store_explicit(&test->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&test->proceed, memory_order_acquire)) sleep_ms(1);
    fault_store32((uintptr_t)window + 0x500, 0x98765432u);
    if (fault_load32((uintptr_t)window + 0x500, 0) != 0x98765432u)
        atomic_store(&test->failed, 1);
    atomic_store_explicit(&test->completed, 1, memory_order_release);
    if (!xbox_PosixMmioUnregisterThread()) atomic_store(&test->failed, 1);
    pthread_testcancel();
    return NULL;
}

typedef struct ReuseRace {
    atomic_uint ready, proceed, failed;
    unsigned slot;
    int fault;
    uint32_t value;
} ReuseRace;

static void *reuse_race_producer(void *opaque)
{
    ReuseRace *test = opaque;
    producer_marker = 1;
    if (!xbox_PosixMmioRegisterThread()) {
        atomic_store(&test->failed, 1);
        atomic_store(&test->ready, 1);
        return NULL;
    }
    test->slot = xbox_PosixMmioTestCurrentSlot();
    atomic_store_explicit(&test->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&test->proceed, memory_order_acquire)) sleep_ms(1);
    if (test->fault)
        test->value = fault_load32((uintptr_t)window + 0x40, 0x55667788u);
    if (!xbox_PosixMmioUnregisterThread()) atomic_store(&test->failed, 1);
    return NULL;
}

static int install_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    if (sigaction(SIGBUS, &action, NULL) || sigaction(SIGSEGV, &action, NULL))
        return 0;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = nested_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    return sigaction(SIGUSR1, &action, NULL) == 0;
}

static int install_bus_page(size_t page)
{
    char name[80];
    snprintf(name, sizeof(name), "mmio-sigbus-%ld.bin", (long)getpid());
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return 0;
    int ok = ftruncate(fd, (off_t)page) == 0;
    if (ok) {
        void *mapped = mmap(window + 4 * page, page, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, fd, (off_t)page);
        ok = mapped == window + 4 * page;
    }
    close(fd);
    if (unlink(name) != 0) ok = 0;
    return ok;
}

static int fail_fast_test(FakeDevice *device)
{
    int descriptors[2];
    if (pipe(descriptors) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(descriptors[0]);
        device->stall_fd = descriptors[1];
        if (!xbox_PosixMmioBrokerStart(fake_device, NULL, NULL, UINT64_C(500000000)) ||
            !xbox_PosixMmioBrokerBind((uintptr_t)window, window_size, device) ||
            !xbox_PosixMmioRegisterThread())
            _exit(87);
        fault_store32((uintptr_t)window + 0x80, 42);
        _exit(86);
    }
    close(descriptors[1]);
    if (pid < 0) { close(descriptors[0]); return 0; }
    int status;
    if (waitpid(pid, &status, 0) != pid) { close(descriptors[0]); return 0; }
    char byte = 0;
    ssize_t got = read(descriptors[0], &byte, 1);
    close(descriptors[0]);
    return got == 1 && byte == 1 && WIFEXITED(status) &&
        WEXITSTATUS(status) == XBOX_POSIX_MMIO_STALLED_EXIT;
}

typedef struct SyntheticProducer {
    unsigned index;
    atomic_uint *failures;
} SyntheticProducer;

static void *synthetic_producer(void *opaque)
{
    SyntheticProducer *producer = opaque;
    producer_marker = 1;
    if (!xbox_PosixMmioRegisterThread()) {
        atomic_fetch_add(producer->failures, 1);
        return NULL;
    }
    for (unsigned i = 0; i < 200; ++i) {
        ucontext_t uc;
        memset(&uc, 0, sizeof(uc));
#if defined(__APPLE__)
        struct __darwin_mcontext64 machine;
        memset(&machine, 0, sizeof(machine));
        uc.uc_mcontext = &machine;
#endif
        uintptr_t sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        A64_CTX_SP(&uc) = sp;
        uintptr_t address = (uintptr_t)window + THREAD_BASE + producer->index * 16;
        A64_CTX_X(&uc, 1) = address;
        uint32_t instruction = 0xb9000020u; /* str w0, [x1] */
        uint32_t value = (producer->index << 16) | i;
        A64_CTX_X(&uc, 0) = value;
        A64_CTX_PC(&uc) = (uintptr_t)&instruction;
        if (!xbox_PosixMmioFault(&uc, (const void *)address)) {
            atomic_fetch_add(producer->failures, 1);
            break;
        }
        instruction = 0xb9400020u; /* ldr w0, [x1] */
        A64_CTX_X(&uc, 0) = 0;
        A64_CTX_PC(&uc) = (uintptr_t)&instruction;
        if (!xbox_PosixMmioFault(&uc, (const void *)address) || A64_CTX_X(&uc, 0) != value) {
            atomic_fetch_add(producer->failures, 1);
            break;
        }
    }
    if (!xbox_PosixMmioUnregisterThread()) atomic_fetch_add(producer->failures, 1);
    return NULL;
}

static int synthetic_concurrency_test(void)
{
    window_size = 0x8000;
    window = mmap(NULL, window_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(window != MAP_FAILED);
    FakeDevice device = {0};
    device.bytes = calloc(1, window_size);
    device.stall_fd = -1;
    CHECK(device.bytes);
    for (unsigned round = 0; round < 4; ++round) {
        CHECK(xbox_PosixMmioBrokerStart(fake_device, publish, unpublish, UINT64_C(20000000000)));
        CHECK(xbox_PosixMmioBrokerBind((uintptr_t)window, window_size, &device));
        enum { THREADS = 8 };
        pthread_t threads[THREADS];
        SyntheticProducer producers[THREADS];
        atomic_uint failures = 0;
        for (unsigned i = 0; i < THREADS; ++i) {
            producers[i] = (SyntheticProducer){ i, &failures };
            CHECK(!pthread_create(&threads[i], NULL, synthetic_producer, &producers[i]));
        }
        for (unsigned i = 0; i < THREADS; ++i) CHECK(!pthread_join(threads[i], NULL));
        CHECK(!atomic_load(&failures) && !atomic_load(&device.wrong_thread));
        CHECK(xbox_PosixMmioSealProducers() && xbox_PosixMmioBeginShutdown());
        CHECK(xbox_PosixMmioStop());
    }
    CHECK(munmap(window, window_size) == 0);
    free(device.bytes);
    puts("POSIX MMIO: synthetic concurrent publication and restart passed");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--synthetic-tsan"))
        return synthetic_concurrency_test();
    unsigned char overlapping_stack[128 * 1024];
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    window_size = 6 * page;
    window = mmap(NULL, window_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *alternate_memory = malloc(128 * 1024);
    CHECK(alternate_memory);
    stack_t alternate = { .ss_sp = alternate_memory, .ss_size = 128 * 1024, .ss_flags = 0 };
    CHECK(sigaltstack(&alternate, NULL) == 0);
    CHECK(window != MAP_FAILED && install_bus_page(page) && install_handlers());
    FakeDevice device = {0};
    device.bytes = calloc(1, window_size);
    device.stall_fd = -1;
    CHECK(device.bytes);
    producer_marker = 1;

    atomic_store(&allow_chain, 1);
    CHECK(!xbox_PosixMmioRegisterThread());
    CHECK(fault_load32((uintptr_t)window + 0x40, 0x12345678) == 0x12345678);
    CHECK(atomic_load(&chained_count) == 1 && !atomic_load(&context_errors));
    CHECK(xbox_PosixMmioBrokerStart(fake_device, publish, unpublish, UINT64_C(20000000000)));
    CHECK(!xbox_PosixMmioRegisterThread());
    CHECK(xbox_PosixMmioBrokerBind((uintptr_t)window, window_size, &device));
    CHECK(!xbox_PosixMmioBrokerBind((uintptr_t)window, window_size, &device));
    CHECK(fault_load32((uintptr_t)window + 0x40, 0xabcdef01) == 0xabcdef01);
    sigset_t blocked_mask, previous_mask;
    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGBUS);
    CHECK(!pthread_sigmask(SIG_BLOCK, &blocked_mask, &previous_mask));
    CHECK(!xbox_PosixMmioRegisterThread());
    CHECK(!pthread_sigmask(SIG_SETMASK, &previous_mask, NULL));
    CHECK(xbox_PosixMmioRegisterThread() && !xbox_PosixMmioRegisterThread());
    ucontext_t invalid_pc = {0};
#if defined(__APPLE__)
    struct __darwin_mcontext64 invalid_machine = {0};
    invalid_pc.uc_mcontext = &invalid_machine;
#endif
    uintptr_t native_sp;
    __asm__ volatile("mov %0, sp" : "=r"(native_sp));
    A64_CTX_SP(&invalid_pc) = native_sp;
    unsigned before_invalid_pc = atomic_load(&device.callbacks);
    for (uintptr_t pc = 0; pc <= 1; ++pc) {
        A64_CTX_PC(&invalid_pc) = pc;
        CHECK(!xbox_PosixMmioFault(&invalid_pc, window + 0x40));
        CHECK(A64_CTX_PC(&invalid_pc) == pc && A64_CTX_SP(&invalid_pc) == native_sp);
    }
    CHECK(atomic_load(&device.callbacks) == before_invalid_pc);
    OverlapResult overlap = { overlapping_stack, sizeof(overlapping_stack), 0, 0 };
    pthread_t overlap_id;
    CHECK(!pthread_create(&overlap_id, NULL, overlap_thread, &overlap));
    CHECK(!pthread_join(overlap_id, NULL) && overlap.configured && !overlap.registered);
    atomic_store(&allow_chain, 0);

    size_t stack_size = 64 * page;
    void *reused_stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(reused_stack != MAP_FAILED);
    pthread_attr_t stack_attr;
    CHECK(!pthread_attr_init(&stack_attr));
    CHECK(!pthread_attr_setstack(&stack_attr, reused_stack, stack_size));
    pthread_t reused_thread;
    int abandoned_registration = 0;
    CHECK(!pthread_create(&reused_thread, &stack_attr, exit_without_unregister, &abandoned_registration));
    CHECK(!pthread_join(reused_thread, NULL) && abandoned_registration);
    uint32_t reused_value = 0;
    unsigned before_reuse_calls = atomic_load(&device.callbacks);
    unsigned before_reuse_chains = atomic_load(&chained_count);
    atomic_store(&allow_chain, 1);
    CHECK(!pthread_create(&reused_thread, &stack_attr, reused_stack_fault, &reused_value));
    CHECK(!pthread_join(reused_thread, NULL) && reused_value == 0x87654321u);
    CHECK(atomic_load(&chained_count) == before_reuse_chains + 1 &&
          atomic_load(&device.callbacks) == before_reuse_calls);
    atomic_store(&allow_chain, 0);
    CHECK(!pthread_attr_destroy(&stack_attr) && !munmap(reused_stack, stack_size));

    Cancellation cancellation = {0};
    pthread_t cancelled;
    CHECK(!pthread_create(&cancelled, NULL, cancelled_producer, &cancellation));
    CHECK(wait_at_least(&cancellation.ready, 1) && !atomic_load(&cancellation.failed));
    CHECK(!pthread_cancel(cancelled));
    atomic_store_explicit(&cancellation.proceed, 1, memory_order_release);
    void *cancel_result;
    CHECK(!pthread_join(cancelled, &cancel_result) && cancel_result == PTHREAD_CANCELED);
    CHECK(atomic_load(&cancellation.completed) && !atomic_load(&cancellation.failed));

    void *race_stacks = mmap(NULL, 3 * stack_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(race_stacks != MAP_FAILED);
    pthread_attr_t race_attr[3];
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!pthread_attr_init(&race_attr[i]));
        CHECK(!pthread_attr_setstack(&race_attr[i], (char *)race_stacks + i * stack_size,
                                     stack_size));
    }
    ReuseRace old_slot = {0}, faulting_slot = { .fault = 1 }, new_slot = {0};
    pthread_t old_thread, faulting_thread, new_thread;
    CHECK(!pthread_create(&old_thread, &race_attr[0], reuse_race_producer, &old_slot));
    CHECK(wait_at_least(&old_slot.ready, 1) && old_slot.slot && !atomic_load(&old_slot.failed));
    CHECK(!pthread_create(&faulting_thread, &race_attr[1], reuse_race_producer, &faulting_slot));
    CHECK(wait_at_least(&faulting_slot.ready, 1) && faulting_slot.slot &&
          !atomic_load(&faulting_slot.failed));
    uintptr_t old_low = xbox_PosixMmioTestSlotLow(old_slot.slot);
    uintptr_t faulting_low = xbox_PosixMmioTestSlotLow(faulting_slot.slot);
    CHECK(old_low < faulting_low);
    fault_store32((uintptr_t)window + 0x40, 0x10203040u);
    xbox_PosixMmioTestPauseScan(old_low);
    atomic_store_explicit(&faulting_slot.proceed, 1, memory_order_release);
    unsigned paused;
    for (paused = 0; paused < 5000 && !xbox_PosixMmioTestScanPaused(); ++paused)
        sleep_ms(1);
    CHECK(paused < 5000);
    atomic_store_explicit(&old_slot.proceed, 1, memory_order_release);
    CHECK(!pthread_join(old_thread, NULL) && !atomic_load(&old_slot.failed));
    CHECK(!pthread_create(&new_thread, &race_attr[2], reuse_race_producer, &new_slot));
    CHECK(wait_at_least(&new_slot.ready, 1) && !atomic_load(&new_slot.failed));
    CHECK(new_slot.slot == old_slot.slot &&
          xbox_PosixMmioTestSlotLow(new_slot.slot) > faulting_low);
    xbox_PosixMmioTestResumeScan();
    CHECK(!pthread_join(faulting_thread, NULL) && !atomic_load(&faulting_slot.failed));
    CHECK(faulting_slot.value == 0x10203040u &&
          xbox_PosixMmioTestClaimedSlot() == faulting_slot.slot);
    atomic_store_explicit(&new_slot.proceed, 1, memory_order_release);
    CHECK(!pthread_join(new_thread, NULL) && !atomic_load(&new_slot.failed));
    for (unsigned i = 0; i < 3; ++i) CHECK(!pthread_attr_destroy(&race_attr[i]));
    CHECK(!munmap(race_stacks, 3 * stack_size));

    fault_store32((uintptr_t)window + 0x40, 0xdeadbeef);
    CHECK(fault_load32((uintptr_t)window + 0x40, 0) == 0xdeadbeef);
    *(volatile uint8_t *)(window + 0x60) = 0x80;
    CHECK((int64_t)*(volatile int8_t *)(window + 0x60) == -128);
    uint8_t *p = window + 0x100;
    __asm__ volatile("str %w1, [%0], #4\n\tstr %w2, [%0], #4"
                     : "+r"(p) : "r"(1u), "r"(2u) : "memory");
    CHECK(p == window + 0x108 && fault_load32((uintptr_t)window + 0x100, 0) == 1);
    uint64_t a, b;
    __asm__ volatile("stp %w0, %w1, [%2]" : : "r"(5u), "r"(6u), "r"(window + 0x300) : "memory");
    __asm__ volatile("ldp %w0, %w1, [%2]" : "=r"(a), "=r"(b) : "r"(window + 0x300) : "memory");
    CHECK(a == 5 && b == 6);
    const uint64_t vector[] = { UINT64_C(0x0011223344556677), UINT64_C(0x8899aabbccddeeff) };
    __asm__ volatile("ldr q0, [%0]\n\tstr q0, [%1]"
                     : : "r"(vector), "r"(window + 0x400) : "v0", "memory");
    __asm__ volatile("ldr q0, [%2]\n\tfmov %0, d0\n\tmov %1, v0.d[1]"
                     : "=r"(a), "=r"(b) : "r"(window + 0x400) : "v0", "memory");
    CHECK(a == vector[0] && b == vector[1]);

    unsigned before_bus = atomic_load(&sigbus_count);
    fault_store32((uintptr_t)window + 4 * page + 16, 0x11223344);
    CHECK(fault_load32((uintptr_t)window + 4 * page + 16, 0) == 0x11223344);
    CHECK(atomic_load(&sigbus_count) > before_bus);
    /* Darwin reports PROT_NONE as SIGBUS; an unmapped page exercises SIGSEGV. */
    CHECK(munmap(window + 5 * page, page) == 0);
    unsigned before_segv = atomic_load(&sigsegv_count);
    fault_store32((uintptr_t)window + 5 * page + 16, 0x55667788);
    CHECK(fault_load32((uintptr_t)window + 5 * page + 16, 0) == 0x55667788);
    CHECK(atomic_load(&sigsegv_count) > before_segv);
    CHECK(mmap(window + 5 * page, page, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
               -1, 0) == window + 5 * page);

    atomic_store(&allow_chain, 1);
    unsigned before_calls = atomic_load(&device.callbacks);
    uint32_t exclusive = 0x87654321;
    __asm__ volatile("ldxr %w0, [%1]" : "+r"(exclusive) : "r"(window + 0x40) : "memory");
    CHECK(exclusive == 0x87654321 && atomic_load(&device.callbacks) == before_calls);
    CHECK(fault_load32((uintptr_t)window + UNAVAILABLE, 0x24681357) == 0x24681357);
    fault_store32((uintptr_t)window + UNAVAILABLE, 0xffffffffu);
    CHECK(device.bytes[UNAVAILABLE] == 0 && device.bytes[UNAVAILABLE + 3] == 0);
    CHECK(!atomic_load(&context_errors));
    atomic_store(&allow_chain, 0);

    device.nested_target = pthread_self();
    fault_store32((uintptr_t)window + NESTED, 99);
    CHECK(atomic_load(&device.nested_sent) && atomic_load(&nested_rejected) == 1);
    struct sigaction fault_action;
    CHECK(!sigaction(SIGBUS, NULL, &fault_action));
    fault_action.sa_flags &= ~SA_ONSTACK;
    CHECK(!sigaction(SIGBUS, &fault_action, NULL) && !sigaction(SIGSEGV, &fault_action, NULL));
    fault_store32((uintptr_t)window + NESTED, 100);
    CHECK(atomic_load(&device.nested_sent) == 2 && atomic_load(&nested_rejected) == 2);
    fault_action.sa_flags |= SA_ONSTACK;
    CHECK(!sigaction(SIGBUS, &fault_action, NULL) && !sigaction(SIGSEGV, &fault_action, NULL));
    atomic_store(&allow_chain, 1);
    fault_store32((uintptr_t)window + WORKER_FAULT, 77);
    CHECK(atomic_load(&device.worker_fault_rejected));
    atomic_store(&allow_chain, 0);

    enum { CONCURRENT = 8 };
    Group group = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                    .loops = 100 };
    Producer producers[CONCURRENT];
    pthread_t threads[CONCURRENT];
    for (unsigned i = 0; i < CONCURRENT; ++i) {
        producers[i] = (Producer){ &group, i, 0 };
        CHECK(!pthread_create(&threads[i], NULL, producer_thread, &producers[i]));
    }
    group_wait_ready(&group, CONCURRENT);
    group_release(&group);
    for (unsigned i = 0; i < CONCURRENT; ++i) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!atomic_load(&group.failures) && !atomic_load(&device.wrong_thread));

    enum { EXTRA = 6 };
    Group blocked = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                      .loops = 1, .special_offset = BLOCKED };
    Producer blocker = { &blocked, 0, 0 };
    pthread_t blocker_thread;
    CHECK(!pthread_create(&blocker_thread, NULL, producer_thread, &blocker));
    group_wait_ready(&blocked, 1);
    group_release(&blocked);
    CHECK(wait_at_least(&device.gate_entered, 1));
    CHECK(xbox_PosixMmioTestFillWakePipe());
    Group queued = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                     .loops = 1 };
    Producer pending[EXTRA];
    pthread_t pending_threads[EXTRA];
    for (unsigned i = 0; i < EXTRA; ++i) {
        pending[i] = (Producer){ &queued, i + 10, 0 };
        CHECK(!pthread_create(&pending_threads[i], NULL, producer_thread, &pending[i]));
    }
    group_wait_ready(&queued, EXTRA);
    unsigned before_full = xbox_PosixMmioTestFullWakeups();
    group_release(&queued);
    for (unsigned i = 0; i < 5000 && xbox_PosixMmioTestFullWakeups() < before_full + EXTRA; ++i)
        sleep_ms(1);
    CHECK(xbox_PosixMmioTestFullWakeups() >= before_full + EXTRA);
    atomic_store_explicit(&device.gate_release, 1, memory_order_release);
    CHECK(!pthread_join(blocker_thread, NULL));
    for (unsigned i = 0; i < EXTRA; ++i) CHECK(!pthread_join(pending_threads[i], NULL));
    CHECK(!atomic_load(&blocked.failures) && !atomic_load(&queued.failures));

    enum { OCCUPY = XBOX_POSIX_MMIO_MAX_PRODUCERS - 1 };
    Group full = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
    Producer occupants[OCCUPY];
    pthread_t occupied[OCCUPY], overflow;
    for (unsigned i = 0; i < OCCUPY; ++i) {
        occupants[i] = (Producer){ &full, i, 0 };
        CHECK(!pthread_create(&occupied[i], NULL, producer_thread, &occupants[i]));
    }
    group_wait_ready(&full, OCCUPY);
    OverflowResult overflow_result = { -1, 0 };
    before_calls = atomic_load(&device.callbacks);
    unsigned before_chains = atomic_load(&chained_count);
    atomic_store(&allow_chain, 1);
    CHECK(!pthread_create(&overflow, NULL, overflow_thread, &overflow_result));
    CHECK(!pthread_join(overflow, NULL) && !overflow_result.registered &&
          overflow_result.value == 0x12344321u);
    CHECK(atomic_load(&chained_count) == before_chains + 1 &&
          atomic_load(&device.callbacks) == before_calls);
    atomic_store(&allow_chain, 0);
    CHECK(!atomic_load(&full.failures));
    group_release(&full);
    for (unsigned i = 0; i < OCCUPY; ++i) CHECK(!pthread_join(occupied[i], NULL));

    atomic_store(&device.gate_entered, 0);
    atomic_store(&device.gate_release, 0);
    Group shutdown = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                       .loops = 1, .special_offset = BLOCKED, .write_only = 1 };
    Producer active = { &shutdown, 3, 0 };
    pthread_t active_thread;
    CHECK(!pthread_create(&active_thread, NULL, producer_thread, &active));
    group_wait_ready(&shutdown, 1);
    CHECK(xbox_PosixMmioSealProducers());
    CHECK(!xbox_PosixMmioDevicesMayTeardown());
    CHECK(fault_load32((uintptr_t)window + 0x40, 0) == 0xdeadbeef);
    group_release(&shutdown);
    CHECK(wait_at_least(&device.gate_entered, 1));
    CHECK(xbox_PosixMmioBeginShutdown() && !xbox_PosixMmioStop());
    atomic_store(&allow_chain, 1);
    before_calls = atomic_load(&device.callbacks);
    CHECK(fault_load32((uintptr_t)window + 0x40, 0x55555555) == 0x55555555);
    CHECK(atomic_load(&device.callbacks) == before_calls && !atomic_load(&context_errors));
    CHECK(!atomic_load(&device.unpublications));
    atomic_store_explicit(&device.gate_release, 1, memory_order_release);
    CHECK(!pthread_join(active_thread, NULL) && !atomic_load(&shutdown.failures));
    CHECK(xbox_PosixMmioUnregisterThread());
    CHECK(xbox_PosixMmioDevicesMayTeardown() && xbox_PosixMmioStop());
    CHECK(atomic_load(&device.publications) == 1 && atomic_load(&device.unpublications) == 1);

    atomic_store(&allow_chain, 0);
    for (unsigned i = 0; i < 20; ++i) {
        CHECK(xbox_PosixMmioBrokerStart(fake_device, publish, unpublish, UINT64_C(20000000000)));
        CHECK(xbox_PosixMmioBrokerBind((uintptr_t)window, window_size, &device));
        CHECK(xbox_PosixMmioRegisterThread());
        fault_store32((uintptr_t)window + 0x40, i);
        CHECK(fault_load32((uintptr_t)window + 0x40, UINT32_MAX) == i);
        CHECK(xbox_PosixMmioUnregisterThread() && xbox_PosixMmioSealProducers());
        CHECK(xbox_PosixMmioBeginShutdown() && xbox_PosixMmioStop());
    }
    CHECK(atomic_load(&device.publications) == 21 && atomic_load(&device.unpublications) == 21);
    CHECK(!atomic_load(&device.wrong_thread) && !atomic_load(&context_errors));
    CHECK(fail_fast_test(&device));
    CHECK(munmap(window, window_size) == 0);
    stack_t disabled = alternate;
    disabled.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&disabled, NULL) == 0);
    free(alternate_memory);
    free(device.bytes);
    printf("POSIX MMIO: %u handled faults, %u chained, SIGBUS and SIGSEGV, concurrency and restart passed\n",
           atomic_load(&handled_count), atomic_load(&chained_count));
    return 0;
}
#else
int main(void) { return 77; }
#endif
