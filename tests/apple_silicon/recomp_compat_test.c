#include "recomp_compat.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static __declspec(thread) int thread_value;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int release_threads;

typedef struct Worker {
    int value;
    int observed;
} Worker;

static void *worker(void *opaque)
{
    Worker *state = opaque;
    thread_value = state->value;
    pthread_mutex_lock(&lock);
    ready++;
    pthread_cond_broadcast(&changed);
    while (!release_threads)
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    state->observed = thread_value;
    return NULL;
}

int main(void)
{
    Worker states[2] = {{11, 0}, {22, 0}};
    pthread_t threads[2];
    volatile int32_t bits = -1;
    thread_value = 37;
    if (InterlockedAnd(&bits, ~(int32_t)1) != -1 || bits != -2 ||
        InterlockedAnd(&bits, 0) != -2 || bits != 0)
        return 1;
    YieldProcessor();
    for (unsigned i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, worker, &states[i]) != 0)
            return 2;
    pthread_mutex_lock(&lock);
    while (ready != 2)
        pthread_cond_wait(&changed, &lock);
    release_threads = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 2; i++)
        if (pthread_join(threads[i], NULL) != 0)
            return 3;
    if (thread_value != 37 || states[0].observed != 11 || states[1].observed != 22) {
        fputs("The MSVC thread declaration did not retain thread-local storage\n", stderr);
        return 4;
    }
    return 0;
}
