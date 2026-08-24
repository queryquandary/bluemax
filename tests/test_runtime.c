/**
 * @file test_runtime.c
 * @brief Tests for BlueMax runtime hardware and policy initialization.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "test_helpers.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MOCK_BAR0_LENGTH ((off_t)16 * 1024 * 1024)

/** @brief Synthetic hardware tree owned by one runtime test. */
struct runtime_fixture {
    char root[PATH_MAX];
    char hwmon_device[PATH_MAX];
    char pstate_path[PATH_MAX];
    char resource_path[PATH_MAX];
    struct runtime_paths paths;
};

/** Evaluate one condition and retain the test's cleanup path on failure. */
#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
            result = -1;                                                        \
            goto cleanup;                                                       \
        }                                                                       \
    } while (0)

/** Remove the explicitly known files and directories in a fixture. */
static void fixture_destroy(struct runtime_fixture *fixture)
{
    static const char *const thermal_files[] = {
        "name",
        "temp1_input",
        "temp1_max",
        "temp1_max_hyst",
    };

    test_remove_file(fixture->root, "pstate");
    test_remove_file(fixture->root, "resource0");

    for (size_t index = 0; index < sizeof(thermal_files) / sizeof(thermal_files[0]); index++)
        test_remove_file(fixture->hwmon_device, thermal_files[index]);

    if (fixture->hwmon_device[0] != '\0')
        rmdir(fixture->hwmon_device);

    if (fixture->root[0] != '\0')
        rmdir(fixture->root);
}

/** Create representative synthetic thermal, pstate, and BAR0 interfaces. */
static int fixture_create(struct runtime_fixture *fixture)
{
    static const char root_template[] = "/tmp/bluemax-runtime-test-XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->root, root_template, sizeof(root_template));

    if (mkdtemp(fixture->root) == NULL
        || test_build_path(fixture->hwmon_device, sizeof(fixture->hwmon_device), fixture->root, "hwmon0") == -1
        || mkdir(fixture->hwmon_device, 0700) == -1
        || test_write_text(fixture->hwmon_device, "name", "nouveau\n") == -1
        || test_write_text(fixture->hwmon_device, "temp1_input", "51000\n") == -1
        || test_write_text(fixture->hwmon_device, "temp1_max", "95000\n") == -1
        || test_write_text(fixture->hwmon_device, "temp1_max_hyst", "3000\n") == -1
        || test_write_text(
               fixture->root,
               "pstate",
               "03: core 135 MHz shader 270 MHz memory 135 MHz *\n"
               "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
               "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n") == -1
        || test_create_sized_file(fixture->root, "resource0", MOCK_BAR0_LENGTH) == -1
        || test_build_path(fixture->pstate_path, sizeof(fixture->pstate_path), fixture->root, "pstate") == -1
        || test_build_path(fixture->resource_path, sizeof(fixture->resource_path), fixture->root, "resource0") == -1)
    {
        int setup_error = errno;
        fixture_destroy(fixture);
        errno = setup_error;
        return -1;
    }

    fixture->paths = (struct runtime_paths){
        .hwmon_root = fixture->root,
        .pstate_path = fixture->pstate_path,
        .bar0_resource_path = fixture->resource_path,
    };
    return 0;
}

/** Convert a valid monotonic timestamp to whole milliseconds. */
static uint64_t monotonic_time_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return 0;

    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/** Verify complete startup state, non-mutating pstate observation, and cleanup. */
