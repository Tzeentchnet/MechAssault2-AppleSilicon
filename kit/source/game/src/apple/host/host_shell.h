#ifndef MA2_APPLE_HOST_SHELL_H
#define MA2_APPLE_HOST_SHELL_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <mach/mach.h>

#define MA2_HOST_PATH_CAP 1024

typedef struct Ma2HostSettings {
    unsigned width;
    unsigned height;
    int safe_start;
    int smoke;
    int guest_init_only;
} Ma2HostSettings;

enum {
    MA2_OPTIONS_ERROR = -1,
    MA2_OPTIONS_RUN = 0,
    MA2_OPTIONS_HELP = 1
};

int ma2_host_parse_options(int argc, char *const argv[], Ma2HostSettings *settings);
int ma2_host_monotonic_ns(uint64_t *value);

typedef struct Ma2HostPaths {
    char support[MA2_HOST_PATH_CAP];
    char caches[MA2_HOST_PATH_CAP];
    char logs[MA2_HOST_PATH_CAP];
    int support_fd;
    int caches_fd;
    int logs_fd;
} Ma2HostPaths;

typedef enum Ma2HostPathResult {
    MA2_PATH_OK,
    MA2_PATH_INVALID_HOME,
    MA2_PATH_UNAVAILABLE
} Ma2HostPathResult;

Ma2HostPathResult ma2_host_paths_open(Ma2HostPaths *paths, const char *home);
int ma2_host_paths_close(Ma2HostPaths *paths);

typedef enum Ma2HostInstanceResult {
    MA2_INSTANCE_OK,
    MA2_INSTANCE_BUSY,
    MA2_INSTANCE_ERROR
} Ma2HostInstanceResult;

typedef struct Ma2HostInstance {
    int file_fd;
    mach_port_t session_port;
} Ma2HostInstance;

Ma2HostInstanceResult ma2_host_instance_lock(const Ma2HostPaths *paths,
                                             Ma2HostInstance *instance);
int ma2_host_instance_unlock(Ma2HostInstance *instance);

typedef enum Ma2HostLogEvent {
    MA2_LOG_BAD_OPTIONS,
    MA2_LOG_START,
    MA2_LOG_NO_GUEST,
    MA2_LOG_GUEST_SELECTED,
    MA2_LOG_GUEST_OPTIONAL_UNAVAILABLE,
    MA2_LOG_SAFE_START_ON,
    MA2_LOG_SAFE_START_OFF,
    MA2_LOG_INVALID_HOME,
    MA2_LOG_PATH_FAILURE,
    MA2_LOG_INSTANCE_BUSY,
    MA2_LOG_INSTANCE_FAILURE,
    MA2_LOG_LOG_FAILURE,
    MA2_LOG_PRIVATE_DIAGNOSTICS,
    MA2_LOG_STDERR_FAILURE,
    MA2_LOG_SIGNALS_FAILURE,
    MA2_LOG_SIGNALS_RESTORE_FAILURE,
    MA2_LOG_WINDOW_FAILURE,
    MA2_LOG_PRESENT_SOFTWARE,
    MA2_LOG_PRESENT_ACCELERATED,
    MA2_LOG_WORKER_FAILURE,
    MA2_LOG_WORKER_TIMEOUT,
    MA2_LOG_WORKER_STARTED,
    MA2_LOG_READY,
    MA2_LOG_SIGNAL_STOP,
    MA2_LOG_STOP_REQUESTED,
    MA2_LOG_WORKER_JOINED,
    MA2_LOG_GUEST_INPUT_FAILURE,
    MA2_LOG_GUEST_SIGNAL_FAILURE,
    MA2_LOG_GUEST_XBE_READY,
    MA2_LOG_GUEST_MMIO_READY,
    MA2_LOG_GUEST_MEMORY_FAILURE,
    MA2_LOG_GUEST_FENCE_FAILURE,
    MA2_LOG_GUEST_APU_FAILURE,
    MA2_LOG_GUEST_MEMORY_READY,
    MA2_LOG_GUEST_METAL_READY,
    MA2_LOG_GUEST_METAL_CPU,
    MA2_LOG_GUEST_METAL_FAILURE,
    MA2_LOG_GUEST_BACKEND_INVALID,
    MA2_LOG_GUEST_XMM_FAILURE,
    MA2_LOG_GUEST_OHCI_FAILURE,
    MA2_LOG_GUEST_REGISTER_FAILURE,
    MA2_LOG_GUEST_KERNEL_READY,
    MA2_LOG_GUEST_INIT_FAILED,
    MA2_LOG_GUEST_INIT_PASSED,
    MA2_LOG_GUEST_ENTRY,
    MA2_LOG_GUEST_REQUESTED_EXIT,
    MA2_LOG_GUEST_RETURNED,
    MA2_LOG_GUEST_CLEANUP_FAILED,
    MA2_LOG_GUEST_CLEANUP_PASSED,
    MA2_LOG_WINDOW_CLOSED,
    MA2_LOG_CLEANUP_FAILURE,
    MA2_LOG_SMOKE_PASSED,
    MA2_LOG_EXIT
} Ma2HostLogEvent;

