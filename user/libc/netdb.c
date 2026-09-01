#include "liteos/libc.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>

/*
 * The kernel deliberately keeps name resolution out of the syscall ABI.  The
 * libc resolver therefore owns the small, deterministic database used by the
 * base image.  Numeric addresses and localhost are always available; an
 * /etc/hosts file extends that set without introducing a second network
 * protocol implementation in the kernel.
 */
#define NETDB_HOST_BUFFER_SIZE 8192U
#define NETDB_HOST_NAME_SIZE   256U
#define NETDB_MAX_ALIASES      8U
#define NETDB_MAX_ADDRESSES    8U

int h_errno;

typedef struct netdb_host_state {
    struct hostent result;
    char name[NETDB_HOST_NAME_SIZE];
    char aliases[NETDB_MAX_ALIASES][NETDB_HOST_NAME_SIZE];
    char *alias_list[NETDB_MAX_ALIASES + 1U];
    unsigned char addresses[NETDB_MAX_ADDRESSES][16];
    char *address_list[NETDB_MAX_ADDRESSES + 1U];
    unsigned int alias_count;
    unsigned int address_count;
    int family;
    int length;
} netdb_host_state_t;

static netdb_host_state_t g_host_state;

static bool netdb_space(int value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\v' || value == '\f';
}

static bool netdb_copy_text(char *destination, size_t capacity,
                            const char *source) {
    size_t length;
    if (destination == 0 || source == 0) return false;
    length = strlen(source);
    if (length >= capacity) return false;
    memcpy(destination, source, length + 1U);
    return true;
}

/* Read one whitespace-delimited token, stopping cleanly at comments. */
static bool netdb_token(const char **cursor, char *token, size_t capacity) {
    const char *scan;
    size_t length = 0U;
    if (cursor == 0 || *cursor == 0 || token == 0 || capacity == 0U) return false;
    scan = *cursor;
    while (netdb_space((unsigned char)*scan)) ++scan;
    if (*scan == '\0' || *scan == '#') {
        *cursor = scan;
        token[0] = '\0';
        return false;
    }
    while (*scan != '\0' && !netdb_space((unsigned char)*scan) && *scan != '#') {
        if (length + 1U >= capacity) {
            while (*scan != '\0' && !netdb_space((unsigned char)*scan) &&
                   *scan != '#') ++scan;
            *cursor = scan;
            token[0] = '\0';
            return false;
        }
        token[length++] = *scan++;
    }
    token[length] = '\0';
    *cursor = scan;
    return true;
}

static void host_state_reset(netdb_host_state_t *state, int family) {
    unsigned int index;
    memset(state, 0, sizeof(*state));
    state->family = family;
    state->length = family == AF_INET ? 4 : 16;
    for (index = 0U; index <= NETDB_MAX_ALIASES; ++index) {
        state->alias_list[index] = index < NETDB_MAX_ALIASES ?
                                    state->aliases[index] : 0;
    }
    for (index = 0U; index <= NETDB_MAX_ADDRESSES; ++index) {
        state->address_list[index] = index < NETDB_MAX_ADDRESSES ?
                                     (char *)state->addresses[index] : 0;
    }
    state->result.h_name = state->name;
    state->result.h_aliases = state->alias_list;
    state->result.h_addrtype = family;
    state->result.h_length = state->length;
    state->result.h_addr_list = state->address_list;
}

static bool host_state_add_address(netdb_host_state_t *state,
                                   const void *address, const char *name) {
    unsigned int index;
    if (state == 0 || address == 0 || state->address_count >= NETDB_MAX_ADDRESSES) {
        return false;
    }
    memcpy(state->addresses[state->address_count], address,
           (size_t)state->length);
    ++state->address_count;
    if (state->name[0] == '\0' && name != 0 &&
        !netdb_copy_text(state->name, sizeof(state->name), name)) return false;
    if (name != 0 && state->name[0] == '\0') {
        if (!netdb_copy_text(state->name, sizeof(state->name), name)) return false;
    }
    for (index = 0U; index < state->address_count; ++index) {
        state->address_list[index] = (char *)state->addresses[index];
    }
    state->address_list[state->address_count] = 0;
    return true;
}

static void host_state_add_alias(netdb_host_state_t *state, const char *alias) {
    unsigned int index;
    if (state == 0 || alias == 0 || *alias == '\0' ||
        state->alias_count >= NETDB_MAX_ALIASES) return;
    for (index = 0U; index < state->alias_count; ++index) {
        if (strcasecmp(state->aliases[index], alias) == 0) return;
    }
    if (!netdb_copy_text(state->aliases[state->alias_count],
                         sizeof(state->aliases[0]), alias)) return;
    state->alias_list[state->alias_count] = state->aliases[state->alias_count];
    ++state->alias_count;
    state->alias_list[state->alias_count] = 0;
}

