#define RECOMP_GENERATED_CODE
#include <recomp_types.h>

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint16_t g_fp_control_word;

uint64_t alignment_probe(uint32_t address)
{
    MEM16(address) = UINT16_C(0x1234);
    uint16_t value16 = MEM16(address);
    MEM32(address) = UINT32_C(0x12345678);
    uint32_t value32 = MEM32(address);
    SMEM64(address) = INT64_C(0x123456789abcdef);
    int64_t value64 = SMEM64(address);
    MEMF(address) = 1.5f;
    float value_float = MEMF(address);
    MEMD(address) = 2.5;
    double value_double = MEMD(address);
    return value16 + value32 + (uint64_t)value64 +
           (uint64_t)value_float + (uint64_t)value_double;
}
