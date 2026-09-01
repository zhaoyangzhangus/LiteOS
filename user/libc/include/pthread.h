#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct liteos_pthread;
typedef struct liteos_pthread *pthread_t;

typedef struct pthread_attr {
    void *stack_address;
    size_t stack_size;
    size_t guard_size;
    int detach_state;
} pthread_attr_t;

typedef struct pthread_mutexattr {
    int type;
    int process_shared;
} pthread_mutexattr_t;

typedef struct pthread_condattr {
    clockid_t clock_id;
    int process_shared;
} pthread_condattr_t;

typedef struct pthread_mutex {
    volatile uint32_t state;
} pthread_mutex_t;

typedef struct pthread_cond {
    volatile uint32_t sequence;
} pthread_cond_t;

typedef struct pthread_once {
    volatile uint32_t state;
} pthread_once_t;

typedef unsigned int pthread_key_t;
typedef struct pthread_rwlock {
    volatile uint32_t state;
} pthread_rwlock_t;

typedef struct pthread_rwlockattr {
    int process_shared;
} pthread_rwlockattr_t;

typedef struct pthread_barrier {
    uint32_t threshold;
    volatile uint32_t arrived;
    volatile uint32_t generation;
} pthread_barrier_t;

typedef struct pthread_barrierattr {
    int process_shared;
} pthread_barrierattr_t;

typedef struct pthread_spinlock {
    volatile uint32_t state;
} pthread_spinlock_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2

#define PTHREAD_STACK_MIN 16384U
#define PTHREAD_CANCELED ((void *)-1)
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

#define PTHREAD_MUTEX_INITIALIZER {0U}
#define PTHREAD_COND_INITIALIZER {0U}
#define PTHREAD_ONCE_INIT {0U}
#define PTHREAD_RWLOCK_INITIALIZER {0U}

int pthread_attr_init(pthread_attr_t *attribute);
int pthread_attr_destroy(pthread_attr_t *attribute);
int pthread_attr_setstacksize(pthread_attr_t *attribute, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attribute, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *attribute, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *attribute, size_t *size);
int pthread_attr_setstack(pthread_attr_t *attribute, void *address,
                          size_t size);
int pthread_attr_getstack(const pthread_attr_t *attribute, void **address,
                          size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *attribute, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attribute, int *state);

int pthread_mutexattr_init(pthread_mutexattr_t *attribute);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attribute);
int pthread_mutexattr_settype(pthread_mutexattr_t *attribute, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attribute, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attribute, int shared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attribute,
                                 int *shared);
int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attribute);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_condattr_init(pthread_condattr_t *attribute);
int pthread_condattr_destroy(pthread_condattr_t *attribute);
int pthread_condattr_setclock(pthread_condattr_t *attribute, clockid_t clock_id);
int pthread_condattr_getclock(const pthread_condattr_t *attribute,
                              clockid_t *clock_id);
int pthread_condattr_setpshared(pthread_condattr_t *attribute, int shared);
int pthread_condattr_getpshared(const pthread_condattr_t *attribute,
                                int *shared);
int pthread_cond_init(pthread_cond_t *condition,
                      const pthread_condattr_t *attribute);
int pthread_cond_destroy(pthread_cond_t *condition);
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *condition);
int pthread_cond_broadcast(pthread_cond_t *condition);

int pthread_once(pthread_once_t *once, void (*function)(void));
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_rwlockattr_init(pthread_rwlockattr_t *attribute);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attribute);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attribute, int shared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attribute,
                                  int *shared);
int pthread_rwlock_init(pthread_rwlock_t *lock,
                        const pthread_rwlockattr_t *attribute);
int pthread_rwlock_destroy(pthread_rwlock_t *lock);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);

int pthread_barrierattr_init(pthread_barrierattr_t *attribute);
int pthread_barrierattr_destroy(pthread_barrierattr_t *attribute);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *attribute, int shared);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attribute,
                                   int *shared);
int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attribute,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

int pthread_spin_init(pthread_spinlock_t *lock, int shared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

int pthread_create(pthread_t *thread, const pthread_attr_t *attribute,
                   void *(*start_routine)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
void pthread_exit(void *result) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);
