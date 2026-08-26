/**
 * @file runtime.c
 * @brief BlueMax hardware, policy, and sampling-cycle lifecycle.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int runtime_monotonic_time_ms(uint64_t *now_ms)
{
    if (now_ms == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return -1;

    // Guard the multiplication as well as the sub-second addition so the
    // conversion remains valid if time_t has a wider range than uint64_t.
    if (now.tv_sec < 0 || (uint64_t)now.tv_sec > (UINT64_MAX - 999U) / 1000U)
    {
        errno = EOVERFLOW;
        return -1;
    }

    *now_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return 0;
}

int runtime_sleep_until_ms(uint64_t deadline_ms)
{
    uint64_t seconds = deadline_ms / 1000U;
    time_t deadline_seconds = (time_t)seconds;

    if (deadline_seconds < 0 || (uint64_t)deadline_seconds != seconds)
    {
        errno = EOVERFLOW;
        return -1;
    }

    const struct timespec deadline = {
        .tv_sec = deadline_seconds,
        .tv_nsec = (long)(deadline_ms % 1000U) * 1000000L,
    };

    int sleep_error;
    do
    {
        sleep_error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    } while (sleep_error == EINTR);

    if (sleep_error != 0)
    {
        errno = sleep_error;
        return -1;
    }

    return 0;
}

/** Release a candidate mapping without hiding the startup failure. */
static enum runtime_startup_result rollback_mapping(struct governor_context *candidate, enum runtime_startup_result result)
{
    int startup_error = errno;
    gpu_mmio_unmap(&candidate->gpu);
    errno = startup_error;
    return result;
}

enum runtime_startup_result runtime_start(struct governor_context *context, const struct runtime_config *config, const struct runtime_paths *paths)
{
    if (context == NULL
        || config == NULL
        || paths == NULL
        || paths->hwmon_root == NULL
        || paths->pstate_path == NULL
        || paths->bar0_resource_path == NULL)
    {
        errno = EINVAL;
        return RUNTIME_STARTUP_INVALID_ARGUMENT;
    }

    // Build startup state privately so callers never receive a context that
    // owns only some of the required hardware resources.
    struct governor_context candidate = {
        .config = *config,
    };

    if (thermal_sensor_discover(paths->hwmon_root, &candidate.thermal, &candidate.temperature_millidegrees) == -1)
        return RUNTIME_STARTUP_THERMAL_ERROR;

    if (gpu_pstate_read(paths->pstate_path, &candidate.applied_pstate) == -1)
        return RUNTIME_STARTUP_PSTATE_ERROR;

    if (gpu_mmio_map(paths->bar0_resource_path, &candidate.gpu) == -1)
        return RUNTIME_STARTUP_MMIO_ERROR;

    uint64_t now_ms;

    if (runtime_monotonic_time_ms(&now_ms) == -1)
        return rollback_mapping(&candidate, RUNTIME_STARTUP_CLOCK_ERROR);

    if (governor_policy_init(
            &candidate.policy,
            candidate.applied_pstate,
            candidate.thermal.max_millidegrees,
            candidate.thermal.max_hyst_millidegrees,
            candidate.temperature_millidegrees,
            now_ms) == -1)
        return rollback_mapping(&candidate, RUNTIME_STARTUP_POLICY_ERROR);

    // Share the policy's startup timestamp so its initial state and the first
    // activity deadline describe the same point in the monotonic timeline.
    if (sampling_schedule_init(&candidate.schedule, &candidate.config, now_ms) == -1)
        return rollback_mapping(&candidate, RUNTIME_STARTUP_SCHEDULE_ERROR);

    *context = candidate;
    return RUNTIME_STARTUP_OK;
}

const char *runtime_startup_result_description(enum runtime_startup_result result)
{
    switch (result)
    {
        case RUNTIME_STARTUP_OK:
            return "runtime initialized";

        case RUNTIME_STARTUP_INVALID_ARGUMENT:
            return "invalid runtime startup argument";

        case RUNTIME_STARTUP_THERMAL_ERROR:
            return "cannot discover Nouveau thermal sensor";

        case RUNTIME_STARTUP_PSTATE_ERROR:
            return "cannot read current GPU pstate";

        case RUNTIME_STARTUP_MMIO_ERROR:
            return "cannot map GPU BAR0 resource";

        case RUNTIME_STARTUP_CLOCK_ERROR:
            return "cannot read monotonic clock";

        case RUNTIME_STARTUP_POLICY_ERROR:
            return "cannot initialize governor policy";

        case RUNTIME_STARTUP_SCHEDULE_ERROR:
            return "cannot initialize sampling schedule";
    }

    return "unknown runtime startup error";
}

