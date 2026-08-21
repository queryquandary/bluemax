/**
 * @file governor_policy.c
 * @brief Deterministic GPU performance-state governor policy.
 */

#include "governor_policy.h"

#include <errno.h>
#include <string.h>

enum {
    /** Minimum time the governor must remain in HIGH. */
    HIGH_MIN_RESIDENCY_MS = 500,

    /** Required continuous inactivity before returning to LOW. */
    IDLE_DOWNSHIFT_DELAY_MS = 2000,

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
static bool history_reaches_threshold(uint64_t history, unsigned int window, unsigned int required)
{
    unsigned int active_samples = 0;

    for (unsigned int index = 0; index < window; index++)
        active_samples += (unsigned int)((history >> index) & 1U);

    return active_samples >= required;
}

/**
 * @brief Determine whether a monotonic-time interval reached its duration.
 *
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] since_ms Monotonic time at which the interval began.
 * @param[in] duration_ms Required interval duration in milliseconds.
 *
 * @return @c true when the duration has elapsed, otherwise @c false.
 */
static bool interval_has_elapsed(uint64_t now_ms, uint64_t since_ms, uint64_t duration_ms)
{
    return now_ms - since_ms >= duration_ms;
}

/**
 * @brief Apply maximum-temperature limiting and hysteresis recovery.
 *
 * Observations other than a fresh valid sample are ignored by this helper.
 * Reaching the configured maximum immediately forces LOW. Once active, the
 * limit clears only below the recovery threshold.
 *
 * @param[in,out] policy Policy state to update.
 * @param[in] input Current policy inputs.
 * @param[in,out] result Step result receiving recommendations and events.
 */
static void process_valid_temperature(struct governor_policy *policy, const struct governor_policy_input *input, struct governor_policy_result *result)
{
    // Don't do anything if a valid temperature sample isn't available.
    if (input->temperature_observation != GOVERNOR_TEMPERATURE_VALID)
        return;

    if (input->temperature_millidegrees >= policy->temperature_max_millidegrees)
    {
        if (!policy->thermal_limit_active)
            result->events |= GOVERNOR_POLICY_EVENT_THERMAL_LIMIT;

        policy->thermal_limit_active = true;
        policy->target_pstate = GPU_PSTATE_LOW;
        result->recommended_pstate = policy->target_pstate;
        return;
    }

    int recovery_temperature = policy->temperature_max_millidegrees - policy->temperature_hysteresis_millidegrees;

    if (policy->thermal_limit_active && input->temperature_millidegrees < recovery_temperature)
    {
        policy->thermal_limit_active = false;
        result->events |= GOVERNOR_POLICY_EVENT_THERMAL_RECOVERY;
    }
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

    initialized.target_pstate = initial_pstate == GPU_PSTATE_HIGH ? GPU_PSTATE_HIGH : GPU_PSTATE_LOW;
    initialized.high_since_ms = now_ms;
    initialized.last_activity_ms = now_ms;
    initialized.temperature_max_millidegrees = temperature_max_millidegrees;
    initialized.temperature_hysteresis_millidegrees = temperature_hysteresis_millidegrees;

    if (initial_temperature_millidegrees >= temperature_max_millidegrees)
    {
        initialized.target_pstate = GPU_PSTATE_LOW;
        initialized.thermal_limit_active = true;
    }

    *policy = initialized;
    return 0;
}

struct governor_policy_result governor_policy_step(struct governor_policy *policy, const struct governor_policy_input *input)
{
    policy->graphics_history = (policy->graphics_history << 1) | (input->graphics_activity_detected ? 1U : 0U);
    policy->video_history = (policy->video_history << 1) | (input->video_activity_detected ? 1U : 0U);

    bool activity_detected = input->graphics_activity_detected || input->video_activity_detected;

    if (activity_detected)
        policy->last_activity_ms = input->now_ms;

    struct governor_policy_result result = {
        .recommended_pstate = policy->target_pstate,
        .events = GOVERNOR_POLICY_EVENT_NONE,
    };

    process_valid_temperature(policy, input, &result);

    if (policy->thermal_limit_active || policy->temperature_fault_active)
        return result;

    // Decide if we should upshift to HIGH or downshift to LOW based on activity and temperature.
    if (policy->target_pstate == GPU_PSTATE_LOW)
    {
        bool graphics_triggered = history_reaches_threshold(policy->graphics_history, GRAPHICS_TRIGGER_WINDOW, GRAPHICS_TRIGGER_COUNT);
        bool video_triggered = history_reaches_threshold(policy->video_history, VIDEO_TRIGGER_WINDOW, VIDEO_TRIGGER_COUNT);

        if (graphics_triggered || video_triggered)
        {
            policy->high_since_ms = input->now_ms;
            policy->target_pstate = GPU_PSTATE_HIGH;
            result.recommended_pstate = policy->target_pstate;

            if (graphics_triggered)
                result.events |= GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT;

            if (video_triggered)
                result.events |= GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT;
        }
    }
    else if (policy->target_pstate == GPU_PSTATE_HIGH
        && !activity_detected
        && interval_has_elapsed(input->now_ms, policy->high_since_ms, HIGH_MIN_RESIDENCY_MS)
        && interval_has_elapsed(input->now_ms, policy->last_activity_ms, IDLE_DOWNSHIFT_DELAY_MS))
    {
        policy->target_pstate = GPU_PSTATE_LOW;
        result.recommended_pstate = policy->target_pstate;
        result.events |= GOVERNOR_POLICY_EVENT_IDLE_DOWNSHIFT;
    }

    return result;
}
