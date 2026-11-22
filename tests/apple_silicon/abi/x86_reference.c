#include "check.h"

#if defined(__x86_64__)
#include <limits.h>
#include <math.h>
#include <mmintrin.h>
#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>

int main(void)
{
    uint16_t original, downward;
    double input = -2.5, rounded;
    __asm__ volatile("fnstcw %0" : "=m"(original));
    downward = (uint16_t)((original & ~0x0c00u) | 0x0400u);
    __asm__ volatile("fldcw %0" : : "m"(downward));
    __asm__ volatile("fldl %1; frndint; fstpl %0" : "=m"(rounded) : "m"(input));
    __asm__ volatile("fldcw %0" : : "m"(original));
    CHECK(rounded == -3.0);

    double unordered = NAN, one = 1.0;
    unsigned char parity;
    __asm__ volatile("fldl %2; fldl %1; fucomip %%st(1), %%st; "
                     "setp %0; fstp %%st(0)"
                     : "=qm"(parity) : "m"(unordered), "m"(one) : "cc");
    CHECK(parity == 1);

    float result[4];
    __m128 a = _mm_setr_ps(3.0f, -0.0f, NAN, 8.0f);
    __m128 b = _mm_setr_ps(4.0f, 0.0f, 7.0f, NAN);
    _mm_storeu_ps(result, _mm_min_ps(a, b));
    CHECK(result[0] == 3.0f && !signbit(result[1]) &&
          result[2] == 7.0f && isnan(result[3]));

    int16_t lanes[4] = {INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN};
    __m64 packed, product;
    uint64_t bits;
    memcpy(&packed, lanes, sizeof(packed));
    product = _mm_madd_pi16(packed, packed);
    memcpy(&bits, &product, sizeof(bits));
    _mm_empty();
    CHECK(bits == UINT64_C(0x8000000080000000));

    uint32_t sum = UINT32_MAX;
    unsigned char carry;
    __asm__ volatile("addl $1, %0; setc %1" : "+r"(sum), "=qm"(carry) : : "cc");
    CHECK(sum == 0 && carry == 1);
    return 0;
}
#else
int main(void) { return 0; }
#endif
