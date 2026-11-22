#ifndef MA2_APPLE_RECOMP_COMPAT_H
#define MA2_APPLE_RECOMP_COMPAT_H

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "This compatibility header is only for the macOS arm64 compile-only graph"
#endif

#include <stdint.h>
#include "abi/arm64_helpers.h"

/* Implemented by the runtime and game hooks; this header does not stub them. */
#ifdef __cplusplus
extern "C" {
#endif
int _stricmp(const char *left, const char *right);
int ma2_bot_ai_select(uint32_t ai, uint32_t *out);
#ifdef __cplusplus
}
#endif

/* Preserve the generated/manual TLS declarations rather than letting Clang
 * ignore MSVC's thread attribute. Unknown declspecs must remain errors. */
#define MA2_APPLE_DECLSPEC_thread __thread
#define MA2_APPLE_DECLSPEC_dllimport
#define __declspec(attribute) MA2_APPLE_DECLSPEC_##attribute
#ifndef __stdcall
#define __stdcall
#endif

static inline int32_t ma2_apple_interlocked_and(volatile int32_t *value, int32_t mask)
{
    return __atomic_fetch_and(value, mask, __ATOMIC_SEQ_CST);
}
#define InterlockedAnd(value, mask) ma2_apple_interlocked_and((value), (mask))
#define YieldProcessor() __asm__ __volatile__("yield" ::: "memory")

#endif
