#include "host_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef MA2_GUEST_ENABLED
#define MA2_GUEST_ENABLED 0
#endif

#if MA2_GUEST_ENABLED
#include "guest_bootstrap.h"
#endif

static int synthetic_initialize(Ma2HostWorker *worker, void *argument)
{
    const Ma2HostSettings *settings = argument;
    const Ma2HostThreadState *state = ma2_host_worker_current();
    return !settings || !state || state->worker != worker || pthread_main_np() ||
        (settings->safe_start != 0 && settings->safe_start != 1) ? -1 : 0;
}

static int synthetic_wait(Ma2HostWorker *worker, void *argument)
{
    (void)argument;
    return ma2_host_worker_wait_for_stop(worker);
}

static int help(void)
{
#if MA2_GUEST_ENABLED
    static const char text[] =
        "MechAssault 2 native macOS host\n"
        "Usage: mechassault2_recomp_guest [--smoke] [--guest-init-only] [--safe-start] [--help]\n"
        "  --smoke            Open and pump a real SDL window without loading private game data.\n"
        "  --guest-init-only   Load the retail XBE, initialize every guest service, and tear down without entry.\n"
        "  --safe-start        Skip retail movie loops through their normal stop and close transition.\n"
        "Set MA2_GAME_DIR to the absolute extracted retail game directory for guest startup.\n";
#else
    static const char text[] =
        "MechAssault 2 native macOS host shell\n"
        "Usage: mechassault2_recomp [--smoke] [--safe-start] [--help]\n"
        "  --smoke       Open and pump a real SDL window, then stop and join the synthetic worker.\n"
        "  --safe-start  Request deterministic startup/cutscene skipping for the future guest path.\n"
        "Guest entry, movie skipping, and game rendering are not implemented; no gameplay runs.\n";
#endif
    return fputs(text, stdout) >= 0 && fflush(stdout) == 0;
}

