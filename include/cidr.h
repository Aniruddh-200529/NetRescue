#ifndef NETRESCUE_CIDR_H
#define NETRESCUE_CIDR_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t address, mask, network, broadcast, first_usable, last_usable;
    uint64_t usable_hosts;
    unsigned prefix;
} NrCidr;
bool nr_parse_cidr(const char *text, NrCidr *result);
void nr_format_ipv4(uint32_t address, char output[16]);
#endif
