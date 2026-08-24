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

enum {
    MOCK_PGRAPH_OFFSET = 0x400700,
    MOCK_PVLD_OFFSET = 0x084048,
    MOCK_PPDEC_OFFSET = 0x08504c,
    MOCK_PPPP_OFFSET = 0x08604c
};

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

/** Write one synthetic activity-register snapshot into the mapped resource. */
static int fixture_write_activity(const struct runtime_fixture *fixture, const struct gpu_activity_sample *activity)
{
    if (test_write_bytes_at(fixture->root, "resource0", &activity->pgraph, sizeof(activity->pgraph), MOCK_PGRAPH_OFFSET) == -1
        || test_write_bytes_at(fixture->root, "resource0", &activity->pvld, sizeof(activity->pvld), MOCK_PVLD_OFFSET) == -1
        || test_write_bytes_at(fixture->root, "resource0", &activity->ppdec, sizeof(activity->ppdec), MOCK_PPDEC_OFFSET) == -1
        || test_write_bytes_at(fixture->root, "resource0", &activity->pppp, sizeof(activity->pppp), MOCK_PPPP_OFFSET) == -1)
        return -1;

    return 0;
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

    // Policy and scheduling startup share one timestamp. Temperature waits a
    // full configured interval because discovery already supplied a valid read.
    CHECK(context.schedule.next_sample_deadline_ms == context.policy.last_activity_ms);
    CHECK(context.schedule.next_temperature_poll_deadline_ms == context.policy.last_activity_ms + 500);
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

/** Verify raw MMIO snapshots and derived graphics and video activity signals. */
static int test_cycle_classifies_activity(void)
{
    static const struct {
        struct gpu_activity_sample activity;
        bool graphics;
        bool video;
    } cases[] = {
        {{0, 0, 0, 0}, false, false},
        {{0x11, 0, 0, 0}, true, false},
        {{0, 0x22, 0, 0}, false, true},
        {{0, 0, 0x33, 0}, false, true},
        {{0, 0, 0, 0x44}, false, true},
        {{0x55, 0x66, 0x77, 0x88}, true, true},
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
    struct runtime_cycle_result cycle;

    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    struct governor_policy initial_policy = context.policy;
    test_remove_file(fixture.root, "pstate");

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        context.policy = initial_policy;
        context.applied_pstate = GPU_PSTATE_LOW;
        CHECK(fixture_write_activity(&fixture, &cases[index].activity) == 0);
        CHECK(runtime_run_cycle(&context, &fixture.paths, false, initial_policy.last_activity_ms + index + 1, &cycle) == RUNTIME_CYCLE_OK);
        CHECK(cycle.activity.pgraph == cases[index].activity.pgraph);
        CHECK(cycle.activity.pvld == cases[index].activity.pvld);
        CHECK(cycle.activity.ppdec == cases[index].activity.ppdec);
        CHECK(cycle.activity.pppp == cases[index].activity.pppp);
        CHECK(cycle.graphics_activity_detected == cases[index].graphics);
        CHECK(cycle.video_activity_detected == cases[index].video);
        CHECK(cycle.temperature_observation == GOVERNOR_TEMPERATURE_NOT_POLLED);
        CHECK(cycle.temperature_read_error == 0);
        CHECK(!cycle.pstate_transition_requested);
    }

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify valid and failed scheduled temperature observations. */
static int test_cycle_processes_temperature_reads(void)
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
    struct runtime_cycle_result cycle;

    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    uint64_t initial_ms = context.policy.last_activity_ms;

    CHECK(test_write_text(fixture.hwmon_device, "temp1_input", "52000\n") == 0);
    CHECK(runtime_run_cycle(&context, &fixture.paths, true, initial_ms + 1, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(cycle.temperature_observation == GOVERNOR_TEMPERATURE_VALID);
    CHECK(cycle.temperature_read_error == 0);
    CHECK(cycle.temperature_millidegrees == 52000);
    CHECK(context.temperature_millidegrees == 52000);

    test_remove_file(fixture.hwmon_device, "temp1_input");
    CHECK(runtime_run_cycle(&context, &fixture.paths, true, initial_ms + 2, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(cycle.temperature_observation == GOVERNOR_TEMPERATURE_READ_FAILED);
    CHECK(cycle.temperature_read_error == ENOENT);
    CHECK(cycle.temperature_millidegrees == 52000);
    CHECK(context.temperature_millidegrees == 52000);
    CHECK(context.policy.temperature_failure_pending);

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify MEDIUM reconciliation and avoidance of redundant pstate writes. */
static int test_cycle_applies_only_required_pstate_transition(void)
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
    struct runtime_cycle_result cycle;
    char command[16];

    CHECK(test_write_text(fixture.root, "pstate", "03: low\n07: medium *\n0f: high\n") == 0);
    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    CHECK(context.applied_pstate == GPU_PSTATE_MEDIUM);
    CHECK(context.policy.target_pstate == GPU_PSTATE_LOW);

    CHECK(test_write_text(fixture.root, "pstate", "") == 0);
    CHECK(runtime_run_cycle(&context, &fixture.paths, false, context.policy.last_activity_ms, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(cycle.pstate_transition_requested);
    CHECK(cycle.pstate_transition_succeeded);
    CHECK(cycle.applied_pstate == GPU_PSTATE_LOW);
    CHECK(context.applied_pstate == GPU_PSTATE_LOW);
    CHECK(test_read_text(fixture.root, "pstate", command, sizeof(command)) == 0);
    CHECK(strcmp(command, "03\n") == 0);

    test_remove_file(fixture.root, "pstate");
    CHECK(runtime_run_cycle(&context, &fixture.paths, false, context.policy.last_activity_ms + 1, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(!cycle.pstate_transition_requested);
    CHECK(!cycle.pstate_transition_succeeded);
    CHECK(cycle.applied_pstate == GPU_PSTATE_LOW);

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Verify a failed workload upshift remains eligible for retry. */
static int test_cycle_retries_failed_pstate_transition(void)
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
    struct runtime_cycle_result cycle;
    const struct gpu_activity_sample graphics_activity = {.pgraph = 1};
    char command[16];

    CHECK(runtime_start(&context, &config, &fixture.paths) == RUNTIME_STARTUP_OK);
    CHECK(fixture_write_activity(&fixture, &graphics_activity) == 0);
    uint64_t initial_ms = context.policy.last_activity_ms;

    CHECK(runtime_run_cycle(&context, &fixture.paths, false, initial_ms + 10, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(!cycle.pstate_transition_requested);
    CHECK(runtime_run_cycle(&context, &fixture.paths, false, initial_ms + 20, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(!cycle.pstate_transition_requested);

    test_remove_file(fixture.root, "pstate");
    errno = 0;
    CHECK(runtime_run_cycle(&context, &fixture.paths, false, initial_ms + 30, &cycle) == RUNTIME_CYCLE_PSTATE_ERROR);
    CHECK(errno == ENOENT);
    CHECK(cycle.policy.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK((cycle.policy.events & GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT) != 0);
    CHECK(cycle.pstate_transition_requested);
    CHECK(!cycle.pstate_transition_succeeded);
    CHECK(cycle.applied_pstate == GPU_PSTATE_LOW);
    CHECK(context.applied_pstate == GPU_PSTATE_LOW);
    CHECK(context.policy.target_pstate == GPU_PSTATE_HIGH);

    CHECK(test_write_text(fixture.root, "pstate", "") == 0);
    CHECK(runtime_run_cycle(&context, &fixture.paths, false, initial_ms + 40, &cycle) == RUNTIME_CYCLE_OK);
    CHECK(cycle.policy.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(cycle.policy.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(cycle.pstate_transition_requested);
    CHECK(cycle.pstate_transition_succeeded);
    CHECK(context.applied_pstate == GPU_PSTATE_HIGH);
    CHECK(test_read_text(fixture.root, "pstate", command, sizeof(command)) == 0);
    CHECK(strcmp(command, "0f\n") == 0);

cleanup:
    if (context.gpu.bar0_address != NULL)
        runtime_cleanup(&context);

    fixture_destroy(&fixture);
    return result;
}

/** Capture and verify the one-shot console report for a cycle. */
static int test_prints_cycle_summary(void)
{
    int result = 0;
    FILE *stream = tmpfile();
    char summary[2048];
    const struct runtime_cycle_result cycle = {
        .activity = {0x11, 0x22, 0x33, 0x44},
        .graphics_activity_detected = true,
        .video_activity_detected = true,
        .temperature_observation = GOVERNOR_TEMPERATURE_NOT_POLLED,
        .temperature_millidegrees = 51000,
        .policy = {GPU_PSTATE_HIGH, GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT},
        .pstate_transition_requested = true,
        .pstate_transition_succeeded = true,
        .applied_pstate = GPU_PSTATE_HIGH,
    };

    CHECK(stream != NULL);
    runtime_print_cycle_summary(stream, &cycle);
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);

    size_t received = fread(summary, 1, sizeof(summary) - 1, stream);
    CHECK(!ferror(stream));
    summary[received] = '\0';

    CHECK(strstr(summary, "Governor cycle") != NULL);
    CHECK(strstr(summary, "PGRAPH:                    0x00000011") != NULL);
    CHECK(strstr(summary, "PVLD:                      0x00000022") != NULL);
    CHECK(strstr(summary, "PPDEC:                     0x00000033") != NULL);
    CHECK(strstr(summary, "PPPP:                      0x00000044") != NULL);
    CHECK(strstr(summary, "Graphics activity:         active") != NULL);
    CHECK(strstr(summary, "Video activity:            active") != NULL);
    CHECK(strstr(summary, "Temperature observation:   not polled") != NULL);
    CHECK(strstr(summary, "Policy recommendation:     HIGH (0f)") != NULL);
    CHECK(strstr(summary, "Pstate transition:         succeeded") != NULL);
    CHECK(strstr(summary, "Applied pstate:            HIGH (0f)") != NULL);

cleanup:
    if (stream != NULL)
        fclose(stream);

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
    struct runtime_cycle_result cycle;

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

    errno = 0;
    CHECK(runtime_monotonic_time_ms(NULL) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(runtime_run_cycle(NULL, &paths, false, 0, &cycle) == RUNTIME_CYCLE_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(runtime_run_cycle(&context, NULL, false, 0, &cycle) == RUNTIME_CYCLE_INVALID_ARGUMENT);
    CHECK(errno == EINVAL);

    paths.pstate_path = "pstate";
    errno = 0;
    CHECK(runtime_run_cycle(&context, &paths, false, 0, NULL) == RUNTIME_CYCLE_INVALID_ARGUMENT);
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
        {"cycle classifies activity", test_cycle_classifies_activity},
        {"cycle processes temperature reads", test_cycle_processes_temperature_reads},
        {"cycle applies only required transition", test_cycle_applies_only_required_pstate_transition},
        {"cycle retries failed transition", test_cycle_retries_failed_pstate_transition},
        {"prints cycle summary", test_prints_cycle_summary},
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
