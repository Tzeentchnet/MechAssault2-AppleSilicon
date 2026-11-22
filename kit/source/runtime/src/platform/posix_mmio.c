#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "posix_mmio.h"

#if defined(__aarch64__) && !defined(_WIN32)

#include "mmio_decode_a64.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "MMIO signal state must be lock-free");
_Static_assert(__atomic_always_lock_free(sizeof(uintptr_t), 0),
               "MMIO signal pointers must be lock-free");
_Static_assert(__atomic_always_lock_free(sizeof(size_t), 0),
               "MMIO signal sizes must be lock-free");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
               "MMIO slot generations must be lock-free");

enum BrokerState { BROKER_STOPPED, BROKER_RUNNING, BROKER_DRAINING,
                   BROKER_STOPPING, BROKER_JOINING };
enum SlotState { SLOT_FREE, SLOT_IDLE, SLOT_PREPARING, SLOT_PENDING,
                 SLOT_PROCESSING, SLOT_DONE, SLOT_RETIRED };

#define SLOT_PHASE_MASK UINT64_C(7)
#define SLOT_GENERATION_STEP UINT64_C(8)

static unsigned slot_phase(uint64_t state)
{
    return (unsigned)(state & SLOT_PHASE_MASK);
}

static uint64_t slot_with_phase(uint64_t state, unsigned phase)
{
    return (state & ~SLOT_PHASE_MASK) | phase;
}

static uint64_t slot_after_release(uint64_t state)
{
    uint64_t generation = state & ~SLOT_PHASE_MASK;
    if (generation == (UINT64_MAX & ~SLOT_PHASE_MASK))
        return generation | SLOT_RETIRED;
    return generation + SLOT_GENERATION_STEP;
}

typedef struct MmioSlot {
    _Atomic(uint64_t) state;
    _Atomic(uintptr_t) stack_low, stack_high;
    pthread_t owner;                 /* lifecycle mutex only */
    uintptr_t alt_low, alt_high;     /* lifecycle mutex only */
    int previous_cancel_state;       /* lifecycle mutex only */
    XboxPosixMmioRequest request;    /* transferred by state release/acquire */
    int handled;
} MmioSlot;

static MmioSlot s_slots[XBOX_POSIX_MMIO_MAX_PRODUCERS];
static pthread_mutex_t s_lifecycle = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ready_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t s_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t s_slot_key;
static int s_key_ready;
static atomic_int s_state = ATOMIC_VAR_INIT(BROKER_STOPPED);
static atomic_int s_worker_stop = ATOMIC_VAR_INIT(0);
static _Atomic(uintptr_t) s_base = ATOMIC_VAR_INIT(0);
static _Atomic(size_t) s_size = ATOMIC_VAR_INIT(0);
static unsigned s_producers;
static pthread_t s_worker;
static int s_worker_ready;
static int s_pipe[2] = { -1, -1 };
static XboxPosixMmioBackend s_backend;
static XboxPosixMmioPublication s_publish, s_unpublish;
static void *s_opaque;
static uint64_t s_spin_limit;
#if defined(XBOX_POSIX_MMIO_TESTING)
static atomic_uint s_full_wakeups;
static _Atomic(uintptr_t) s_test_pause_low;
static atomic_uint s_test_scan_paused, s_test_scan_resume, s_test_claimed_slot;
#endif

static void notify_worker(void)
{
    const unsigned char byte = 1;
    int saved_errno = errno;
    /* A full pipe already contains a wakeup. The worker also polls and scans,
     * so even EINTR or another failed notification cannot lose a request. */
    ssize_t written = write(s_pipe[1], &byte, 1);
#if defined(XBOX_POSIX_MMIO_TESTING)
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        atomic_fetch_add_explicit(&s_full_wakeups, 1, memory_order_relaxed);
#else
    (void)written;
#endif
    errno = saved_errno;
}

