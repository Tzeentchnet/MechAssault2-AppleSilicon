#include "host_shell.h"
#include "host_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <servers/bootstrap.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int home_is_valid(const char *home, size_t *length)
{
    if (!home || home[0] != '/') return 0;
    size_t n = strnlen(home, MA2_HOST_PATH_CAP);
    if (n == MA2_HOST_PATH_CAP) return 0;
    while (n > 1 && home[n - 1] == '/') --n;
    if (n <= 1) return 0;
    size_t component = 1;
    for (size_t i = 1; i <= n; ++i) {
        unsigned char c = (unsigned char)home[i];
        if (i < n && (c < 32 || c == 127)) return 0;
        if (i == n || c == '/') {
            size_t size = i - component;
            if ((size == 1 && home[component] == '.') ||
                (size == 2 && home[component] == '.' && home[component + 1] == '.'))
                return 0;
            component = i + 1;
        }
    }
    *length = n;
    return 1;
}

static int make_path(char *out, const char *home, size_t length, const char *suffix)
{
    int n = snprintf(out, MA2_HOST_PATH_CAP, "%.*s%s", (int)length, home, suffix);
    return n >= 0 && n < MA2_HOST_PATH_CAP;
}

static int open_directory(int parent, const char *name)
{
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status;
    if (fd < 0) return -1;
    if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid()) {
        close(fd);
        return -1;
    }
    return fd;
}

