#include "liteos/libc.h"

#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#define PTHREAD_DEFAULT_STACK_SIZE (64U * 1024U)
#define PTHREAD_PAGE_SIZE 4096U

typedef struct pthread_start_context {
    pthread_t thread;
    void *(*start_routine)(void *);
    void *argument;
    int error_number;
} pthread_start_context_t;

struct liteos_pthread {
    os_handle_t handle;
    void *stack_address;
    size_t stack_size;
    bool owns_stack;
    volatile uint32_t lifecycle_lock;
    volatile bool joined;
    volatile bool detached;
    volatile bool exited;
    bool listed;
    bool cleanup;
    bool handle_ready;
    struct liteos_pthread *detached_next;
    pthread_start_context_t *context;
    void *specific[PTHREAD_KEYS_MAX];
    uint32_t specific_generation[PTHREAD_KEYS_MAX];
};

/* Keep pthread_mutex_t's historical one-word ABI.  Recursive ownership is
 * process-local metadata keyed by the public mutex address. */
typedef struct pthread_mutex_metadata {
    pthread_mutex_t *mutex;
    int type;
    pthread_t owner;
    uint32_t depth;
    bool owner_valid;
    struct pthread_mutex_metadata *next;
} pthread_mutex_metadata_t;

static volatile uint64_t g_key_bitmap;
static void (*g_key_destructors[PTHREAD_KEYS_MAX])(void *);
static uint32_t g_key_generation[PTHREAD_KEYS_MAX];
static volatile uint32_t g_key_lock;
static void *g_main_specific[PTHREAD_KEYS_MAX];
static uint32_t g_main_specific_generation[PTHREAD_KEYS_MAX];
static pthread_start_context_t g_main_context;
static struct liteos_pthread g_main_thread;
static volatile uint32_t g_detached_lock;
static pthread_t g_detached_threads;
static volatile uint32_t g_mutex_metadata_lock;
static pthread_mutex_metadata_t *g_mutex_metadata;