static void *broker_thread(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&s_lifecycle);
    s_worker_ready = 1;
    pthread_cond_signal(&s_ready_cond);
    pthread_mutex_unlock(&s_lifecycle);

    while (!atomic_load_explicit(&s_worker_stop, memory_order_acquire)) {
        struct pollfd pfd = { s_pipe[0], POLLIN, 0 };
        unsigned char bytes[128];
        (void)poll(&pfd, 1, 10);
        while (read(s_pipe[0], bytes, sizeof(bytes)) > 0) {}

        for (unsigned i = 0; i < XBOX_POSIX_MMIO_MAX_PRODUCERS; ++i) {
            MmioSlot *slot = &s_slots[i];
            uint64_t expected = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (slot_phase(expected) != SLOT_PENDING) continue;
            if (!atomic_compare_exchange_strong_explicit(
                    &slot->state, &expected, slot_with_phase(expected, SLOT_PROCESSING),
                    memory_order_acquire, memory_order_relaxed))
                continue;
            slot->handled = s_backend(s_opaque, &slot->request);
            atomic_store_explicit(&slot->state, slot_with_phase(expected, SLOT_DONE),
                                  memory_order_release);
        }
    }
    return NULL;
}

static int set_pipe_flags(int fd)
{
    int status = fcntl(fd, F_GETFL);
    int descriptor = fcntl(fd, F_GETFD);
    return status >= 0 && descriptor >= 0 &&
        fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == 0;
}

