#ifndef MA2_APPLE_GUEST_BOOTSTRAP_H
#define MA2_APPLE_GUEST_BOOTSTRAP_H

#include "host_shell.h"

typedef struct Ma2Guest Ma2Guest;

Ma2Guest *ma2_guest_create(const Ma2HostSettings *settings,
                           const Ma2HostPaths *paths, Ma2HostLog *log);
void ma2_guest_destroy(Ma2Guest *guest);
void ma2_guest_release_entry(Ma2Guest *guest);
int ma2_guest_initialize(Ma2HostWorker *worker, void *argument);
int ma2_guest_run(Ma2HostWorker *worker, void *argument);

#endif