static bool host_name_matches(const char *wanted, const char *canonical,
                              char aliases[][NETDB_HOST_NAME_SIZE],
                              unsigned int alias_count) {
    unsigned int index;
    if (wanted == 0 || canonical == 0) return false;
    if (strcasecmp(wanted, canonical) == 0) return true;
    for (index = 0U; index < alias_count; ++index) {
        if (strcasecmp(wanted, aliases[index]) == 0) return true;
    }
    return false;
}

static bool host_state_from_hosts(const char *wanted, int family,
                                  netdb_host_state_t *state,
                                  const void *address_filter) {
    char contents[NETDB_HOST_BUFFER_SIZE];
    char address_text[64];
    char canonical[NETDB_HOST_NAME_SIZE];
    char aliases[NETDB_MAX_ALIASES][NETDB_HOST_NAME_SIZE];
    char *line;
    int descriptor;
    ssize_t bytes;
    bool found = false;
    descriptor = open("/etc/hosts", O_RDONLY);
    if (descriptor < 0) return false;
    bytes = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (bytes <= 0) return false;
    contents[(size_t)bytes] = '\0';
    line = contents;
    while (*line != '\0') {
        const char *cursor;
        char *next_line = strchr(line, '\n');
        struct in_addr address4;
        struct in6_addr address6;
        unsigned int alias_count = 0U;
        bool parsed;
        if (next_line != 0) *next_line = '\0';
        cursor = line;
        if (!netdb_token(&cursor, address_text, sizeof(address_text)) ||
            !netdb_token(&cursor, canonical, sizeof(canonical))) {
            if (next_line == 0) break;
            line = next_line + 1;
            continue;
        }
        while (alias_count < NETDB_MAX_ALIASES &&
               netdb_token(&cursor, aliases[alias_count],
                           sizeof(aliases[alias_count]))) {
            ++alias_count;
        }
        parsed = family == AF_INET ? inet_pton(AF_INET, address_text, &address4) == 1 :
                                      inet_pton(AF_INET6, address_text, &address6) == 1;
        if (parsed && (wanted == 0 ||
                       host_name_matches(wanted, canonical, aliases, alias_count))) {
            const void *binary = family == AF_INET ? (const void *)&address4 :
                                                       (const void *)&address6;
            if (address_filter != 0 &&
                memcmp(binary, address_filter, (size_t)state->length) != 0) {
                parsed = false;
            }
            if (parsed) {
                unsigned int index;
                if (!found) {
                    (void)netdb_copy_text(state->name, sizeof(state->name), canonical);
                    for (index = 0U; index < alias_count; ++index) {
                        host_state_add_alias(state, aliases[index]);
                    }
                }
                found = host_state_add_address(state, binary, canonical) || found;
            }
        }
        if (next_line == 0) break;
        line = next_line + 1;
    }
    return found;
}

static bool host_state_numeric(const char *name, int family,
                               netdb_host_state_t *state) {
    struct in_addr address4;
    struct in6_addr address6;
    if (name == 0) return false;
    if (family == AF_INET && inet_pton(AF_INET, name, &address4) == 1) {
        (void)netdb_copy_text(state->name, sizeof(state->name), name);
        return host_state_add_address(state, &address4, name);
    }
    if (family == AF_INET6 && inet_pton(AF_INET6, name, &address6) == 1) {
        (void)netdb_copy_text(state->name, sizeof(state->name), name);
        return host_state_add_address(state, &address6, name);
    }
    return false;
}

static bool host_state_lookup(const char *name, int family,
                              netdb_host_state_t *state) {
    struct in_addr loop4;
    struct in6_addr loop6;
    host_state_reset(state, family);
    if (name == 0 || *name == '\0') return false;
    if (host_state_numeric(name, family, state)) return true;
    if (strcasecmp(name, "localhost") == 0 ||
        (family == AF_INET6 && strcasecmp(name, "ip6-localhost") == 0)) {
        if (family == AF_INET) {
            loop4.s_addr = htonl(0x7F000001U);
            (void)netdb_copy_text(state->name, sizeof(state->name), "localhost");
            return host_state_add_address(state, &loop4, "localhost");
        }
        memset(&loop6, 0, sizeof(loop6));
        loop6.s6_addr[15] = 1U;
        (void)netdb_copy_text(state->name, sizeof(state->name),
                               "ip6-localhost");
        return host_state_add_address(state, &loop6, "ip6-localhost");
    }
    return host_state_from_hosts(name, family, state, 0);
}

