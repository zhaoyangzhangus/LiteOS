#include "liteos/libc.h"

#include <threads.h>

typedef struct thrd_start_context {
    thrd_start_t function;
    void *argument;
} thrd_start_context_t;

static int thrd_result(int error) {
    if (error == 0) return thrd_success;
    if (error == EBUSY) return thrd_busy;
    if (error == ETIMEDOUT) return thrd_timedout;
    if (error == ENOMEM || error == EAGAIN) return thrd_nomem;
    return thrd_error;
}

static void *thrd_start(void *raw_context) {
    thrd_start_context_t *context = (thrd_start_context_t *)raw_context;
    int result = context->function(context->argument);
    free(context);
    return (void *)(intptr_t)result;
}

int thrd_create(thrd_t *thread, thrd_start_t function, void *argument) {
    thrd_start_context_t *context;
    int error;
    if (thread == 0 || function == 0) return thrd_error;
    context = (thrd_start_context_t *)malloc(sizeof(*context));
    if (context == 0) return thrd_nomem;
    context->function = function;
    context->argument = argument;
    error = pthread_create(thread, 0, thrd_start, context);
    if (error != 0) free(context);
    return thrd_result(error);
}

int thrd_equal(thrd_t left, thrd_t right) {
    return pthread_equal(left, right) != 0;
}

thrd_t thrd_current(void) {
    return pthread_self();
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    if (duration == 0 || duration->tv_sec < 0 || duration->tv_nsec < 0 ||
        duration->tv_nsec >= 1000000000L) return -1;
    return nanosleep(duration, remaining) == 0 ? 0 : -1;
}

void thrd_yield(void) {
    struct timespec duration = {0, 0};
    __libc_reap_detached_threads();
    (void)nanosleep(&duration, 0);
}

_Noreturn void thrd_exit(int result) {
    pthread_exit((void *)(intptr_t)result);
}

int thrd_join(thrd_t thread, int *result) {
    void *value = 0;
    int error = pthread_join(thread, &value);
    if (error == 0 && result != 0) *result = (int)(intptr_t)value;
    return thrd_result(error);
}

int thrd_detach(thrd_t thread) {
    return thrd_result(pthread_detach(thread));
}

int mtx_init(mtx_t *mutex, int type) {
    pthread_mutexattr_t attribute;
    int error;
    if (mutex == 0 || (type & ~(mtx_recursive | mtx_timed)) != 0) {
        return thrd_error;
    }
    error = pthread_mutexattr_init(&attribute);
    if (error == 0 && (type & mtx_recursive) != 0) {
        error = pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE);
    }
    if (error == 0) error = pthread_mutex_init(mutex, &attribute);
    (void)pthread_mutexattr_destroy(&attribute);
    return thrd_result(error);
}

void mtx_destroy(mtx_t *mutex) {
    (void)pthread_mutex_destroy(mutex);
}

int mtx_lock(mtx_t *mutex) {
    return thrd_result(pthread_mutex_lock(mutex));
}

int mtx_timedlock(mtx_t *mutex, const struct timespec *time_point) {
    return thrd_result(pthread_mutex_timedlock(mutex, time_point));
}

int mtx_trylock(mtx_t *mutex) {
    return thrd_result(pthread_mutex_trylock(mutex));
}

int mtx_unlock(mtx_t *mutex) {
    return thrd_result(pthread_mutex_unlock(mutex));
}

int cnd_init(cnd_t *condition) {
    return thrd_result(pthread_cond_init(condition, 0));
}

void cnd_destroy(cnd_t *condition) {
    (void)pthread_cond_destroy(condition);
}

int cnd_wait(cnd_t *condition, mtx_t *mutex) {
    return thrd_result(pthread_cond_wait(condition, mutex));
}

int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
                  const struct timespec *time_point) {
    return thrd_result(pthread_cond_timedwait(condition, mutex, time_point));
}

int cnd_signal(cnd_t *condition) {
    return thrd_result(pthread_cond_signal(condition));
}

int cnd_broadcast(cnd_t *condition) {
    return thrd_result(pthread_cond_broadcast(condition));
}

void call_once(once_flag *flag, void (*function)(void)) {
    (void)pthread_once(flag, function);
}

int tss_create(tss_t *key, tss_dtor_t destructor) {
    return thrd_result(pthread_key_create(key, destructor));
}

void tss_delete(tss_t key) {
    (void)pthread_key_delete(key);
}

int tss_set(tss_t key, void *value) {
    return thrd_result(pthread_setspecific(key, value));
}

void *tss_get(tss_t key) {
    return pthread_getspecific(key);
}
