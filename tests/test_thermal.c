/**
 * @file test_thermal.c
 * @brief Tests for Nouveau hwmon discovery and temperature parsing.
 */

#define _POSIX_C_SOURCE 200809L

#include "test_helpers.h"
#include "thermal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Mock hwmon tree owned by one test case. */
struct thermal_fixture {
    /// Unique temporary directory containing this fixture.
    char root[PATH_MAX];

    /// Mock unrelated hwmon device used to exercise discovery filtering.
    char hwmon0[PATH_MAX];

    /// Mock Nouveau hwmon device containing the thermal attributes under test.
    char hwmon1[PATH_MAX];
};

/// Evaluate one test condition and preserve fixture cleanup on failure.
///
/// The surrounding test must provide a `result` variable and `cleanup` label.
/// Wrapping the expansion in `do ... while (0)` makes it behave like one C
/// statement, including when used inside a conditional without braces.
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(                                                            \
                stderr,                                                         \
                "FAIL %s:%d: %s\n",                                            \
                __func__,                                                       \
                __LINE__,                                                       \
                #condition);                                                    \
            result = -1;                                                        \
            goto cleanup;                                                       \
        }                                                                       \
    } while (0)

/**
 * @brief Remove only the explicitly known files and directories in a fixture.
 *
 * Avoiding recursive deletion ensures cleanup cannot traverse outside the
 * uniquely named temporary tree created by fixture_create().
 */
static void fixture_destroy(struct thermal_fixture *fixture)
{
    static const char *const files[] = {
        "name",
        "temp1_input",
        "temp1_max",
        "temp1_max_hyst"
    };

    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++) {
        test_remove_file(fixture->hwmon0, files[index]);
        test_remove_file(fixture->hwmon1, files[index]);
    }

    if (fixture->hwmon0[0] != '\0') {
        rmdir(fixture->hwmon0);
    }
    if (fixture->hwmon1[0] != '\0') {
        rmdir(fixture->hwmon1);
    }
    if (fixture->root[0] != '\0') {
        rmdir(fixture->root);
    }
}

/**
 * @brief Create a representative two-device hwmon tree under `/tmp`.
 *
 * `hwmon0` represents an unrelated ThinkPad sensor, while `hwmon1` represents
 * Nouveau with valid defaults matching the target system. Each test owns a
 * uniquely named tree and may alter it to exercise a particular condition.
 *
 * @return 0 on success, or -1 on failure with @c errno set. Partial setup is
 *         cleaned up before failure is returned.
 */
static int fixture_create(struct thermal_fixture *fixture)
{
    static const char root_template[] = "/tmp/bluemax-thermal-test-XXXXXX";

    memset(fixture, 0, sizeof(*fixture));

    // mkdtemp() replaces the six trailing X characters in a writable buffer.
    memcpy(fixture->root, root_template, sizeof(root_template));

    // Short-circuit at the first setup failure, then remove any partial tree.
    if (mkdtemp(fixture->root) == NULL
        || test_build_path(fixture->hwmon0, sizeof(fixture->hwmon0), fixture->root, "hwmon0") == -1
        || mkdir(fixture->hwmon0, 0700) == -1
        || test_build_path(fixture->hwmon1, sizeof(fixture->hwmon1), fixture->root, "hwmon1") == -1
        || mkdir(fixture->hwmon1, 0700) == -1
        || test_write_text(fixture->hwmon0, "name", "thinkpad\n") == -1
        || test_write_text(fixture->hwmon1, "name", "nouveau\n") == -1
        || test_write_text(fixture->hwmon1, "temp1_input", "51000\n") == -1
        || test_write_text(fixture->hwmon1, "temp1_max", "95000\n") == -1
        || test_write_text(fixture->hwmon1, "temp1_max_hyst", "3000\n") == -1) {
        // Cleanup calls may change errno, so retain the original setup error.
        int setup_error = errno;
        fixture_destroy(fixture);
        errno = setup_error;
        return -1;
    }

    return 0;
}

