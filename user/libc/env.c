#include "liteos/libc.h"

#define LIBC_ENVIRONMENT_INITIAL_CAPACITY 16U

static char *g_empty_environment[1] = {0};
static bool g_environment_owned;
static size_t g_environment_capacity;

char **environ = g_empty_environment;

static size_t environment_count(void) {
    size_t count = 0U;
    if (environ == 0) return 0U;
    while (environ[count] != 0) ++count;
    return count;
}

static bool valid_name(const char *name) {
    return name != 0 && name[0] != '\0' && strchr(name, '=') == 0;
}

static bool entry_matches(const char *entry, const char *name,
                          size_t name_length) {
    return entry != 0 && strncmp(entry, name, name_length) == 0 &&
           entry[name_length] == '=';
}

static int find_entry(const char *name, size_t name_length) {
    size_t count = environment_count();
    for (size_t index = 0U; index < count; ++index) {
        if (entry_matches(environ[index], name, name_length)) return (int)index;
    }
    return -1;
}

static void free_owned_environment(char **environment, size_t count) {
    if (!environment) return;
    for (size_t index = 0U; index < count; ++index) free(environment[index]);
    free(environment);
}

static int make_environment_owned(size_t extra_entries) {
    size_t count = environment_count();
    size_t required = count + extra_entries + 1U;
    size_t capacity = g_environment_capacity;
    char **replacement;

    if (required < count || required > SIZE_MAX / sizeof(*replacement)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (g_environment_owned && capacity >= required) return 0;
    if (!g_environment_owned) {
        capacity = LIBC_ENVIRONMENT_INITIAL_CAPACITY;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        replacement = (char **)malloc(capacity * sizeof(*replacement));
        if (replacement == 0) return -1;
        for (size_t index = 0U; index < count; ++index) {
            replacement[index] = strdup(environ[index]);
            if (replacement[index] == 0) {
                for (size_t cleanup = 0U; cleanup < index; ++cleanup) {
                    free(replacement[cleanup]);
                }
                free(replacement);
                return -1;
            }
        }
        replacement[count] = 0;
        environ = replacement;
        g_environment_owned = true;
        g_environment_capacity = capacity;
        return 0;
    }
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    replacement = (char **)realloc(environ, capacity * sizeof(*replacement));
    if (replacement == 0) return -1;
    environ = replacement;
    g_environment_capacity = capacity;
    return 0;
}

void __libc_init_environment(char **environment) {
    if (g_environment_owned) {
        free_owned_environment(environ, environment_count());
    }
    environ = environment != 0 ? environment : g_empty_environment;
    g_environment_owned = false;
    g_environment_capacity = 0U;
}

char *getenv(const char *name) {
    size_t length;
    int index;
    if (!valid_name(name)) return 0;
    length = strlen(name);
    index = find_entry(name, length);
    return index < 0 ? 0 : environ[index] + length + 1U;
}

char *secure_getenv(const char *name) {
    /* LiteOS has no set-user-ID transition or privilege-changing exec path. */
    return getenv(name);
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t name_length;
    size_t value_length;
    size_t total;
    int index;
    char *assignment;

    if (!valid_name(name) || value == 0) {
        errno = EINVAL;
        return -1;
    }
    name_length = strlen(name);
    value_length = strlen(value);
    if (name_length > SIZE_MAX - 2U ||
        value_length > SIZE_MAX - name_length - 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    total = name_length + value_length + 2U;
    index = find_entry(name, name_length);
    if (index >= 0 && !overwrite) return 0;
    assignment = (char *)malloc(total);
    if (assignment == 0) return -1;
    memcpy(assignment, name, name_length);
    assignment[name_length] = '=';
    memcpy(assignment + name_length + 1U, value, value_length + 1U);
    if (make_environment_owned(index < 0 ? 1U : 0U) < 0) {
        free(assignment);
        return -1;
    }
    if (index >= 0) {
        free(environ[index]);
        environ[index] = assignment;
    } else {
        size_t count = environment_count();
        environ[count] = assignment;
        environ[count + 1U] = 0;
    }
    return 0;
}

int putenv(char *assignment) {
    char *separator;
    size_t name_length;
    int index;
    char *copy;
    if (assignment == 0 || assignment[0] == '\0' ||
        (separator = strchr(assignment, '=')) == 0 || separator == assignment) {
        errno = EINVAL;
        return -1;
    }
    name_length = (size_t)(separator - assignment);
    index = find_entry(assignment, name_length);
    copy = strdup(assignment);
    if (copy == 0) return -1;
    if (make_environment_owned(index < 0 ? 1U : 0U) < 0) {
        free(copy);
        return -1;
    }
    if (index >= 0) {
        free(environ[index]);
        environ[index] = copy;
    } else {
        size_t count = environment_count();
        environ[count] = copy;
        environ[count + 1U] = 0;
    }
    return 0;
}

int unsetenv(const char *name) {
    size_t name_length;
    size_t count;
    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    name_length = strlen(name);
    if (make_environment_owned(0U) < 0) return -1;
    count = environment_count();
    for (size_t index = 0U; index < count;) {
        if (!entry_matches(environ[index], name, name_length)) {
            ++index;
            continue;
        }
        free(environ[index]);
        for (size_t move = index + 1U; move <= count; ++move) {
            environ[move - 1U] = environ[move];
        }
        --count;
    }
    return 0;
}
