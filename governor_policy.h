#ifndef BLUEMAX_GOVERNOR_POLICY_H
#define BLUEMAX_GOVERNOR_POLICY_H

#include "gpu_pstate.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Describes temperature telemetry supplied during a policy step. */
enum governor_temperature_observation {
    /** No temperature read was scheduled for this policy step. */
    GOVERNOR_TEMPERATURE_NOT_POLLED,

    /** The step includes a fresh, valid temperature sample. */
    GOVERNOR_TEMPERATURE_VALID,

    /** A scheduled temperature read failed. */
    GOVERNOR_TEMPERATURE_READ_FAILED
};

/**
 * @brief Meaningful state changes reported by the governor policy.
 *
 * Values are flags because one input may cause more than one meaningful
 * event. Routine samples that do not change policy state report NONE.
 */
enum governor_policy_event {
    GOVERNOR_POLICY_EVENT_NONE = 0,
    GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT = 1,
    GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT = 2,
    GOVERNOR_POLICY_EVENT_IDLE_DOWNSHIFT = 4,
    GOVERNOR_POLICY_EVENT_THERMAL_LIMIT = 8,
    GOVERNOR_POLICY_EVENT_THERMAL_RECOVERY = 16,
    GOVERNOR_POLICY_EVENT_TEMPERATURE_FAULT = 32,
    GOVERNOR_POLICY_EVENT_TEMPERATURE_RECOVERY = 64
};

/** @brief Inputs for one deterministic governor policy step. */
struct governor_policy_input {
    /** Current monotonic time in milliseconds. */
    uint64_t now_ms;

    /** Whether the current sample shows graphics-engine activity. */
    bool graphics_activity_detected;

    /** Whether the current sample shows hardware video-engine activity. */
    bool video_activity_detected;

    /** Kind of temperature telemetry supplied with this step. */
    enum governor_temperature_observation temperature_observation;

    /**
     * Fresh temperature in millidegrees Celsius when the observation is
     * GOVERNOR_TEMPERATURE_VALID; ignored for other observations.
     */
    int temperature_millidegrees;
};

/** @brief Output from one governor policy step. */
struct governor_policy_result {
    /** Performance state the policy recommends that the runtime apply. */
    enum gpu_pstate recommended_pstate;

    /** Bitwise combination of values from enum governor_policy_event. */
    unsigned int events;
};

/** @brief State retained between deterministic governor policy steps. */
struct governor_policy {
    /** Performance state the policy is currently trying to maintain. */
    enum gpu_pstate target_pstate;

    /** Graphics activity history, with the newest sample in bit 0. */
    uint64_t graphics_history;

    /** Video activity history, with the newest sample in bit 0. */
    uint64_t video_history;

    /** Monotonic time at which the policy most recently entered HIGH. */
    uint64_t high_since_ms;

    /** Monotonic time of the most recent graphics or video activity. */
    uint64_t last_activity_ms;

    /** Monotonic time at which the current telemetry failure began. */
    uint64_t temperature_failure_since_ms;

    /** Configured maximum temperature, in millidegrees Celsius. */
    int temperature_max_millidegrees;

    /** Configured temperature hysteresis, in millidegrees Celsius. */
    int temperature_hysteresis_millidegrees;

    /** Whether the maximum-temperature safety limit is active. */
    bool thermal_limit_active;

    /** Whether one or more consecutive temperature reads have failed. */
    bool temperature_failure_pending;

    /** Whether temperature telemetry has exceeded its failure timeout. */
    bool temperature_fault_active;
};

/**
 * @brief Initialize a governor policy from known startup state.
 *
 * The initial temperature is a valid reading obtained during thermal-sensor
 * discovery. The policy uses only LOW and HIGH automatically, so an initial
 * MEDIUM state is normalized to LOW.
 *
 * @param[out] policy Destination for the initialized policy state.
 * @param[in] initial_pstate Performance state active at startup.
 * @param[in] temperature_max_millidegrees Configured maximum temperature.
 * @param[in] temperature_hysteresis_millidegrees Configured hysteresis.
 * @param[in] initial_temperature_millidegrees Valid startup temperature.
 * @param[in] now_ms Current monotonic time in milliseconds.
 *
 * @return 0 on success, or -1 on invalid input with @c errno set.
 */
int governor_policy_init(
    struct governor_policy *policy,
    enum gpu_pstate initial_pstate,
    int temperature_max_millidegrees,
    int temperature_hysteresis_millidegrees,
    int initial_temperature_millidegrees,
    uint64_t now_ms);

/**
 * @brief Process one activity sample and optional temperature observation.
 *
 * Inputs must use nondecreasing monotonic timestamps. The returned event flags
 * describe only meaningful changes caused by this step.
 *
 * @param[in,out] policy Previously initialized policy state.
 * @param[in] input Inputs for the current policy step.
 *
 * @return The recommended performance state and any meaningful policy events.
 */
struct governor_policy_result governor_policy_step(
    struct governor_policy *policy,
    const struct governor_policy_input *input);

#endif