static int test_discovers_nouveau_and_reads_temperatures(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct thermal_sensor sensor;
    int temperature;

    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == 0);
    CHECK(temperature == 51000);
    CHECK(sensor.max_millidegrees == 95000);
    CHECK(sensor.max_hyst_millidegrees == 3000);

    // Discovery must cache the input path belonging to Nouveau, not hwmon0.
    char expected_path[PATH_MAX];
    CHECK(test_build_path(expected_path, sizeof(expected_path), fixture.hwmon1, "temp1_input") == 0);
    CHECK(strcmp(sensor.input_path, expected_path) == 0);

    // Changing the mock attribute verifies that later reads fetch fresh data.
    CHECK(test_write_text(fixture.hwmon1, "temp1_input", "52000\n") == 0);
    CHECK(thermal_sensor_read(&sensor, &temperature) == 0);
    CHECK(temperature == 52000);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_skips_unreadable_hwmon_entry(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;

    // A device without a readable name must not hide a later Nouveau device.
    test_remove_file(fixture.hwmon0, "name");

    struct thermal_sensor sensor;
    int temperature;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == 0);
    CHECK(temperature == 51000);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_malformed_value(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    CHECK(test_write_text(fixture.hwmon1, "temp1_max", "95000 hot\n") == 0);

    struct thermal_sensor sensor;
    int temperature;
    errno = 0;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == -1);
    CHECK(errno == EINVAL);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_out_of_range_value(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    CHECK(test_write_text(fixture.hwmon1, "temp1_max", "999999999999999999999\n") == 0);

    struct thermal_sensor sensor;
    int temperature;
    errno = 0;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == -1);
    CHECK(errno == ERANGE);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_invalid_thresholds(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;

    // Hysteresis equal to the maximum would produce an invalid 0 C recovery.
    CHECK(test_write_text(fixture.hwmon1, "temp1_max_hyst", "95000\n") == 0);

    struct thermal_sensor sensor;
    int temperature;
    errno = 0;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == -1);
    CHECK(errno == EINVAL);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_reports_missing_required_file(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    test_remove_file(fixture.hwmon1, "temp1_max_hyst");

    struct thermal_sensor sensor;
    int temperature;
    errno = 0;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == -1);
    CHECK(errno == ENOENT);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_reports_missing_nouveau_device(void)
{
    struct thermal_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;

    // Rename the only Nouveau entry so discovery must report no matching device.
    CHECK(test_write_text(fixture.hwmon1, "name", "acpitz\n") == 0);

    struct thermal_sensor sensor;
    int temperature;
    errno = 0;
    CHECK(thermal_sensor_discover(fixture.root, &sensor, &temperature) == -1);
    CHECK(errno == ENODEV);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_null_arguments(void)
{
    int result = 0;
    struct thermal_sensor sensor = {0};
    int temperature;

    errno = 0;
    CHECK(thermal_sensor_discover(NULL, &sensor, &temperature) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(thermal_sensor_read(NULL, &temperature) == -1);
    CHECK(errno == EINVAL);

cleanup:
    return result;
}

int main(void)
{
    // Keeping names beside function pointers makes adding cases straightforward
    // while centralizing consistent PASS/FAIL reporting in the loop below.
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"discovers Nouveau and reads temperatures", test_discovers_nouveau_and_reads_temperatures},
        {"skips an unreadable hwmon entry", test_skips_unreadable_hwmon_entry},
        {"rejects a malformed value", test_rejects_malformed_value},
        {"rejects an out-of-range value", test_rejects_out_of_range_value},
        {"rejects invalid thresholds", test_rejects_invalid_thresholds},
        {"reports a missing required file", test_reports_missing_required_file},
        {"reports a missing Nouveau device", test_reports_missing_nouveau_device},
        {"rejects null arguments", test_rejects_null_arguments}
    };

    int failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (tests[index].run() == 0) {
            printf("PASS: %s\n", tests[index].name);
        } else {
            failures++;
        }
    }

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All thermal tests passed\n");
    return 0;
}