typedef struct Ma2HostLog {
    int console_fd;
    int file_fd;
    int runtime_fd;
    int stderr_muted;
    int failed;
} Ma2HostLog;

int ma2_host_log_console_open(Ma2HostLog *log);
int ma2_host_log_file_open(Ma2HostLog *log, const Ma2HostPaths *paths);
int ma2_host_log_mute_runtime_stderr(Ma2HostLog *log);
int ma2_host_log_restore_stderr(Ma2HostLog *log);
int ma2_host_log_event(Ma2HostLog *log, Ma2HostLogEvent event);
int ma2_host_log_close(Ma2HostLog *log);

int ma2_host_signals_install(const Ma2HostLog *log);
int ma2_host_signals_restore(void);
int ma2_host_signal_stop_requested(void);

typedef enum Ma2HostWindowResult {
    MA2_WINDOW_ERROR = -1,
    MA2_WINDOW_WRONG_THREAD = -2,
    MA2_WINDOW_CLOSED = 0,
    MA2_WINDOW_RUNNING = 1
} Ma2HostWindowResult;

typedef enum Ma2HostPresentation {
    MA2_PRESENT_SOFTWARE,
    MA2_PRESENT_ACCELERATED
} Ma2HostPresentation;

typedef struct Ma2HostWindow {
    int open;
    unsigned sdl_before;
} Ma2HostWindow;

Ma2HostWindowResult ma2_host_window_open(Ma2HostWindow *window,
                                         const Ma2HostSettings *settings,
                                         Ma2HostPresentation *presentation);
Ma2HostWindowResult ma2_host_window_pump(Ma2HostWindow *window, unsigned timeout_ms);
Ma2HostWindowResult ma2_host_window_close(Ma2HostWindow *window);

struct Ma2HostWorker;
typedef int (*Ma2HostWorkerCallback)(struct Ma2HostWorker *worker, void *argument);

typedef enum Ma2HostWorkerState {
    MA2_WORKER_INITIALIZED,
    MA2_WORKER_STARTING,
    MA2_WORKER_RUNNING,
    MA2_WORKER_FINISHED,
    MA2_WORKER_JOINED
} Ma2HostWorkerState;

typedef struct Ma2HostThreadState {
    struct Ma2HostWorker *worker;
    uint64_t started_ns;
} Ma2HostThreadState;

typedef struct Ma2HostWorkerSnapshot {
    Ma2HostWorkerState state;
    int stop_requested;
    int initialization_result;
    int callback_result;
} Ma2HostWorkerSnapshot;

typedef struct Ma2HostWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Ma2HostWorkerState state;
    int initialized;
    int stop_requested;
    int initialization_result;
    int callback_result;
    Ma2HostWorkerCallback initialize;
    Ma2HostWorkerCallback callback;
    void *argument;
} Ma2HostWorker;

int ma2_host_worker_init(Ma2HostWorker *worker);
int ma2_host_worker_start(Ma2HostWorker *worker, Ma2HostWorkerCallback initialize,
                          Ma2HostWorkerCallback callback, void *argument);
const Ma2HostThreadState *ma2_host_worker_current(void);
int ma2_host_worker_snapshot(Ma2HostWorker *worker, Ma2HostWorkerSnapshot *snapshot);
int ma2_host_worker_stop(Ma2HostWorker *worker);
int ma2_host_worker_wait_for_stop(Ma2HostWorker *worker);
int ma2_host_worker_join(Ma2HostWorker *worker);
int ma2_host_worker_destroy(Ma2HostWorker *worker);

#endif
