#include "host_shell.h"

#include <string.h>

int ma2_host_parse_options(int argc, char *const argv[], Ma2HostSettings *settings)
{
    if (!settings || argc < 1 || !argv) return MA2_OPTIONS_ERROR;
    *settings = (Ma2HostSettings){960, 540, 0, 0, 0};
    int help = 0;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return MA2_OPTIONS_ERROR;
        if (strcmp(argv[i], "--smoke") == 0) settings->smoke = 1;
        else if (strcmp(argv[i], "--safe-start") == 0) settings->safe_start = 1;
        else if (strcmp(argv[i], "--guest-init-only") == 0) settings->guest_init_only = 1;
        else if (strcmp(argv[i], "--help") == 0) help = 1;
        else return MA2_OPTIONS_ERROR;
    }
    return help ? MA2_OPTIONS_HELP : MA2_OPTIONS_RUN;
}
