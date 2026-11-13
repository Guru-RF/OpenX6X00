/* Host test shim (tools/smartcw): no WR12 */
#pragma once

#include <stdbool.h>

static inline bool wr12_port(const char *dev) {
    (void)dev;
    return false;
}
