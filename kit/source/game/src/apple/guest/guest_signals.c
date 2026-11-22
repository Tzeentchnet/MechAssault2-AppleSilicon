#include "guest_signals.h"

#include "posix_mmio.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

enum { GUEST_SIGNAL_COUNT = 2 };

static const int guest_signals[GUEST_SIGNAL_COUNT] = {SIGBUS, SIGSEGV};
static struct sigaction previous[GUEST_SIGNAL_COUNT];
static int installed[GUEST_SIGNAL_COUNT];
static int active;

static int guest_signal_index(int number)
{
    for (int i = 0; i < GUEST_SIGNAL_COUNT; ++i)
        if (guest_signals[i] == number) return i;
    return -1;
}

static int is_synchronous_fault(int number, const siginfo_t *info)
{
    if (!info || info->si_code <= 0) return 0;
    if (number == SIGBUS)
        return info->si_code == BUS_ADRALN || info->si_code == BUS_ADRERR ||
               info->si_code == BUS_OBJERR;
    if (number == SIGSEGV)
        return info->si_code == SEGV_MAPERR || info->si_code == SEGV_ACCERR;
    return 0;
}

static void chain_signal(int index, int number, siginfo_t *info, void *context)
{
    const struct sigaction *action = &previous[index];
    if (action->sa_handler == SIG_IGN || action->sa_handler == SIG_DFL) {
        struct sigaction reset;
        memset(&reset, 0, sizeof(reset));
        reset.sa_handler = SIG_DFL;
        sigemptyset(&reset.sa_mask);
        if (sigaction(number, &reset, NULL) != 0 ||
            kill(getpid(), number) != 0)
            _exit(128 + number);
        return;
    }
    if (action->sa_flags & SA_SIGINFO)
        action->sa_sigaction(number, info, context);
    else
        action->sa_handler(number);
}

static void guest_signal_handler(int number, siginfo_t *info, void *context)
{
    int saved_errno = errno;
    int index = guest_signal_index(number);
    if (index < 0) _exit(128 + number);
    if (is_synchronous_fault(number, info) &&
        xbox_PosixMmioFault(context, info->si_addr)) {
        errno = saved_errno;
        return;
    }
    chain_signal(index, number, info, context);
    errno = saved_errno;
}

static int is_ours(const struct sigaction *action)
{
    return (action->sa_flags & SA_SIGINFO) &&
           action->sa_sigaction == guest_signal_handler;
}

int ma2_guest_signals_install(void)
{
    if (active) return 0;
    memset(installed, 0, sizeof(installed));
    for (int i = 0; i < GUEST_SIGNAL_COUNT; ++i)
        if (sigaction(guest_signals[i], NULL, &previous[i]) != 0)
            return 0;
    active = 1;
    for (int i = 0; i < GUEST_SIGNAL_COUNT; ++i) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = guest_signal_handler;
        action.sa_mask = previous[i].sa_mask;
        sigaddset(&action.sa_mask, SIGBUS);
        sigaddset(&action.sa_mask, SIGSEGV);
        action.sa_flags = SA_SIGINFO |
            (previous[i].sa_flags & (SA_ONSTACK | SA_RESTART));
        if (sigaction(guest_signals[i], &action, NULL) != 0) {
            (void)ma2_guest_signals_restore();
            return 0;
        }
        installed[i] = 1;
    }
    return 1;
}

int ma2_guest_signals_restore(void)
{
    if (!active) return 0;
    int ok = 1;
    for (int i = GUEST_SIGNAL_COUNT - 1; i >= 0; --i) {
        if (!installed[i]) continue;
        struct sigaction current;
        if (sigaction(guest_signals[i], NULL, &current) != 0 ||
            !is_ours(&current) ||
            sigaction(guest_signals[i], &previous[i], NULL) != 0)
            ok = 0;
        installed[i] = 0;
    }
    active = 0;
    return ok;
}