static int test_initializes_and_cleans_up_runtime(void)
{
    struct runtime_fixture fixture;
    if (fixture_create(&fixture) == -1)
    {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct runtime_config config = {
        .sample_interval_ms = 20,
        .temperature_poll_interval_ms = 500,
    };
    struct governor_context context = {0};
    char original_pstate[256];
    char current_pstate[256];

    CHECK(test_read_text(fixture.root, "pstate", original_pstate, sizeof(original_pstate)) == 0);

    uint64_t earliest_ms = monotonic_time_ms();
    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    uint64_t latest_ms = monotonic_time_ms();

    CHECK(context.config.sample_interval_ms == 20);
    CHECK(context.config.temperature_poll_interval_ms == 500);
    CHECK(context.applied_pstate == GPU_PSTATE_LOW);
    CHECK(context.thermal.max_millidegrees == 95000);
    CHECK(context.thermal.max_hyst_millidegrees == 3000);
    CHECK(context.temperature_millidegrees == 51000);
    CHECK(context.policy.target_pstate == GPU_PSTATE_LOW);
    CHECK(context.policy.temperature_max_millidegrees == 95000);
    CHECK(context.policy.temperature_hysteresis_millidegrees == 3000);
    CHECK(context.policy.last_activity_ms >= earliest_ms);
    CHECK(context.policy.last_activity_ms <= latest_ms);
    CHECK(context.policy.high_since_ms == context.policy.last_activity_ms);
    CHECK(context.gpu.bar0_address != NULL);
    CHECK(context.gpu.bar0_length == (size_t)MOCK_BAR0_LENGTH);

    char expected_input_path[PATH_MAX];
    CHECK(test_build_path(expected_input_path, sizeof(expected_input_path), fixture.hwmon_device, "temp1_input") == 0);
    CHECK(strcmp(context.thermal.input_path, expected_input_path) == 0);

    CHECK(test_read_text(fixture.root, "pstate", current_pstate, sizeof(current_pstate)) == 0);
    CHECK(strcmp(current_pstate, original_pstate) == 0);

    CHECK(runtime_cleanup(&context) == 0);
    CHECK(context.gpu.bar0_address == NULL);
    CHECK(context.gpu.bar0_length == 0);
    CHECK(context.gpu.pgraph_reg == NULL);
    CHECK(context.gpu.pvld_reg == NULL);
    CHECK(context.gpu.ppdec_reg == NULL);
    CHECK(context.gpu.pppp_reg == NULL);
    CHECK(runtime_cleanup(&context) == 0);

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Capture and verify the one-time console summary of initialized state. */
static int test_prints_startup_summary(void)
{
    struct runtime_fixture fixture;
    if (fixture_create(&fixture) == -1)
    {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct runtime_config config = {10, 1000};
    struct governor_context context = {0};
    FILE *stream = NULL;
    char summary[2048];

    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    stream = tmpfile();
    CHECK(stream != NULL);

    runtime_print_startup_summary(stream, &context);
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);

    size_t received = fread(summary, 1, sizeof(summary) - 1, stream);
    CHECK(!ferror(stream));
    summary[received] = '\0';

    CHECK(strstr(summary, "BlueMax 0.1.0 startup") != NULL);
    CHECK(strstr(summary, "Activity sample interval:       10 ms") != NULL);
    CHECK(strstr(summary, "Temperature poll interval:   1000 ms") != NULL);
    CHECK(strstr(summary, context.thermal.input_path) != NULL);
    CHECK(strstr(summary, "Initial temperature:         51.0 C") != NULL);
    CHECK(strstr(summary, "Maximum temperature:         95.0 C") != NULL);
    CHECK(strstr(summary, "Recovery threshold:        below 92.0 C") != NULL);
    CHECK(strstr(summary, "Applied pstate:            LOW (03)") != NULL);
    CHECK(strstr(summary, "Policy target:             LOW (03)") != NULL);
    CHECK(strstr(summary, "Thermal limit:             inactive") != NULL);
    CHECK(strstr(summary, "BAR0 telemetry:            mapped read-only (16 MiB)") != NULL);

cleanup:
    if (stream != NULL)
        fclose(stream);

    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify observed startup pstates and policy normalization independently. */
static int test_initializes_supported_pstates(void)
{
    static const struct {
        const char *listing;
        enum gpu_pstate applied;
        enum gpu_pstate target;
    } cases[] = {
        {"03: low *\n07: medium\n0f: high\n", GPU_PSTATE_LOW, GPU_PSTATE_LOW},
        {"03: low\n07: medium *\n0f: high\n", GPU_PSTATE_MEDIUM, GPU_PSTATE_LOW},
        {"03: low\n07: medium\n0f: high *\n", GPU_PSTATE_HIGH, GPU_PSTATE_HIGH},
    };

    struct runtime_fixture fixture;
    if (fixture_create(&fixture) == -1)
    {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct runtime_config config = {10, 1000};
    struct governor_context context = {0};

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        CHECK(test_write_text(fixture.root, "pstate", cases[index].listing) == 0);
        CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
        CHECK(context.applied_pstate == cases[index].applied);
        CHECK(context.policy.target_pstate == cases[index].target);
        CHECK(runtime_cleanup(&context) == 0);
    }

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify unsafe startup telemetry selects LOW without changing the GPU. */
static int test_initial_temperature_applies_thermal_limit(void)
{
    struct runtime_fixture fixture;
    if (fixture_create(&fixture) == -1)
    {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct runtime_config config = {10, 1000};
    struct governor_context context = {0};
    char pstate_contents[128];

    CHECK(test_write_text(fixture.hwmon_device, "temp1_input", "95000\n") == 0);
    CHECK(test_write_text(fixture.root, "pstate", "03: low\n07: medium\n0f: high *\n") == 0);
    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    CHECK(context.applied_pstate == GPU_PSTATE_HIGH);
    CHECK(context.policy.target_pstate == GPU_PSTATE_LOW);
    CHECK(context.policy.thermal_limit_active);
    CHECK(test_read_text(fixture.root, "pstate", pstate_contents, sizeof(pstate_contents)) == 0);
    CHECK(strcmp(pstate_contents, "03: low\n07: medium\n0f: high *\n") == 0);

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify each hardware-stage failure leaves the destination unpublished. */
static int test_reports_hardware_failures_transactionally(void)
{
    struct runtime_fixture fixture;
    if (fixture_create(&fixture) == -1)
    {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct runtime_config config = {10, 1000};
    struct governor_context context;
    struct governor_context unchanged;

    memset(&context, 0xa5, sizeof(context));
    unchanged = context;
    struct runtime_paths missing_thermal = fixture.paths;
    missing_thermal.hwmon_root = "/tmp/bluemax-runtime-test-does-not-exist";
    errno = 0;
    CHECK(runtime_start(&context, &config, &missing_thermal) == RUNTIME_STARTUP_THERMAL_ERROR);
    CHECK(errno == ENOENT);
    CHECK(memcmp(&context, &unchanged, sizeof(context)) == 0);

    test_remove_file(fixture.root, "pstate");
    errno = 0;
    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_PSTATE_ERROR);
    CHECK(errno == ENOENT);
    CHECK(memcmp(&context, &unchanged, sizeof(context)) == 0);

    CHECK(test_write_text(fixture.root, "pstate", "03: low *\n") == 0);
    CHECK(test_create_sized_file(fixture.root, "resource0", 4096) == 0);
    errno = 0;
    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_MMIO_ERROR);
    CHECK(errno == EINVAL);
    CHECK(memcmp(&context, &unchanged, sizeof(context)) == 0);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/** Verify programmer-error inputs and cleanup arguments are rejected safely. */
static int test_rejects_invalid_arguments(void)
{
    int result = 0;
    struct governor_context context = {0};
    struct runtime_config config = {10, 1000};
    struct runtime_paths paths = {"hwmon", "pstate", "resource0"};

    errno = 0;
    CHECK(runtime_start(NULL, &config, &paths) == RUNTIME_STARTUP_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(runtime_start(&context, NULL, &paths) == RUNTIME_STARTUP_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(runtime_start(&context, &config, NULL) == RUNTIME_STARTUP_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    paths.pstate_path = NULL;
    errno = 0;
    CHECK(runtime_start(&context, &config, &paths) == RUNTIME_STARTUP_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(runtime_cleanup(NULL) == -1);
    CHECK(errno == EINVAL);

cleanup:
    return result;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"initializes and cleans up runtime", test_initializes_and_cleans_up_runtime},
        {"prints startup summary", test_prints_startup_summary},
        {"initializes supported pstates", test_initializes_supported_pstates},
        {"applies startup thermal limit", test_initial_temperature_applies_thermal_limit},
        {"reports hardware failures transactionally", test_reports_hardware_failures_transactionally},
        {"rejects invalid arguments", test_rejects_invalid_arguments},
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
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All runtime tests passed\n");
    return 0;
}
