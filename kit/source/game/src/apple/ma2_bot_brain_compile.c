#include "../bots/ma2_bot_api.h"

/* Clang needs declarations for local helpers used before their definitions. */
static int kind_ok(int);
static int vtol_carrying(const Ma2World *, const Ma2PlayerView *, const Ma2Senses *);
static int cargo_mate(const Ma2World *, const Ma2PlayerView *);

#include "../bots/ma2_bot_brain.c"
