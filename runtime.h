#ifndef BLUEMAX_RUNTIME_H
#define BLUEMAX_RUNTIME_H

#include "governor_policy.h"
#include "gpu_mmio.h"
#include "gpu_pstate.h"
#include "runtime_config.h"
#include "sampling_schedule.h"
#include "thermal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/** @brief System paths used to initialize the BlueMax runtime. */
struct runtime_paths {
    /** Root directory containing Linux hwmon devices. */
    const char *hwmon_root;

    /** Nouveau debugfs file containing the available and active pstates. */
    const char *pstate_path;

    /** Linux PCI sysfs file representing the GPU's BAR0 resource. */
    const char *bar0_resource_path;
};

enum {
    /** Minimum monotonic interval between pstate write attempts. */
    RUNTIME_PSTATE_TRANSITION_ATTEMPT_INTERVAL_MS = 1000
};

/** @brief Runtime state and resources owned by the single-threaded governor. */
struct governor_context {
    /** Validated runtime intervals supplied through the command line. */
    struct runtime_config config;

    /** Discovered thermal sensor retained for fresh temperature reads. */
    struct thermal_sensor thermal;

    /** Most recent valid temperature, initialized during sensor discovery. */
    int temperature_millidegrees;

    /** Last performance state successfully read from or applied to the GPU. */
    enum gpu_pstate applied_pstate;

    /** Whether at least one pstate write has been attempted. */
    bool has_pstate_transition_attempt;

    /** Monotonic time of the most recent pstate write attempt. */
    uint64_t last_pstate_transition_attempt_ms;

    /** Target of the most recent pstate write attempt. */
    enum gpu_pstate last_pstate_transition_attempt_target;

    /** Workload and thermal policy state. */
    struct governor_policy policy;

    /** Absolute activity-sampling and temperature-poll deadlines. */
    struct sampling_schedule schedule;

    /** Read-only GPU BAR0 mapping and resolved activity registers. */
    struct gpu_mmio gpu;
};

/** @brief Identifies the result and, on failure, the stage of runtime startup. */
enum runtime_startup_result {
    RUNTIME_STARTUP_OK,
    RUNTIME_STARTUP_INVALID_ARGUMENT,
    RUNTIME_STARTUP_THERMAL_ERROR,
    RUNTIME_STARTUP_PSTATE_ERROR,
    RUNTIME_STARTUP_MMIO_ERROR,
    RUNTIME_STARTUP_CLOCK_ERROR,
    RUNTIME_STARTUP_POLICY_ERROR,
    RUNTIME_STARTUP_SCHEDULE_ERROR
};

/** @brief Result category for one runtime governor cycle. */
enum runtime_cycle_status {
    RUNTIME_CYCLE_OK,
    RUNTIME_CYCLE_INVALID_ARGUMENT,
    RUNTIME_CYCLE_PSTATE_ERROR
};

/**
 * @brief Return whether the continuous loop may proceed after a cycle failure.
 *
 * Recoverable failures publish a complete cycle result and preserve enough
 * runtime state for a later bounded retry. Programmer errors remain fatal.
 */
bool runtime_cycle_status_is_recoverable(enum runtime_cycle_status status);

/** @brief Hardware observations and decisions produced by one governor cycle. */
struct runtime_cycle_result {
    /** Monotonic time associated with this cycle. */
    uint64_t now_ms;

    /** Raw snapshot of the four GPU activity registers. */
    struct gpu_activity_sample activity;

    /** Whether the graphics engine was active in this sample. */
    bool graphics_activity_detected;

    /** Whether any monitored video engine was active in this sample. */
    bool video_activity_detected;

    /** Graphics history after this sample, with the newest sample in bit 0. */
    uint64_t graphics_history;

    /** Video history after this sample, with the newest sample in bit 0. */
    uint64_t video_history;

    /** Number of real samples represented in each history. */
    unsigned int activity_history_samples;

    /** Temperature observation supplied to the governor policy. */
    enum governor_temperature_observation temperature_observation;

    /** Error from a failed scheduled temperature read, otherwise zero. */
    int temperature_read_error;

    /** Most recent valid temperature retained after this cycle. */
    int temperature_millidegrees;

    /** Recommendation and event flags returned by the governor policy. */
    struct governor_policy_result policy;

    /** Whether the policy recommendation differed from the applied pstate. */
    bool pstate_transition_requested;

    /** Whether this cycle attempted to write a requested pstate. */
    bool pstate_transition_attempted;

