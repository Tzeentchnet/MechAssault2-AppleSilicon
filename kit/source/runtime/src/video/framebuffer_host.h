#ifndef XBOX_FRAMEBUFFER_HOST_H
#define XBOX_FRAMEBUFFER_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(_WIN32)
typedef enum XboxFramebufferFormat {
    XBOX_FRAMEBUFFER_X8R8G8B8,
    XBOX_FRAMEBUFFER_RGB565
} XboxFramebufferFormat;

/* Open, pump and close on the process main thread. The host owns the guest
 * thread and must stop/join it before closing this window or guest memory.
 * Open returns 0 on failure; Pump returns 1 while open, 0 on a close request,
 * and -1 on a presentation failure. No game or renderer is started here. */
int xbox_FramebufferHostOpen(const char *title, unsigned width, unsigned height);
int xbox_FramebufferHostPump(unsigned timeout_ms);
void xbox_FramebufferHostClose(void);
int xbox_FramebufferHostMouseCapture(int capture);

/* Thread-safe copy into a bounded latest-frame handoff. Pixels remain owned
 * by the caller. Returns 0 for an invalid frame, allocation failure or when
 * the host window is not open. */
int xbox_FramebufferSubmit(const void *pixels, unsigned width, unsigned height,
                           size_t pitch, XboxFramebufferFormat format);

/* SDL keycode to Win32-compatible virtual key, zero if unsupported. */
int xbox_FramebufferSdlKeyToVk(int keycode);
#endif

#ifdef __cplusplus
}
#endif
#endif
