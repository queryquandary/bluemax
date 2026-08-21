/**
 * @file governor_policy.c
 * @brief Deterministic GPU performance-state governor policy.
 */

#include "governor_policy.h"

#include <errno.h>
#include <string.h>

int governor_policy_init(
    struct governor_policy *policy,
    enum gpu_pstate initial_pstate,
    int temperature_max_millidegrees,
    int temperature_hysteresis_millidegrees,
    int initial_temperature_millidegrees,
    uint64_t now_ms)
{
    if (policy == NULL
        || (initial_pstate != GPU_PSTATE_LOW && initial_pstate != GPU_PSTATE_MEDIUM && initial_pstate != GPU_PSTATE_HIGH)
        || temperature_max_millidegrees <= 0
        || temperature_hysteresis_millidegrees < 0
        || temperature_hysteresis_millidegrees >= temperature_max_millidegrees) {
        errno = EINVAL;
        return -1;
    }

    struct governor_policy initialized;
    memset(&initialized, 0, sizeof(initialized));

    initialized.desired_pstate = initial_pstate == GPU_PSTATE_HIGH ? GPU_PSTATE_HIGH : GPU_PSTATE_LOW;
    initialized.high_since_ms = now_ms;
    initialized.last_activity_ms = now_ms;
    initialized.temperature_max_millidegrees = temperature_max_millidegrees;
    initialized.temperature_hysteresis_millidegrees = temperature_hysteresis_millidegrees;

    if (initial_temperature_millidegrees >= temperature_max_millidegrees) {
        initialized.desired_pstate = GPU_PSTATE_LOW;
        initialized.thermal_limit_active = true;
    }

    *policy = initialized;
    return 0;
}
