#ifndef MA2_APPLE_ARM64_ABI_HELPERS_H
#define MA2_APPLE_ARM64_ABI_HELPERS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The guest ABI adapter requires a little-endian host"
#endif

typedef uint16_t ma2_guest_u16 __attribute__((__aligned__(1), __may_alias__));
typedef uint32_t ma2_guest_u32 __attribute__((__aligned__(1), __may_alias__));
typedef int16_t ma2_guest_i16 __attribute__((__aligned__(1), __may_alias__));
typedef int32_t ma2_guest_i32 __attribute__((__aligned__(1), __may_alias__));
typedef int64_t ma2_guest_i64 __attribute__((__aligned__(1), __may_alias__));
typedef float ma2_guest_f32 __attribute__((__aligned__(1), __may_alias__));
typedef double ma2_guest_f64 __attribute__((__aligned__(1), __may_alias__));

static inline int ma2_abi_fcmp(double left, double right)
{
    return isnan(left) || isnan(right) ? 2 : left < right ? -1 : left > right ? 1 : 0;
}

static inline int ma2_abi_stack_returned(uint32_t before, uint32_t after)
{
    uint32_t popped = after - before;
    return popped >= 4u && popped <= INT32_MAX;
}

static inline void ma2_abi_record_icall(volatile uint32_t *trace,
                                        volatile uint32_t *index,
                                        volatile uint64_t *count,
                                        uint32_t mask, uint32_t target)
{
    uint32_t slot = __atomic_fetch_add(index, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&trace[slot & mask], target, __ATOMIC_RELAXED);
    __atomic_fetch_add(count, 1u, __ATOMIC_RELAXED);
}

/* PC=24 changes significand precision, not the x87 exponent range. */
static inline double ma2_abi_fp_round24(double value, uint16_t control)
{
    uint64_t bits, magnitude, fraction, mask, remainder, halfway;
    unsigned drop, rounding;
    int increment = 0;
    memcpy(&bits, &value, sizeof(bits));
    magnitude = bits & UINT64_C(0x7fffffffffffffff);
    if (magnitude >= UINT64_C(0x7ff0000000000000) || magnitude == 0)
        return value;
    if (magnitude < UINT64_C(0x0010000000000000)) {
        fraction = magnitude;
        unsigned highest = 63u - (unsigned)__builtin_clzll(fraction);
        if (highest <= 23u)
            return value;
        drop = highest - 23u;
    } else {
        drop = 29u;
    }
    mask = (UINT64_C(1) << drop) - 1u;
    remainder = magnitude & mask;
    halfway = UINT64_C(1) << (drop - 1u);
    rounding = (control >> 10) & 3u;
    if (rounding == 0)
        increment = remainder > halfway ||
                    (remainder == halfway && ((magnitude >> drop) & 1u));
    else if (rounding == 1)
        increment = (bits >> 63) && remainder;
    else if (rounding == 2)
        increment = !(bits >> 63) && remainder;
    magnitude &= ~mask;
    if (increment)
        magnitude += UINT64_C(1) << drop;
    bits = (bits & UINT64_C(0x8000000000000000)) | magnitude;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

#endif