static bool host_state_reverse(const void *address, socklen_t length,
                               int family, netdb_host_state_t *state) {
    unsigned char address_copy[16];

    if (address == 0 || state == 0 ||
        (family == AF_INET && length != sizeof(struct in_addr)) ||
        (family == AF_INET6 && length != sizeof(struct in6_addr))) return false;
    /* The non-reentrant API returns pointers into g_host_state.  Copy the
     * caller's address before resetting that state so gethostbyaddr() can
     * safely consume its own previous result. */
    memcpy(address_copy, address, (size_t)(family == AF_INET ?
                                           sizeof(struct in_addr) :
                                           sizeof(struct in6_addr)));
    host_state_reset(state, family);
    if (host_state_from_hosts(0, family, state, address_copy)) return true;
    if (family == AF_INET &&
        memcmp(address_copy, "\x7f\0\0\1", sizeof(struct in_addr)) == 0) {
        (void)netdb_copy_text(state->name, sizeof(state->name), "localhost");
        return host_state_add_address(state, address_copy, "localhost");
    }
    if (family == AF_INET6) {
        static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 1};
        if (memcmp(address_copy, loopback, sizeof(loopback)) == 0) {
            (void)netdb_copy_text(state->name, sizeof(state->name),
                                   "ip6-localhost");
            return host_state_add_address(state, address_copy, "ip6-localhost");
        }
    }
    {
        char text[INET6_ADDRSTRLEN];
        if (inet_ntop(family, address_copy, text, sizeof(text)) == 0) return false;
        (void)netdb_copy_text(state->name, sizeof(state->name), text);
        return host_state_add_address(state, address_copy, text);
    }
}

static int netdb_fail(int error_number, int system_error) {
    h_errno = error_number;
    if (system_error != 0) errno = system_error;
    return -1;
}

