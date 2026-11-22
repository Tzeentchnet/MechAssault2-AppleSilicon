#include "host_shell.h"
#include "host_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Log records are selected from this fixed vocabulary, never from argv, paths,
 * SDL error strings, signal context, or other untrusted text. */
static const char *const records[] = {
    [MA2_LOG_BAD_OPTIONS] =
        "{\"level\":\"error\",\"event\":\"bad_options\",\"message\":\"Unknown or invalid option; use --help.\"}\n",
    [MA2_LOG_START] =
        "{\"level\":\"info\",\"event\":\"host_start\",\"message\":\"Starting the native macOS host shell.\"}\n",
    [MA2_LOG_NO_GUEST] =
        "{\"level\":\"warning\",\"event\":\"guest_unavailable\",\"message\":\"Guest entry and game rendering are not implemented; no gameplay will run.\"}\n",
    [MA2_LOG_GUEST_SELECTED] =
        "{\"level\":\"info\",\"event\":\"guest_selected\",\"message\":\"Private generated guest startup is selected.\"}\n",
    [MA2_LOG_GUEST_OPTIONAL_UNAVAILABLE] =
        "{\"level\":\"warning\",\"event\":\"guest_optional_unavailable\",\"message\":\"Native movie decoding, downstream settings, offline bots, and custom scenario/editor integration are disabled for this bring-up milestone.\"}\n",
    [MA2_LOG_SAFE_START_ON] =
        "{\"level\":\"info\",\"event\":\"safe_start_on\",\"message\":\"Safe-start is enabled; retail movie loops will take their normal stop and close transition.\"}\n",
    [MA2_LOG_SAFE_START_OFF] =
        "{\"level\":\"info\",\"event\":\"safe_start_off\",\"message\":\"Safe-start is disabled.\"}\n",
    [MA2_LOG_INVALID_HOME] =
        "{\"level\":\"error\",\"event\":\"invalid_home\",\"message\":\"Set HOME to an existing absolute user directory with a bounded path.\"}\n",
    [MA2_LOG_PATH_FAILURE] =
        "{\"level\":\"error\",\"event\":\"paths_unavailable\",\"message\":\"Cannot create or open the user Library directories; check ownership, permissions and symlinks.\"}\n",
    [MA2_LOG_INSTANCE_BUSY] =
        "{\"level\":\"error\",\"event\":\"instance_busy\",\"message\":\"Another host instance is running for this user; close it and retry.\"}\n",
    [MA2_LOG_INSTANCE_FAILURE] =
        "{\"level\":\"error\",\"event\":\"instance_lock_failed\",\"message\":\"Cannot acquire the instance lock; check the macOS login session and Application Support permissions and symlinks.\"}\n",
    [MA2_LOG_LOG_FAILURE] =
        "{\"level\":\"error\",\"event\":\"log_failed\",\"message\":\"Cannot write the private host log; check Logs permissions and symlinks.\"}\n",
    [MA2_LOG_PRIVATE_DIAGNOSTICS] =
        "{\"level\":\"warning\",\"event\":\"private_diagnostics\",\"message\":\"Owner-only runtime diagnostics are enabled; the separate runtime log can contain retail paths and generated symbols and must not be shared.\"}\n",
    [MA2_LOG_STDERR_FAILURE] =
        "{\"level\":\"error\",\"event\":\"stderr_failed\",\"message\":\"Cannot protect runtime diagnostics from exposing private paths.\"}\n",
    [MA2_LOG_SIGNALS_FAILURE] =
        "{\"level\":\"error\",\"event\":\"signals_failed\",\"message\":\"Cannot install signal diagnostics; check signal permissions and remove inherited custom handlers before retrying.\"}\n",
    [MA2_LOG_SIGNALS_RESTORE_FAILURE] =
        "{\"level\":\"error\",\"event\":\"signals_restore_failed\",\"message\":\"Cannot safely restore the previous signal handlers.\"}\n",
    [MA2_LOG_WINDOW_FAILURE] =
        "{\"level\":\"error\",\"event\":\"window_failed\",\"message\":\"Cannot open or pump SDL presentation on the main thread; check the display and SDL video driver.\"}\n",
    [MA2_LOG_PRESENT_SOFTWARE] =
        "{\"level\":\"warning\",\"event\":\"presentation_software\",\"message\":\"SDL software presentation is selected; this is not a game renderer.\"}\n",
    [MA2_LOG_PRESENT_ACCELERATED] =
        "{\"level\":\"info\",\"event\":\"presentation_accelerated\",\"message\":\"SDL accelerated presentation is selected; this is not a game renderer.\"}\n",
    [MA2_LOG_WORKER_FAILURE] =
        "{\"level\":\"error\",\"event\":\"worker_failed\",\"message\":\"The off-main worker could not start, stop, or join cleanly.\"}\n",
    [MA2_LOG_WORKER_TIMEOUT] =
        "{\"level\":\"error\",\"event\":\"worker_timeout\",\"message\":\"The off-main worker did not start before the monotonic deadline.\"}\n",
    [MA2_LOG_WORKER_STARTED] =
        "{\"level\":\"info\",\"event\":\"worker_started\",\"message\":\"The off-main worker initialized its host thread state.\"}\n",
    [MA2_LOG_READY] =
        "{\"level\":\"info\",\"event\":\"host_ready\",\"message\":\"The main-thread presentation host is ready.\"}\n",
    [MA2_LOG_SIGNAL_STOP] =
        "{\"level\":\"info\",\"event\":\"signal_stop\",\"message\":\"A termination signal requested cooperative shutdown.\"}\n",
    [MA2_LOG_STOP_REQUESTED] =
        "{\"level\":\"info\",\"event\":\"worker_stop_requested\",\"message\":\"Requesting cooperative worker shutdown.\"}\n",
    [MA2_LOG_WORKER_JOINED] =
        "{\"level\":\"info\",\"event\":\"worker_joined\",\"message\":\"The off-main worker stopped and was joined before window teardown.\"}\n",
    [MA2_LOG_GUEST_INPUT_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_input_failed\",\"message\":\"Private game input is unavailable or unsafe; set MA2_GAME_DIR to an absolute owner-controlled extracted game directory.\"}\n",
    [MA2_LOG_GUEST_SIGNAL_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_signals_failed\",\"message\":\"Cannot install or restore the synchronous guest MMIO signal layer.\"}\n",
    [MA2_LOG_GUEST_XBE_READY] =
        "{\"level\":\"info\",\"event\":\"guest_xbe_ready\",\"message\":\"The private retail executable passed bounded loading checks.\"}\n",
    [MA2_LOG_GUEST_MMIO_READY] =
        "{\"level\":\"info\",\"event\":\"guest_mmio_ready\",\"message\":\"The synchronous ARM64 MMIO broker and chained fault layer are ready.\"}\n",
    [MA2_LOG_GUEST_MEMORY_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_memory_failed\",\"message\":\"The Xbox virtual memory layout rejected the retail image or host mapping.\"}\n",
    [MA2_LOG_GUEST_FENCE_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_fence_failed\",\"message\":\"The NV2A guest fence acknowledgement path could not start.\"}\n",
    [MA2_LOG_GUEST_APU_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_apu_failed\",\"message\":\"The APU model could not initialize and attach to the MMIO broker.\"}\n",
    [MA2_LOG_GUEST_MEMORY_READY] =
        "{\"level\":\"info\",\"event\":\"guest_memory_ready\",\"message\":\"The Xbox virtual memory layout and executable image are initialized.\"}\n",
    [MA2_LOG_GUEST_METAL_READY] =
        "{\"level\":\"info\",\"event\":\"guest_metal_ready\",\"message\":\"The synchronous Metal NV2A backend is registered; unsupported states use the CPU fallback.\"}\n",
    [MA2_LOG_GUEST_METAL_CPU] =
        "{\"level\":\"info\",\"event\":\"guest_cpu_renderer\",\"message\":\"The CPU NV2A renderer was selected explicitly.\"}\n",
    [MA2_LOG_GUEST_METAL_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_metal_init_failed\",\"message\":\"The Metal NV2A renderer could not be initialized or registered.\"}\n",
    [MA2_LOG_GUEST_BACKEND_INVALID] =
        "{\"level\":\"error\",\"event\":\"guest_backend_invalid\",\"message\":\"RECOMP_PB_BACKEND must be metal or cpu.\"}\n",
    [MA2_LOG_GUEST_XMM_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_xmm_failed\",\"message\":\"The generated ARM64 XMM helper self-test failed.\"}\n",
    [MA2_LOG_GUEST_OHCI_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_ohci_failed\",\"message\":\"The required OHCI controller model could not start.\"}\n",
    [MA2_LOG_GUEST_REGISTER_FAILURE] =
        "{\"level\":\"error\",\"event\":\"guest_register_failed\",\"message\":\"The guest worker could not register as an ARM64 MMIO fault producer.\"}\n",
    [MA2_LOG_GUEST_KERNEL_READY] =
        "{\"level\":\"info\",\"event\":\"guest_kernel_ready\",\"message\":\"Kernel, storage, dispatch, audio, and USB startup completed.\"}\n",
    [MA2_LOG_GUEST_INIT_FAILED] =
        "{\"level\":\"error\",\"event\":\"guest_init_failed\",\"message\":\"Guest initialization failed closed; private diagnostics remain suppressed.\"}\n",
    [MA2_LOG_GUEST_INIT_PASSED] =
        "{\"level\":\"info\",\"event\":\"guest_init_passed\",\"message\":\"Full retail guest initialization and checked teardown passed without entering generated code.\"}\n",
    [MA2_LOG_GUEST_ENTRY] =
        "{\"level\":\"info\",\"event\":\"guest_entry\",\"message\":\"Entering the generated retail executable on the off-main guest thread.\"}\n",
    [MA2_LOG_GUEST_REQUESTED_EXIT] =
        "{\"level\":\"warning\",\"event\":\"guest_requested_exit\",\"message\":\"The retail guest requested a process exit; the native host intercepted it for checked teardown.\"}\n",
    [MA2_LOG_GUEST_RETURNED] =
        "{\"level\":\"info\",\"event\":\"guest_returned\",\"message\":\"The generated retail executable returned to the native host.\"}\n",
    [MA2_LOG_GUEST_CLEANUP_FAILED] =
        "{\"level\":\"error\",\"event\":\"guest_cleanup_failed\",\"message\":\"Guest teardown could not release every initialized resource safely.\"}\n",
    [MA2_LOG_GUEST_CLEANUP_PASSED] =
        "{\"level\":\"info\",\"event\":\"guest_cleanup_passed\",\"message\":\"Guest resources stopped and passed checked teardown.\"}\n",
    [MA2_LOG_WINDOW_CLOSED] =
        "{\"level\":\"info\",\"event\":\"window_closed\",\"message\":\"The main-thread SDL window and presentation resources are closed.\"}\n",
    [MA2_LOG_CLEANUP_FAILURE] =
        "{\"level\":\"error\",\"event\":\"cleanup_failed\",\"message\":\"A host resource could not be closed cleanly.\"}\n",
    [MA2_LOG_SMOKE_PASSED] =
        "{\"level\":\"info\",\"event\":\"smoke_passed\",\"message\":\"Real window pumping and synthetic worker shutdown passed; no guest or frames ran.\"}\n",
    [MA2_LOG_EXIT] =
        "{\"level\":\"info\",\"event\":\"host_exit\",\"message\":\"The native macOS host exited.\"}\n"
};

static int write_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        text += (size_t)written;
        length -= (size_t)written;
    }
    return 1;
}

int ma2_host_log_console_open(Ma2HostLog *log)
{
    if (!log) return 0;
    *log = (Ma2HostLog){.console_fd = -1, .file_fd = -1, .runtime_fd = -1};
    log->console_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    return log->console_fd >= 0;
}

int ma2_host_log_file_open(Ma2HostLog *log, const Ma2HostPaths *paths)
{
    if (!log || !paths || paths->logs_fd < 0 || log->file_fd >= 0) return 0;
    log->file_fd = ma2_host_open_private_file(paths->logs_fd, "host.jsonl",
                                              O_WRONLY | O_APPEND);
    if (log->file_fd < 0) return 0;
    const char *diagnostics = getenv("MA2_PRIVATE_DIAGNOSTICS");
    if (diagnostics && strcmp(diagnostics, "1") == 0) {
        log->runtime_fd = ma2_host_open_private_file(
            paths->logs_fd, "runtime-private.log", O_WRONLY | O_TRUNC);
        if (log->runtime_fd < 0) {
            close(log->file_fd);
            log->file_fd = -1;
            return 0;
        }
    }
    return 1;
}

int ma2_host_log_mute_runtime_stderr(Ma2HostLog *log)
{
    if (!log || log->console_fd < 0 || log->stderr_muted) return 0;
    if (fflush(stderr) != 0) return 0;
    int target_fd = log->runtime_fd;
    int null_fd = -1;
    if (target_fd < 0) {
        null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd < 0) return 0;
        target_fd = null_fd;
    }
    int result = dup2(target_fd, STDERR_FILENO);
    if (result >= 0) log->stderr_muted = 1;
    int closed = null_fd < 0 || close(null_fd) == 0;
    if (result < 0 || !closed) return 0;
    return 1;
}

int ma2_host_log_restore_stderr(Ma2HostLog *log)
{
    if (!log || log->console_fd < 0) return 0;
    if (!log->stderr_muted) return 1;
    if (fflush(stderr) != 0 || dup2(log->console_fd, STDERR_FILENO) < 0) return 0;
    log->stderr_muted = 0;
    return 1;
}

int ma2_host_log_event(Ma2HostLog *log, Ma2HostLogEvent event)
{
    static const char fallback[] =
        "{\"level\":\"error\",\"event\":\"internal_log_error\",\"message\":\"Invalid log event.\"}\n";
    if (!log || log->console_fd < 0) return 0;
    const char *record = fallback;
    if ((unsigned)event < sizeof(records) / sizeof(records[0]) && records[event])
        record = records[event];
    size_t length = strlen(record);
    int ok = write_all(log->console_fd, record, length);
    if (log->file_fd >= 0 && !write_all(log->file_fd, record, length)) {
        log->failed = 1;
        ok = 0;
    }
    return ok;
}

int ma2_host_log_close(Ma2HostLog *log)
{
    if (!log) return 0;
    int ok = ma2_host_log_restore_stderr(log);
    if (log->runtime_fd >= 0 && close(log->runtime_fd) != 0) ok = 0;
    if (log->file_fd >= 0 && close(log->file_fd) != 0) ok = 0;
    if (log->console_fd >= 0 && close(log->console_fd) != 0) ok = 0;
    if (log->failed) ok = 0;
    log->console_fd = log->file_fd = log->runtime_fd = -1;
    return ok;
}
