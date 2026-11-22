#include "guest_bootstrap.h"
#include "guest_signals.h"

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu.h"
#include "ohci.h"
#include "posix_mmio.h"
#include "platform/win32_compat.h"
#include "nv2a_metal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    MA2_XBE_MIN_SIZE = 4096,
    MA2_XBE_MAX_SIZE = 64 * 1024 * 1024
};

struct Ma2Guest {
    const Ma2HostSettings *settings;
    const Ma2HostPaths *paths;
    Ma2HostLog *log;
    void *xbe_data;
    size_t xbe_size;
    MCPXAPUState *apu;
    Ma2MetalRenderer *metal;
    _Atomic int entry_released;
    int signals_installed;
    int broker_started;
    int memory_initialized;
    int backend_registered;
    int kernel_initialized;
    int ohci_initialized;
    int thread_registered;
    int exit_scope;
    unsigned exit_code;
    jmp_buf exit_jump;
    char game_dir[MA2_HOST_PATH_CAP];
    char save_dir[MA2_HOST_PATH_CAP];
};

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern int recomp_dispatch_fast_init(void);
extern int recomp_xmm_selftest(void);
extern void ma2_gpu_software_method(uint32_t value);
extern void nv2a_pb_set_software_method(void (*callback)(uint32_t));

int g_recomp_fastcode;

static void guest_exit_process(UINT exit_code, PVOID context)
{
    Ma2Guest *guest = context;
    if (!guest || !guest->exit_scope) _Exit((int)exit_code);
    guest->exit_code = exit_code;
    longjmp(guest->exit_jump, 1);
}

static int guest_log(Ma2Guest *guest, Ma2HostLogEvent event)
{
    return ma2_host_log_event(guest->log, event);
}

static int path_is_bounded_absolute(const char *path, size_t *length)
{
    if (!path || path[0] != '/') return 0;
    size_t n = strnlen(path, MA2_HOST_PATH_CAP);
    if (n == MA2_HOST_PATH_CAP) return 0;
    while (n > 1 && path[n - 1] == '/') --n;
    if (n <= 1) return 0;
    size_t component = 1;
    for (size_t i = 1; i <= n; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (i < n && (c < 32 || c == 127)) return 0;
        if (i == n || c == '/') {
            size_t size = i - component;
            if (!size ||
                (size == 1 && path[component] == '.') ||
                (size == 2 && path[component] == '.' &&
                 path[component + 1] == '.'))
                return 0;
            component = i + 1;
        }
    }
    *length = n;
    return 1;
}

