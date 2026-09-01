#pragma once

#include <netinet/in.h>

uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);
int inet_aton(const char *text, struct in_addr *address);
in_addr_t inet_addr(const char *text);
int inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *buffer,
                      socklen_t capacity);
char *inet_ntoa(struct in_addr address);

