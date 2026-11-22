#include "host_shell.h"

#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "host lifecycle check failed at line %d: %s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)

typedef struct Probe {
    Ma2HostWindow *window;
    atomic_int entered;
    atomic_int active;
    atomic_int exited;
    atomic_int release;
    atomic_int callback_count;
    const Ma2HostThreadState *initialized_state;
} Probe;

static _Thread_local int probe_tls;

static int wait_for_value(atomic_int *value, int expected, uint64_t timeout_ns)
{
    uint64_t start, now;
    if (!ma2_host_monotonic_ns(&start)) return 0;
    do {
        if (atomic_load(value) == expected) return 1;
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
        if (!ma2_host_monotonic_ns(&now) || now < start) return 0;
    } while (now - start < timeout_ns);
    return 0;
}

static int initialize_probe(Ma2HostWorker *worker, void *argument)
{
    Probe *probe = argument;
    const Ma2HostThreadState *state = ma2_host_worker_current();
    if (!state || state->worker != worker || pthread_main_np()) return 11;
    probe_tls = 417;
    probe->initialized_state = state;
    return 0;
}

static int run_probe(Ma2HostWorker *worker, void *argument)
{
    Probe *probe = argument;
    Ma2HostSettings settings = {64, 48, 0, 0};
    Ma2HostPresentation presentation;
    const Ma2HostThreadState *state = ma2_host_worker_current();
    int ok = state && state == probe->initialized_state && state->worker == worker &&
        probe_tls == 417 && !pthread_main_np() &&
        ma2_host_window_open(probe->window, &settings, &presentation) == MA2_WINDOW_WRONG_THREAD &&
        ma2_host_window_pump(probe->window, 0) == MA2_WINDOW_WRONG_THREAD &&
        ma2_host_window_close(probe->window) == MA2_WINDOW_WRONG_THREAD;
    atomic_store(&probe->active, 1);
    atomic_store(&probe->entered, 1);
    if (ma2_host_worker_wait_for_stop(worker) != 0) ok = 0;
    if (ma2_host_worker_current() != state || probe_tls != 417) ok = 0;
    atomic_store(&probe->active, 0);
    atomic_store(&probe->exited, 1);
    return ok ? 0 : 12;
}

static int fail_initialize(Ma2HostWorker *worker, void *argument)
{
    (void)worker;
    (void)argument;
    return 37;
}

static int count_callback(Ma2HostWorker *worker, void *argument)
{
    (void)worker;
    Probe *probe = argument;
    atomic_fetch_add(&probe->callback_count, 1);
    return 0;
}