int xbox_PosixMmioBrokerStart(XboxPosixMmioBackend backend,
                              XboxPosixMmioPublication publish,
                              XboxPosixMmioPublication unpublish,
                              uint64_t spin_limit)
{
    if (!backend || !spin_limit) return 0;
    pthread_mutex_lock(&s_lifecycle);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != BROKER_STOPPED) {
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    if (pipe(s_pipe) != 0) {
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    if (!set_pipe_flags(s_pipe[0]) || !set_pipe_flags(s_pipe[1])) {
        close(s_pipe[0]); close(s_pipe[1]);
        s_pipe[0] = s_pipe[1] = -1;
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    s_backend = backend;
    s_publish = publish;
    s_unpublish = unpublish;
    s_opaque = NULL;
    atomic_store_explicit(&s_size, 0, memory_order_relaxed);
    s_producers = 0;
    s_spin_limit = spin_limit;
    s_worker_ready = 0;
    atomic_store_explicit(&s_worker_stop, 0, memory_order_relaxed);
    if (pthread_create(&s_worker, NULL, broker_thread, NULL) != 0) {
        close(s_pipe[0]); close(s_pipe[1]);
        s_pipe[0] = s_pipe[1] = -1;
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    while (!s_worker_ready)
        pthread_cond_wait(&s_ready_cond, &s_lifecycle);
    atomic_store_explicit(&s_state, BROKER_RUNNING, memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return 1;
}

int xbox_PosixMmioBrokerBind(uintptr_t base, size_t size, void *opaque)
{
    if (!base || !size || size > UINT32_MAX || base > UINTPTR_MAX - size)
        return 0;
    pthread_mutex_lock(&s_lifecycle);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != BROKER_RUNNING ||
        atomic_load_explicit(&s_base, memory_order_relaxed) || s_producers) {
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    atomic_store_explicit(&s_size, size, memory_order_relaxed);
    s_opaque = opaque;
    if (s_publish) s_publish(opaque);
    atomic_store_explicit(&s_base, base, memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return 1;
}

int xbox_PosixMmioIsStarted(void)
{
    return atomic_load_explicit(&s_state, memory_order_acquire) == BROKER_RUNNING;
}

int xbox_PosixMmioIsBound(void)
{
    return atomic_load_explicit(&s_base, memory_order_acquire) != 0;
}

static int current_stack(uintptr_t *low, uintptr_t *high)
{
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    if (!top || !size || size > top) return 0;
    *low = top - size;
    *high = top;
    return 1;
#else
    pthread_attr_t attr;
    void *base;
    size_t size;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    int ok = pthread_attr_getstack(&attr, &base, &size) == 0 &&
        base && size && (uintptr_t)base <= UINTPTR_MAX - size;
    pthread_attr_destroy(&attr);
    if (!ok) return 0;
    *low = (uintptr_t)base;
    *high = *low + size;
    return 1;
#endif
}

static void producer_exited(void *opaque)
{
    MmioSlot *slot = opaque;
    pthread_mutex_lock(&s_lifecycle);
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (slot_phase(state) != SLOT_IDLE || !s_producers) {
        pthread_mutex_unlock(&s_lifecycle);
        _exit(XBOX_POSIX_MMIO_STALLED_EXIT);
    }
    atomic_store_explicit(&slot->state, slot_after_release(state), memory_order_release);
    --s_producers;
    pthread_mutex_unlock(&s_lifecycle);
}

static void make_slot_key(void)
{
    s_key_ready = pthread_key_create(&s_slot_key, producer_exited) == 0;
}

int xbox_PosixMmioRegisterThread(void)
{
    int previous_cancel_state;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel_state) != 0)
        return 0;
    if (pthread_once(&s_key_once, make_slot_key) != 0 || !s_key_ready)
        goto failed;
    uintptr_t low, high;
    if (!current_stack(&low, &high)) goto failed;
    sigset_t mask;
    stack_t alternate;
    if (pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0 ||
        sigismember(&mask, SIGBUS) || sigismember(&mask, SIGSEGV) ||
        sigaltstack(NULL, &alternate) != 0 || (alternate.ss_flags & SS_ONSTACK))
        goto failed;
    uintptr_t alt_low = 0, alt_high = 0;
    if (!(alternate.ss_flags & SS_DISABLE)) {
        alt_low = (uintptr_t)alternate.ss_sp;
        if (!alt_low || !alternate.ss_size || alt_low > UINTPTR_MAX - alternate.ss_size)
            goto failed;
        alt_high = alt_low + alternate.ss_size;
    }
    pthread_t self = pthread_self();
    pthread_mutex_lock(&s_lifecycle);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != BROKER_RUNNING ||
        !atomic_load_explicit(&s_base, memory_order_acquire) ||
        pthread_equal(self, s_worker)) {
        pthread_mutex_unlock(&s_lifecycle);
        goto failed;
    }
    MmioSlot *free_slot = NULL;
    uint64_t free_state = 0;
    for (unsigned i = 0; i < XBOX_POSIX_MMIO_MAX_PRODUCERS; ++i) {
        MmioSlot *slot = &s_slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        unsigned phase = slot_phase(state);
        if (phase == SLOT_FREE) {
            if (!free_slot) { free_slot = slot; free_state = state; }
        } else if (phase != SLOT_RETIRED) {
            uintptr_t other_low = atomic_load_explicit(&slot->stack_low, memory_order_relaxed);
            uintptr_t other_high = atomic_load_explicit(&slot->stack_high, memory_order_relaxed);
            if (pthread_equal(self, slot->owner) || (low < other_high && other_low < high) ||
                (alt_low && alt_low < other_high && other_low < alt_high) ||
                (slot->alt_low && low < slot->alt_high && slot->alt_low < high) ||
                (alt_low && slot->alt_low && alt_low < slot->alt_high && slot->alt_low < alt_high)) {
                pthread_mutex_unlock(&s_lifecycle);
                goto failed;
            }
        }
    }
    if (!free_slot) {
        pthread_mutex_unlock(&s_lifecycle);
        goto failed;
    }
    if (pthread_setspecific(s_slot_key, free_slot) != 0) {
        pthread_mutex_unlock(&s_lifecycle);
        goto failed;
    }
    free_slot->owner = self;
    free_slot->alt_low = alt_low;
    free_slot->alt_high = alt_high;
    free_slot->previous_cancel_state = previous_cancel_state;
    atomic_store_explicit(&free_slot->stack_low, low, memory_order_relaxed);
    atomic_store_explicit(&free_slot->stack_high, high, memory_order_relaxed);
    ++s_producers;
    atomic_store_explicit(&free_slot->state, slot_with_phase(free_state, SLOT_IDLE),
                          memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return 1;
failed:
    pthread_setcancelstate(previous_cancel_state, NULL);
    return 0;
}

int xbox_PosixMmioUnregisterThread(void)
{
    int entry_cancel_state;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &entry_cancel_state) != 0)
        return 0;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&s_lifecycle);
    for (unsigned i = 0; i < XBOX_POSIX_MMIO_MAX_PRODUCERS; ++i) {
        MmioSlot *slot = &s_slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        unsigned phase = slot_phase(state);
        if (phase != SLOT_FREE && phase != SLOT_RETIRED && pthread_equal(self, slot->owner)) {
            if (phase != SLOT_IDLE || pthread_setspecific(s_slot_key, NULL) != 0) {
                pthread_mutex_unlock(&s_lifecycle);
                pthread_setcancelstate(entry_cancel_state, NULL);
                return 0;
            }
            int previous_cancel_state = slot->previous_cancel_state;
            atomic_store_explicit(&slot->state, slot_after_release(state), memory_order_release);
            --s_producers;
            pthread_mutex_unlock(&s_lifecycle);
            pthread_setcancelstate(previous_cancel_state, NULL);
            return 1;
        }
    }
    pthread_mutex_unlock(&s_lifecycle);
    pthread_setcancelstate(entry_cancel_state, NULL);
    return 0;
}

