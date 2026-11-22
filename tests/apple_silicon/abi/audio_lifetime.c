#include "platform/win32_compat.h"
#include "check.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_lifetime.h"

ptrdiff_t g_xbox_mem_offset;

int main(void)
{
    _Alignas(16) uint8_t memory[64] = {0};
    const uint32_t object = 4;
    volatile LONG *references = (volatile LONG *)(memory + object + 4);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    *references = 1;
    CHECK(ma2_audio_deferred_retain(object) == 1 && *references == 2);
    *references = 0;
    CHECK(ma2_audio_deferred_retain(object) == 0 && *references == 0);
    *references = INT32_MAX;
    CHECK(ma2_audio_deferred_retain(object) == 0 && *references == INT32_MAX);
    return 0;
}