/** Return the display name for a valid GPU pstate. */
static const char *pstate_name(enum gpu_pstate pstate)
{
    switch (pstate)
    {
        case GPU_PSTATE_LOW:
            return "LOW (03)";

        case GPU_PSTATE_MEDIUM:
            return "MEDIUM (07)";

        case GPU_PSTATE_HIGH:
            return "HIGH (0f)";
    }

    return "UNKNOWN";
}

void runtime_print_startup_summary(FILE *stream, const struct governor_context *context)
{
    int recovery_millidegrees = context->thermal.max_millidegrees - context->thermal.max_hyst_millidegrees;

    fprintf(stream, "BlueMax %s startup\n\n", BLUEMAX_VERSION);
    fprintf(stream, "Activity sample interval:   %6u ms\n", context->config.sample_interval_ms);
    fprintf(stream, "Temperature poll interval: %6u ms\n", context->config.temperature_poll_interval_ms);
    fprintf(stream, "Thermal sensor:            %s\n", context->thermal.input_path);
    fprintf(stream, "Initial temperature:       %6.1f C\n", context->temperature_millidegrees / 1000.0);
    fprintf(stream, "Maximum temperature:       %6.1f C\n", context->thermal.max_millidegrees / 1000.0);
    fprintf(stream, "Recovery threshold:        below %.1f C\n", recovery_millidegrees / 1000.0);
    fprintf(stream, "Applied pstate:            %s\n", pstate_name(context->applied_pstate));
    fprintf(stream, "Policy target:             %s\n", pstate_name(context->policy.target_pstate));
    fprintf(stream, "Thermal limit:             %s\n", context->policy.thermal_limit_active ? "active" : "inactive");
    fprintf(stream, "BAR0 telemetry:            mapped read-only (%zu MiB)\n\n", context->gpu.bar0_length / (1024U * 1024U));
}

/** Return whether the minimum interval has elapsed without overflowing. */
static bool pstate_attempt_interval_elapsed(const struct governor_context *context, uint64_t now_ms)
{
    // Subtraction is safe only after ordering the monotonic timestamps. This
    // also treats an unexpected clock regression as ineligible for a retry.
    return now_ms >= context->last_pstate_transition_attempt_ms
        && now_ms - context->last_pstate_transition_attempt_ms >= RUNTIME_PSTATE_TRANSITION_ATTEMPT_INTERVAL_MS;
}

/** Return whether a requested transition may write the pstate interface. */
static bool pstate_transition_attempt_allowed(const struct governor_context *context, enum gpu_pstate target, uint64_t now_ms)
{
    if (!context->has_pstate_transition_attempt)
        return true;

    bool safety_low = target == GPU_PSTATE_LOW && (context->policy.thermal_limit_active || context->policy.temperature_fault_active);

    // A newly requested safety downshift must not wait behind an earlier HIGH
    // or MEDIUM attempt. Once LOW itself has been attempted, however, the
    // normal interval bounds retries of a persistently failing safety write.
    if (safety_low && context->last_pstate_transition_attempt_target != GPU_PSTATE_LOW)
        return true;

    return pstate_attempt_interval_elapsed(context, now_ms);
}

/** Execute one cycle with hardware actuation either enabled or suppressed. */
static enum runtime_cycle_status execute_cycle(struct governor_context *context, const struct runtime_paths *paths, bool poll_temperature, bool apply_pstate, uint64_t now_ms, struct runtime_cycle_result *result)
{
    if (context == NULL || paths == NULL || paths->pstate_path == NULL || result == NULL)
    {
        errno = EINVAL;
        return RUNTIME_CYCLE_INVALID_ARGUMENT;
    }

    struct runtime_cycle_result cycle = {
        .now_ms = now_ms,
    };

    // Capture all four registers before doing slower filesystem work so they
    // describe one closely grouped activity observation.
    gpu_mmio_read_activity(&context->gpu, &cycle.activity);