int xbox_PosixMmioSealProducers(void)
{
    pthread_mutex_lock(&s_lifecycle);
    int state = atomic_load_explicit(&s_state, memory_order_relaxed);
    if (state == BROKER_RUNNING)
        atomic_store_explicit(&s_state, BROKER_DRAINING, memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return state == BROKER_RUNNING || state == BROKER_DRAINING;
}

int xbox_PosixMmioBeginShutdown(void)
{
    pthread_mutex_lock(&s_lifecycle);
    int state = atomic_load_explicit(&s_state, memory_order_relaxed);
    if (state == BROKER_DRAINING)
        atomic_store_explicit(&s_state, BROKER_STOPPING, memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return state == BROKER_DRAINING || state == BROKER_STOPPING;
}

int xbox_PosixMmioDevicesMayTeardown(void)
{
    pthread_mutex_lock(&s_lifecycle);
    int state = atomic_load_explicit(&s_state, memory_order_relaxed);
    int ok = state == BROKER_STOPPED ||
        ((state == BROKER_DRAINING || state == BROKER_STOPPING) && !s_producers);
    pthread_mutex_unlock(&s_lifecycle);
    return ok;
}

int xbox_PosixMmioStop(void)
{
    pthread_mutex_lock(&s_lifecycle);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != BROKER_STOPPING ||
        s_producers) {
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    atomic_store_explicit(&s_state, BROKER_JOINING, memory_order_release);
    atomic_store_explicit(&s_worker_stop, 1, memory_order_release);
    notify_worker();
    pthread_mutex_unlock(&s_lifecycle);

    int joined = pthread_join(s_worker, NULL) == 0;
    pthread_mutex_lock(&s_lifecycle);
    if (!joined) {
        atomic_store_explicit(&s_state, BROKER_STOPPING, memory_order_release);
        pthread_mutex_unlock(&s_lifecycle);
        return 0;
    }
    atomic_store_explicit(&s_base, 0, memory_order_release);
    if (s_unpublish) s_unpublish(s_opaque);
    s_opaque = NULL;
    atomic_store_explicit(&s_size, 0, memory_order_relaxed);
    close(s_pipe[0]); close(s_pipe[1]);
    s_pipe[0] = s_pipe[1] = -1;
    atomic_store_explicit(&s_state, BROKER_STOPPED, memory_order_release);
    pthread_mutex_unlock(&s_lifecycle);
    return 1;
}

/* Volatile byte copies prevent the compiler from introducing libc memcpy or
 * memset calls in the handler. Only the signal frame and handler stack are
 * accessed; device and shared request bytes move via the slot state. */
static void signal_copy(void *destination, const void *source, size_t bytes)
{
    volatile unsigned char *dst = destination;
    const volatile unsigned char *src = source;
    for (size_t i = 0; i < bytes; ++i) dst[i] = src[i];
}

typedef struct Capture {
    XboxPosixMmioRequest *request;
    unsigned next;
    int valid;
} Capture;

static uint64_t capture_read(void *opaque, uint32_t offset, int size)
{
    Capture *capture = opaque;
    if (capture->next >= 2) {
        capture->valid = 0;
        return 0;
    }
    XboxPosixMmioOp *op = &capture->request->op[capture->next++];
    op->offset = offset; op->size = (unsigned)size; op->write = 0; op->value = 0;
    return 0;
}

static void capture_write(void *opaque, uint32_t offset, uint64_t value, int size)
{
    Capture *capture = opaque;
    if (capture->next >= 2) {
        capture->valid = 0;
        return;
    }
    XboxPosixMmioOp *op = &capture->request->op[capture->next++];
    op->offset = offset; op->size = (unsigned)size; op->write = 1; op->value = value;
}

static uint64_t replay_read(void *opaque, uint32_t offset, int size)
{
    Capture *capture = opaque;
    if (capture->next >= capture->request->count) {
        capture->valid = 0;
        return 0;
    }
    XboxPosixMmioOp *op = &capture->request->op[capture->next++];
    if (op->write || op->offset != offset || op->size != (unsigned)size)
        capture->valid = 0;
    return op->value;
}

static void replay_write(void *opaque, uint32_t offset, uint64_t value, int size)
{
    Capture *capture = opaque;
    if (capture->next >= capture->request->count) {
        capture->valid = 0;
        return;
    }
    XboxPosixMmioOp *op = &capture->request->op[capture->next++];
    if (!op->write || op->offset != offset || op->size != (unsigned)size ||
        op->value != value)
        capture->valid = 0;
}

int xbox_PosixMmioFault(void *context, const void *fault_address)
{
    int state = atomic_load_explicit(&s_state, memory_order_acquire);
    if (!context || (state != BROKER_RUNNING && state != BROKER_DRAINING))
        return 0;
    ucontext_t *original = context;
#if defined(__APPLE__)
    if (!original->uc_mcontext) return 0;
#endif
    uintptr_t base = atomic_load_explicit(&s_base, memory_order_acquire);
    uintptr_t fault = (uintptr_t)fault_address;
    size_t size = atomic_load_explicit(&s_size, memory_order_relaxed);
    if (!base || fault < base || fault - base >= size) return 0;

    uintptr_t sp = (uintptr_t)A64_CTX_SP(original);
    MmioSlot *slot = NULL;
    uint64_t ticket = 0;
    for (unsigned i = 0; i < XBOX_POSIX_MMIO_MAX_PRODUCERS; ++i) {
        MmioSlot *candidate = &s_slots[i];
        uint64_t observed = atomic_load_explicit(&candidate->state, memory_order_acquire);
        if (slot_phase(observed) != SLOT_IDLE) continue;
        uintptr_t low = atomic_load_explicit(&candidate->stack_low, memory_order_relaxed);
#if defined(XBOX_POSIX_MMIO_TESTING)
        if (low && low == atomic_load_explicit(&s_test_pause_low, memory_order_relaxed)) {
            atomic_store_explicit(&s_test_scan_paused, 1, memory_order_release);
            uint64_t spins;
            for (spins = 0; spins < s_spin_limit; ++spins) {
                if (atomic_load_explicit(&s_test_scan_resume, memory_order_acquire)) break;
                __asm__ volatile("yield" ::: "memory");
            }
            if (spins == s_spin_limit) _exit(XBOX_POSIX_MMIO_STALLED_EXIT);
        }
#endif
        uintptr_t high = atomic_load_explicit(&candidate->stack_high, memory_order_relaxed);
        if (sp < low || sp >= high) continue;
        uint64_t expected = observed;
        if (!atomic_compare_exchange_strong_explicit(
                &candidate->state, &expected, slot_with_phase(observed, SLOT_PREPARING),
                memory_order_acquire, memory_order_relaxed))
            continue;
        slot = candidate;
        ticket = observed;
#if defined(XBOX_POSIX_MMIO_TESTING)
        atomic_store_explicit(&s_test_claimed_slot, i + 1, memory_order_relaxed);
#endif
        break;
    }
    if (!slot) return 0;
    state = atomic_load_explicit(&s_state, memory_order_acquire);
    if (state != BROKER_RUNNING && state != BROKER_DRAINING) {
        atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
        return 0;
    }

    uintptr_t pc = (uintptr_t)A64_CTX_PC(original);
    if (!pc || (pc & 3u)) {
        atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
        return 0;
    }
    uint32_t instruction = *(const uint32_t *)pc;
    ucontext_t shadow;
#if defined(__APPLE__)
    struct __darwin_mcontext64 machine;
    shadow.uc_mcontext = &machine;
    signal_copy(&machine, original->uc_mcontext, sizeof(machine));
#else
    signal_copy(&shadow, original, sizeof(shadow));
#endif
    A64_CTX_PC(&shadow) = (uintptr_t)&instruction;
    slot->request.fault_offset = (uint32_t)(fault - base);
    Capture capture = { &slot->request, 0, 1 };
    int decoded = mmio_emulate_a64(&shadow, slot->request.fault_offset, fault,
                                    &capture, capture_read, capture_write);
    slot->request.count = capture.next;
    if (!decoded || !capture.valid || !capture.next) {
        atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
        return 0;
    }
    for (unsigned i = 0; i < capture.next; ++i) {
        XboxPosixMmioOp *op = &slot->request.op[i];
        if (!op->size || op->size > 8 || op->offset >= size ||
            op->size > size - op->offset) {
            atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
            return 0;
        }
    }
    atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_PENDING), memory_order_release);
    notify_worker();

    uint64_t spins;
    for (spins = 0; spins < s_spin_limit; ++spins) {
        if (atomic_load_explicit(&slot->state, memory_order_acquire) ==
            slot_with_phase(ticket, SLOT_DONE))
            break;
        __asm__ volatile("yield" ::: "memory");
    }
    if (spins == s_spin_limit) _exit(XBOX_POSIX_MMIO_STALLED_EXIT);
    if (!slot->handled) {
        atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
        return 0;
    }

#if defined(__APPLE__)
    signal_copy(&machine, original->uc_mcontext, sizeof(machine));
#else
    signal_copy(&shadow, original, sizeof(shadow));
#endif
    A64_CTX_PC(&shadow) = (uintptr_t)&instruction;
    capture.next = 0; capture.valid = 1;
    decoded = mmio_emulate_a64(&shadow, slot->request.fault_offset, fault,
                               &capture, replay_read, replay_write);
    if (!decoded || !capture.valid || capture.next != slot->request.count)
        _exit(XBOX_POSIX_MMIO_STALLED_EXIT);
    A64_CTX_PC(&shadow) = pc + 4;
#if defined(__APPLE__)
    signal_copy(original->uc_mcontext, &machine, sizeof(machine));
#else
    signal_copy(&original->uc_mcontext, &shadow.uc_mcontext,
                sizeof(original->uc_mcontext));
#endif
    atomic_store_explicit(&slot->state, slot_with_phase(ticket, SLOT_IDLE), memory_order_release);
    return 1;
}

