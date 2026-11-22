#ifndef XBOX_HOST_CPU_H
#define XBOX_HOST_CPU_H

#if defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_ARM64)
static inline void xbox_host_cpu_relax(void) { __yield(); }
#else
static inline void xbox_host_cpu_relax(void) { _mm_pause(); }
#endif
#elif defined(__aarch64__)
static inline void xbox_host_cpu_relax(void) { __asm__ volatile("yield"); }
#elif defined(__i386__) || defined(__x86_64__)
static inline void xbox_host_cpu_relax(void) { __asm__ volatile("pause"); }
#else
static inline void xbox_host_cpu_relax(void) { }
#endif

#endif
