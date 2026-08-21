/**
 * @file bluemax.c
 * @brief BlueMax application entry point and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include "governor_policy.h"
#include "gpu_mmio.h"
#include "gpu_pstate.h"

enum {
    /** Interval between GPU activity samples. */
    SAMPLE_INTERVAL_MS = 10,

    /** Interval between temperature samples. */
    TEMPERATURE_POLL_INTERVAL_MS = 1000
};

/** @brief Runtime state and resources owned by the single-threaded governor. */
struct governor_context {
    /** Last performance state successfully read from or applied to the GPU. */
    enum gpu_pstate applied_pstate;

    /** Workload and thermal policy state. */
    struct governor_policy policy;

    /** Read-only GPU BAR0 mapping and resolved activity registers. */
    struct gpu_mmio gpu;
};

int main(void)
{
    return 0;
}
