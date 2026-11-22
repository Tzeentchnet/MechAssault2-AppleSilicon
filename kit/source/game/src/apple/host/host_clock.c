#include "host_shell.h"

#include <time.h>

int ma2_host_monotonic_ns(uint64_t *value)
{
    struct timespec now;
    if (!value || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - (uint64_t)now.tv_nsec) / 1000000000u)
        return 0;
    *value = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
    return 1;
}
