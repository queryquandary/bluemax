/**
 * @file runtime_config.c
 * @brief Command-line parsing and presentation for BlueMax runtime settings.
 */

#include "runtime_config.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/** Print the standard hint following a command-line error. */
static void print_help_hint(FILE *stream, const char *program_name)
{
    fprintf(stream, "Try '%s --help' for more information.\n", program_name);
}

/** Report that an option requiring exclusive use appeared with another arg. */
static enum runtime_config_parse_result report_exclusive_option_error(FILE *stream, const char *program_name, const char *option)
{
    fprintf(stream, "%s: option '%s' must be used alone\n", program_name, option);
    print_help_hint(stream, program_name);
    return RUNTIME_CONFIG_PARSE_ERROR;
}

/** Report a generic command-line argument error and its offending argument. */
static enum runtime_config_parse_result report_argument_error(FILE *stream, const char *program_name, const char *description, const char *argument)
{
    fprintf(stream, "%s: %s '%s'\n", program_name, description, argument);
    print_help_hint(stream, program_name);
    return RUNTIME_CONFIG_PARSE_ERROR;
}

/** Parse a bounded unsigned decimal value without accepting signs or suffixes. */
static bool parse_interval(const char *text, unsigned int minimum, unsigned int maximum, unsigned int *value)
{
    if (text == NULL || text[0] == '\0')
        return false;

    unsigned int parsed = 0;

    for (const unsigned char *position = (const unsigned char *)text; *position != '\0'; position++)
    {
        if (*position < '0' || *position > '9')
            return false;

        unsigned int digit = (unsigned int)(*position - '0');

        if (parsed > (UINT_MAX - digit) / 10U)
            return false;

        parsed = parsed * 10U + digit;
    }

    if (parsed < minimum || parsed > maximum)
        return false;

    *value = parsed;
    return true;
}

/** Report a missing value for an interval option. */
static enum runtime_config_parse_result report_missing_value(FILE *stream, const char *program_name, const char *option)
{
    fprintf(stream, "%s: option '%s' requires a value\n", program_name, option);
    print_help_hint(stream, program_name);
    return RUNTIME_CONFIG_PARSE_ERROR;
}

/** Report an invalid or out-of-range interval value. */
static enum runtime_config_parse_result report_invalid_value(FILE *stream, const char *program_name, const char *option, const char *value, unsigned int minimum, unsigned int maximum)
{
    fprintf(stream, "%s: invalid value '%s' for '%s'; expected a decimal integer from %u to %u\n", program_name, value, option, minimum, maximum);
    print_help_hint(stream, program_name);
    return RUNTIME_CONFIG_PARSE_ERROR;
}

enum runtime_config_parse_result runtime_config_parse(int argc, char *const argv[], struct runtime_config *config, FILE *error_stream)
{
    if (argc < 1 || argv == NULL || argv[0] == NULL || config == NULL || error_stream == NULL)
    {
        errno = EINVAL;
        return RUNTIME_CONFIG_PARSE_ERROR;
    }

    const char *program_name = argv[0];

    for (int index = 1; index < argc; index++)
    {
        if (argv[index] == NULL)
        {
            errno = EINVAL;
            return RUNTIME_CONFIG_PARSE_ERROR;
        }

        bool help = strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0;
        bool version = strcmp(argv[index], "-V") == 0 || strcmp(argv[index], "--version") == 0;

        if (help || version)
        {
            if (argc != 2)
                return report_exclusive_option_error(error_stream, program_name, argv[index]);

            return help ? RUNTIME_CONFIG_PARSE_HELP
                        : RUNTIME_CONFIG_PARSE_VERSION;
        }
    }

    struct runtime_config candidate = {
        .sample_interval_ms = RUNTIME_CONFIG_DEFAULT_SAMPLE_INTERVAL_MS,
        .temperature_poll_interval_ms = RUNTIME_CONFIG_DEFAULT_TEMPERATURE_POLL_INTERVAL_MS,
        .pstate_actuation_enabled = false,
    };

    bool sample_seen = false;
    bool temperature_seen = false;
    bool actuate_seen = false;

