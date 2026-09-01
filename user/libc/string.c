#include "liteos/libc.h"

static int ascii_lower(int value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static bool is_delimiter(char value, const char *delimiters) {
    if (delimiters == 0) return false;
    while (*delimiters != '\0') {
        if (*delimiters++ == value) return true;
    }
    return false;
}

char *strchrnul(const char *text, int value) {
    if (text == 0) return 0;
    while (*text != '\0' && (unsigned char)*text != (unsigned char)value) ++text;
    return (char *)text;
}

char *strcat(char *destination, const char *source) {
    char *cursor;
    if (destination == 0 || source == 0) return destination;
    cursor = destination + strlen(destination);
    while ((*cursor++ = *source++) != '\0') { }
    return destination;
}

char *strncat(char *destination, const char *source, size_t length) {
    char *cursor;
    if (destination == 0 || source == 0) return destination;
    cursor = destination + strlen(destination);
    while (length != 0U && *source != '\0') {
        *cursor++ = *source++;
        --length;
    }
    *cursor = '\0';
    return destination;
}

size_t strspn(const char *text, const char *accept) {
    size_t length = 0U;
    if (text == 0 || accept == 0) return 0U;
    while (text[length] != '\0' && is_delimiter(text[length], accept)) ++length;
    return length;
}

size_t strcspn(const char *text, const char *reject) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0' && !is_delimiter(text[length], reject)) ++length;
    return length;
}

char *strpbrk(const char *text, const char *accept) {
    size_t index;
    if (text == 0 || accept == 0) return 0;
    for (index = 0U; text[index] != '\0'; ++index) {
        if (is_delimiter(text[index], accept)) return (char *)(text + index);
    }
    return 0;
}