int xbox_PosixMmioFaultOffset(void *context, uint32_t offset)
{
    uintptr_t base = atomic_load_explicit(&s_base, memory_order_acquire);
    size_t size = atomic_load_explicit(&s_size, memory_order_relaxed);
    if (!base || offset >= size) return 0;
    return xbox_PosixMmioFault(context, (const void *)(base + offset));
}

#if defined(XBOX_POSIX_MMIO_TESTING)
int xbox_PosixMmioTestFillWakePipe(void)
{
    const unsigned char byte = 1;
    int filled = 0;
    pthread_mutex_lock(&s_lifecycle);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) == BROKER_RUNNING) {
        for (unsigned i = 0; i < 1048576; ++i) {
            if (write(s_pipe[1], &byte, 1) == 1) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) filled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_lifecycle);
    return filled;
}

unsigned xbox_PosixMmioTestFullWakeups(void)
{
    return atomic_load_explicit(&s_full_wakeups, memory_order_relaxed);
}

unsigned xbox_PosixMmioTestCurrentSlot(void)
{
    unsigned result = 0;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&s_lifecycle);
    for (unsigned i = 0; i < XBOX_POSIX_MMIO_MAX_PRODUCERS; ++i) {
        unsigned phase = slot_phase(atomic_load_explicit(&s_slots[i].state, memory_order_acquire));
        if (phase != SLOT_FREE && phase != SLOT_RETIRED &&
            pthread_equal(self, s_slots[i].owner)) {
            result = i + 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_lifecycle);
    return result;
}

