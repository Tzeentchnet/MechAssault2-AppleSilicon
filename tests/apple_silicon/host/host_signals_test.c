#include "host_shell.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "host signal check failed at line %d: %s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static volatile sig_atomic_t caught;
static volatile sig_atomic_t forward_fault;
static struct sigaction outer_next;

static void previous_handler(int number, siginfo_t *info, void *context)
{
    if ((number == SIGBUS || number == SIGTERM) && info && context) caught = number;
}

static void replacement_handler(int number)
{
    (void)number;
}

static void outer_handler(int number, siginfo_t *info, void *context)
{
    if (forward_fault) outer_next.sa_sigaction(number, info, context);
    else caught = number;
}

static int test_default_chain(int number, const char *name, int use_outer_handler)
{
    int channel[2];
    CHECK(pipe(channel) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(channel[0]);
        struct rlimit limit = {0, 0};
        if (setrlimit(RLIMIT_CORE, &limit) != 0) _exit(91);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(number, &action, NULL) != 0) _exit(92);
        Ma2HostLog log = {.console_fd = channel[1], .file_fd = -1};
        if (!ma2_host_signals_install(&log)) _exit(93);
        if (use_outer_handler) {
            if (sigaction(number, NULL, &outer_next) != 0) _exit(95);
            memset(&action, 0, sizeof(action));
            sigemptyset(&action.sa_mask);
            action.sa_sigaction = outer_handler;
            action.sa_flags = SA_SIGINFO;
            forward_fault = 1;
            if (sigaction(number, &action, NULL) != 0) _exit(96);
        }
        raise(number);
        _exit(94);
    }
    close(channel[1]);
    char output[512];
    ssize_t size = read(channel[0], output, sizeof(output) - 1);
    close(channel[0]);
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(size > 0);
    output[size] = '\0';
    CHECK(strstr(output, "\"event\":\"signal\"") && strstr(output, name));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == number);
    return 0;
}

int main(void)
{
    CHECK(pthread_main_np());
    Ma2HostLog log;
    CHECK(ma2_host_log_console_open(&log));

    struct sigaction original_bus, original_term, action, current;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = previous_handler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    CHECK(sigaction(SIGBUS, &action, &original_bus) == 0);
    CHECK(!ma2_host_signals_install(&log));
    caught = 0;
    CHECK(raise(SIGBUS) == 0 && caught == SIGBUS);
    CHECK(sigaction(SIGBUS, NULL, &current) == 0 && current.sa_handler == SIG_DFL);
    CHECK(sigaction(SIGBUS, &original_bus, NULL) == 0);

    action.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGTERM, &action, &original_term) == 0);
    CHECK(!ma2_host_signals_install(&log));
    caught = 0;
    CHECK(raise(SIGTERM) == 0 && caught == SIGTERM);
    CHECK(sigaction(SIGTERM, NULL, &current) == 0 &&
          (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == previous_handler);
    CHECK(sigaction(SIGTERM, &original_term, NULL) == 0);

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGTERM, &action, &original_term) == 0);
    CHECK(ma2_host_signals_install(&log));
    CHECK(raise(SIGTERM) == 0 && !ma2_host_signal_stop_requested());
    CHECK(ma2_host_signals_restore());
    CHECK(sigaction(SIGTERM, NULL, &current) == 0 && current.sa_handler == SIG_IGN);
    CHECK(sigaction(SIGTERM, &original_term, NULL) == 0);

    action.sa_handler = SIG_DFL;
    CHECK(sigaction(SIGTERM, &action, &original_term) == 0);
    CHECK(ma2_host_signals_install(&log));
    CHECK(raise(SIGTERM) == 0 && ma2_host_signal_stop_requested());
    CHECK(ma2_host_signals_restore());
    CHECK(sigaction(SIGTERM, &original_term, NULL) == 0);

    struct sigaction before_replacement;
    CHECK(sigaction(SIGBUS, NULL, &before_replacement) == 0);
    CHECK(ma2_host_signals_install(&log));
    CHECK(sigaction(SIGBUS, NULL, &outer_next) == 0);
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = outer_handler;
    action.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGBUS, &action, NULL) == 0);
    forward_fault = 0;
    caught = 0;
    CHECK(raise(SIGBUS) == 0 && caught == SIGBUS);
    CHECK(sigaction(SIGBUS, &outer_next, NULL) == 0);
    CHECK(ma2_host_signals_restore());

    CHECK(ma2_host_signals_install(&log));
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = replacement_handler;
    CHECK(sigaction(SIGBUS, &action, NULL) == 0);
    CHECK(!ma2_host_signals_restore());
    CHECK(sigaction(SIGBUS, NULL, &current) == 0 && current.sa_handler == replacement_handler);
    CHECK(sigaction(SIGBUS, &before_replacement, NULL) == 0);

    CHECK(test_default_chain(SIGSEGV, "SIGSEGV", 0) == 0);
    CHECK(test_default_chain(SIGBUS, "SIGBUS", 0) == 0);
    CHECK(test_default_chain(SIGBUS, "SIGBUS", 1) == 0);
    CHECK(ma2_host_log_close(&log));
    puts("Host signals: inherited-handler safety, MMIO-style chaining and default chains passed");
    return 0;
}
