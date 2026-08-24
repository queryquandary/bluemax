/**
 * @file runtime.c
 * @brief BlueMax hardware and policy startup lifecycle.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/** Read the current monotonic time and convert it to whole milliseconds. */
static int read_monotonic_time_ms(uint64_t *now_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return -1;

    if (now.tv_sec < 0 || (uint64_t)now.tv_sec > (UINT64_MAX - 999U) / 1000U)
    {
        errno = EOVERFLOW;
        return -1;
    }

    *now_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
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
    
    if (read_monotonic_time_ms(&now_ms) == -1)
        return rollback_mapping(&candidate, RUNTIME_STARTUP_CLOCK_ERROR);

    if (governor_policy_init(
            &candidate.policy,
            candidate.applied_pstate,
            candidate.thermal.max_millidegrees,
            candidate.thermal.max_hyst_millidegrees,
            candidate.temperature_millidegrees,
            now_ms) == -1)
        return rollback_mapping(&candidate, RUNTIME_STARTUP_POLICY_ERROR);

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
