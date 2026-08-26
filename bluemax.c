/**
 * @file bluemax.c
 * @brief BlueMax application entry point and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "runtime_config.h"
#include "shutdown_signal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    /** Exit status used when the command line is invalid. */
    COMMAND_LINE_ERROR_EXIT_STATUS = 2
};

/** @brief Hardware interface locations for the supported target GPU. */
static const struct runtime_paths system_paths = {
    .hwmon_root = "/sys/class/hwmon",
    .pstate_path = "/sys/kernel/debug/dri/0000:01:00.0/pstate",
    .bar0_resource_path = "/sys/bus/pci/devices/0000:01:00.0/resource0",
};

int main(int argc, char *argv[])
{
    struct runtime_config config;
    enum runtime_config_parse_result result = runtime_config_parse(argc, argv, &config, stderr);

    switch (result)
    {
        case RUNTIME_CONFIG_PARSE_OK:
            break;

        case RUNTIME_CONFIG_PARSE_HELP:
            runtime_config_print_help(stdout, argv[0]);
            return 0;

        case RUNTIME_CONFIG_PARSE_VERSION:
            runtime_config_print_version(stdout);
            return 0;

        case RUNTIME_CONFIG_PARSE_ERROR:
            return COMMAND_LINE_ERROR_EXIT_STATUS;
    }

    if (shutdown_signal_install() == -1)
    {
        fprintf(stderr, "%s: cannot install shutdown signal handlers: %s\n", argv[0], strerror(errno));
        return EXIT_FAILURE;
    }

    struct governor_context context;
    enum runtime_startup_result startup_result = runtime_start(&context, &config, &system_paths);
    if (startup_result != RUNTIME_STARTUP_OK)
    {
        fprintf(stderr, "%s: %s: %s\n", argv[0], runtime_startup_result_description(startup_result), strerror(errno));

        if (shutdown_signal_restore() == -1)
            fprintf(stderr, "%s: cannot restore shutdown signal handlers: %s\n", argv[0], strerror(errno));

        return EXIT_FAILURE;
    }

    runtime_print_startup_summary(stdout, &context);
    fputs("Pstate actuation:          disabled (observation only)\n\n", stdout);

    int exit_status = EXIT_SUCCESS;
    if (fflush(stdout) == EOF)
    {
        fprintf(stderr, "%s: cannot write startup summary: %s\n", argv[0], strerror(errno));
        exit_status = EXIT_FAILURE;
    }

    while (exit_status == EXIT_SUCCESS && !shutdown_signal_requested())
    {
        if (runtime_sleep_until_ms(context.schedule.next_sample_deadline_ms) == -1)
        {
            fprintf(stderr, "%s: cannot wait for sampling deadline: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        if (shutdown_signal_requested())
            break;

        uint64_t now_ms;
        if (runtime_monotonic_time_ms(&now_ms) == -1)
        {
            fprintf(stderr, "%s: cannot read monotonic clock: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        bool poll_temperature;

        // The independent temperature timeline decides whether this activity
        // cycle should include the slower hwmon read.
        if (sampling_schedule_prepare(&context.schedule, now_ms, &poll_temperature) == -1)
        {
            fprintf(stderr, "%s: cannot prepare sampling schedule: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        struct runtime_cycle_result cycle;
        enum runtime_cycle_status cycle_status = runtime_observe_cycle(&context, &system_paths, poll_temperature, now_ms, &cycle);
        if (cycle_status != RUNTIME_CYCLE_OK)
        {
            fprintf(stderr, "%s: cannot execute governor cycle: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        bool report_written = cycle.policy.events != GOVERNOR_POLICY_EVENT_NONE || poll_temperature;

        if (cycle.policy.events != GOVERNOR_POLICY_EVENT_NONE)
            runtime_print_cycle_summary(stdout, &cycle);
        else if (poll_temperature)
            runtime_print_observation_summary(stdout, &cycle);

        if (report_written)
        {
            if (fflush(stdout) == EOF)
            {
                fprintf(stderr, "%s: cannot write governor observation: %s\n", argv[0], strerror(errno));
                exit_status = EXIT_FAILURE;
                break;
            }
        }

        // Advance from time measured after MMIO, temperature, policy, and
        // pstate work. A slow cycle therefore skips deadlines consumed while
        // that work was running instead of replaying them.
        uint64_t completed_ms;
        if (runtime_monotonic_time_ms(&completed_ms) == -1)
        {
            fprintf(stderr, "%s: cannot read monotonic clock after governor cycle: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        if (sampling_schedule_complete(&context.schedule, &context.config, completed_ms, poll_temperature) == -1)
        {
            fprintf(stderr, "%s: cannot advance sampling schedule: %s\n", argv[0], strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }
    }

    // Every path after successful startup reaches cleanup, including signal,
    // clock, and telemetry exits, so BAR0 is never intentionally retained.
    bool cleanup_succeeded = runtime_cleanup(&context) == 0;
    if (!cleanup_succeeded)
    {
        fprintf(stderr, "%s: cannot unmap GPU BAR0 resource: %s\n", argv[0], strerror(errno));
        exit_status = EXIT_FAILURE;
    }

    bool signals_restored = shutdown_signal_restore() == 0;
    if (!signals_restored)
    {
        fprintf(stderr, "%s: cannot restore shutdown signal handlers: %s\n", argv[0], strerror(errno));
        exit_status = EXIT_FAILURE;
    }

    if (!cleanup_succeeded || !signals_restored)
        return exit_status;

    fputs("BAR0 telemetry unmapped\nBlueMax shutdown complete\n", stdout);
    return exit_status;
}
