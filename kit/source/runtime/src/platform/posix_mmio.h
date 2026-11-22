#ifndef XBOX_POSIX_MMIO_H
#define XBOX_POSIX_MMIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MCPXAPUState;

/* The host owns SIGBUS/SIGSEGV installation and chaining. Start before
 * MemoryLayoutInit can protect APU/AC97 pages, then attach the initialized
 * layout and optional APU before starting any fault producer. */
int xbox_PosixMmioStart(void);
int xbox_PosixMmioAttach(struct MCPXAPUState *apu);

/* Every native thread that can execute guest code must register itself before
 * doing so and unregister after its last possible fault. Registration is
 * bounded and may fail; failure must prevent guest execution. Alternate signal
 * stacks are supported, but guest execution must use the registered native
 * thread stack. Registration disables pthread cancellation until unregister,
 * which restores its previous state. SIGBUS/SIGSEGV must be unblocked outside
 * their handlers, and alternate stacks must not change or overlap another
 * producer's stack while registered. Neither function may be called from a
 * signal handler. */
enum { XBOX_POSIX_MMIO_MAX_PRODUCERS = 32 };
int xbox_PosixMmioRegisterThread(void);
int xbox_PosixMmioUnregisterThread(void);

/* From the host's SA_SIGINFO handler for genuine synchronous faults only.
 * The host must block both SIGBUS and SIGSEGV during either handler. Returns
 * 1 after the complete instruction has been serviced; 0 leaves its context
 * untouched and must be chained. No runtime signal handler is installed. */
int xbox_PosixMmioFault(void *context, const void *fault_address);

/* Stop creating guest threads, stop/join existing guests, and quiesce the APU
 * before sealing. Seal prevents new registrations while existing registered
 * threads and device interrupts finish. Stop OHCI after sealing and joining
 * producers. BeginShutdown then rejects even registered faults; Stop refuses
 * to tear down until all registrations are gone. Only after Stop succeeds may
 * the caller destroy the APU and release guest memory. */
int xbox_PosixMmioSealProducers(void);
int xbox_PosixMmioBeginShutdown(void);
int xbox_PosixMmioStop(void);

/* A worker that cannot finish an accepted request must not resume the guest
 * with invented data. The signal path terminates the process with this status
 * after its bounded spin budget is exhausted. */
enum { XBOX_POSIX_MMIO_STALLED_EXIT = 124 };

/* Runtime-internal broker interface. A backend is called only on the broker
 * thread, once per decoded instruction. Return zero only before making any
 * side effect. Two operations represent paired or 128-bit accesses. */
typedef struct XboxPosixMmioOp {
    uint32_t offset;
    unsigned size;
    int write;
    uint64_t value;
} XboxPosixMmioOp;

typedef struct XboxPosixMmioRequest {
    uint32_t fault_offset;
    unsigned count;
    XboxPosixMmioOp op[2];
} XboxPosixMmioRequest;

typedef int (*XboxPosixMmioBackend)(void *opaque, XboxPosixMmioRequest *request);
typedef void (*XboxPosixMmioPublication)(void *opaque);

int xbox_PosixMmioBrokerStart(XboxPosixMmioBackend backend,
                              XboxPosixMmioPublication publish,
                              XboxPosixMmioPublication unpublish,
                              uint64_t spin_limit);
int xbox_PosixMmioBrokerBind(uintptr_t base, size_t size, void *opaque);
int xbox_PosixMmioIsStarted(void);
int xbox_PosixMmioIsBound(void);
int xbox_PosixMmioDevicesMayTeardown(void);
int xbox_PosixMmioFaultOffset(void *context, uint32_t offset);

#if defined(XBOX_POSIX_MMIO_TESTING)
/* Fill the nonblocking wake pipe to exercise the saturation path. */
int xbox_PosixMmioTestFillWakePipe(void);
unsigned xbox_PosixMmioTestFullWakeups(void);
unsigned xbox_PosixMmioTestCurrentSlot(void);
uintptr_t xbox_PosixMmioTestSlotLow(unsigned slot);
void xbox_PosixMmioTestPauseScan(uintptr_t low);
int xbox_PosixMmioTestScanPaused(void);
void xbox_PosixMmioTestResumeScan(void);
unsigned xbox_PosixMmioTestClaimedSlot(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
