#ifndef MA2_APPLE_RECOMP_INTRIN_H
#define MA2_APPLE_RECOMP_INTRIN_H

/* MSVC's compiler barrier, used by the game-side environment cache. */
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")

#endif
