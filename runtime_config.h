#ifndef BLUEMAX_RUNTIME_CONFIG_H
#define BLUEMAX_RUNTIME_CONFIG_H

#include <stdio.h>

/** @brief Current BlueMax release version. */
#define BLUEMAX_VERSION "0.1.0"

enum {
    RUNTIME_CONFIG_DEFAULT_SAMPLE_INTERVAL_MS = 10,
    RUNTIME_CONFIG_MIN_SAMPLE_INTERVAL_MS = 1,
    RUNTIME_CONFIG_MAX_SAMPLE_INTERVAL_MS = 1000,
    RUNTIME_CONFIG_DEFAULT_TEMPERATURE_POLL_INTERVAL_MS = 1000,
    RUNTIME_CONFIG_MIN_TEMPERATURE_POLL_INTERVAL_MS = 100,
    RUNTIME_CONFIG_MAX_TEMPERATURE_POLL_INTERVAL_MS = 1000
};

/** @brief Validated settings controlling runtime polling intervals. */
struct runtime_config {
    /** Interval between GPU activity samples. */
    unsigned int sample_interval_ms;

    /** Interval between scheduled temperature reads. */
    unsigned int temperature_poll_interval_ms;
};

/** @brief Outcomes that determine the application command-line path. */
enum runtime_config_parse_result {
    RUNTIME_CONFIG_PARSE_OK,
    RUNTIME_CONFIG_PARSE_HELP,
    RUNTIME_CONFIG_PARSE_VERSION,
    RUNTIME_CONFIG_PARSE_ERROR
};

/**
 * @brief Parse and validate the BlueMax command line.
 *
 * Options taking a value require it in the following argv element. The
 * destination is assigned only after the complete command line is valid.
 * Diagnostics for user input errors are written to @p error_stream.
 *
 * @param[in] argc Number of command-line arguments, including the program.
 * @param[in] argv Command-line argument vector.
 * @param[out] config Destination for validated settings.
 * @param[in,out] error_stream Stream that receives command-line diagnostics.
 *
 * @return A result selecting normal execution, help, version, or error.
 */
enum runtime_config_parse_result runtime_config_parse(int argc, char *const argv[], struct runtime_config *config, FILE *error_stream);

/**
 * @brief Print command-line usage and available options.
 *
 * @param[in,out] stream Destination stream.
 * @param[in] program_name Name used in the usage line.
 */
void runtime_config_print_help(FILE *stream, const char *program_name);

/**
 * @brief Print the compiled BlueMax version.
 *
 * @param[in,out] stream Destination stream.
 */
void runtime_config_print_version(FILE *stream);

#endif