    /** Whether an attempt was deferred by the minimum attempt interval. */
    bool pstate_transition_deferred;

    /** Whether a requested pstate transition completed successfully. */
    bool pstate_transition_succeeded;

    /** Pstate known to be applied after this cycle. */
    enum gpu_pstate applied_pstate;
};

/**
 * @brief Initialize all hardware resources and policy state used at runtime.
 *
 * Startup is transactional: @p context is changed only after every stage
 * succeeds. Any BAR0 mapping acquired before a later failure is released while
 * the original failure remains available through @c errno.
 *
 * @param[out] context Destination for the completed runtime context.
 * @param[in] config Validated runtime configuration to retain.
 * @param[in] paths System paths used for hardware discovery and access.
 *
 * @return RUNTIME_STARTUP_OK on success, or the failed startup stage with
 *         @c errno set.
 */
enum runtime_startup_result runtime_start(struct governor_context *context, const struct runtime_config *config, const struct runtime_paths *paths);

/**
 * @brief Read the current CLOCK_MONOTONIC time in whole milliseconds.
 *
 * @param[out] now_ms Destination for the current monotonic time.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int runtime_monotonic_time_ms(uint64_t *now_ms);

/**
 * @brief Sleep until an absolute CLOCK_MONOTONIC deadline.
 *
 * Interrupted sleeps resume against the same absolute deadline so they cannot
 * introduce schedule drift.
 *
 * @param[in] deadline_ms Absolute monotonic deadline in milliseconds.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int runtime_sleep_until_ms(uint64_t deadline_ms);

/**
 * @brief Return a concise description of a runtime startup result.
 *
 * @param[in] result Result returned by runtime_start().
 *
 * @return Static text suitable for an error diagnostic.
 */
const char *runtime_startup_result_description(enum runtime_startup_result result);

/**
 * @brief Print the hardware and policy state discovered during startup.
 *
 * @param[in] stream Destination for the startup summary.
 * @param[in] context Successfully initialized runtime context.
 */
void runtime_print_startup_summary(FILE *stream, const struct governor_context *context);

/**
 * @brief Execute one activity, temperature, policy, and pstate cycle.
 *
 * A temperature read failure is returned as a policy observation rather than
 * a cycle failure. Pstate write attempts are bounded by a fixed monotonic
 * interval. A failed pstate transition leaves the retained applied pstate
 * unchanged and publishes the completed cycle result for diagnostics.
 *
 * @param[in,out] context Successfully initialized runtime context.
 * @param[in] paths System paths used for hardware control.
 * @param[in] poll_temperature Whether to read a fresh temperature this cycle.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[out] result Observations and decisions from the cycle.
 *
 * @return RUNTIME_CYCLE_OK on success, or a failure category with @c errno
 *         set.
 */
enum runtime_cycle_status runtime_run_cycle(struct governor_context *context, const struct runtime_paths *paths, bool poll_temperature, uint64_t now_ms, struct runtime_cycle_result *result);

/**
 * @brief Execute one governor cycle without applying pstate recommendations.
 *
 * Hardware telemetry and policy state advance normally, but the pstate file is
 * never opened for writing and transition-attempt timing is not consumed.
 * Requested transitions are published in @p result with
 * pstate_transition_attempted and pstate_transition_deferred set to false.
 *
 * @param[in,out] context Successfully initialized runtime context.
 * @param[in] paths System paths retained for the common cycle interface.
 * @param[in] poll_temperature Whether to read a fresh temperature this cycle.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[out] result Observations and decisions from the cycle.
 *
 * @return RUNTIME_CYCLE_OK on success, or RUNTIME_CYCLE_INVALID_ARGUMENT with
 *         @c errno set.
 */
enum runtime_cycle_status runtime_observe_cycle(struct governor_context *context, const struct runtime_paths *paths, bool poll_temperature, uint64_t now_ms, struct runtime_cycle_result *result);

/** Print the observations and decisions produced by one governor cycle. */
void runtime_print_cycle_summary(FILE *stream, const struct runtime_cycle_result *result);

/** Print one compact periodic observation on a single line. */
void runtime_print_observation_summary(FILE *stream, const struct runtime_cycle_result *result);

/**
 * @brief Release resources retained by a runtime context.
 *
 * Cleanup is idempotent. A context without an active BAR0 mapping is already
 * considered clean.
 *
 * @param[in,out] context Runtime context to clean up.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int runtime_cleanup(struct governor_context *context);

#endif
