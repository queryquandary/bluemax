/**
 * @file test_runtime_config.c
 * @brief Tests for BlueMax command-line runtime configuration.
 */

#include "runtime_config.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
            return -1;                                                          \
        }                                                                       \
    } while (0)

/** Parse arguments while capturing any diagnostic text. */
static int parse_with_diagnostics(int argc, char *const argv[], struct runtime_config *config, enum runtime_config_parse_result *result, char *diagnostic, size_t capacity)
{
    FILE *stream = tmpfile();
    if (stream == NULL)
        return -1;

    *result = runtime_config_parse(argc, argv, config, stream);

    if (fflush(stream) == EOF || fseek(stream, 0, SEEK_SET) == -1)
    {
        fclose(stream);
        return -1;
    }

    size_t received = fread(diagnostic, 1, capacity - 1, stream);

    if (ferror(stream))
    {
        fclose(stream);
        return -1;
    }

    diagnostic[received] = '\0';
    return fclose(stream);
}

/** Verify defaults and the absence of diagnostics for an empty command line. */
static int test_applies_defaults(void)
{
    char *argv[] = {"bluemax"};
    struct runtime_config config = {0};
    enum runtime_config_parse_result result;
    char diagnostic[512];

    CHECK(parse_with_diagnostics(1, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_OK);
    CHECK(config.sample_interval_ms == 10);
    CHECK(config.temperature_poll_interval_ms == 1000);
    CHECK(strcmp(diagnostic, "") == 0);
    return 0;
}

/** Verify both short and long option names with separated values. */
static int test_accepts_short_and_long_options(void)
{
    char *short_argv[] = {"bluemax", "-s", "20", "-t", "500"};
    char *long_argv[] = {"bluemax", "--sample-interval-ms", "050", "--temperature-poll-interval-ms", "01000"};
    struct runtime_config config;
    enum runtime_config_parse_result result;
    char diagnostic[512];

    CHECK(parse_with_diagnostics(5, short_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_OK);
    CHECK(config.sample_interval_ms == 20);
    CHECK(config.temperature_poll_interval_ms == 500);
    CHECK(parse_with_diagnostics(5, long_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_OK);
    CHECK(config.sample_interval_ms == 50);
    CHECK(config.temperature_poll_interval_ms == 1000);
    return 0;
}

/** Verify inclusive minimum and maximum interval boundaries. */
static int test_accepts_interval_boundaries(void)
{
    char *minimum_argv[] = {"bluemax", "-s", "1", "-t", "100"};
    char *maximum_argv[] = {"bluemax", "-s", "1000", "-t", "1000"};
    struct runtime_config config;
    enum runtime_config_parse_result result;
    char diagnostic[512];

    CHECK(parse_with_diagnostics(5, minimum_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_OK);
    CHECK(config.sample_interval_ms == 1);
    CHECK(config.temperature_poll_interval_ms == 100);
    CHECK(parse_with_diagnostics(5, maximum_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_OK);
    CHECK(config.sample_interval_ms == 1000);
    CHECK(config.temperature_poll_interval_ms == 1000);
    return 0;
}

/** Verify malformed, overflowing, and out-of-range values are rejected. */
static int test_rejects_invalid_values_transactionally(void)
{
    static const char *values[] = {
        "",
        "+10",
        "-10",
        " 10",
        "10 ",
        "10ms",
        "0",
        "1001",
        "999999999999999999999999999999999999",
    };

    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++)
    {
        char *argv[] = {"bluemax", "-s", (char *)values[index]};
        struct runtime_config config = {77, 777};
        enum runtime_config_parse_result result;
        char diagnostic[512];

        CHECK(parse_with_diagnostics(3, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
        CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
        CHECK(config.sample_interval_ms == 77);
        CHECK(config.temperature_poll_interval_ms == 777);
        CHECK(strstr(diagnostic, "invalid value") != NULL);
        CHECK(strstr(diagnostic, "--help") != NULL);
    }

    char *temperature_argv[] = {"bluemax", "-t", "99"};
    struct runtime_config config = {77, 777};
    enum runtime_config_parse_result result;
    char diagnostic[512];
    CHECK(parse_with_diagnostics(3, temperature_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
    CHECK(config.sample_interval_ms == 77);
    CHECK(config.temperature_poll_interval_ms == 777);
    return 0;
}

/** Verify values must occupy a separate following command-line argument. */
static int test_rejects_unsupported_value_forms(void)
{
    static const char *arguments[] = {
        "-s20",
        "-t1000",
        "--sample-interval-ms=20",
        "--temperature-poll-interval-ms=1000",
    };

    for (size_t index = 0; index < sizeof(arguments) / sizeof(arguments[0]); index++)
    {
        char *argv[] = {"bluemax", (char *)arguments[index]};
        struct runtime_config config = {77, 777};
        enum runtime_config_parse_result result;
        char diagnostic[512];

        CHECK(parse_with_diagnostics(2, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
        CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
        CHECK(strstr(diagnostic, "unrecognized option") != NULL);
    }

    return 0;
}

/** Verify missing values and duplicate aliases are rejected. */
static int test_rejects_missing_and_duplicate_options(void)
{
    char *missing_argv[] = {"bluemax", "--sample-interval-ms"};
    char *duplicate_argv[] = {"bluemax", "-s", "10", "--sample-interval-ms", "20"};
    struct runtime_config config = {77, 777};
    enum runtime_config_parse_result result;
    char diagnostic[512];

    CHECK(parse_with_diagnostics(2, missing_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
    CHECK(strstr(diagnostic, "requires a value") != NULL);
    CHECK(parse_with_diagnostics(5, duplicate_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
    CHECK(strstr(diagnostic, "duplicate option") != NULL);
    CHECK(config.sample_interval_ms == 77);
    CHECK(config.temperature_poll_interval_ms == 777);
    return 0;
}

/** Verify unknown options, positional arguments, and clusters are rejected. */
static int test_rejects_unrecognized_arguments(void)
{
    static const char *arguments[] = {"--unknown", "unexpected", "-hV", "--actuate", "-a"};

    for (size_t index = 0; index < sizeof(arguments) / sizeof(arguments[0]); index++)
    {
        char *argv[] = {"bluemax", (char *)arguments[index]};
        struct runtime_config config = {77, 777};
        enum runtime_config_parse_result result;
        char diagnostic[512];

        CHECK(parse_with_diagnostics(2, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
        CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
        CHECK(config.sample_interval_ms == 77);
        CHECK(config.temperature_poll_interval_ms == 777);
    }
    return 0;
}

/** Verify temperature polling cannot be faster than activity sampling. */
static int test_rejects_incompatible_intervals(void)
{
    char *argv[] = {"bluemax", "-s", "101", "-t", "100"};
    struct runtime_config config = {77, 777};
    enum runtime_config_parse_result result;
    char diagnostic[512];

    CHECK(parse_with_diagnostics(5, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
    CHECK(config.sample_interval_ms == 77);
    CHECK(config.temperature_poll_interval_ms == 777);
    CHECK(strstr(diagnostic, "greater than or equal") != NULL);
    return 0;
}

/** Verify help and version results for both aliases and exclusive use. */
static int test_recognizes_help_and_version(void)
{
    static const struct {
        const char *option;
        enum runtime_config_parse_result expected;
    } cases[] = {
        {"-h", RUNTIME_CONFIG_PARSE_HELP},
        {"--help", RUNTIME_CONFIG_PARSE_HELP},
        {"-V", RUNTIME_CONFIG_PARSE_VERSION},
        {"--version", RUNTIME_CONFIG_PARSE_VERSION},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        char *argv[] = {"bluemax", (char *)cases[index].option};
        struct runtime_config config = {77, 777};
        enum runtime_config_parse_result result;
        char diagnostic[512];

        CHECK(parse_with_diagnostics(2, argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
        CHECK(result == cases[index].expected);
        CHECK(config.sample_interval_ms == 77);
        CHECK(config.temperature_poll_interval_ms == 777);
        CHECK(strcmp(diagnostic, "") == 0);
    }

    char *mixed_argv[] = {"bluemax", "-s", "10", "--version"};
    struct runtime_config config = {77, 777};
    enum runtime_config_parse_result result;
    char diagnostic[512];
    CHECK(parse_with_diagnostics(4, mixed_argv, &config, &result, diagnostic, sizeof(diagnostic)) == 0);
    CHECK(result == RUNTIME_CONFIG_PARSE_ERROR);
    CHECK(strstr(diagnostic, "must be used alone") != NULL);
    return 0;
}

/** Verify stable help and version presentation. */
static int test_prints_help_and_version(void)
{
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    runtime_config_print_help(stream, "bluemax");
    runtime_config_print_version(stream);
    CHECK(fflush(stream) != EOF);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);

    char output[2048];
    size_t received = fread(output, 1, sizeof(output) - 1, stream);
    CHECK(!ferror(stream));
    output[received] = '\0';
    CHECK(fclose(stream) == 0);

    CHECK(strstr(output, "Usage: bluemax [OPTIONS]") != NULL);
    CHECK(strstr(output, "-s N, --sample-interval-ms N") != NULL);
    CHECK(strstr(output, "-t N, --temperature-poll-interval-ms N") != NULL);
    CHECK(strstr(output, "--actuate") == NULL);
    CHECK(strstr(output, "sample-based") != NULL);
    CHECK(strstr(output, "BlueMax 0.1.0\n") != NULL);
    return 0;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"applies defaults", test_applies_defaults},
        {"accepts short and long options", test_accepts_short_and_long_options},
        {"accepts interval boundaries", test_accepts_interval_boundaries},
        {"rejects invalid values transactionally", test_rejects_invalid_values_transactionally},
        {"rejects unsupported value forms", test_rejects_unsupported_value_forms},
        {"rejects missing and duplicate options", test_rejects_missing_and_duplicate_options},
        {"rejects unrecognized arguments", test_rejects_unrecognized_arguments},
        {"rejects incompatible intervals", test_rejects_incompatible_intervals},
        {"recognizes help and version", test_recognizes_help_and_version},
        {"prints help and version", test_prints_help_and_version},
    };

    int failures = 0;

    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
    {
        if (tests[index].run() == 0)
            printf("PASS: %s\n", tests[index].name);
        else
            failures++;
    }

    if (failures != 0)
    {
        fprintf(stderr, "%d runtime configuration test(s) failed\n", failures);
        return 1;
    }

    printf("All runtime configuration tests passed\n");
    return 0;
}