static void detached_lock(void) {
    while (__atomic_exchange_n(&g_detached_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void detached_unlock(void) {
    __atomic_store_n(&g_detached_lock, 0U, __ATOMIC_RELEASE);
}

static void thread_lifecycle_lock(pthread_t thread) {
    while (__atomic_exchange_n(&thread->lifecycle_lock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void thread_lifecycle_unlock(pthread_t thread) {
    __atomic_store_n(&thread->lifecycle_lock, 0U, __ATOMIC_RELEASE);
}

/* The caller must hold detached_lock(). */
static void detached_list_add_locked(pthread_t thread) {
    if (thread == 0) return;
    thread_lifecycle_lock(thread);
    if (!thread->listed && !thread->cleanup) {
        thread->detached_next = g_detached_threads;
        g_detached_threads = thread;
        thread->listed = true;
    }
    thread_lifecycle_unlock(thread);
}

static void detached_list_add(pthread_t thread) {
    if (thread == 0) return;
    detached_lock();
    detached_list_add_locked(thread);
    detached_unlock();
}

/* Reclaim detached records only after the kernel has published THREAD_DEAD. */
static void detached_reap(void) {
    pthread_t self = pthread_self();

    for (;;) {
        pthread_t candidate = 0;
        pthread_t *link = 0;
        os_wait_result_t wait_result = {0};
        int64_t status;

        detached_lock();
        for (pthread_t *entry = &g_detached_threads; *entry != 0;
             entry = &(*entry)->detached_next) {
            pthread_t thread = *entry;
            if (thread == self) {
                continue;
            }
            thread_lifecycle_lock(thread);
            if (!thread->handle_ready ||
                !__atomic_load_n(&thread->exited, __ATOMIC_ACQUIRE) ||
                thread->cleanup) {
                thread_lifecycle_unlock(thread);
                continue;
            }
            candidate = thread;
            link = entry;
            thread->cleanup = true;
            thread->listed = false;
            *link = thread->detached_next;
            thread->detached_next = 0;
            thread_lifecycle_unlock(thread);
            break;
        }
        detached_unlock();
        if (candidate == 0) return;

        status = liteos_syscall6(OS_SYS_WAIT_ONE, candidate->handle, 0U,
                                 (uint64_t)(uintptr_t)&wait_result,
                                 0U, 0U, 0U);
        if (status >= 0) {
            (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, candidate->handle,
                                  0U, 0U, 0U, 0U, 0U);
            free(candidate->context);
            free(candidate);
            continue;
        }

        /* The exit flag can precede kernel publication; retry on a later API. */
        detached_lock();
        thread_lifecycle_lock(candidate);
        candidate->cleanup = false;
        candidate->detached_next = g_detached_threads;
        g_detached_threads = candidate;
        candidate->listed = true;
        thread_lifecycle_unlock(candidate);
        detached_unlock();
        return;
    }
}

void __libc_reap_detached_threads(void) {
    detached_reap();
}

int __libc_thread_init(void) {
    g_main_context.thread = &g_main_thread;
    g_main_thread.context = &g_main_context;
    int64_t status = liteos_syscall6(
        OS_SYS_THREAD_CONTEXT, OS_THREAD_CONTEXT_SET_FS,
        (uint64_t)(uintptr_t)&g_main_context, 0U, 0U, 0U, 0U);
    return status < 0 ? -1 : 0;
}

int *__libc_thread_errno_slot(void) {
    pthread_t thread = pthread_self();
    return thread == 0 ? &__libc_errno : &thread->context->error_number;
}

#define PTHREAD_RWLOCK_WRITER 0x80000000U
#define PTHREAD_RWLOCK_READERS 0x7fffffffU

static int pthread_invalid(void) {
    return EINVAL;
}

static int pthread_status_error(int64_t status) {
    int saved_errno;
    int error;
    if (status >= 0) return 0;
    /* pthread APIs return errors directly and must not clobber errno. */
    saved_errno = errno;
    (void)__libc_status_result(status);
    error = errno != 0 ? errno : EIO;
    errno = saved_errno;
    return error;
}

static int futex_wait(volatile uint32_t *address, uint32_t expected,
                      uint64_t timeout_ns) {
    return pthread_status_error(liteos_syscall6(
        OS_SYS_FUTEX_WAIT, (uint64_t)(uintptr_t)address, expected,
        timeout_ns, 0U, 0U, 0U));
}

static int futex_wake(volatile uint32_t *address, uint32_t count) {
    int64_t status = liteos_syscall6(OS_SYS_FUTEX_WAKE,
                                     (uint64_t)(uintptr_t)address, count,
                                     0U, 0U, 0U, 0U);
    return status < 0 ? pthread_status_error(status) : 0;
}

static void key_lock(void) {
    while (__atomic_exchange_n(&g_key_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void key_unlock(void) {
    __atomic_store_n(&g_key_lock, 0U, __ATOMIC_RELEASE);
}

static bool key_is_valid_locked(pthread_key_t key) {
    return key < PTHREAD_KEYS_MAX &&
           ((__atomic_load_n(&g_key_bitmap, __ATOMIC_RELAXED) >> key) & 1U) != 0U;
}

static void run_key_destructors(pthread_t thread) {
    void **values = thread == 0 ? g_main_specific : thread->specific;
    uint32_t *generations = thread == 0 ? g_main_specific_generation :
                                         thread->specific_generation;
    for (unsigned int pass = 0U; pass < PTHREAD_DESTRUCTOR_ITERATIONS; ++pass) {
        bool called = false;
        for (pthread_key_t key = 0U; key < PTHREAD_KEYS_MAX; ++key) {
            void *value = 0;
            void (*destructor)(void *) = 0;

            /* Snapshot one slot under the key lock, then call user code
             * without the lock so destructors may use key APIs. */
            key_lock();
            if (key_is_valid_locked(key) &&
                generations[key] == g_key_generation[key] &&
                values[key] != 0) {
                value = values[key];
                values[key] = 0;
                generations[key] = 0;
                destructor = g_key_destructors[key];
            }
            key_unlock();
            if (destructor != 0) {
                called = true;
                destructor(value);
            }
        }
        if (!called) return;
    }
}

static size_t page_round(size_t size) {
    if (size == 0U || size > SIZE_MAX - (PTHREAD_PAGE_SIZE - 1U)) return 0U;
    return (size + PTHREAD_PAGE_SIZE - 1U) &
           ~(size_t)(PTHREAD_PAGE_SIZE - 1U);
}

static int absolute_timeout(const struct timespec *absolute,
                            uint64_t *timeout_ns) {
    struct timespec now;
    uint64_t seconds;
    uint64_t nanoseconds;
    if (absolute == 0 || timeout_ns == 0 || absolute->tv_nsec < 0 ||
        absolute->tv_nsec >= 1000000000L) return EINVAL;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return errno;
    if (absolute->tv_sec < now.tv_sec ||
        (absolute->tv_sec == now.tv_sec &&
         absolute->tv_nsec <= now.tv_nsec)) {
        *timeout_ns = 0U;
        return 0;
    }
    seconds = (uint64_t)(absolute->tv_sec - now.tv_sec);
    if (absolute->tv_nsec < now.tv_nsec) {
        --seconds;
        nanoseconds = (uint64_t)absolute->tv_nsec + 1000000000ULL -
                      (uint64_t)now.tv_nsec;
    } else {
        nanoseconds = (uint64_t)absolute->tv_nsec -
                      (uint64_t)now.tv_nsec;
    }
    if (seconds > (UINT64_MAX - nanoseconds) / 1000000000ULL) {
        *timeout_ns = UINT64_MAX;
    } else {
        *timeout_ns = seconds * 1000000000ULL + nanoseconds;
    }
    return 0;
}

int pthread_attr_init(pthread_attr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->stack_address = 0;
    attribute->stack_size = PTHREAD_DEFAULT_STACK_SIZE;
    attribute->guard_size = 0U;
    attribute->detach_state = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attribute, size_t size) {
    if (attribute == 0 || size < PTHREAD_STACK_MIN) return pthread_invalid();
    if (page_round(size) == 0U) return EOVERFLOW;
    attribute->stack_address = 0;
    attribute->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attribute, size_t *size) {
    if (attribute == 0 || size == 0) return pthread_invalid();
    *size = attribute->stack_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attribute, size_t size) {
    size_t rounded;
    if (attribute == 0) return pthread_invalid();
    if (size == 0U) {
        attribute->guard_size = 0U;
        return 0;
    }
    rounded = page_round(size);
    if (rounded == 0U) return EOVERFLOW;
    attribute->guard_size = rounded;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attribute, size_t *size) {
    if (attribute == 0 || size == 0) return pthread_invalid();
    *size = attribute->guard_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *attribute, void *address,
                          size_t size) {
    if (attribute == 0 || address == 0 || size < PTHREAD_STACK_MIN ||
        (uintptr_t)address > UINTPTR_MAX - size) return pthread_invalid();
    attribute->stack_address = address;
    attribute->stack_size = size;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attribute, void **address,
                          size_t *size) {
    if (attribute == 0 || address == 0 || size == 0) return pthread_invalid();
    *address = attribute->stack_address;
    *size = attribute->stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attribute, int state) {
    if (attribute == 0 ||
        (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
        return pthread_invalid();
    }
    attribute->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attribute, int *state) {
    if (attribute == 0 || state == 0) return pthread_invalid();
    *state = attribute->detach_state;
    return 0;
}

static void mutex_metadata_lock(void) {
    while (__atomic_exchange_n(&g_mutex_metadata_lock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void mutex_metadata_unlock(void) {
    __atomic_store_n(&g_mutex_metadata_lock, 0U, __ATOMIC_RELEASE);
}

/* The caller must hold g_mutex_metadata_lock. */
static pthread_mutex_metadata_t *mutex_metadata_find_locked(
    pthread_mutex_t *mutex) {
    for (pthread_mutex_metadata_t *entry = g_mutex_metadata; entry != 0;
         entry = entry->next) {
        if (entry->mutex == mutex) return entry;
    }
    return 0;
}

/* The caller must hold g_mutex_metadata_lock. */
static pthread_mutex_metadata_t *mutex_metadata_remove_locked(
    pthread_mutex_t *mutex) {
    pthread_mutex_metadata_t **link = &g_mutex_metadata;
    while (*link != 0) {
        pthread_mutex_metadata_t *entry = *link;
        if (entry->mutex == mutex) {
            *link = entry->next;
            entry->next = 0;
            return entry;
        }
        link = &entry->next;
    }
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->type = PTHREAD_MUTEX_NORMAL;
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attribute, int type) {
    if (attribute == 0) return pthread_invalid();
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
        type != PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    attribute->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attribute, int *type) {
    if (attribute == 0 || type == 0) return pthread_invalid();
    *type = attribute->type;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attribute,
                                 int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attribute) {
    pthread_mutex_metadata_t *entry;
    pthread_mutex_metadata_t *removed = 0;
    int type = attribute == 0 ? PTHREAD_MUTEX_NORMAL : attribute->type;
    if (mutex == 0 ||
        (attribute != 0 && attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
         attribute->process_shared != PTHREAD_PROCESS_SHARED) ||
        (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
         type != PTHREAD_MUTEX_ERRORCHECK)) {
        return pthread_invalid();
    }
    if (attribute != 0 && attribute->process_shared == PTHREAD_PROCESS_SHARED &&
        type != PTHREAD_MUTEX_NORMAL) return ENOTSUP;

    /* Metadata is private to this libc image, so the public mutex layout stays
     * compatible with the original one-word state and static initializer. */
    mutex_metadata_lock();
    if (__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) != 0U) {
        mutex_metadata_unlock();
        return EBUSY;
    }
    entry = mutex_metadata_find_locked(mutex);
    if (type == PTHREAD_MUTEX_RECURSIVE || type == PTHREAD_MUTEX_ERRORCHECK) {
        if (entry == 0) {
            entry = (pthread_mutex_metadata_t *)malloc(sizeof(*entry));
            if (entry == 0) {
                mutex_metadata_unlock();
                return ENOMEM;
            }
            entry->mutex = mutex;
            entry->next = g_mutex_metadata;
            g_mutex_metadata = entry;
        }
        entry->type = type;
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
    } else if (entry != 0) {
        removed = mutex_metadata_remove_locked(mutex);
    }
    __atomic_store_n(&mutex->state, 0U, __ATOMIC_RELAXED);
    mutex_metadata_unlock();
    free(removed);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    pthread_mutex_metadata_t *removed;
    if (mutex == 0) return pthread_invalid();
    mutex_metadata_lock();
    if (__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) != 0U) {
        mutex_metadata_unlock();
        return EBUSY;
    }
    removed = mutex_metadata_remove_locked(mutex);
    mutex_metadata_unlock();
    free(removed);
    return 0;
}

static int mutex_try_acquire(pthread_mutex_t *mutex, bool try_only) {
    pthread_mutex_metadata_t *entry;
    pthread_t self = 0;
    uint32_t expected = 0U;
    int result;

    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (entry->owner_valid && entry->owner == self) {
            if (entry->type == PTHREAD_MUTEX_ERRORCHECK) {
                result = try_only ? EBUSY : EDEADLK;
            } else if (entry->depth == UINT32_MAX) {
                result = EAGAIN;
            } else {
                ++entry->depth;
                result = 0;
            }
            mutex_metadata_unlock();
            return result;
        }
    }
    if (!__atomic_compare_exchange_n(&mutex->state, &expected, 1U, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex_metadata_unlock();
        return EBUSY;
    }
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        entry->owner = self;
        entry->owner_valid = true;
        entry->depth = 1U;
    }
    mutex_metadata_unlock();
    return 0;
}

static int mutex_lock_until(pthread_mutex_t *mutex, uint64_t timeout_ns,
                            bool timed) {
    for (;;) {
        int error = mutex_try_acquire(mutex, false);
        if (error == 0 || error == EAGAIN) return error;
        if (error != EBUSY) return error;
        error = futex_wait(&mutex->state, 1U,
                           timed ? timeout_ns : OS_WAIT_INFINITE);
        if (error == EAGAIN) continue;
        if (error != 0) return error;
    }
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return mutex == 0 ? pthread_invalid() : mutex_lock_until(mutex, 0U, false);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (mutex == 0) return pthread_invalid();
    return mutex_try_acquire(mutex, true);
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abstime) {
    uint64_t timeout_ns;
    int error;
    if (mutex == 0) return pthread_invalid();
    error = absolute_timeout(abstime, &timeout_ns);
    if (error != 0) return error;
    for (;;) {
        error = mutex_try_acquire(mutex, false);
        if (error == 0 || error == EAGAIN) return error;
        if (error != EBUSY) return error;
        error = futex_wait(&mutex->state, 1U, timeout_ns);
        if (error == EAGAIN) {
            error = absolute_timeout(abstime, &timeout_ns);
            if (error != 0) return error;
            continue;
        }
        if (error != 0) return error;
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    pthread_mutex_metadata_t *entry;
    pthread_t self;
    if (mutex == 0) return pthread_invalid();

    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (!entry->owner_valid || entry->owner != self) {
            mutex_metadata_unlock();
            return EPERM;
        }
        if (entry->type == PTHREAD_MUTEX_RECURSIVE && entry->depth > 1U) {
            --entry->depth;
            mutex_metadata_unlock();
            return 0;
        }
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
        __atomic_store_n(&mutex->state, 0U, __ATOMIC_RELEASE);
        mutex_metadata_unlock();
        return futex_wake(&mutex->state, 1U);
    }
    mutex_metadata_unlock();
    if (__atomic_exchange_n(&mutex->state, 0U, __ATOMIC_RELEASE) == 0U) {
        return EPERM;
    }
    return futex_wake(&mutex->state, 1U);
}

int pthread_condattr_init(pthread_condattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->clock_id = CLOCK_REALTIME;
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attribute, clockid_t clock_id) {
    if (attribute == 0) return pthread_invalid();
    if (clock_id != CLOCK_REALTIME) return ENOTSUP;
    attribute->clock_id = clock_id;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attribute,
                              clockid_t *clock_id) {
    if (attribute == 0 || clock_id == 0) return pthread_invalid();
    *clock_id = attribute->clock_id;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attribute,
                                int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_cond_init(pthread_cond_t *condition,
                      const pthread_condattr_t *attribute) {
    if (condition == 0 || (attribute != 0 &&
        attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
        attribute->process_shared != PTHREAD_PROCESS_SHARED)) {
        return pthread_invalid();
    }
    __atomic_store_n(&condition->sequence, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition) {
    return condition == 0 ? pthread_invalid() : 0;
}

/* POSIX requires a condition wait to release the mutex completely.  Keep the
 * recursive depth in the private metadata and restore it after the wait so a
 * recursive mutex has the same ownership contract on both sides. */
static int mutex_release_for_condition(pthread_mutex_t *mutex,
                                       uint32_t *depth) {
    pthread_mutex_metadata_t *entry;
    pthread_t self;
    uint32_t state;

    if (depth == 0) return pthread_invalid();
    *depth = 1U;
    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (!entry->owner_valid || entry->owner != self || entry->depth == 0U) {
            mutex_metadata_unlock();
            return EPERM;
        }
        *depth = entry->type == PTHREAD_MUTEX_RECURSIVE ? entry->depth : 1U;
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
        __atomic_store_n(&mutex->state, 0U, __ATOMIC_RELEASE);
        mutex_metadata_unlock();
        return futex_wake(&mutex->state, 1U);
    }
    state = __atomic_exchange_n(&mutex->state, 0U, __ATOMIC_RELEASE);
    mutex_metadata_unlock();
    if (state == 0U) return EPERM;
    return futex_wake(&mutex->state, 1U);
}

static int mutex_restore_after_condition(pthread_mutex_t *mutex,
                                         uint32_t depth) {
    if (depth == 0U) return EINVAL;
    for (uint32_t level = 0U; level < depth; ++level) {
        int error = pthread_mutex_lock(mutex);
        if (error != 0) return error;
    }
    return 0;
}

static int condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                          uint64_t timeout_ns) {
    uint32_t sequence;
    uint32_t depth;
    int error;
    int restore_error;
    if (condition == 0 || mutex == 0) return pthread_invalid();
    sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    error = mutex_release_for_condition(mutex, &depth);
    if (error != 0) return error;
    error = futex_wait(&condition->sequence, sequence, timeout_ns);
    if (error == EAGAIN) error = 0;
    restore_error = mutex_restore_after_condition(mutex, depth);
    if (error == 0 && restore_error != 0) error = restore_error;
    return error;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return condition_wait(condition, mutex, OS_WAIT_INFINITE);
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    uint64_t timeout_ns;
    int error;
    if (condition == 0 || mutex == 0) return pthread_invalid();
    error = absolute_timeout(abstime, &timeout_ns);
    if (error != 0) return error;
    return condition_wait(condition, mutex, timeout_ns);
}

int pthread_cond_signal(pthread_cond_t *condition) {
    if (condition == 0) return pthread_invalid();
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    return futex_wake(&condition->sequence, 1U);
}

int pthread_cond_broadcast(pthread_cond_t *condition) {
    if (condition == 0) return pthread_invalid();
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    return futex_wake(&condition->sequence, UINT32_MAX);
}

int pthread_once(pthread_once_t *once, void (*function)(void)) {
    if (once == 0 || function == 0) return pthread_invalid();
    for (;;) {
        uint32_t state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (state == 2U) return 0;
        if (state == 0U) {
            uint32_t expected = 0U;
            if (__atomic_compare_exchange_n(&once->state, &expected, 1U, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                function();
                __atomic_store_n(&once->state, 2U, __ATOMIC_RELEASE);
                return futex_wake(&once->state, UINT32_MAX);
            }
            continue;
        }
        if (state != 1U) return EINVAL;
        if (futex_wait(&once->state, 1U, OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (key == 0) return pthread_invalid();
    key_lock();
    for (pthread_key_t index = 0U; index < PTHREAD_KEYS_MAX; ++index) {
        uint64_t bit = 1ULL << index;
        if ((__atomic_load_n(&g_key_bitmap, __ATOMIC_RELAXED) & bit) == 0U) {
            uint32_t generation = g_key_generation[index] + 1U;
            if (generation == 0U) generation = 1U;
            g_key_generation[index] = generation;
            g_key_destructors[index] = destructor;
            __atomic_fetch_or(&g_key_bitmap, bit, __ATOMIC_RELEASE);
            *key = index;
            key_unlock();
            return 0;
        }
    }
    key_unlock();
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    key_lock();
    if (!key_is_valid_locked(key)) {
        key_unlock();
        return EINVAL;
    }
    __atomic_fetch_and(&g_key_bitmap, ~(1ULL << key), __ATOMIC_RELEASE);
    g_key_destructors[key] = 0;
    /* The initial thread normally uses g_main_thread.specific.  Clear both
     * representations so a key can be deleted and reused without exposing
     * the previous value. */
    g_main_specific[key] = 0;
    g_main_specific_generation[key] = 0;
    g_main_thread.specific[key] = 0;
    g_main_thread.specific_generation[key] = 0;
    key_unlock();
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    pthread_t thread;
    void **values;
    uint32_t *generations;
    void *value = 0;
    thread = pthread_self();
    values = thread == 0 ? g_main_specific : thread->specific;
    generations = thread == 0 ? g_main_specific_generation :
                                thread->specific_generation;
    key_lock();
    if (key_is_valid_locked(key) &&
        generations[key] == g_key_generation[key]) {
        value = values[key];
    }
    key_unlock();
    return value;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    pthread_t thread;
    void **values;
    uint32_t *generations;
    thread = pthread_self();
    values = thread == 0 ? g_main_specific : thread->specific;
    generations = thread == 0 ? g_main_specific_generation :
                                thread->specific_generation;
    key_lock();
    if (!key_is_valid_locked(key)) {
        key_unlock();
        return EINVAL;
    }
    values[key] = (void *)value;
    generations[key] = value == 0 ? 0U : g_key_generation[key];
    key_unlock();
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attribute,
                                  int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *lock,
                        const pthread_rwlockattr_t *attribute) {
    if (lock == 0 || (attribute != 0 &&
        attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
        attribute->process_shared != PTHREAD_PROCESS_SHARED)) return EINVAL;
    if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) != 0U) return EBUSY;
    __atomic_store_n(&lock->state, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    return __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 0U ? 0 : EBUSY;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock) {
    uint32_t state;
    if (lock == 0) return pthread_invalid();
    state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
    while ((state & PTHREAD_RWLOCK_WRITER) == 0U) {
        if ((state & PTHREAD_RWLOCK_READERS) == PTHREAD_RWLOCK_READERS) return EAGAIN;
        if (__atomic_compare_exchange_n(&lock->state, &state, state + 1U,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) return 0;
    }
    return EBUSY;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
    int error;
    if (lock == 0) return pthread_invalid();
    for (;;) {
        error = pthread_rwlock_tryrdlock(lock);
        if (error == 0) return 0;
        if (error != EBUSY && error != EAGAIN) return error;
        uint32_t state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
        if (futex_wait(&lock->state, state, OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    uint32_t expected = 0U;
    if (lock == 0) return pthread_invalid();
    return __atomic_compare_exchange_n(&lock->state, &expected,
                                       PTHREAD_RWLOCK_WRITER, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    for (;;) {
        int error = pthread_rwlock_trywrlock(lock);
        if (error == 0) return 0;
        if (futex_wait(&lock->state,
                       __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE),
                       OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock) {
    uint32_t state;
    if (lock == 0) return pthread_invalid();
    state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_RWLOCK_WRITER) {
        uint32_t expected = PTHREAD_RWLOCK_WRITER;
        if (!__atomic_compare_exchange_n(&lock->state, &expected, 0U, false,
                                         __ATOMIC_RELEASE,
                                         __ATOMIC_RELAXED)) return EPERM;
        return futex_wake(&lock->state, UINT32_MAX);
    }
    for (;;) {
        uint32_t readers = state & PTHREAD_RWLOCK_READERS;
        if (state & PTHREAD_RWLOCK_WRITER || readers == 0U) return EPERM;
        if (__atomic_compare_exchange_n(&lock->state, &state, state - 1U,
                                        false, __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE)) {
            return readers == 1U ? futex_wake(&lock->state, UINT32_MAX) : 0;
        }
    }
}

int pthread_barrierattr_init(pthread_barrierattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attribute,
                                   int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attribute,
                         unsigned int count) {
    if (barrier == 0 || count == 0U || (attribute != 0 &&
        attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
        attribute->process_shared != PTHREAD_PROCESS_SHARED)) return EINVAL;
    barrier->threshold = count;
    __atomic_store_n(&barrier->arrived, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&barrier->generation, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    if (barrier == 0) return pthread_invalid();
    return __atomic_load_n(&barrier->arrived, __ATOMIC_ACQUIRE) == 0U ? 0 : EBUSY;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    uint32_t generation;
    uint32_t arrived;
    if (barrier == 0 || barrier->threshold == 0U) return pthread_invalid();
    generation = __atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE);
    arrived = __atomic_add_fetch(&barrier->arrived, 1U, __ATOMIC_ACQ_REL);
    if (arrived == barrier->threshold) {
        __atomic_store_n(&barrier->arrived, 0U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&barrier->generation, 1U, __ATOMIC_RELEASE);
        (void)futex_wake(&barrier->generation, UINT32_MAX);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (__atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE) == generation) {
        (void)futex_wait(&barrier->generation, generation, OS_WAIT_INFINITE);
    }
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int shared) {
    if (lock == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    __atomic_store_n(&lock->state, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    return lock == 0 ? pthread_invalid() : 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    uint32_t expected = 0U;
    if (lock == 0) return pthread_invalid();
    return __atomic_compare_exchange_n(&lock->state, &expected, 1U, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    while (pthread_spin_trylock(lock) != 0) __asm__ volatile("pause");
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    if (__atomic_exchange_n(&lock->state, 0U, __ATOMIC_RELEASE) == 0U) return EPERM;
    return 0;
}

static void pthread_thread_start(void *raw_context) {
    pthread_start_context_t *context = (pthread_start_context_t *)raw_context;
    void *result = context->start_routine(context->argument);
    pthread_exit(result);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attribute,
                   void *(*start_routine)(void *), void *argument) {
    pthread_attr_t defaults;
    pthread_t record;
    pthread_start_context_t *context;
    void *stack;
    void *mapping_base;
    size_t stack_size;
    size_t guard_size;
    size_t mapped_size;
    size_t mapping_size;
    size_t stack_extent;
    uintptr_t stack_top;
    os_thread_create_t arguments;
    os_thread_stack_t stack_arguments;
    os_handle_t handle = OS_INVALID_HANDLE;
    int64_t status;

    detached_reap();
    if (thread == 0 || start_routine == 0) return pthread_invalid();
    if (attribute == 0) {
        if (pthread_attr_init(&defaults) != 0) return EINVAL;
        attribute = &defaults;
    }
    if ((attribute->detach_state != PTHREAD_CREATE_JOINABLE &&
         attribute->detach_state != PTHREAD_CREATE_DETACHED) ||
        attribute->stack_size < PTHREAD_STACK_MIN) return EINVAL;
    stack_size = attribute->stack_size;
    guard_size = attribute->guard_size;
    mapped_size = page_round(stack_size);
    if (mapped_size == 0U) return EOVERFLOW;
    if (attribute->stack_address == 0) {
        if (guard_size > SIZE_MAX - mapped_size) return EOVERFLOW;
        mapping_size = guard_size + mapped_size;
        if (mapping_size == 0U) return EOVERFLOW;
    } else {
        mapping_size = mapped_size;
    }
    if (attribute->stack_address != 0) {
        stack = attribute->stack_address;
        mapping_base = stack;
        stack_extent = stack_size;
    } else {
        mapping_base = mmap(0, mapping_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mapping_base == MAP_FAILED) return errno != 0 ? errno : ENOMEM;
        if (guard_size != 0U &&
            mprotect(mapping_base, guard_size, PROT_NONE) < 0) {
            (void)munmap(mapping_base, mapping_size);
            return errno != 0 ? errno : EIO;
        }
        if ((uintptr_t)mapping_base > UINTPTR_MAX - guard_size) {
            (void)munmap(mapping_base, mapping_size);
            return EOVERFLOW;
        }
        stack = (void *)((uintptr_t)mapping_base + guard_size);
        stack_extent = mapped_size;
    }
    if ((uintptr_t)stack > UINTPTR_MAX - stack_extent) {
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return EOVERFLOW;
    }
    stack_top = ((uintptr_t)stack + stack_extent) & ~(uintptr_t)0x0fU;
    if (stack_top <= (uintptr_t)stack) {
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return EINVAL;
    }
    record = (pthread_t)malloc(sizeof(*record));
    context = (pthread_start_context_t *)malloc(sizeof(*context));
    if (record == 0 || context == 0) {
        free(record);
        free(context);
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return ENOMEM;
    }
    memset(record, 0, sizeof(*record));
    record->stack_address = stack;
    record->stack_size = stack_extent;
    record->owns_stack = attribute->stack_address == 0;
    record->lifecycle_lock = 0U;
    record->joined = false;
    record->detached = attribute->detach_state == PTHREAD_CREATE_DETACHED;
    record->exited = false;
    record->listed = false;
    record->cleanup = false;
    record->handle_ready = false;
    record->detached_next = 0;
    record->context = context;
    context->thread = record;
    context->start_routine = start_routine;
    context->argument = argument;
    context->error_number = 0;
    memset(&arguments, 0, sizeof(arguments));
    arguments.hdr.size = sizeof(arguments);
    arguments.hdr.version = OS_SYSCALL_ABI_VERSION;
    arguments.entry = (uint64_t)(uintptr_t)pthread_thread_start;
    arguments.stack_top = (uint64_t)stack_top;
    arguments.fs_base = (uint64_t)(uintptr_t)context;
    arguments.argument = (uint64_t)(uintptr_t)context;
    memset(&stack_arguments, 0, sizeof(stack_arguments));
    if (record->owns_stack) {
        stack_arguments.hdr.size = sizeof(stack_arguments);
        stack_arguments.hdr.version = OS_SYSCALL_ABI_VERSION;
        stack_arguments.base = (uint64_t)(uintptr_t)mapping_base;
        stack_arguments.size = (uint64_t)mapping_size;
        stack_arguments.flags = OS_THREAD_STACK_OWNED;
    }
    status = liteos_syscall6(OS_SYS_THREAD_CREATE, OS_INVALID_HANDLE,
                             (uint64_t)(uintptr_t)&arguments,
                             (uint64_t)(uintptr_t)&handle,
                             record->owns_stack ?
                                 (uint64_t)(uintptr_t)&stack_arguments : 0U,
                             0U, 0U);
    if (status < 0) {
        int error = pthread_status_error(status);
        free(context);
        free(record);
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return error;
    }
    record->handle = handle;
    record->handle_ready = true;
    if (record->detached) detached_list_add(record);
    *thread = record;
    return 0;
}

int pthread_join(pthread_t thread, void **result) {
    os_wait_result_t wait_result = {0};
    int64_t status;
    int error;
    if (thread == 0) {
        return pthread_invalid();
    }
    if (thread == pthread_self()) return EDEADLK;

    /* Claim join ownership before waiting so a second joiner cannot free the
     * same record after the first waiter completes. */
    thread_lifecycle_lock(thread);
    if (thread->joined || thread->detached || thread->cleanup) {
        thread_lifecycle_unlock(thread);
        return pthread_invalid();
    }
    thread->joined = true;
    thread_lifecycle_unlock(thread);

    status = liteos_syscall6(OS_SYS_WAIT_ONE, thread->handle,
                             OS_WAIT_INFINITE,
                             (uint64_t)(uintptr_t)&wait_result,
                             0U, 0U, 0U);
    error = pthread_status_error(status);
    if (error != 0) {
        thread_lifecycle_lock(thread);
        thread->joined = false;
        thread_lifecycle_unlock(thread);
        return error;
    }
    if (result != 0) *result = (void *)(uintptr_t)wait_result.value;
    (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, thread->handle,
                          0U, 0U, 0U, 0U, 0U);
    /* Owned stacks are reclaimed by the kernel after the context switch. */
    free(thread->context);
    free(thread);
    detached_reap();
    return 0;
}

int pthread_detach(pthread_t thread) {
    bool handle_ready;
    if (thread == 0) {
        return pthread_invalid();
    }

    thread_lifecycle_lock(thread);
    if (thread->joined || thread->detached || thread->cleanup) {
        thread_lifecycle_unlock(thread);
        return pthread_invalid();
    }
    thread->detached = true;
    handle_ready = thread->handle_ready;
    thread_lifecycle_unlock(thread);

    if (handle_ready) detached_list_add(thread);
    detached_reap();
    return 0;
}

void pthread_exit(void *result) {
    pthread_t thread = pthread_self();

    if (thread != 0) {
        run_key_destructors(thread);
        /*
         * The record and its FS context stay live until a joining caller (or
         * a deferred detached-thread reaper) observes WAIT_ONE completion.
         * In particular, neither the current user stack nor the FS object may
         * be unmapped/freed before THREAD_EXIT switches to another context.
         */
        __atomic_store_n(&thread->exited, true, __ATOMIC_RELEASE);
    }
    (void)liteos_syscall6(OS_SYS_THREAD_EXIT,
                          (uint64_t)(uintptr_t)result,
                          0U, 0U, 0U, 0U, 0U);
    for (;;) __asm__ volatile("pause");
}

pthread_t pthread_self(void) {
    pthread_t thread = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(thread));
    return thread;
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left == right;
}