static int delayed_stop_callback(Ma2HostWorker *worker, void *argument)
{
    Probe *probe = argument;
    atomic_store(&probe->active, 1);
    atomic_store(&probe->entered, 1);
    if (ma2_host_worker_wait_for_stop(worker) != 0) return 21;
    while (!atomic_load(&probe->release)) {
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
    atomic_store(&probe->active, 0);
    return 0;
}

static int gated_initialize(Ma2HostWorker *worker, void *argument)
{
    Probe *probe = argument;
    if (!ma2_host_worker_current() || ma2_host_worker_current()->worker != worker) return 31;
    atomic_store(&probe->entered, 1);
    while (!atomic_load(&probe->release)) {
        struct timespec delay = {0, 1000000L};
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int early_stop_callback(Ma2HostWorker *worker, void *argument)
{
    Probe *probe = argument;
    Ma2HostWorkerSnapshot snapshot;
    if (!ma2_host_worker_snapshot(worker, &snapshot) || !snapshot.stop_requested) return 32;
    atomic_fetch_add(&probe->callback_count, 1);
    return ma2_host_worker_wait_for_stop(worker);
}

static int test_options(void)
{
    Ma2HostSettings settings;
    char *defaults[] = {"host", NULL};
    char *safe[] = {"host", "--safe-start", "--smoke", NULL};
    char *help[] = {"host", "--help", NULL};
    char *bad[] = {"host", "--private-path-argument", NULL};
    CHECK(ma2_host_parse_options(1, defaults, &settings) == MA2_OPTIONS_RUN);
    CHECK(settings.width == 960 && settings.height == 540 && !settings.safe_start && !settings.smoke);
    CHECK(ma2_host_parse_options(3, safe, &settings) == MA2_OPTIONS_RUN);
    CHECK(settings.width == 960 && settings.height == 540 && settings.safe_start && settings.smoke);
    CHECK(ma2_host_parse_options(2, help, &settings) == MA2_OPTIONS_HELP);
    CHECK(ma2_host_parse_options(2, bad, &settings) == MA2_OPTIONS_ERROR);
    return 0;
}

int main(void)
{
    CHECK(pthread_main_np());
    CHECK(test_options() == 0);
    uint64_t before, after;
    CHECK(ma2_host_monotonic_ns(&before));
    struct timespec delay = {0, 2000000L};
    nanosleep(&delay, NULL);
    CHECK(ma2_host_monotonic_ns(&after) && after >= before);
    CHECK(ma2_host_worker_current() == NULL);

    unsigned sdl_before = SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    Ma2HostSettings settings = {64, 48, 0, 1};
    Ma2HostPresentation presentation;
    Ma2HostWindow window = {0};
    CHECK(ma2_host_window_open(&window, &settings, &presentation) == MA2_WINDOW_RUNNING);
    CHECK(presentation == MA2_PRESENT_SOFTWARE || presentation == MA2_PRESENT_ACCELERATED);
    CHECK(ma2_host_window_pump(&window, 1) == MA2_WINDOW_RUNNING);

    Probe probe = {.window = &window};
    Ma2HostWorker worker;
    Ma2HostWorkerSnapshot snapshot;
    CHECK(ma2_host_worker_init(&worker));
    CHECK(!ma2_host_worker_join(&worker));
    CHECK(!ma2_host_worker_start(&worker, NULL, NULL, NULL));
    CHECK(ma2_host_worker_start(&worker, initialize_probe, run_probe, &probe));
    CHECK(wait_for_value(&probe.entered, 1, UINT64_C(2000000000)));
    CHECK(ma2_host_worker_snapshot(&worker, &snapshot));
    CHECK(snapshot.state == MA2_WORKER_RUNNING && !snapshot.stop_requested);
    CHECK(!ma2_host_worker_destroy(&worker));
    CHECK(ma2_host_worker_wait_for_stop(&worker) != 0);
    CHECK(ma2_host_window_pump(&window, 1) == MA2_WINDOW_RUNNING);
    CHECK(ma2_host_worker_stop(&worker));
    CHECK(ma2_host_worker_stop(&worker));
    CHECK(ma2_host_worker_join(&worker));
    CHECK(ma2_host_worker_snapshot(&worker, &snapshot));
    CHECK(snapshot.state == MA2_WORKER_JOINED && snapshot.stop_requested &&
          snapshot.initialization_result == 0 && snapshot.callback_result == 0);
    CHECK(atomic_load(&probe.exited) == 1 && atomic_load(&probe.active) == 0);
    CHECK(!ma2_host_worker_join(&worker));
    CHECK(ma2_host_worker_destroy(&worker));
    CHECK(ma2_host_worker_current() == NULL);
    CHECK(ma2_host_window_pump(&window, 1) == MA2_WINDOW_RUNNING);
    CHECK(ma2_host_window_close(&window) == MA2_WINDOW_CLOSED);
    CHECK(ma2_host_window_close(&window) == MA2_WINDOW_ERROR);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == sdl_before);

    Probe failed = {0};
    CHECK(ma2_host_worker_init(&worker));
    CHECK(ma2_host_worker_start(&worker, fail_initialize, count_callback, &failed));
    CHECK(ma2_host_worker_join(&worker));
    CHECK(ma2_host_worker_snapshot(&worker, &snapshot));
    CHECK(snapshot.state == MA2_WORKER_JOINED && snapshot.initialization_result == 37 &&
          atomic_load(&failed.callback_count) == 0);
    CHECK(ma2_host_worker_destroy(&worker));

    Probe early = {0};
    CHECK(ma2_host_worker_init(&worker));
    CHECK(ma2_host_worker_start(&worker, gated_initialize, early_stop_callback, &early));
    CHECK(wait_for_value(&early.entered, 1, UINT64_C(2000000000)));
    CHECK(ma2_host_worker_stop(&worker));
    atomic_store(&early.release, 1);
    CHECK(ma2_host_worker_join(&worker));
    CHECK(ma2_host_worker_snapshot(&worker, &snapshot));
    CHECK(snapshot.initialization_result == 0 && snapshot.callback_result == 0 &&
          atomic_load(&early.callback_count) == 1);
    CHECK(ma2_host_worker_destroy(&worker));

    Probe slow = {0};
    CHECK(ma2_host_worker_init(&worker));
    CHECK(ma2_host_worker_start(&worker, NULL, delayed_stop_callback, &slow));
    CHECK(wait_for_value(&slow.entered, 1, UINT64_C(2000000000)));
    CHECK(ma2_host_worker_stop(&worker));
    CHECK(ma2_host_monotonic_ns(&before));
    CHECK(!ma2_host_worker_join(&worker));
    CHECK(ma2_host_monotonic_ns(&after) && after >= before &&
          after - before >= UINT64_C(4000000000));
    CHECK(!ma2_host_worker_destroy(&worker) && atomic_load(&slow.active) == 1);
    atomic_store(&slow.release, 1);
    CHECK(ma2_host_worker_join(&worker));
    CHECK(atomic_load(&slow.active) == 0);
    CHECK(ma2_host_worker_destroy(&worker));
    puts("Host lifecycle: main-thread window, worker TLS, stop, join and failure paths passed");
    return 0;
}