    for (int index = 1; index < argc; index++)
    {
        const char *option = argv[index];
        bool sample = strcmp(option, "-s") == 0 || strcmp(option, "--sample-interval-ms") == 0;
        bool temperature = strcmp(option, "-t") == 0 || strcmp(option, "--temperature-poll-interval-ms") == 0;
        bool actuate = strcmp(option, "--actuate") == 0;

        if (!sample && !temperature && !actuate)
        {
            const char *description = option[0] == '-' ? "unrecognized option" : "unexpected argument";
            return report_argument_error(error_stream, program_name, description, option);
        }

        if (actuate)
        {
            if (actuate_seen)
                return report_argument_error(error_stream, program_name, "duplicate option", option);

            // Actuation is deliberately a long-form, valueless opt-in so the
            // experimental hardware-writing mode cannot be enabled implicitly.
            actuate_seen = true;
            candidate.pstate_actuation_enabled = true;
            continue;
        }

        bool *seen = sample ? &sample_seen : &temperature_seen;

        if (*seen)
            return report_argument_error(error_stream, program_name, "duplicate option", option);

        *seen = true;

        if (index + 1 >= argc)
            return report_missing_value(error_stream, program_name, option);

        const char *value = argv[++index];

        unsigned int minimum = sample
                                   ? RUNTIME_CONFIG_MIN_SAMPLE_INTERVAL_MS
                                   : RUNTIME_CONFIG_MIN_TEMPERATURE_POLL_INTERVAL_MS;

        unsigned int maximum = sample
                                   ? RUNTIME_CONFIG_MAX_SAMPLE_INTERVAL_MS
                                   : RUNTIME_CONFIG_MAX_TEMPERATURE_POLL_INTERVAL_MS;

        unsigned int *destination = sample
                                        ? &candidate.sample_interval_ms
                                        : &candidate.temperature_poll_interval_ms;

        if (!parse_interval(value, minimum, maximum, destination))
            return report_invalid_value(error_stream, program_name, option, value, minimum, maximum);
    }

    if (candidate.temperature_poll_interval_ms < candidate.sample_interval_ms)
    {
        fprintf(error_stream, "%s: temperature polling interval must be greater than or equal to the activity sampling interval\n", program_name);
        print_help_hint(error_stream, program_name);
        return RUNTIME_CONFIG_PARSE_ERROR;
    }

    *config = candidate;
    return RUNTIME_CONFIG_PARSE_OK;
}

void runtime_config_print_help(FILE *stream, const char *program_name)
{
    fprintf(stream, "Usage: %s [OPTIONS]\n", program_name);
    fputs("\nOptions:\n", stream);
    fputs("  -s N, --sample-interval-ms N\n", stream);
    fprintf(stream, "      Activity sampling interval in milliseconds (default %d, range %d-%d).\n", RUNTIME_CONFIG_DEFAULT_SAMPLE_INTERVAL_MS, RUNTIME_CONFIG_MIN_SAMPLE_INTERVAL_MS, RUNTIME_CONFIG_MAX_SAMPLE_INTERVAL_MS);
    fputs("  -t N, --temperature-poll-interval-ms N\n", stream);
    fprintf(stream, "      Temperature polling interval in milliseconds (default %d, range %d-%d).\n", RUNTIME_CONFIG_DEFAULT_TEMPERATURE_POLL_INTERVAL_MS, RUNTIME_CONFIG_MIN_TEMPERATURE_POLL_INTERVAL_MS, RUNTIME_CONFIG_MAX_TEMPERATURE_POLL_INTERVAL_MS);
    fputs("  --actuate\n", stream);
    fputs("      Apply policy pstate recommendations (experimental; disabled by default).\n", stream);
    fputs("  -h, --help\n", stream);
    fputs("      Display this help and exit.\n", stream);
    fputs("  -V, --version\n", stream);
    fputs("      Display version information and exit.\n", stream);
    fputs("\nWorkload triggers are sample-based; increasing the activity sampling\n", stream);
    fputs("interval increases workload-trigger latency proportionally.\n", stream);
}

void runtime_config_print_version(FILE *stream)
{
    fprintf(stream, "BlueMax %s\n", BLUEMAX_VERSION);
}
