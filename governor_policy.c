/**
 * @file governor_policy.c
 * @brief Deterministic GPU performance-state governor policy.
 */

#include "governor_policy.h"

#include <errno.h>
#include <string.h>

enum {
    /** Number of recent samples considered by the video activity trigger. */
    VIDEO_TRIGGER_WINDOW = 3,

    /** Active video samples required to request HIGH. */
    VIDEO_TRIGGER_COUNT = 2,

    /** Number of recent samples considered by the graphics activity trigger. */
    GRAPHICS_TRIGGER_WINDOW = 5,

    /** Active graphics samples required to request HIGH. */
    GRAPHICS_TRIGGER_COUNT = 3
};

/**
 * @brief Determine whether a rolling history reaches an activity threshold.
 *
 * @param[in] history Activity history with the newest sample in bit 0.
 * @param[in] window Number of recent samples to examine.
 * @param[in] required Number of active samples required within the window.
 *
 * @return @c true when at least @p required samples are active, otherwise
 *         @c false.
 */
static bool history_reaches_threshold(
    uint64_t history,
    unsigned int window,
    unsigned int required)
{
    unsigned int active_samples = 0;

    for (unsigned int index = 0; index < window; index++) {
        active_samples += (unsigned int)((history >> index) & 1U);
    }

    return active_samples >= required;
}

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

struct governor_policy_result governor_policy_step(
    struct governor_policy *policy,
    const struct governor_policy_input *input)
{
    policy->graphics_history =
        (policy->graphics_history << 1) | (input->graphics_active ? 1U : 0U);
    policy->video_history =
        (policy->video_history << 1) | (input->video_active ? 1U : 0U);

    if (input->graphics_active || input->video_active) {
        policy->last_activity_ms = input->now_ms;
    }

    struct governor_policy_result result = {
        .desired_pstate = policy->desired_pstate,
        .events = GOVERNOR_POLICY_EVENT_NONE,
    };

    bool graphics_triggered = history_reaches_threshold(
        policy->graphics_history,
        GRAPHICS_TRIGGER_WINDOW,
        GRAPHICS_TRIGGER_COUNT);
    bool video_triggered = history_reaches_threshold(
        policy->video_history,
        VIDEO_TRIGGER_WINDOW,
        VIDEO_TRIGGER_COUNT);

    if (policy->desired_pstate == GPU_PSTATE_LOW
        && !policy->thermal_limit_active
        && !policy->temperature_fault_active
        && (graphics_triggered || video_triggered)) {
        policy->desired_pstate = GPU_PSTATE_HIGH;
        policy->high_since_ms = input->now_ms;

        if (graphics_triggered) {
            result.events |= GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT;
        }
        if (video_triggered) {
            result.events |= GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT;
        }

        result.desired_pstate = policy->desired_pstate;
    }

    return result;
}