static size_t netdb_align(size_t value) {
    size_t alignment = sizeof(void *);
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int pack_hostent(const netdb_host_state_t *source,
                        struct hostent *result, char *buffer,
                        size_t buffer_length, struct hostent **result_pointer,
                        int *error_number) {
    size_t pointer_offset;
    size_t pointer_bytes;
    size_t cursor;
    size_t required;
    char **aliases;
    char **addresses;
    unsigned int index;
    if (result_pointer != 0) *result_pointer = 0;
    if (error_number != 0) *error_number = 0;
    if (source == 0 || result == 0 || buffer == 0) {
        if (error_number != 0) *error_number = NETDB_INTERNAL;
        return EINVAL;
    }
    pointer_offset = netdb_align((size_t)(uintptr_t)buffer) -
                     (size_t)(uintptr_t)buffer;
    pointer_bytes = (source->alias_count + 1U) * sizeof(char *) +
                    (source->address_count + 1U) * sizeof(char *);
    cursor = pointer_offset + pointer_bytes;
    required = cursor + strlen(source->name) + 1U;
    for (index = 0U; index < source->alias_count; ++index) {
        required += strlen(source->aliases[index]) + 1U;
    }
    required += (size_t)source->address_count * (size_t)source->length;
    if (required > buffer_length) {
        if (error_number != 0) *error_number = ERANGE;
        return ERANGE;
    }
    aliases = (char **)(void *)(buffer + pointer_offset);
    addresses = aliases + source->alias_count + 1U;
    cursor = pointer_offset + pointer_bytes;
    result->h_name = buffer + cursor;
    memcpy(result->h_name, source->name, strlen(source->name) + 1U);
    cursor += strlen(source->name) + 1U;
    for (index = 0U; index < source->alias_count; ++index) {
        aliases[index] = buffer + cursor;
        memcpy(aliases[index], source->aliases[index],
               strlen(source->aliases[index]) + 1U);
        cursor += strlen(source->aliases[index]) + 1U;
    }
    aliases[source->alias_count] = 0;
    for (index = 0U; index < source->address_count; ++index) {
        addresses[index] = buffer + cursor;
        memcpy(addresses[index], source->addresses[index],
               (size_t)source->length);
        cursor += (size_t)source->length;
    }
    addresses[source->address_count] = 0;
    result->h_aliases = aliases;
    result->h_addrtype = source->family;
    result->h_length = source->length;
    result->h_addr_list = addresses;
    if (result_pointer != 0) *result_pointer = result;
    return 0;
}

struct hostent *gethostbyname2(const char *name, int address_family) {
    if (address_family != AF_INET && address_family != AF_INET6) {
        h_errno = NO_DATA;
        errno = EAFNOSUPPORT;
        return 0;
    }
    if (!host_state_lookup(name, address_family, &g_host_state)) {
        h_errno = HOST_NOT_FOUND;
        errno = ENOENT;
        return 0;
    }
    h_errno = 0;
    return &g_host_state.result;
}

struct hostent *gethostbyname(const char *name) {
    return gethostbyname2(name, AF_INET);
}

struct hostent *gethostbyaddr(const void *address, socklen_t length,
                              int address_family) {
    if (address_family != AF_INET && address_family != AF_INET6) {
        (void)netdb_fail(NO_DATA, EAFNOSUPPORT);
        return 0;
    }
    if (!host_state_reverse(address, length, address_family, &g_host_state)) {
        (void)netdb_fail(HOST_NOT_FOUND, ENOENT);
        return 0;
    }
    h_errno = 0;
    return &g_host_state.result;
}

int gethostbyname_r(const char *name, struct hostent *result,
                    char *buffer, size_t buffer_length,
                    struct hostent **result_pointer, int *error_number) {
    netdb_host_state_t state;
    if (result_pointer != 0) *result_pointer = 0;
    if (error_number == 0 || result_pointer == 0) return EINVAL;
    if (!host_state_lookup(name, AF_INET, &state)) {
        *error_number = HOST_NOT_FOUND;
        return ENOENT;
    }
    return pack_hostent(&state, result, buffer, buffer_length,
                        result_pointer, error_number);
}

int gethostbyaddr_r(const void *address, socklen_t length,
                    int address_family, struct hostent *result,
                    char *buffer, size_t buffer_length,
                    struct hostent **result_pointer, int *error_number) {
    netdb_host_state_t state;
    if (result_pointer != 0) *result_pointer = 0;
    if (error_number == 0 || result_pointer == 0) return EINVAL;
    if (!host_state_reverse(address, length, address_family, &state)) {
        *error_number = HOST_NOT_FOUND;
        return ENOENT;
    }
    return pack_hostent(&state, result, buffer, buffer_length,
                        result_pointer, error_number);
}

const char *hstrerror(int error_number) {
    switch (error_number) {
    case 0: return "Resolver error 0 (no error)";
    case HOST_NOT_FOUND: return "Host name lookup failure";
    case TRY_AGAIN: return "Temporary resolver failure";
    case NO_RECOVERY: return "Non-recoverable resolver failure";
    case NO_DATA: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}

void herror(const char *prefix) {
    if (prefix != 0 && *prefix != '\0') {
        (void)fprintf(stderr, "%s: %s\n", prefix, hstrerror(h_errno));
    } else {
        (void)fprintf(stderr, "%s\n", hstrerror(h_errno));
    }
}

typedef struct netdb_service_entry {
    const char *name;
    const char *alias;
    uint16_t port;
    const char *protocol;
} netdb_service_entry_t;

static const netdb_service_entry_t g_services[] = {
    {"ftp", "file-transfer", 21U, "tcp"},
    {"ssh", 0, 22U, "tcp"},
    {"telnet", 0, 23U, "tcp"},
    {"domain", "dns", 53U, "udp"},
    {"http", "www", 80U, "tcp"},
    {"https", 0, 443U, "tcp"},
    {"ntp", 0, 123U, "udp"},
};

static const netdb_service_entry_t *service_lookup(const char *name,
                                                    int port,
                                                    const char *protocol) {
    size_t index;
    for (index = 0U; index < sizeof(g_services) / sizeof(g_services[0]); ++index) {
        const netdb_service_entry_t *entry = &g_services[index];
        if (protocol != 0 && strcasecmp(protocol, entry->protocol) != 0) continue;
        if (name != 0 && strcasecmp(name, entry->name) != 0 &&
            (entry->alias == 0 || strcasecmp(name, entry->alias) != 0)) continue;
        if (port >= 0 && ntohs((uint16_t)port) != (int)entry->port) continue;
        return entry;
    }
    return 0;
}

static int service_pack(const netdb_service_entry_t *entry,
                        struct servent *result, char *buffer,
                        size_t buffer_length, struct servent **result_pointer) {
    size_t pointer_offset;
    size_t required;
    size_t cursor;
    char **aliases;
    if (result_pointer != 0) *result_pointer = 0;
    if (entry == 0 || result == 0 || buffer == 0) return EINVAL;
    pointer_offset = netdb_align((size_t)(uintptr_t)buffer) -
                     (size_t)(uintptr_t)buffer;
    cursor = pointer_offset + sizeof(char *) * 2U;
    required = cursor + strlen(entry->name) + 1U +
               (entry->alias != 0 ? strlen(entry->alias) + 1U : 0U) +
               strlen(entry->protocol) + 1U;
    if (required > buffer_length) return ERANGE;
    aliases = (char **)(void *)(buffer + pointer_offset);
    result->s_name = buffer + cursor;
    memcpy(result->s_name, entry->name, strlen(entry->name) + 1U);
    cursor += strlen(entry->name) + 1U;
    if (entry->alias != 0) {
        aliases[0] = buffer + cursor;
        memcpy(aliases[0], entry->alias, strlen(entry->alias) + 1U);
        cursor += strlen(entry->alias) + 1U;
    } else {
        aliases[0] = 0;
    }
    aliases[1] = 0;
    result->s_aliases = aliases;
    result->s_port = htons(entry->port);
    result->s_proto = buffer + cursor;
    memcpy(result->s_proto, entry->protocol, strlen(entry->protocol) + 1U);
    if (result_pointer != 0) *result_pointer = result;
    return 0;
}

static struct servent g_service_result;
static char *g_service_aliases[2];

static struct servent *service_result(const netdb_service_entry_t *entry) {
    if (entry == 0) return 0;
    g_service_result.s_name = (char *)entry->name;
    g_service_aliases[0] = (char *)entry->alias;
    g_service_aliases[1] = 0;
    g_service_result.s_aliases = g_service_aliases;
    g_service_result.s_port = htons(entry->port);
    g_service_result.s_proto = (char *)entry->protocol;
    return &g_service_result;
}

struct servent *getservbyname(const char *name, const char *protocol) {
    return service_result(service_lookup(name, -1, protocol));
}

struct servent *getservbyport(int port, const char *protocol) {
    return service_result(service_lookup(0, port, protocol));
}

int getservbyname_r(const char *name, const char *protocol,
                    struct servent *result, char *buffer,
                    size_t buffer_length, struct servent **result_pointer) {
    const netdb_service_entry_t *entry = service_lookup(name, -1, protocol);
    int status;
    if (result_pointer == 0) return EINVAL;
    if (entry == 0) {
        *result_pointer = 0;
        return ENOENT;
    }
    status = service_pack(entry, result, buffer, buffer_length, result_pointer);
    return status;
}

int getservbyport_r(int port, const char *protocol,
                    struct servent *result, char *buffer,
                    size_t buffer_length, struct servent **result_pointer) {
    const netdb_service_entry_t *entry = service_lookup(0, port, protocol);
    int status;
    if (result_pointer == 0) return EINVAL;
    if (entry == 0) {
        *result_pointer = 0;
        return ENOENT;
    }
    status = service_pack(entry, result, buffer, buffer_length, result_pointer);
    return status;
}

typedef struct netdb_protocol_entry {
    const char *name;
    const char *alias;
    int number;
} netdb_protocol_entry_t;

static const netdb_protocol_entry_t g_protocols[] = {
    {"ip", 0, 0},
    {"tcp", 0, IPPROTO_TCP},
    {"udp", 0, IPPROTO_UDP},
};

static const netdb_protocol_entry_t *protocol_lookup(const char *name,
                                                      int number) {
    size_t index;
    for (index = 0U; index < sizeof(g_protocols) / sizeof(g_protocols[0]); ++index) {
        const netdb_protocol_entry_t *entry = &g_protocols[index];
        if (name != 0 && strcasecmp(name, entry->name) != 0 &&
            (entry->alias == 0 || strcasecmp(name, entry->alias) != 0)) continue;
        if (number >= 0 && number != entry->number) continue;
        return entry;
    }
    return 0;
}

static struct protoent g_protocol_result;
static char *g_protocol_aliases[2];

static struct protoent *protocol_result(const netdb_protocol_entry_t *entry) {
    if (entry == 0) return 0;
    g_protocol_result.p_name = (char *)entry->name;
    g_protocol_aliases[0] = (char *)entry->alias;
    g_protocol_aliases[1] = 0;
    g_protocol_result.p_aliases = g_protocol_aliases;
    g_protocol_result.p_proto = entry->number;
    return &g_protocol_result;
}

struct protoent *getprotobyname(const char *name) {
    return protocol_result(protocol_lookup(name, -1));
}

struct protoent *getprotobynumber(int number) {
    return protocol_result(protocol_lookup(0, number));
}

static int protocol_pack(const netdb_protocol_entry_t *entry,
                         struct protoent *result, char *buffer,
                         size_t buffer_length,
                         struct protoent **result_pointer) {
    size_t pointer_offset;
    size_t cursor;
    size_t required;
    char **aliases;
    if (result_pointer != 0) *result_pointer = 0;
    if (entry == 0 || result == 0 || buffer == 0) return EINVAL;
    pointer_offset = netdb_align((size_t)(uintptr_t)buffer) -
                     (size_t)(uintptr_t)buffer;
    cursor = pointer_offset + sizeof(char *) * 2U;
    required = cursor + strlen(entry->name) + 1U +
               (entry->alias != 0 ? strlen(entry->alias) + 1U : 0U);
    if (required > buffer_length) return ERANGE;
    aliases = (char **)(void *)(buffer + pointer_offset);
    result->p_name = buffer + cursor;
    memcpy(result->p_name, entry->name, strlen(entry->name) + 1U);
    cursor += strlen(entry->name) + 1U;
    if (entry->alias != 0) {
        aliases[0] = buffer + cursor;
        memcpy(aliases[0], entry->alias, strlen(entry->alias) + 1U);
    } else {
        aliases[0] = 0;
    }
    aliases[1] = 0;
    result->p_aliases = aliases;
    result->p_proto = entry->number;
    if (result_pointer != 0) *result_pointer = result;
    return 0;
}

int getprotobyname_r(const char *name, struct protoent *result,
                     char *buffer, size_t buffer_length,
                     struct protoent **result_pointer) {
    const netdb_protocol_entry_t *entry = protocol_lookup(name, -1);
    if (result_pointer == 0) return EINVAL;
    if (entry == 0) {
        *result_pointer = 0;
        return ENOENT;
    }
    return protocol_pack(entry, result, buffer, buffer_length, result_pointer);
}

int getprotobynumber_r(int number, struct protoent *result,
                       char *buffer, size_t buffer_length,
                       struct protoent **result_pointer) {
    const netdb_protocol_entry_t *entry = protocol_lookup(0, number);
    if (result_pointer == 0) return EINVAL;
    if (entry == 0) {
        *result_pointer = 0;
        return ENOENT;
    }
    return protocol_pack(entry, result, buffer, buffer_length, result_pointer);
}

#define NETDB_SUPPORTED_AI_FLAGS (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | \
                                  AI_NUMERICSERV)
#define NETDB_SUPPORTED_NI_FLAGS (NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN | \
                                  NI_NAMEREQD | NI_DGRAM)