static int open_owner_directory(const char *path, char *normalized)
{
    size_t length;
    if (!path_is_bounded_absolute(path, &length)) return -1;
    memcpy(normalized, path, length);
    normalized[length] = '\0';

    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    size_t start = 1;
    while (current >= 0 && start < length) {
        size_t end = start;
        while (end < length && normalized[end] != '/') ++end;
        char component[NAME_MAX + 1];
        size_t component_length = end - start;
        if (!component_length || component_length > NAME_MAX) {
            close(current);
            return -1;
        }
        memcpy(component, normalized + start, component_length);
        component[component_length] = '\0';
        int next = openat(current, component,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(current);
        if (next < 0) return -1;
        struct stat status;
        if (fstat(next, &status) != 0 || !S_ISDIR(status.st_mode) ||
            (status.st_uid != 0 && status.st_uid != geteuid()) ||
            (status.st_mode & (S_IWGRP | S_IWOTH))) {
            close(next);
            return -1;
        }
        current = next;
        start = end + 1;
    }
    struct stat status;
    if (current < 0 || fstat(current, &status) != 0 ||
        status.st_uid != geteuid()) {
        if (current >= 0) close(current);
        return -1;
    }
    return current;
}

static int read_xbe(Ma2Guest *guest, int game_fd)
{
    int fd = openat(game_fd, "ma2.xbe",
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat status;
    int valid = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == geteuid() && status.st_nlink == 1 &&
        status.st_size >= MA2_XBE_MIN_SIZE &&
        status.st_size <= MA2_XBE_MAX_SIZE;
    if (!valid) {
        close(fd);
        return 0;
    }
    size_t size = (size_t)status.st_size;
    void *data = malloc(size);
    if (!data) {
        close(fd);
        return 0;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, (unsigned char *)data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += (size_t)count;
    }
    int closed = close(fd) == 0;
    if (offset != size || !closed || memcmp(data, "XBEH", 4) != 0) {
        free(data);
        return 0;
    }
    guest->xbe_data = data;
    guest->xbe_size = size;
    return 1;
}

static int prepare_private_input(Ma2Guest *guest)
{
    const char *game_dir = getenv("MA2_GAME_DIR");
    int game_fd = open_owner_directory(game_dir, guest->game_dir);
    if (game_fd < 0) return 0;
    int ok = read_xbe(guest, game_fd);
    if (close(game_fd) != 0) ok = 0;
    if (!ok) return 0;

    if (mkdirat(guest->paths->support_fd, "Saved Games", 0700) != 0 &&
        errno != EEXIST)
        return 0;
    int save_fd = openat(guest->paths->support_fd, "Saved Games",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status;
    if (save_fd < 0 || fstat(save_fd, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) || fchmod(save_fd, 0700) != 0) {
        if (save_fd >= 0) close(save_fd);
        return 0;
    }
    int closed = close(save_fd) == 0;
    int length = snprintf(guest->save_dir, sizeof(guest->save_dir),
                          "%s/Saved Games", guest->paths->support);
    return closed && length > 0 && (size_t)length < sizeof(guest->save_dir);
}

static int set_default_environment(const Ma2Guest *guest)
{
    static const struct {
        const char *name;
        const char *value;
    } defaults[] = {
        {"RECOMP_AC97_READY", "1"},
        {"RECOMP_ASYNC_IO", "1"},
        {"RECOMP_FB_WINDOW", "1"},
        {"RECOMP_HEAP_RECLAIM", "1"},
        {"RECOMP_PB_EXEC", "1"},
        {"RECOMP_TITLE_KEVENTS", "1"},
        {"RECOMP_USB", "1"},
        {"RECOMP_VBLANK", "1"}
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
        if (!getenv(defaults[i].name) &&
            setenv(defaults[i].name, defaults[i].value, 0) != 0)
            return 0;
    if (guest->settings->safe_start &&
        setenv("RECOMP_SAFE_START", "1", 1) != 0)
        return 0;
    return 1;
}

static int guest_shutdown(Ma2Guest *guest)
{
    int safe = 1;
    int ok = 1;
    if (guest->kernel_initialized && !xbox_kernel_bridge_quiesce()) {
        safe = 0;
        ok = 0;
    }
    if (guest->thread_registered) {
        if (!xbox_PosixMmioUnregisterThread()) {
            safe = 0;
            ok = 0;
        } else {
            guest->thread_registered = 0;
        }
    }
    if (guest->apu && !mcpx_apu_quiesce(guest->apu)) {
        safe = 0;
        ok = 0;
    }
    if (guest->broker_started && safe &&
        !xbox_PosixMmioSealProducers()) {
        safe = 0;
        ok = 0;
    }
    if (guest->ohci_initialized) {
        if (!safe || !xbox_OhciShutdown()) {
            safe = 0;
            ok = 0;
        } else {
            guest->ohci_initialized = 0;
        }
    }
    if (guest->broker_started && safe &&
        (!xbox_PosixMmioBeginShutdown() || !xbox_PosixMmioStop())) {
        safe = 0;
        ok = 0;
    } else if (guest->broker_started && safe) {
        guest->broker_started = 0;
    }
    if (guest->apu && safe) {
        if (!mcpx_apu_shutdown_checked(guest->apu)) {
            safe = 0;
            ok = 0;
        } else {
            guest->apu = NULL;
            g_apu_state = NULL;
        }
    }
    if (guest->kernel_initialized && safe) {
        xbox_kernel_shutdown();
        guest->kernel_initialized = 0;
    }
    if (guest->memory_initialized && safe) {
        if (!xbox_MemoryLayoutShutdownChecked()) {
            safe = 0;
            ok = 0;
        } else {
            guest->memory_initialized = 0;
        }
    }
    if (guest->metal && !guest->memory_initialized && safe) {
        if (guest->backend_registered) {
            if (!nv2a_backend_register(NULL)) ok = 0;
            else guest->backend_registered = 0;
        }
        if (!ma2_metal_destroy(guest->metal)) ok = 0;
        guest->metal = NULL;
    }
    if (guest->signals_installed) {
        if (!ma2_guest_signals_restore()) ok = 0;
        else guest->signals_installed = 0;
    }
    free(guest->xbe_data);
    guest->xbe_data = NULL;
    guest->xbe_size = 0;
    guest_log(guest, ok ? MA2_LOG_GUEST_CLEANUP_PASSED :
              MA2_LOG_GUEST_CLEANUP_FAILED);
    return ok;
}

Ma2Guest *ma2_guest_create(const Ma2HostSettings *settings,
                           const Ma2HostPaths *paths, Ma2HostLog *log)
{
    if (!settings || !paths || !log) return NULL;
    Ma2Guest *guest = calloc(1, sizeof(*guest));
    if (!guest) return NULL;
    guest->settings = settings;
    guest->paths = paths;
    guest->log = log;
    return guest;
}

void ma2_guest_destroy(Ma2Guest *guest)
{
    if (!guest) return;
    free(guest->xbe_data);
    free(guest);
}

void ma2_guest_release_entry(Ma2Guest *guest)
{
    if (guest)
        atomic_store_explicit(&guest->entry_released, 1, memory_order_release);
}

int ma2_guest_initialize(Ma2HostWorker *worker, void *argument)
{
    Ma2Guest *guest = argument;
    const Ma2HostThreadState *state = ma2_host_worker_current();
    if (!guest || !worker || !state || state->worker != worker ||
        pthread_main_np())
        return -1;

    if (!prepare_private_input(guest)) {
        guest_log(guest, MA2_LOG_GUEST_INPUT_FAILURE);
        guest_log(guest, MA2_LOG_GUEST_INIT_FAILED);
        (void)guest_shutdown(guest);
        return -2;
    }
    if (!guest_log(guest, MA2_LOG_GUEST_XBE_READY) ||
        !guest_log(guest, MA2_LOG_GUEST_OPTIONAL_UNAVAILABLE) ||
        !set_default_environment(guest))
        goto failed;
    {
        const char *selection = getenv("RECOMP_PB_BACKEND");
        if (!selection || strcmp(selection, "metal") == 0) {
            guest->metal = ma2_metal_create(guest->log->console_fd, guest->log->file_fd);
            Nv2aBackend backend;
            ma2_metal_backend(guest->metal, &backend);
            if (!guest->metal || !nv2a_backend_register(&backend)) {
                guest_log(guest, MA2_LOG_GUEST_METAL_FAILURE);
                goto failed;
            }
            guest->backend_registered = 1;
            if (!guest_log(guest, MA2_LOG_GUEST_METAL_READY)) goto failed;
        } else if (strcmp(selection, "cpu") == 0) {
            if (!guest_log(guest, MA2_LOG_GUEST_METAL_CPU)) goto failed;
        } else {
            guest_log(guest, MA2_LOG_GUEST_BACKEND_INVALID);
            goto failed;
        }
    }
    if (!ma2_guest_signals_install()) {
        guest_log(guest, MA2_LOG_GUEST_SIGNAL_FAILURE);
        goto failed;
    }
    guest->signals_installed = 1;
    if (!xbox_PosixMmioStart()) goto failed;
    guest->broker_started = 1;
    if (!guest_log(guest, MA2_LOG_GUEST_MMIO_READY)) goto failed;
    if (!xbox_MemoryLayoutInit(guest->xbe_data, guest->xbe_size)) {
        guest_log(guest, MA2_LOG_GUEST_MEMORY_FAILURE);
        goto failed;
    }
    guest->memory_initialized = 1;
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    if (!guest_log(guest, MA2_LOG_GUEST_MEMORY_READY)) goto failed;
    if (xbox_Nv2aMirrorFence(0x0041BC68u, 0x2Cu, 0x30u) < 0) {
        guest_log(guest, MA2_LOG_GUEST_FENCE_FAILURE);
        goto failed;
    }
    nv2a_pb_set_software_method(ma2_gpu_software_method);
    guest->apu = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    if (!guest->apu || !xbox_PosixMmioAttach(guest->apu)) {
        guest_log(guest, MA2_LOG_GUEST_APU_FAILURE);
        goto failed;
    }

    xbox_kernel_init();
    guest->kernel_initialized = 1;
    xbox_path_init(guest->game_dir, guest->save_dir);
    xbox_kernel_bridge_init();
    xbox_SetDispatchSerializationRange(0x0041D000u, 0x0042584Cu);
    g_esp = XBOX_STACK_TOP;
    (void)recomp_dispatch_init();
    g_recomp_fastcode = 1;
    (void)recomp_dispatch_fast_init();
    if (recomp_xmm_selftest() != 0) {
        guest_log(guest, MA2_LOG_GUEST_XMM_FAILURE);
        goto failed;
    }
    xbox_OhciInit();
    guest->ohci_initialized = 1;
    if (!xbox_OhciIsEnabled()) {
        guest_log(guest, MA2_LOG_GUEST_OHCI_FAILURE);
        goto failed;
    }
    if (!xbox_PosixMmioRegisterThread()) {
        guest_log(guest, MA2_LOG_GUEST_REGISTER_FAILURE);
        goto failed;
    }
    guest->thread_registered = 1;
    xbox_WatchdogStart();
    if (!guest_log(guest, MA2_LOG_GUEST_KERNEL_READY)) goto failed;
    return 0;

failed:
    guest_log(guest, MA2_LOG_GUEST_INIT_FAILED);
    return guest_shutdown(guest) ? -3 : -4;
}

int ma2_guest_run(Ma2HostWorker *worker, void *argument)
{
    Ma2Guest *guest = argument;
    if (!guest || !worker) return -1;
    while (!atomic_load_explicit(&guest->entry_released, memory_order_acquire)) {
        Ma2HostWorkerSnapshot snapshot;
        if (!ma2_host_worker_snapshot(worker, &snapshot)) {
            (void)guest_shutdown(guest);
            return -2;
        }
        if (snapshot.stop_requested)
            return guest_shutdown(guest) ? 0 : -3;
        usleep(1000);
    }
    if (guest->settings->guest_init_only) {
        if (ma2_host_worker_wait_for_stop(worker) != 0)
            return guest_shutdown(guest) ? -4 : -5;
        int ok = guest_shutdown(guest);
        if (ok) guest_log(guest, MA2_LOG_GUEST_INIT_PASSED);
        return ok ? 0 : -6;
    }
    if (!guest_log(guest, MA2_LOG_GUEST_ENTRY)) {
        (void)guest_shutdown(guest);
        return -7;
    }
    int requested_exit = setjmp(guest->exit_jump);
    if (!requested_exit) {
        guest->exit_scope = 1;
        Win32CompatSetExitProcessHandler(guest_exit_process, guest);
        xbe_entry_point();
    }
    Win32CompatSetExitProcessHandler(NULL, NULL);
    guest->exit_scope = 0;
    if (requested_exit)
        guest_log(guest, MA2_LOG_GUEST_REQUESTED_EXIT);
    else
        guest_log(guest, MA2_LOG_GUEST_RETURNED);
    int shutdown_ok = guest_shutdown(guest);
    if (!shutdown_ok) return -8;
    return requested_exit && guest->exit_code ? -9 : 0;
}
