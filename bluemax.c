/**
 * @file bluemax.c
 * @brief BlueMax application entry point and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include "governor_policy.h"
#include "gpu_mmio.h"
#include "gpu_pstate.h"
#include "runtime_config.h"

enum {
    /** Exit status used when the command line is invalid. */
    COMMAND_LINE_ERROR_EXIT_STATUS = 2
};

/** @brief Runtime state and resources owned by the single-threaded governor. */
struct governor_context {
    /** Validated runtime intervals supplied through the command line. */
    struct runtime_config config;

    /** Last performance state successfully read from or applied to the GPU. */
    enum gpu_pstate applied_pstate;

    /** Workload and thermal policy state. */
    struct governor_policy policy;

    /** Read-only GPU BAR0 mapping and resolved activity registers. */
    struct gpu_mmio gpu;
};

int main(int argc, char *argv[])
{
    struct governor_context context;
    enum runtime_config_parse_result result = runtime_config_parse(argc, argv, &context.config, stderr);

    switch (result)
    {
        case RUNTIME_CONFIG_PARSE_OK:
            return 0;

        case RUNTIME_CONFIG_PARSE_HELP:
            runtime_config_print_help(stdout, argv[0]);
            return 0;

        case RUNTIME_CONFIG_PARSE_VERSION:
            runtime_config_print_version(stdout);
            return 0;

        case RUNTIME_CONFIG_PARSE_ERROR:
            return COMMAND_LINE_ERROR_EXIT_STATUS;
    }

    return COMMAND_LINE_ERROR_EXIT_STATUS;
}