    cycle.graphics_activity_detected = cycle.activity.pgraph != 0;
    cycle.video_activity_detected = cycle.activity.pvld != 0
        || cycle.activity.ppdec != 0
        || cycle.activity.pppp != 0;

    if (poll_temperature)
    {
        int temperature_millidegrees;
        if (thermal_sensor_read(&context->thermal, &temperature_millidegrees) == -1)
        {
            // Telemetry loss is policy input rather than an immediate runtime
            // failure. Keep the last valid value for reporting and recovery.
            cycle.temperature_observation = GOVERNOR_TEMPERATURE_READ_FAILED;
            cycle.temperature_read_error = errno;
        }
        else
        {
            context->temperature_millidegrees = temperature_millidegrees;
            cycle.temperature_observation = GOVERNOR_TEMPERATURE_VALID;
        }
    }
    else
    {
        cycle.temperature_observation = GOVERNOR_TEMPERATURE_NOT_POLLED;
    }

    cycle.temperature_millidegrees = context->temperature_millidegrees;

    const struct governor_policy_input input = {
        .now_ms = now_ms,
        .graphics_activity_detected = cycle.graphics_activity_detected,
        .video_activity_detected = cycle.video_activity_detected,
        .temperature_observation = cycle.temperature_observation,
        .temperature_millidegrees = context->temperature_millidegrees,
    };

    // The policy advances even if hardware actuation later fails. Because the
    // retained applied state changes only on success, a later cycle will retry
    // any recommendation that the GPU did not accept.
    cycle.policy = governor_policy_step(&context->policy, &input);
    cycle.graphics_history = context->policy.graphics_history;
    cycle.video_history = context->policy.video_history;
    cycle.activity_history_samples = context->policy.activity_history_samples;
    cycle.pstate_transition_requested = cycle.policy.recommended_pstate != context->applied_pstate;

    if (cycle.pstate_transition_requested && apply_pstate)
    {
        if (!pstate_transition_attempt_allowed(context, cycle.policy.recommended_pstate, now_ms))
        {
            cycle.pstate_transition_deferred = true;
            cycle.applied_pstate = context->applied_pstate;
            *result = cycle;
            return RUNTIME_CYCLE_OK;
        }

        // Record eligibility consumption before writing. Failed writes must be
        // bounded just like successful ones or a persistent error could cause
        // an attempt on every activity-sampling cycle.
        context->has_pstate_transition_attempt = true;
        context->last_pstate_transition_attempt_ms = now_ms;
        context->last_pstate_transition_attempt_target = cycle.policy.recommended_pstate;
        cycle.pstate_transition_attempted = true;

        if (gpu_pstate_set(paths->pstate_path, cycle.policy.recommended_pstate) == -1)
        {
            // Publish the observations and decision on failure so callers can
            // explain what was attempted without claiming that it was applied.
            int transition_error = errno;
            cycle.applied_pstate = context->applied_pstate;
            *result = cycle;
            errno = transition_error;
            return RUNTIME_CYCLE_PSTATE_ERROR;
        }

        context->applied_pstate = cycle.policy.recommended_pstate;
        cycle.pstate_transition_succeeded = true;
    }

    cycle.applied_pstate = context->applied_pstate;
    *result = cycle;
    return RUNTIME_CYCLE_OK;
}

enum runtime_cycle_status runtime_run_cycle(struct governor_context *context, const struct runtime_paths *paths, bool poll_temperature, uint64_t now_ms, struct runtime_cycle_result *result)
{
    return execute_cycle(context, paths, poll_temperature, true, now_ms, result);
}

enum runtime_cycle_status runtime_observe_cycle(struct governor_context *context, const struct runtime_paths *paths, bool poll_temperature, uint64_t now_ms, struct runtime_cycle_result *result)
{
    return execute_cycle(context, paths, poll_temperature, false, now_ms, result);
}

bool runtime_cycle_status_is_recoverable(enum runtime_cycle_status status)
{
    return status == RUNTIME_CYCLE_PSTATE_ERROR;
}

/** Count active samples in one 64-bit activity history. */
static unsigned int history_active_samples(uint64_t history)
{
    unsigned int active_samples = 0;

    while (history != 0)
    {
        active_samples += (unsigned int)(history & 1U);
        history >>= 1;
    }

    return active_samples;
}

