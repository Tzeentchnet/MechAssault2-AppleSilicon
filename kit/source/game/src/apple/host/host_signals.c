#include "host_shell.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

enum { SIGNAL_COUNT = 7 };
static const int managed_signals[SIGNAL_COUNT] = {
    SIGINT, SIGTERM, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE
};
static struct sigaction previous[SIGNAL_COUNT];
static struct sigaction default_action;
static int installed[SIGNAL_COUNT];
static _Atomic sig_atomic_t stop_signal;
static _Atomic sig_atomic_t console_fd = -1;
static _Atomic sig_atomic_t log_fd = -1;
static int active;

static void signal_record(const char *text, size_t length)
{
    int console = (int)atomic_load_explicit(&console_fd, memory_order_relaxed);
    int file = (int)atomic_load_explicit(&log_fd, memory_order_relaxed);
    if (console >= 0) (void)write(console, text, length);
    if (file >= 0 && file != console) (void)write(file, text, length);
}

static void fatal_record(int number)
{
    static const char segv[] =
        "{\"level\":\"fatal\",\"event\":\"signal\",\"signal\":\"SIGSEGV\",\"message\":\"Unhandled synchronous host or guest fault.\"}\n";
    static const char bus[] =
        "{\"level\":\"fatal\",\"event\":\"signal\",\"signal\":\"SIGBUS\",\"message\":\"Unhandled synchronous host or guest fault.\"}\n";
    static const char abort_msg[] =
        "{\"level\":\"fatal\",\"event\":\"signal\",\"signal\":\"SIGABRT\",\"message\":\"Unhandled host or guest fault.\"}\n";
    static const char illegal[] =
        "{\"level\":\"fatal\",\"event\":\"signal\",\"signal\":\"SIGILL\",\"message\":\"Unhandled host or guest fault.\"}\n";
    static const char floating[] =
        "{\"level\":\"fatal\",\"event\":\"signal\",\"signal\":\"SIGFPE\",\"message\":\"Unhandled host or guest fault.\"}\n";
    switch (number) {
    case SIGSEGV: signal_record(segv, sizeof(segv) - 1); break;
    case SIGBUS: signal_record(bus, sizeof(bus) - 1); break;
    case SIGABRT: signal_record(abort_msg, sizeof(abort_msg) - 1); break;
    case SIGILL: signal_record(illegal, sizeof(illegal) - 1); break;
    case SIGFPE: signal_record(floating, sizeof(floating) - 1); break;
    default: break;
    }
}

static void host_signal_handler(int number, siginfo_t *info, void *context)
{
    int index = -1;
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        if (managed_signals[i] == number) { index = i; break; }
    }
    if (index < 0) _exit(128 + number);
    (void)info;
    (void)context;
    int termination = number == SIGINT || number == SIGTERM;
    if (termination) atomic_store_explicit(&stop_signal, number, memory_order_relaxed);
    else fatal_record(number);
    if (termination) return;

    /* An ignored synchronous fault cannot safely resume the faulting code. */
    if (sigaction(number, &default_action, NULL) != 0 || raise(number) != 0)
        _exit(128 + number);
}

static int is_ours(const struct sigaction *action)
{
    return (action->sa_flags & SA_SIGINFO) && action->sa_sigaction == host_signal_handler;
}

int ma2_host_signals_install(const Ma2HostLog *log)
{
    if (!log || log->console_fd < 0 || active || !pthread_main_np() ||
        !atomic_is_lock_free(&stop_signal) ||
        !atomic_is_lock_free(&console_fd) || !atomic_is_lock_free(&log_fd))
        return 0;
    /* Darwin does not report SA_RESETHAND when querying an installed action.
     * Replacing an unknown custom handler could silently destroy its semantics. */
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        if (sigaction(managed_signals[i], NULL, &previous[i]) != 0 ||
            (previous[i].sa_handler != SIG_DFL && previous[i].sa_handler != SIG_IGN))
            return 0;
    }
    memset(installed, 0, sizeof(installed));
    atomic_store(&stop_signal, 0);
    atomic_store(&console_fd, log->console_fd);
    atomic_store(&log_fd, log->file_fd);
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    active = 1;

    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        if (i < 2 && previous[i].sa_handler == SIG_IGN) continue;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = host_signal_handler;
        action.sa_mask = previous[i].sa_mask;
        action.sa_flags = SA_SIGINFO |
            (previous[i].sa_flags & (SA_ONSTACK | SA_NODEFER | SA_RESTART));
        if (sigaction(managed_signals[i], &action, NULL) != 0) goto failed;
        installed[i] = 1;
    }
    return 1;

failed:
    (void)ma2_host_signals_restore();
    return 0;
}

int ma2_host_signals_restore(void)
{
    if (!active || !pthread_main_np()) return 0;
    int ok = 1;
    for (int i = SIGNAL_COUNT - 1; i >= 0; --i) {
        if (!installed[i]) continue;
        struct sigaction current;
        if (sigaction(managed_signals[i], NULL, &current) != 0) {
            ok = 0;
        } else if (!is_ours(&current)) {
            ok = 0;
        } else {
            if (sigaction(managed_signals[i], &previous[i], NULL) != 0) ok = 0;
        }
        installed[i] = 0;
    }
    atomic_store(&console_fd, -1);
    atomic_store(&log_fd, -1);
    active = 0;
    return ok;
}

int ma2_host_signal_stop_requested(void)
{
    return atomic_load(&stop_signal) != 0;
}