int main(int argc, char **argv)
{
    static const char console_failure[] =
        "{\"level\":\"error\",\"event\":\"console_failed\",\"message\":\"Cannot open host diagnostics.\"}\n";
    Ma2HostLog log;
    if (!ma2_host_log_console_open(&log)) {
        (void)write(STDERR_FILENO, console_failure, sizeof(console_failure) - 1);
        return 1;
    }

    Ma2HostSettings settings;
    int options = ma2_host_parse_options(argc, argv, &settings);
    if (options != MA2_OPTIONS_RUN) {
        int ok = options == MA2_OPTIONS_HELP ? help() :
            ma2_host_log_event(&log, MA2_LOG_BAD_OPTIONS);
        if (!ma2_host_log_close(&log)) ok = 0;
        return options == MA2_OPTIONS_HELP && ok ? 0 : options == MA2_OPTIONS_ERROR ? 2 : 1;
    }
#if !MA2_GUEST_ENABLED
    if (settings.guest_init_only) {
        int ok = ma2_host_log_event(&log, MA2_LOG_BAD_OPTIONS);
        if (!ma2_host_log_close(&log)) ok = 0;
        return ok ? 2 : 1;
    }
#endif

    Ma2HostPaths paths = {.support_fd = -1, .caches_fd = -1, .logs_fd = -1};
    Ma2HostWindow window = {0};
    Ma2HostWorker worker;
    Ma2HostInstance instance = {.file_fd = -1, .session_port = MACH_PORT_NULL};
    int paths_open = 0, instance_locked = 0, signals_installed = 0;
    int worker_initialized = 0, worker_started = 0;
    int result = 0, ready = 0, short_run_finished = 0;
    unsigned pumps = 0;
#if MA2_GUEST_ENABLED
    Ma2Guest *guest = NULL;
    int guest_mode = !settings.smoke;
#else
    int guest_mode = 0;
#endif

    Ma2HostPathResult path_result = ma2_host_paths_open(&paths, getenv("HOME"));
    if (path_result != MA2_PATH_OK) {
        ma2_host_log_event(&log, path_result == MA2_PATH_INVALID_HOME ?
                           MA2_LOG_INVALID_HOME : MA2_LOG_PATH_FAILURE);
        result = 1;
        goto cleanup;
    }
    paths_open = 1;
    Ma2HostInstanceResult instance_result = ma2_host_instance_lock(&paths, &instance);
    if (instance_result != MA2_INSTANCE_OK) {
        ma2_host_log_event(&log, instance_result == MA2_INSTANCE_BUSY ?
                           MA2_LOG_INSTANCE_BUSY : MA2_LOG_INSTANCE_FAILURE);
        result = instance_result == MA2_INSTANCE_BUSY ? 3 : 1;
        goto cleanup;
    }
    instance_locked = 1;
    if (!ma2_host_log_file_open(&log, &paths)) {
        ma2_host_log_event(&log, MA2_LOG_LOG_FAILURE);
        result = 1;
        goto cleanup;
    }
    if (!ma2_host_log_event(&log, MA2_LOG_START) ||
        !ma2_host_log_event(&log, guest_mode ?
                            MA2_LOG_GUEST_SELECTED : MA2_LOG_NO_GUEST) ||
        (log.runtime_fd >= 0 &&
         !ma2_host_log_event(&log, MA2_LOG_PRIVATE_DIAGNOSTICS)) ||
        !ma2_host_log_event(&log, settings.safe_start ?
                            MA2_LOG_SAFE_START_ON : MA2_LOG_SAFE_START_OFF)) {
        result = 1;
        goto cleanup;
    }
    if (!ma2_host_log_mute_runtime_stderr(&log)) {
        ma2_host_log_event(&log, MA2_LOG_STDERR_FAILURE);
        result = 1;
        goto cleanup;
    }
    if (!ma2_host_signals_install(&log)) {
        ma2_host_log_event(&log, MA2_LOG_SIGNALS_FAILURE);
        result = 1;
        goto cleanup;
    }
    signals_installed = 1;
    Ma2HostPresentation presentation;
    if (ma2_host_window_open(&window, &settings, &presentation) != MA2_WINDOW_RUNNING) {
        ma2_host_log_event(&log, MA2_LOG_WINDOW_FAILURE);
        result = 1;
        goto cleanup;
    }
    if (!ma2_host_log_event(&log, presentation == MA2_PRESENT_SOFTWARE ?
                            MA2_LOG_PRESENT_SOFTWARE : MA2_LOG_PRESENT_ACCELERATED)) {
        result = 1;
        goto cleanup;
    }
    if (!ma2_host_worker_init(&worker)) {
        ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
        result = 1;
        goto cleanup;
    }
    worker_initialized = 1;
#if MA2_GUEST_ENABLED
    if (guest_mode) guest = ma2_guest_create(&settings, &paths, &log);
    if (guest_mode && !guest) {
        ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
        result = 1;
        goto cleanup;
    }
#endif
    Ma2HostWorkerCallback initialize = synthetic_initialize;
    Ma2HostWorkerCallback run = synthetic_wait;
    void *worker_argument = &settings;
#if MA2_GUEST_ENABLED
    if (guest_mode) {
        initialize = ma2_guest_initialize;
        run = ma2_guest_run;
        worker_argument = guest;
    }
#endif
    if (!ma2_host_worker_start(&worker, initialize, run, worker_argument)) {
        ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
        result = 1;
        goto cleanup;
    }
    worker_started = 1;

    uint64_t start, now;
    const uint64_t startup_timeout_ns =
        guest_mode ? UINT64_C(120000000000) : UINT64_C(3000000000);
    if (!ma2_host_monotonic_ns(&start)) {
        ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
        result = 1;
        goto cleanup;
    }
    for (;;) {
        Ma2HostWorkerSnapshot snapshot;
        if (!ma2_host_worker_snapshot(&worker, &snapshot)) {
            ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
            result = 1;
            break;
        }
        if (snapshot.state == MA2_WORKER_FINISHED) {
            if (guest_mode && ready) break;
            if (!guest_mode) ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
            result = 1;
            break;
        }
        if (!ready && snapshot.state == MA2_WORKER_RUNNING) {
            if (!ma2_host_log_event(&log, MA2_LOG_WORKER_STARTED) ||
                !ma2_host_log_event(&log, MA2_LOG_READY)) {
                result = 1;
                break;
            }
            ready = 1;
#if MA2_GUEST_ENABLED
            if (guest_mode) ma2_guest_release_entry(guest);
#endif
        }
        if (ma2_host_signal_stop_requested()) {
            ma2_host_log_event(&log, MA2_LOG_SIGNAL_STOP);
            if (settings.smoke) result = 1;
            break;
        }
        if (!ready && (!ma2_host_monotonic_ns(&now) || now < start ||
                       now - start >= startup_timeout_ns)) {
            ma2_host_log_event(&log, MA2_LOG_WORKER_TIMEOUT);
            result = 1;
            break;
        }
        Ma2HostWindowResult pump = ma2_host_window_pump(&window, 20);
        if (pump == MA2_WINDOW_CLOSED) {
            if (settings.smoke) result = 1;
            break;
        }
        if (pump != MA2_WINDOW_RUNNING) {
            ma2_host_log_event(&log, MA2_LOG_WINDOW_FAILURE);
            result = 1;
            break;
        }
        ++pumps;
        if ((settings.smoke || settings.guest_init_only) &&
            ready && pumps >= 3) {
            short_run_finished = 1;
            break;
        }
    }

cleanup:
    if (worker_started) {
        ma2_host_log_event(&log, MA2_LOG_STOP_REQUESTED);
        if (!ma2_host_worker_stop(&worker) || !ma2_host_worker_join(&worker)) {
            ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
            /* The worker may still touch the window or other resources. */
            _Exit(1);
        }
        Ma2HostWorkerSnapshot snapshot;
        if (!ma2_host_worker_snapshot(&worker, &snapshot) ||
            snapshot.state != MA2_WORKER_JOINED || snapshot.initialization_result != 0 ||
            snapshot.callback_result != 0) {
            if (!guest_mode) ma2_host_log_event(&log, MA2_LOG_WORKER_FAILURE);
            result = 1;
        }
        if (!ma2_host_log_event(&log, MA2_LOG_WORKER_JOINED)) result = 1;
    }
    if (worker_initialized && !ma2_host_worker_destroy(&worker)) {
        ma2_host_log_event(&log, MA2_LOG_CLEANUP_FAILURE);
        result = 1;
    }
#if MA2_GUEST_ENABLED
    ma2_guest_destroy(guest);
#endif
    if (window.open) {
        if (ma2_host_window_close(&window) != MA2_WINDOW_CLOSED) {
            ma2_host_log_event(&log, MA2_LOG_CLEANUP_FAILURE);
            result = 1;
        } else if (!ma2_host_log_event(&log, MA2_LOG_WINDOW_CLOSED)) {
            result = 1;
        }
    }
    if (signals_installed && !ma2_host_signals_restore()) {
        ma2_host_log_event(&log, MA2_LOG_SIGNALS_RESTORE_FAILURE);
        result = 1;
    }
    if (instance_locked && !ma2_host_instance_unlock(&instance)) {
        ma2_host_log_event(&log, MA2_LOG_CLEANUP_FAILURE);
        result = 1;
    }
    if (paths_open && !ma2_host_paths_close(&paths)) {
        ma2_host_log_event(&log, MA2_LOG_CLEANUP_FAILURE);
        result = 1;
    }
    if (settings.smoke && short_run_finished && result == 0 &&
        !ma2_host_log_event(&log, MA2_LOG_SMOKE_PASSED))
        result = 1;
    if (!ma2_host_log_event(&log, MA2_LOG_EXIT)) result = 1;
    if (!ma2_host_log_restore_stderr(&log)) {
        ma2_host_log_event(&log, MA2_LOG_STDERR_FAILURE);
        result = 1;
    }
    if (!ma2_host_log_close(&log)) result = 1;
    return result;
}