static void free_addrinfo_list(struct addrinfo *result) {
    while (result != 0) {
        struct addrinfo *next = result->ai_next;
        free(result->ai_canonname);
        free(result->ai_addr);
        free(result);
        result = next;
    }
}

static int service_port(const char *service, int flags, uint16_t *port) {
    unsigned int value = 0U;
    const char *cursor;
    if (port == 0) return EAI_SYSTEM;
    if (service == 0) {
        *port = 0U;
        return 0;
    }
    if (*service == '\0') return EAI_SERVICE;
    cursor = service;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (value > (65535U - digit) / 10U) return EAI_SERVICE;
        value = value * 10U + digit;
        ++cursor;
    }
    if (*cursor == '\0') {
        *port = (uint16_t)value;
        return 0;
    }
    if ((flags & AI_NUMERICSERV) != 0) return EAI_NONAME;
    if (strcmp(service, "ftp") == 0) value = 21U;
    else if (strcmp(service, "ssh") == 0) value = 22U;
    else if (strcmp(service, "telnet") == 0) value = 23U;
    else if (strcmp(service, "domain") == 0) value = 53U;
    else if (strcmp(service, "http") == 0) value = 80U;
    else if (strcmp(service, "https") == 0) value = 443U;
    else return EAI_SERVICE;
    *port = (uint16_t)value;
    return 0;
}