void runtime_print_cycle_summary(FILE *stream, const struct runtime_cycle_result *result)
{
    fprintf(stream, "Governor cycle at %llu.%03llu s\n\n", (unsigned long long)(result->now_ms / 1000U), (unsigned long long)(result->now_ms % 1000U));
    fprintf(stream, "PGRAPH:                    0x%08x\n", (unsigned int)result->activity.pgraph);
    fprintf(stream, "PVLD:                      0x%08x\n", (unsigned int)result->activity.pvld);
    fprintf(stream, "PPDEC:                     0x%08x\n", (unsigned int)result->activity.ppdec);
    fprintf(stream, "PPPP:                      0x%08x\n", (unsigned int)result->activity.pppp);
    fprintf(stream, "Graphics activity:         %s\n", result->graphics_activity_detected ? "active" : "idle");
    fprintf(stream, "Video activity:            %s\n", result->video_activity_detected ? "active" : "idle");
    fprintf(stream, "History samples:            %u of 64\n", result->activity_history_samples);
    fprintf(stream, "Graphics history:           0x%016llx (%u active)\n", (unsigned long long)result->graphics_history, history_active_samples(result->graphics_history));
    fprintf(stream, "Video history:              0x%016llx (%u active)\n", (unsigned long long)result->video_history, history_active_samples(result->video_history));

    if (result->temperature_observation == GOVERNOR_TEMPERATURE_NOT_POLLED)
        fprintf(stream, "Temperature observation:   not polled\n");
    else if (result->temperature_observation == GOVERNOR_TEMPERATURE_VALID)
        fprintf(stream, "Temperature observation:   valid\n");
    else
        fprintf(stream, "Temperature observation:   read failed (%s)\n", strerror(result->temperature_read_error));

    fprintf(stream, "Retained temperature:      %6.1f C\n", result->temperature_millidegrees / 1000.0);
    fprintf(stream, "Policy recommendation:     %s\n", pstate_name(result->policy.recommended_pstate));

    // Keep the complete bitmask visible during one-shot hardware validation;
    // later event reporting can translate individual flags into messages.
    fprintf(stream, "Policy events:             0x%02x\n", result->policy.events);

    if (!result->pstate_transition_requested)
        fprintf(stream, "Pstate transition:         not required\n");
    else if (result->pstate_transition_deferred)
        fprintf(stream, "Pstate transition:         deferred (attempt interval)\n");
    else if (!result->pstate_transition_attempted)
        fprintf(stream, "Pstate transition:         suppressed (observation only)\n");
    else if (result->pstate_transition_succeeded)
        fprintf(stream, "Pstate transition:         succeeded\n");
    else
        fprintf(stream, "Pstate transition:         failed\n");

    fprintf(stream, "Applied pstate:            %s\n\n", pstate_name(result->applied_pstate));
}

void runtime_print_observation_summary(FILE *stream, const struct runtime_cycle_result *result)
{
    fprintf(stream, "Observation at %llu.%03llu s: graphics %u/%u 0x%016llx; video %u/%u 0x%016llx; ",
        (unsigned long long)(result->now_ms / 1000U),
        (unsigned long long)(result->now_ms % 1000U),
        history_active_samples(result->graphics_history),
        result->activity_history_samples,
        (unsigned long long)result->graphics_history,
        history_active_samples(result->video_history),
        result->activity_history_samples,
        (unsigned long long)result->video_history);

    if (result->temperature_observation == GOVERNOR_TEMPERATURE_VALID)
        fprintf(stream, "temperature %.1f C; ", result->temperature_millidegrees / 1000.0);
    else if (result->temperature_observation == GOVERNOR_TEMPERATURE_READ_FAILED)
        fprintf(stream, "temperature read failed (%s), retained %.1f C; ", strerror(result->temperature_read_error), result->temperature_millidegrees / 1000.0);
    else
        fprintf(stream, "temperature not polled, retained %.1f C; ", result->temperature_millidegrees / 1000.0);

    fprintf(stream, "target %s; applied %s\n", pstate_name(result->policy.recommended_pstate), pstate_name(result->applied_pstate));
}

int runtime_cleanup(struct governor_context *context)
{
    if (context == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (context->gpu.bar0_address == NULL)
        return 0;

    return gpu_mmio_unmap(&context->gpu);
}
