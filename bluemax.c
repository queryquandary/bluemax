/**
 * @file bluemax.c
 * @brief BlueMax application entry point and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "runtime_config.h"

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

    struct governor_context context;
    enum runtime_startup_result startup_result = runtime_start(&context, &config, &system_paths);
    if (startup_result != RUNTIME_STARTUP_OK)
    {
        fprintf(stderr, "%s: %s: %s\n", argv[0], runtime_startup_result_description(startup_result), strerror(errno));
        return EXIT_FAILURE;
    }

    runtime_print_startup_summary(stdout, &context);

    int exit_status = EXIT_SUCCESS;
    uint64_t now_ms;
    struct runtime_cycle_result cycle;

    if (runtime_monotonic_time_ms(&now_ms) == -1)
    {
        fprintf(stderr, "%s: cannot read monotonic clock: %s\n", argv[0], strerror(errno));
        exit_status = EXIT_FAILURE;
    }
    else
    {
        // Startup already supplied a valid temperature, so this one-shot cycle
        // samples activity without performing an immediate duplicate poll.
        enum runtime_cycle_status cycle_status = runtime_run_cycle(&context, &system_paths, false, now_ms, &cycle);

        // Console output may change errno. Retain the cycle error before
        // printing its structured observations and decision.
        int cycle_error = errno;
        runtime_print_cycle_summary(stdout, &cycle);

        if (cycle_status == RUNTIME_CYCLE_PSTATE_ERROR)
        {
            fprintf(stderr, "%s: cannot apply recommended GPU pstate: %s\n", argv[0], strerror(cycle_error));
            exit_status = EXIT_FAILURE;
        }
        else if (cycle_status != RUNTIME_CYCLE_OK)
        {
            fprintf(stderr, "%s: cannot execute governor cycle: %s\n", argv[0], strerror(cycle_error));
            exit_status = EXIT_FAILURE;
        }
    }

    // Every path after successful startup reaches cleanup, including clock and
    // pstate failures, so the BAR0 mapping is never intentionally retained.
    if (runtime_cleanup(&context) == -1)
    {
        fprintf(stderr, "%s: cannot unmap GPU BAR0 resource: %s\n", argv[0], strerror(errno));
        return EXIT_FAILURE;
    }

    fputs("BAR0 telemetry unmapped\nBlueMax shutdown complete\n", stdout);
    return exit_status;
}