static int validate_hints(const struct addrinfo *hints, int *flags,
                          int *family, int *socktype, int *protocol) {
    if (hints == 0) {
        *flags = 0;
        *family = AF_UNSPEC;
        *socktype = 0;
        *protocol = 0;
        return 0;
    }
    if ((hints->ai_flags & ~NETDB_SUPPORTED_AI_FLAGS) != 0) return EAI_BADFLAGS;
    if (hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET &&
        hints->ai_family != AF_INET6) return EAI_FAMILY;
    if (hints->ai_socktype != 0 && hints->ai_socktype != SOCK_STREAM &&
        hints->ai_socktype != SOCK_DGRAM) return EAI_SOCKTYPE;
    if (hints->ai_protocol != 0 && hints->ai_protocol != IPPROTO_TCP &&
        hints->ai_protocol != IPPROTO_UDP) return EAI_SERVICE;
    if ((hints->ai_socktype == SOCK_STREAM && hints->ai_protocol == IPPROTO_UDP) ||
        (hints->ai_socktype == SOCK_DGRAM && hints->ai_protocol == IPPROTO_TCP)) {
        return EAI_SOCKTYPE;
    }
    *flags = hints->ai_flags;
    *family = hints->ai_family;
    *socktype = hints->ai_socktype;
    *protocol = hints->ai_protocol;
    return 0;
}

