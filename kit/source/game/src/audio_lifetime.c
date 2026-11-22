#include "audio_lifetime.h"
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;
static volatile LONG *references(uint32_t object)
{
    return (volatile LONG *)(g_xbox_mem_offset+object+4);
}

/* Hardware completion and the game's Release can run on different host
 * threads. A queued completion owns one reference until its callback returns.
 * Never resurrect an object whose final Release has already started. */
int ma2_audio_deferred_retain(uint32_t object)
{
    volatile LONG *count=references(object);
    LONG current=InterlockedCompareExchange(count,0,0);
    while(current>0 && current<INT32_MAX) {
        LONG seen=InterlockedCompareExchange(count,current+1,current);
        if(seen==current)return 1;
        current=seen;
    }
    return 0;
}
uint32_t ma2_audio_ref_add(uint32_t object)
{
    return (uint32_t)InterlockedIncrement(references(object));
}
uint32_t ma2_audio_ref_drop(uint32_t object)
{
    return (uint32_t)InterlockedDecrement(references(object));
}