int ma2_host_open_private_file(int directory_fd, const char *name, int flags)
{
    int fd = openat(directory_fd, name,
                    flags | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat status;
    if (fd < 0) return -1;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || status.st_nlink != 1 || fchmod(fd, 0600) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

Ma2HostPathResult ma2_host_paths_open(Ma2HostPaths *paths, const char *home)
{
    if (!paths) return MA2_PATH_INVALID_HOME;
    memset(paths, 0, sizeof(*paths));
    paths->support_fd = paths->caches_fd = paths->logs_fd = -1;
    size_t length;
    if (!home_is_valid(home, &length) ||
        !make_path(paths->support, home, length, "/Library/Application Support/MechAssault2") ||
        !make_path(paths->caches, home, length, "/Library/Caches/MechAssault2") ||
        !make_path(paths->logs, home, length, "/Library/Logs/MechAssault2") ||
        strlen(paths->support) + sizeof("/host.lock") > MA2_HOST_PATH_CAP ||
        strlen(paths->logs) + sizeof("/host.jsonl") > MA2_HOST_PATH_CAP)
        return MA2_PATH_INVALID_HOME;

    char normalized[MA2_HOST_PATH_CAP];
    memcpy(normalized, home, length);
    normalized[length] = '\0';
    int home_fd = open(normalized, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int library_fd = -1, support_parent = -1, caches_parent = -1, logs_parent = -1;
    struct stat status;
    if (home_fd < 0 || fstat(home_fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid())
        goto failed;
    library_fd = open_directory(home_fd, "Library");
    if (library_fd < 0) goto failed;
    support_parent = open_directory(library_fd, "Application Support");
    caches_parent = open_directory(library_fd, "Caches");
    logs_parent = open_directory(library_fd, "Logs");
    if (support_parent < 0 || caches_parent < 0 || logs_parent < 0) goto failed;
    paths->support_fd = open_directory(support_parent, "MechAssault2");
    paths->caches_fd = open_directory(caches_parent, "MechAssault2");
    paths->logs_fd = open_directory(logs_parent, "MechAssault2");
    if (paths->support_fd < 0 || paths->caches_fd < 0 || paths->logs_fd < 0) goto failed;

    close(logs_parent);
    close(caches_parent);
    close(support_parent);
    close(library_fd);
    close(home_fd);
    return MA2_PATH_OK;

failed:
    if (logs_parent >= 0) close(logs_parent);
    if (caches_parent >= 0) close(caches_parent);
    if (support_parent >= 0) close(support_parent);
    if (library_fd >= 0) close(library_fd);
    if (home_fd >= 0) close(home_fd);
    (void)ma2_host_paths_close(paths);
    return MA2_PATH_UNAVAILABLE;
}

int ma2_host_paths_close(Ma2HostPaths *paths)
{
    if (!paths) return 0;
    int ok = 1;
    if (paths->support_fd >= 0 && close(paths->support_fd) != 0) ok = 0;
    if (paths->caches_fd >= 0 && close(paths->caches_fd) != 0) ok = 0;
    if (paths->logs_fd >= 0 && close(paths->logs_fd) != 0) ok = 0;
    paths->support_fd = paths->caches_fd = paths->logs_fd = -1;
    return ok;
}

static int release_session_port(mach_port_t port, int has_send_right)
{
    int ok = 1;
    if (has_send_right && mach_port_deallocate(mach_task_self(), port) != KERN_SUCCESS)
        ok = 0;
    if (mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) != KERN_SUCCESS)
        ok = 0;
    return ok;
}

Ma2HostInstanceResult ma2_host_instance_lock(const Ma2HostPaths *paths,
                                             Ma2HostInstance *instance)
{
    if (!paths || paths->support_fd < 0 || !instance) return MA2_INSTANCE_ERROR;
    *instance = (Ma2HostInstance){.file_fd = -1, .session_port = MACH_PORT_NULL};
    char service[96];
    int length = snprintf(service, sizeof(service), "com.mechassault2.host.%lu",
                          (unsigned long)getuid());
    if (length < 0 || (size_t)length >= sizeof(service)) return MA2_INSTANCE_ERROR;
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS)
        return MA2_INSTANCE_ERROR;
    if (mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
        (void)release_session_port(port, 0);
        return MA2_INSTANCE_ERROR;
    }
    kern_return_t registered = KERN_FAILURE;
    Ma2HostInstanceResult session_result = MA2_INSTANCE_ERROR;
    for (int attempt = 0; attempt < 2; ++attempt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        registered = bootstrap_register(bootstrap_port, service, port);
#pragma clang diagnostic pop
        if (registered == KERN_SUCCESS) break;
        if (registered == BOOTSTRAP_NAME_IN_USE) {
            session_result = MA2_INSTANCE_BUSY;
            break;
        }
        if (registered != BOOTSTRAP_NOT_PRIVILEGED) break;
        mach_port_t existing = MACH_PORT_NULL;
        kern_return_t found = bootstrap_look_up(bootstrap_port, service, &existing);
        if (found == KERN_SUCCESS && existing != MACH_PORT_NULL) {
            if (mach_port_deallocate(mach_task_self(), existing) == KERN_SUCCESS)
                session_result = MA2_INSTANCE_BUSY;
            break;
        }
    }
    if (registered != KERN_SUCCESS) {
        if (!release_session_port(port, 1)) return MA2_INSTANCE_ERROR;
        return session_result;
    }
    int fd = ma2_host_open_private_file(paths->support_fd, "host.lock", O_RDWR);
    if (fd < 0) {
        (void)release_session_port(port, 1);
        return MA2_INSTANCE_ERROR;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        close(fd);
        if (!release_session_port(port, 1)) return MA2_INSTANCE_ERROR;
        return error == EWOULDBLOCK || error == EAGAIN ? MA2_INSTANCE_BUSY : MA2_INSTANCE_ERROR;
    }
    instance->file_fd = fd;
    instance->session_port = port;
    return MA2_INSTANCE_OK;
}

int ma2_host_instance_unlock(Ma2HostInstance *instance)
{
    if (!instance || instance->file_fd < 0 || instance->session_port == MACH_PORT_NULL)
        return 0;
    int ok = flock(instance->file_fd, LOCK_UN) == 0;
    if (close(instance->file_fd) != 0) ok = 0;
    if (!release_session_port(instance->session_port, 1)) ok = 0;
    instance->file_fd = -1;
    instance->session_port = MACH_PORT_NULL;
    return ok;
}
