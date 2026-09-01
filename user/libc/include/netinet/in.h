#pragma once

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define INADDR_ANY       ((in_addr_t)0x00000000U)
#define INADDR_LOOPBACK  ((in_addr_t)0x0100007FU)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFFU)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFFU)

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
