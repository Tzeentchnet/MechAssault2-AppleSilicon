#include "host_shell.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static _Thread_local Ma2HostThreadState thread_state;

const Ma2HostThreadState *ma2_host_worker_current(void)
{
    return thread_state.worker ? &thread_state : NULL;
}

static void *worker_main(void *argument)
{
    Ma2HostWorker *worker = argument;
    uint64_t now = 0;
    int result = -1;
    if (!pthread_main_np() && ma2_host_monotonic_ns(&now)) {
        thread_state = (Ma2HostThreadState){worker, now};
        result = worker->initialize ? worker->initialize(worker, worker->argument) : 0;
    }

    pthread_mutex_lock(&worker->mutex);
    worker->initialization_result = result;
    worker->state = result == 0 ? MA2_WORKER_RUNNING : MA2_WORKER_FINISHED;
    pthread_cond_broadcast(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);

    if (result == 0) {
        result = worker->callback(worker, worker->argument);
        pthread_mutex_lock(&worker->mutex);
        worker->callback_result = result;
        worker->state = MA2_WORKER_FINISHED;
        pthread_cond_broadcast(&worker->condition);
        pthread_mutex_unlock(&worker->mutex);
    }
    memset(&thread_state, 0, sizeof(thread_state));
    return NULL;
}

int ma2_host_worker_init(Ma2HostWorker *worker)
{
    if (!worker) return 0;
    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0) return 0;
    if (pthread_cond_init(&worker->condition, NULL) != 0) {
        pthread_mutex_destroy(&worker->mutex);
        return 0;
    }
    worker->state = MA2_WORKER_INITIALIZED;
    worker->initialized = 1;
    return 1;
}

int ma2_host_worker_start(Ma2HostWorker *worker, Ma2HostWorkerCallback initialize,
                          Ma2HostWorkerCallback callback, void *argument)
{
    if (!worker || !worker->initialized || !callback) return 0;
    if (pthread_mutex_lock(&worker->mutex) != 0) return 0;
    if (worker->state != MA2_WORKER_INITIALIZED) {
        pthread_mutex_unlock(&worker->mutex);
        return 0;
    }
    worker->initialize = initialize;
    worker->callback = callback;
    worker->argument = argument;
    worker->state = MA2_WORKER_STARTING;
    int result = pthread_create(&worker->thread, NULL, worker_main, worker);
    if (result != 0) worker->state = MA2_WORKER_INITIALIZED;
    pthread_mutex_unlock(&worker->mutex);
    return result == 0;
}

int ma2_host_worker_snapshot(Ma2HostWorker *worker, Ma2HostWorkerSnapshot *snapshot)
{
    if (!worker || !worker->initialized || !snapshot) return 0;
    if (pthread_mutex_lock(&worker->mutex) != 0) return 0;
    *snapshot = (Ma2HostWorkerSnapshot){
        worker->state, worker->stop_requested,
        worker->initialization_result, worker->callback_result
    };
    return pthread_mutex_unlock(&worker->mutex) == 0;
}

int ma2_host_worker_stop(Ma2HostWorker *worker)
{
    if (!worker || !worker->initialized || pthread_mutex_lock(&worker->mutex) != 0)
        return 0;
    if (worker->state == MA2_WORKER_INITIALIZED) {
        pthread_mutex_unlock(&worker->mutex);
        return 0;
    }
    worker->stop_requested = 1;
    int notified = pthread_cond_broadcast(&worker->condition) == 0;
    return pthread_mutex_unlock(&worker->mutex) == 0 && notified;
}

int ma2_host_worker_wait_for_stop(Ma2HostWorker *worker)
{
    if (!worker || ma2_host_worker_current() == NULL ||
        ma2_host_worker_current()->worker != worker ||
        pthread_mutex_lock(&worker->mutex) != 0)
        return -1;
    int result = 0;
    while (!worker->stop_requested) {
        if (pthread_cond_wait(&worker->condition, &worker->mutex) != 0) {
            result = -1;
            break;
        }
    }
    if (pthread_mutex_unlock(&worker->mutex) != 0) result = -1;
    return result;
}

int ma2_host_worker_join(Ma2HostWorker *worker)
{
    if (!worker || !worker->initialized) return 0;
    uint64_t started, now;
    if (!ma2_host_monotonic_ns(&started) || pthread_mutex_lock(&worker->mutex) != 0)
        return 0;
    if (worker->state == MA2_WORKER_INITIALIZED || worker->state == MA2_WORKER_JOINED) {
        pthread_mutex_unlock(&worker->mutex);
        return 0;
    }
    if (pthread_equal(pthread_self(), worker->thread)) {
        pthread_mutex_unlock(&worker->mutex);
        return 0;
    }
    while (worker->state != MA2_WORKER_FINISHED) {
        if (!ma2_host_monotonic_ns(&now) || now < started ||
            now - started >= UINT64_C(5000000000)) {
            pthread_mutex_unlock(&worker->mutex);
            return 0;
        }
        struct timespec interval = {0, 10000000L};
        int result = pthread_cond_timedwait_relative_np(&worker->condition, &worker->mutex,
                                                         &interval);
        if (result != 0 && result != ETIMEDOUT && result != EINTR) {
            pthread_mutex_unlock(&worker->mutex);
            return 0;
        }
    }
    if (pthread_mutex_unlock(&worker->mutex) != 0 || pthread_join(worker->thread, NULL) != 0)
        return 0;
    if (pthread_mutex_lock(&worker->mutex) != 0) return 0;
    worker->state = MA2_WORKER_JOINED;
    return pthread_mutex_unlock(&worker->mutex) == 0;
}

int ma2_host_worker_destroy(Ma2HostWorker *worker)
{
    if (!worker || !worker->initialized) return 0;
    if (pthread_mutex_lock(&worker->mutex) != 0) return 0;
    int safe = worker->state == MA2_WORKER_INITIALIZED || worker->state == MA2_WORKER_JOINED;
    if (pthread_mutex_unlock(&worker->mutex) != 0 || !safe) return 0;
    if (pthread_cond_destroy(&worker->condition) != 0 ||
        pthread_mutex_destroy(&worker->mutex) != 0)
        return 0;
    worker->initialized = 0;
    return 1;
}