size_t strxfrm(char *destination, const char *source, size_t length) {
    size_t source_length;
    if (source == 0) {
        errno = EINVAL;
        return 0U;
    }
    source_length = strlen(source);
    if (destination != 0 && length != 0U) {
        size_t copy_length = source_length < length - 1U ? source_length : length - 1U;
        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

int strcoll(const char *left, const char *right) {
    return strcmp(left, right);
}

char *stpcpy(char *destination, const char *source) {
    if (destination == 0 || source == 0) return destination;
    while ((*destination = *source) != '\0') {
        ++destination;
        ++source;
    }
    return destination;
}

char *stpncpy(char *destination, const char *source, size_t length) {
    char *result = destination;
    if (destination == 0 || source == 0) return destination;
    while (length != 0U && *source != '\0') {
        *destination++ = *source++;
        --length;
    }
    result = destination;
    while (length-- != 0U) *destination++ = '\0';
    return result;
}

size_t strlcpy(char *destination, const char *source, size_t capacity) {
    size_t source_length;
    size_t copy_length;
    if (source == 0) {
        errno = EINVAL;
        return 0U;
    }
    source_length = strlen(source);
    if (destination == 0 || capacity == 0U) return source_length;
    copy_length = source_length < capacity - 1U ? source_length : capacity - 1U;
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
    return source_length;
}

size_t strlcat(char *destination, const char *source, size_t capacity) {
    size_t destination_length;
    size_t source_length;
    size_t copy_length;
    if (destination == 0 || source == 0) {
        errno = EINVAL;
        return 0U;
    }
    destination_length = strnlen(destination, capacity);
    source_length = strlen(source);
    if (destination_length == capacity) return capacity + source_length;
    copy_length = source_length < capacity - destination_length - 1U ?
                  source_length : capacity - destination_length - 1U;
    memcpy(destination + destination_length, source, copy_length);
    destination[destination_length + copy_length] = '\0';
    return destination_length + source_length;
}

char *strcasestr(const char *text, const char *needle) {
    size_t needle_length;
    if (text == 0 || needle == 0) return 0;
    needle_length = strlen(needle);
    if (needle_length == 0U) return (char *)text;
    for (; *text != '\0'; ++text) {
        if (strncasecmp(text, needle, needle_length) == 0) return (char *)text;
    }
    return 0;
}

char *strtok_r(char *text, const char *delimiters, char **state) {
    char *start;
    char *end;
    if (state == 0 || delimiters == 0) return 0;
    if (text == 0) text = *state;
    if (text == 0) return 0;
    while (*text != '\0' && is_delimiter(*text, delimiters)) ++text;
    if (*text == '\0') {
        *state = text;
        return 0;
    }
    start = text;
    while (*text != '\0' && !is_delimiter(*text, delimiters)) ++text;
    end = text;
    if (*end != '\0') ++text;
    if (*end != '\0') *end = '\0';
    *state = text;
    return start;
}

char *strtok(char *text, const char *delimiters) {
    static char *state;
    return strtok_r(text, delimiters, &state);
}

char *strdup(const char *text) {
    size_t length;
    char *copy;
    if (text == 0) {
        errno = EINVAL;
        return 0;
    }
    length = strlen(text);
    if (length == SIZE_MAX) {
        errno = EOVERFLOW;
        return 0;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == 0) return 0;
    memcpy(copy, text, length + 1U);
    return copy;
}

char *strndup(const char *text, size_t length) {
    size_t actual;
    char *copy;
    if (text == 0) {
        errno = EINVAL;
        return 0;
    }
    actual = strnlen(text, length);
    if (actual == SIZE_MAX) {
        errno = EOVERFLOW;
        return 0;
    }
    copy = (char *)malloc(actual + 1U);
    if (copy == 0) return 0;
    memcpy(copy, text, actual);
    copy[actual] = '\0';
    return copy;
}

int strcasecmp(const char *left, const char *right) {
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (*left != '\0' && ascii_lower((unsigned char)*left) ==
           ascii_lower((unsigned char)*right)) {
        ++left;
        ++right;
    }
    return ascii_lower((unsigned char)*left) < ascii_lower((unsigned char)*right) ? -1 :
           ascii_lower((unsigned char)*left) > ascii_lower((unsigned char)*right) ? 1 : 0;
}

int strncasecmp(const char *left, const char *right, size_t length) {
    if (length == 0U) return 0;
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (length-- != 0U) {
        int a = ascii_lower((unsigned char)*left);
        int b = ascii_lower((unsigned char)*right);
        if (a != b) return a < b ? -1 : 1;
        if (a == '\0') return 0;
        ++left;
        ++right;
    }
    return 0;
}

typedef struct error_message {
    int number;
    const char *text;
} error_message_t;

static const error_message_t g_error_messages[] = {
    {EPERM, "Operation not permitted"},
    {ENOENT, "No such file or directory"},
    {ESRCH, "No such process"},
    {EINTR, "Interrupted system call"},
    {EIO, "Input/output error"},
    {ENXIO, "No such device or address"},
    {E2BIG, "Argument list too long"},
    {ENOEXEC, "Exec format error"},
    {EBADF, "Bad file descriptor"},
    {ECHILD, "No child processes"},
    {EAGAIN, "Resource temporarily unavailable"},
    {ENOMEM, "Cannot allocate memory"},
    {EACCES, "Permission denied"},
    {EFAULT, "Bad address"},
    {EBUSY, "Device or resource busy"},
    {EEXIST, "File exists"},
    {EXDEV, "Invalid cross-device link"},
    {ENODEV, "No such device"},
    {ENOTDIR, "Not a directory"},
    {EISDIR, "Is a directory"},
    {EINVAL, "Invalid argument"},
    {ENFILE, "Too many open files in system"},
    {EMFILE, "Too many open files"},
    {ENOTTY, "Inappropriate ioctl for device"},
    {ETXTBSY, "Text file busy"},
    {EFBIG, "File too large"},
    {ENOSPC, "No space left on device"},
    {ESPIPE, "Illegal seek"},
    {EROFS, "Read-only file system"},
    {EMLINK, "Too many links"},
    {EPIPE, "Broken pipe"},
    {EDOM, "Numerical argument out of domain"},
    {ERANGE, "Numerical result out of range"},
    {EDEADLK, "Resource deadlock avoided"},
    {ENAMETOOLONG, "File name too long"},
    {ENOSYS, "Function not implemented"},
    {ENOTEMPTY, "Directory not empty"},
    {ELOOP, "Too many levels of symbolic links"},
    {ENOMSG, "No message of desired type"},
    {EIDRM, "Identifier removed"},
    {ENOLCK, "No locks available"},
    {ENOSR, "Out of streams resources"},
    {EILSEQ, "Invalid or incomplete multibyte or wide character"},
    {ENOTSOCK, "Socket operation on non-socket"},
    {EDESTADDRREQ, "Destination address required"},
    {EMSGSIZE, "Message too long"},
    {EPROTOTYPE, "Protocol wrong type for socket"},
    {ENOPROTOOPT, "Protocol not available"},
    {EPROTONOSUPPORT, "Protocol not supported"},
    {ESOCKTNOSUPPORT, "Socket type not supported"},
    {EPFNOSUPPORT, "Protocol family not supported"},
    {EAFNOSUPPORT, "Address family not supported by protocol"},
    {EADDRINUSE, "Address already in use"},
    {EADDRNOTAVAIL, "Cannot assign requested address"},
    {ENETDOWN, "Network is down"},
    {ENETUNREACH, "Network is unreachable"},
    {ENETRESET, "Network dropped connection on reset"},
    {ECONNABORTED, "Software caused connection abort"},
    {ECONNRESET, "Connection reset by peer"},
    {ENOBUFS, "No buffer space available"},
    {EISCONN, "Transport endpoint is already connected"},
    {ENOTCONN, "Transport endpoint is not connected"},
    {ETIMEDOUT, "Connection timed out"},
    {ECONNREFUSED, "Connection refused"},
    {EHOSTUNREACH, "No route to host"},
    {EALREADY, "Operation already in progress"},
    {EINPROGRESS, "Operation now in progress"},
    {ESTALE, "Stale file handle"},
    {EDQUOT, "Disk quota exceeded"},
    {ECANCELED, "Operation canceled"},
    {EOVERFLOW, "Value too large for data type"},
};

char *strerror(int error_number) {
    static char unknown[48];
    for (size_t index = 0U; index < sizeof(g_error_messages) /
         sizeof(g_error_messages[0]); ++index) {
        if (g_error_messages[index].number == error_number) {
            return (char *)g_error_messages[index].text;
        }
    }
    (void)snprintf(unknown, sizeof(unknown), "Unknown error %d", error_number);
    return unknown;
}

int strerror_r(int error_number, char *buffer, size_t length) {
    size_t message_length;
    if (buffer == 0 || length == 0U) {
        errno = EINVAL;
        return EINVAL;
    }
    message_length = strlen(strerror(error_number));
    if (message_length + 1U > length) {
        if (length > 1U) {
            memcpy(buffer, strerror(error_number), length - 1U);
            buffer[length - 1U] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return ERANGE;
    }
    memcpy(buffer, strerror(error_number), message_length + 1U);
    return 0;
}