static int append_addrinfo(struct addrinfo **head, struct addrinfo **tail,
                           int flags, int family, int socktype, int protocol,
                           uint16_t port, uint32_t address4,
                           const uint8_t address6[16], const char *canonical) {
    struct addrinfo *entry = (struct addrinfo *)calloc(1U, sizeof(*entry));
    size_t address_length = family == AF_INET ? sizeof(struct sockaddr_in) :
                            sizeof(struct sockaddr_in6);
    if (entry == 0) return EAI_MEMORY;
    entry->ai_addr = (struct sockaddr *)calloc(1U, address_length);
    if (entry->ai_addr == 0) {
        free(entry);
        return EAI_MEMORY;
    }
    entry->ai_flags = flags;
    entry->ai_family = family;
    entry->ai_socktype = socktype;
    entry->ai_protocol = protocol;
    entry->ai_addrlen = (socklen_t)address_length;
    if (family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)entry->ai_addr;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(port);
        ipv4->sin_addr.s_addr = htonl(address4);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)entry->ai_addr;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(port);
        memcpy(ipv6->sin6_addr.s6_addr, address6, 16U);
    }
    if (canonical != 0) {
        entry->ai_canonname = strdup(canonical);
        if (entry->ai_canonname == 0) {
            free(entry->ai_addr);
            free(entry);
            return EAI_MEMORY;
        }
    }
    if (*tail != 0) (*tail)->ai_next = entry;
    else *head = entry;
    *tail = entry;
    return 0;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result) {
    uint8_t address6[16] = {0};
    uint32_t address4 = 0U;
    uint16_t port;
    int flags;
    int family;
    int socktype;
    int protocol;
    int node_family = AF_UNSPEC;
    bool make_ipv4 = false;
    bool make_ipv6 = false;
    int status;
    struct addrinfo *head = 0;
    struct addrinfo *tail = 0;

    if (result == 0) return EAI_SYSTEM;
    *result = 0;
    status = validate_hints(hints, &flags, &family, &socktype, &protocol);
    if (status != 0) return status;
    status = service_port(service, flags, &port);
    if (status != 0) return status;
    if (node == 0) {
        if ((flags & AI_NUMERICHOST) != 0 && (flags & AI_PASSIVE) == 0) {
            return EAI_NONAME;
        }
        make_ipv4 = family == AF_UNSPEC || family == AF_INET;
        make_ipv6 = family == AF_UNSPEC || family == AF_INET6;
        if (!((flags & AI_PASSIVE) != 0)) {
            address4 = 0x7F000001U;
            address6[15] = 1U;
        }
    } else if (*node == '\0') {
        return EAI_NONAME;
    } else {
        struct in_addr parsed_address4;
        if (inet_pton(AF_INET, node, &parsed_address4) == 1) {
            address4 = ntohl(parsed_address4.s_addr);
            node_family = AF_INET;
            make_ipv4 = family == AF_UNSPEC || family == AF_INET;
            goto node_parsed;
        }
        if (inet_pton(AF_INET6, node, &address6) == 1) {
            node_family = AF_INET6;
            make_ipv6 = family == AF_UNSPEC || family == AF_INET6;
            goto node_parsed;
        }
        if ((flags & AI_NUMERICHOST) == 0 &&
            (strcmp(node, "localhost") == 0 ||
             strcmp(node, "ip6-localhost") == 0)) {
            node_family = strcmp(node, "ip6-localhost") == 0 ? AF_INET6 : AF_UNSPEC;
            address4 = 0x7F000001U;
            address6[15] = 1U;
            make_ipv4 = node_family != AF_INET6 &&
                        (family == AF_UNSPEC || family == AF_INET);
            make_ipv6 = family == AF_UNSPEC || family == AF_INET6;
            goto node_parsed;
        }
        if ((flags & AI_NUMERICHOST) == 0) {
            netdb_host_state_t lookup;
            bool found = false;
            if (family == AF_UNSPEC || family == AF_INET) {
                if (host_state_lookup(node, AF_INET, &lookup) &&
                    lookup.address_count != 0U) {
                    struct in_addr parsed;
                    memcpy(&parsed, lookup.addresses[0], sizeof(parsed));
                    address4 = ntohl(parsed.s_addr);
                    make_ipv4 = true;
                    node_family = AF_INET;
                    found = true;
                }
            }
            if (family == AF_UNSPEC || family == AF_INET6) {
                if (host_state_lookup(node, AF_INET6, &lookup) &&
                    lookup.address_count != 0U) {
                    memcpy(address6, lookup.addresses[0], sizeof(address6));
                    make_ipv6 = true;
                    if (!found) node_family = AF_INET6;
                    found = true;
                }
            }
            if (found) goto node_parsed;
        }
        return EAI_NONAME;
    }
node_parsed:
    if (node != 0 && family != AF_UNSPEC && node_family != family) {
        return EAI_NONAME;
    }
    if (!make_ipv4 && !make_ipv6) return EAI_NONAME;
    if (make_ipv4) {
        status = append_addrinfo(&head, &tail, flags, AF_INET, socktype,
                                 protocol, port, address4, address6,
                                 (flags & AI_CANONNAME) != 0 ? node : 0);
        if (status != 0) goto fail;
    }
    if (make_ipv6) {
        const char *canonical = (flags & AI_CANONNAME) != 0 && head == 0 ? node : 0;
        status = append_addrinfo(&head, &tail, flags, AF_INET6, socktype,
                                 protocol, port, address4, address6, canonical);
        if (status != 0) goto fail;
    }
    if ((flags & AI_CANONNAME) != 0 && head != 0 && head->ai_canonname == 0) {
        const char *canonical = node != 0 ? node : "localhost";
        head->ai_canonname = strdup(canonical);
        if (head->ai_canonname == 0) {
            status = EAI_MEMORY;
            goto fail;
        }
    }
    *result = head;
    return 0;

fail:
    free_addrinfo_list(head);
    return status;
}

