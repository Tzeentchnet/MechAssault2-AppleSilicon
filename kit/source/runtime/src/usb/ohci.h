/*
 * ohci.h -- the Xbox USB host controllers, enough of them to be found.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the OHCI controller registers directly rather than
 * going through anything this runtime can shim. Half-Life 2 registers a 4 KB
 * block at 0xFED00000 and takes an interrupt vector for it, then reads
 * HcRevision out of an aperture backed by zeroed RAM, concludes there is a
 * host controller with no root hub ports, and enumerates nothing.
 *
 * The models provide register behaviour and a worker that walks endpoint and
 * transfer descriptors to deliver the emulated gamepad's reports. They are
 * not a complete implementation of every OHCI or USB device.
 *
 * Written from the OHCI 1.0a specification and this title's own code.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the models up. Safe to call more than once; does nothing unless
 * RECOMP_USB is set, so a title that was working without a controller keeps
 * behaving exactly as it did. POSIX AArch64 requires an attached MMIO broker
 * before Init; check IsEnabled before allowing guest execution. */
void xbox_OhciInit(void);

/* Inspect after Init and before starting guest work. False means either that
 * RECOMP_USB is disabled or initialization failed; call Shutdown after a
 * failed initialization to release any retained traps. */
int xbox_OhciIsEnabled(void);

/* Stop the worker and restore its pages before releasing the memory layout.
 * On POSIX AArch64, seal registrations and stop/join all fault producers
 * first. Returns 0 if producers remain, the worker cannot be stopped, or a
 * page cannot be restored; retain guest memory and retry in that case. */
int xbox_OhciShutdown(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Service a trapped access. Returns 1 if the faulting instruction was decoded
 * and stepped over. `ctx` is a Windows CONTEXT or a POSIX ucontext_t. */
int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

#if defined(__aarch64__) && !defined(_WIN32)
/* Broker thread only; the fault and complete range are validated first. */
struct XboxPosixMmioRequest;
int xbox_OhciPosixSameOwner(uint32_t first_va, uint32_t second_va);
int xbox_OhciPosixRequest(uint32_t fault_va, struct XboxPosixMmioRequest *request);
#endif

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

#endif /* XBOX_OHCI_H */
