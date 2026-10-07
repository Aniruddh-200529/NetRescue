#include "cidr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nr_parse_cidr(const char *text, NrCidr *result) {
    if (text == NULL || result == NULL) return false;
    unsigned octets[4], prefix;
    char tail;
    if (sscanf(text, "%u.%u.%u.%u/%u%c", &octets[0], &octets[1], &octets[2], &octets[3], &prefix, &tail) != 5) return false;
    for (size_t i = 0; i < 4; ++i) if (octets[i] > 255U) return false;
    if (prefix > 32U) return false;
    const uint32_t address = ((uint32_t)octets[0] << 24U) | ((uint32_t)octets[1] << 16U) | ((uint32_t)octets[2] << 8U) | (uint32_t)octets[3];
    const uint32_t mask = prefix == 0U ? 0U : UINT32_MAX << (32U - prefix);
    const uint32_t network = address & mask;
    const uint32_t broadcast = network | ~mask;
    result->address = address; result->mask = mask; result->network = network;
    result->broadcast = broadcast; result->prefix = prefix;
    if (prefix <= 30U) {
        result->first_usable = network + 1U;
        result->last_usable = broadcast - 1U;
        result->usable_hosts = (uint64_t)broadcast - network - 1U;
    } else if (prefix == 31U) {
        result->first_usable = network; result->last_usable = broadcast; result->usable_hosts = 2U;
    } else {
        result->first_usable = address; result->last_usable = address; result->usable_hosts = 1U;
    }
    return true;
}

void nr_format_ipv4(uint32_t address, char output[16]) {
    (void)snprintf(output, 16, "%u.%u.%u.%u", (unsigned)(address >> 24U), (unsigned)((address >> 16U) & 255U), (unsigned)((address >> 8U) & 255U), (unsigned)(address & 255U));
}
