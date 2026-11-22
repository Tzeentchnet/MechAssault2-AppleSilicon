#ifndef MA2_ABI_TEST_CHECK_H
#define MA2_ABI_TEST_CHECK_H

#include <stdio.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "ABI-CHECK line %d: %s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)

#endif