void freeaddrinfo(struct addrinfo *result) {
    free_addrinfo_list(result);
}

const char *gai_strerror(int error_code) {
    switch (error_code) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Invalid flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable name resolution failure";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_SOCKTYPE: return "Socket type not supported";
    case EAI_SERVICE: return "Service not supported for socket type";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
    }
}

static int copy_name(char *destination, socklen_t capacity,
                     const char *source) {
    int written;
    if (destination == 0) return 0;
    if (capacity == 0U) return EAI_OVERFLOW;
    written = snprintf(destination, capacity, "%s", source);
    return written < 0 || (socklen_t)written >= capacity ? EAI_OVERFLOW : 0;
}

static const char *service_name(uint16_t port) {
    if (port == 21U) return "ftp";
    if (port == 22U) return "ssh";
    if (port == 23U) return "telnet";
    if (port == 53U) return "domain";
    if (port == 80U) return "http";
    if (port == 443U) return "https";
    return 0;
}

int getnameinfo(const struct sockaddr *address, socklen_t address_length,
                char *host, socklen_t host_length, char *service,
                socklen_t service_length, int flags) {
    char numeric_host[INET6_ADDRSTRLEN];
    char numeric_service[16];
    uint16_t port;
    int family;
    const void *address_value;
    const char *named_service;
    int status;

    if (address == 0) return EAI_FAMILY;
    if ((flags & ~NETDB_SUPPORTED_NI_FLAGS) != 0) return EAI_BADFLAGS;
    family = address->sa_family;
    if (family == AF_INET) {
        if (address_length < sizeof(struct sockaddr_in)) return EAI_FAMILY;
        address_value = &((const struct sockaddr_in *)address)->sin_addr;
        port = ntohs(((const struct sockaddr_in *)address)->sin_port);
    } else if (family == AF_INET6) {
        if (address_length < sizeof(struct sockaddr_in6)) return EAI_FAMILY;
        address_value = &((const struct sockaddr_in6 *)address)->sin6_addr;
        port = ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (host != 0) {
        if ((flags & NI_NAMEREQD) != 0) return EAI_NONAME;
        if (inet_ntop(family, address_value, numeric_host,
                      sizeof(numeric_host)) == 0) return EAI_SYSTEM;
        status = copy_name(host, host_length, numeric_host);
        if (status != 0) return status;
    }
    if (service != 0) {
        if ((flags & NI_NUMERICSERV) == 0) named_service = service_name(port);
        else named_service = 0;
        if (named_service != 0) {
            status = copy_name(service, service_length, named_service);
        } else {
            (void)snprintf(numeric_service, sizeof(numeric_service), "%u",
                           (unsigned int)port);
            status = copy_name(service, service_length, numeric_service);
        }
        if (status != 0) return status;
    }
    return 0;
}
