#include "platform/pc_frontend.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    CHECK(unsetenv("RECOMP_SAFE_START") == 0);
    CHECK(pc_input_movie_skip_pending(0) == 0);

    CHECK(setenv("RECOMP_SAFE_START", "1", 1) == 0);
    CHECK(pc_input_movie_skip_pending(0) == 1);
    CHECK(pc_input_movie_skip_pending(1) == 1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    CHECK(pc_input_movie_skip_pending(1) == 0);

    pc_input_movie_skip_rearm();
    CHECK(pc_input_movie_skip_pending(1) == 1);
    pc_display_movie(1);
    CHECK(pc_display_movie_active() == 1);
    CHECK(pc_input_movie_skip_pending(1) == 1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_display_movie(1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_display_movie(-1);
    CHECK(pc_display_movie_active() == 1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_display_movie(-1);
    CHECK(pc_display_movie_active() == 0);

    pc_display_movie(1);
    CHECK(pc_input_movie_skip_pending(1) == 1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_display_movie(-1);

    CHECK(setenv("RECOMP_SAFE_START", "0", 1) == 0);
    CHECK(pc_input_movie_skip_pending(0) == 0);

    pc_display_movie(1);
    pc_input_movie_skip();
    CHECK(pc_input_movie_skip_pending(0) == 1);
    CHECK(pc_input_movie_skip_pending(1) == 1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_display_movie(-1);

    pc_display_movie(1);
    CHECK(pc_input_movie_skip_pending(0) == 0);
    pc_input_movie_skip();
    CHECK(pc_input_movie_skip_pending(1) == 1);
    pc_display_movie(-1);
    return 0;
}
