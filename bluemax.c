/**
 * @file bluemax.c
 * @brief BlueMax application entry point and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "runtime_config.h"

#include <errno.h>
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

    if (runtime_cleanup(&context) == -1)
    {
        fprintf(stderr, "%s: cannot unmap GPU BAR0 resource: %s\n", argv[0], strerror(errno));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