uintptr_t xbox_PosixMmioTestSlotLow(unsigned slot)
{
    if (!slot || slot > XBOX_POSIX_MMIO_MAX_PRODUCERS) return 0;
    return atomic_load_explicit(&s_slots[slot - 1].stack_low, memory_order_relaxed);
}

void xbox_PosixMmioTestPauseScan(uintptr_t low)
{
    atomic_store_explicit(&s_test_scan_paused, 0, memory_order_relaxed);
    atomic_store_explicit(&s_test_scan_resume, 0, memory_order_relaxed);
    atomic_store_explicit(&s_test_claimed_slot, 0, memory_order_relaxed);
    atomic_store_explicit(&s_test_pause_low, low, memory_order_release);
}

int xbox_PosixMmioTestScanPaused(void)
{
    return (int)atomic_load_explicit(&s_test_scan_paused, memory_order_acquire);
}

void xbox_PosixMmioTestResumeScan(void)
{
    atomic_store_explicit(&s_test_pause_low, 0, memory_order_relaxed);
    atomic_store_explicit(&s_test_scan_resume, 1, memory_order_release);
}

unsigned xbox_PosixMmioTestClaimedSlot(void)
{
    return atomic_load_explicit(&s_test_claimed_slot, memory_order_relaxed);
}
#endif

#else
int xbox_PosixMmioBrokerStart(XboxPosixMmioBackend backend, XboxPosixMmioPublication publish,
                              XboxPosixMmioPublication unpublish, uint64_t limit)
{ (void)backend; (void)publish; (void)unpublish; (void)limit; return 0; }
int xbox_PosixMmioBrokerBind(uintptr_t base, size_t size, void *opaque)
{ (void)base; (void)size; (void)opaque; return 0; }
int xbox_PosixMmioIsStarted(void) { return 0; }
int xbox_PosixMmioIsBound(void) { return 0; }
int xbox_PosixMmioDevicesMayTeardown(void) { return 1; }
int xbox_PosixMmioRegisterThread(void) { return 0; }
int xbox_PosixMmioUnregisterThread(void) { return 0; }
int xbox_PosixMmioSealProducers(void) { return 0; }
int xbox_PosixMmioBeginShutdown(void) { return 0; }
int xbox_PosixMmioStop(void) { return 0; }
int xbox_PosixMmioFault(void *context, const void *address)
{ (void)context; (void)address; return 0; }
int xbox_PosixMmioFaultOffset(void *context, uint32_t offset)
{ (void)context; (void)offset; return 0; }
#endif
