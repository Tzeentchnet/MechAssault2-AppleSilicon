#include "platform/win32_compat.h"
#include <cstdint>

extern "C" int ma2_bot_ai_select(std::uint32_t value, std::uint32_t *out)
{
    *out = value + 1;
    return 1;
}

int main()
{
    std::uint32_t result = 0;
    return _stricmp("AbC", "aBc") != 0 ||
           !ma2_bot_ai_select(40, &result) || result != 41;
}
