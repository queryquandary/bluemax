/**
 * @file test_gpu_pstate.c
 * @brief Tests for reading and selecting Nouveau GPU performance states.
 */

#define _POSIX_C_SOURCE 200809L

#include "gpu_pstate.h"
#include "test_helpers.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief Temporary Nouveau pstate file owned by one test case. */
struct gpu_pstate_fixture {
    /** Unique temporary directory containing the mock pstate file. */
    char root[PATH_MAX];

    /** Complete path passed to the pstate module. */
    char pstate_path[PATH_MAX];
};

/** Evaluate one condition and retain the test's cleanup path on failure. */
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
 * @brief Remove the mock pstate file and its temporary directory.
 *
 * @param[in,out] fixture Fixture to clean up after a test.
 */
static void fixture_destroy(struct gpu_pstate_fixture *fixture)
{
    test_remove_file(fixture->root, "pstate");

    if (fixture->root[0] != '\0') {
        rmdir(fixture->root);
    }
}

/**
 * @brief Create an empty mock pstate file in a unique temporary directory.
 *
 * @param[out] fixture Destination for the initialized fixture paths.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
static int fixture_create(struct gpu_pstate_fixture *fixture)
{
    static const char root_template[] = "/tmp/bluemax-gpu-pstate-test-XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->root, root_template, sizeof(root_template));

    if (mkdtemp(fixture->root) == NULL
        || test_build_path(
               fixture->pstate_path,
               sizeof(fixture->pstate_path),
               fixture->root,
               "pstate") == -1
        || test_write_text(fixture->root, "pstate", "") == -1) {
        int setup_error = errno;
        fixture_destroy(fixture);
        errno = setup_error;
        return -1;
    }

    return 0;
}

/**
 * @brief Verify that each supported active-state label is read correctly.
 *
 * @return 0 when every case passes, or -1 when a check fails.
 */
static int test_reads_supported_states(void)
{
    static const struct {
        const char *listing;
        enum gpu_pstate expected;
    } cases[] = {
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz *\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 135 MHz shader 270 MHz memory 135 MHz\n",
            GPU_PSTATE_LOW,
        },
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz *\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 405 MHz shader 810 MHz memory 324 MHz\n",
            GPU_PSTATE_MEDIUM,
        },
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0F: core 550 MHz shader 1210 MHz memory 790 MHz *\n"
            "AC: core 550 MHz shader 1210 MHz memory 790 MHz\n",
            GPU_PSTATE_HIGH,
        },
    };

    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        enum gpu_pstate observed = GPU_PSTATE_HIGH;
        CHECK(test_write_text(fixture.root, "pstate", cases[index].listing) == 0);
        CHECK(gpu_pstate_read(fixture.pstate_path, &observed) == 0);
        CHECK(observed == cases[index].expected);
    }

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/**
 * @brief Verify active-state discovery from Nouveau's unmarked AC clocks.
 *
 * @return 0 when every clock tuple selects its matching state, or -1 on
 *         failure.
 */
static int test_matches_unmarked_current_clocks(void)
{
    static const struct {
        const char *listing;
        enum gpu_pstate expected;
    } cases[] = {
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 135 MHz shader 270 MHz memory 135 MHz\n",
            GPU_PSTATE_LOW,
        },
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 405 MHz shader 810 MHz memory 324 MHz\n",
            GPU_PSTATE_MEDIUM,
        },
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 550 MHz shader 1210 MHz memory 790 MHz *\n",
            GPU_PSTATE_HIGH,
        },
    };

    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        enum gpu_pstate observed = GPU_PSTATE_HIGH;
        CHECK(test_write_text(fixture.root, "pstate", cases[index].listing) == 0);
        CHECK(gpu_pstate_read(fixture.pstate_path, &observed) == 0);
        CHECK(observed == cases[index].expected);
    }

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/**
 * @brief Verify that invalid listings fail without changing the output value.
 *
 * @return 0 when every case passes, or -1 when a check fails.
 */
static int test_rejects_invalid_listings(void)
{
    static const struct {
        const char *listing;
        int expected_error;
    } cases[] = {
        {
            "03: core 135 MHz\n07: core 405 MHz\n0f: core 550 MHz\n",
            ENODATA,
        },
        {"not-a-state *\n", EINVAL},
        {"0e: core 500 MHz *\n", EOPNOTSUPP},
        {"03: core 135 MHz *\n0f: core 550 MHz *\n", EINVAL},
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 405 MHz shader 810 MHz memory 324 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 999 MHz shader 999 MHz memory 999 MHz\n",
            ENODATA,
        },
        {
            "03: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "07: core 135 MHz shader 270 MHz memory 135 MHz\n"
            "0f: core 550 MHz shader 1210 MHz memory 790 MHz\n"
            "AC: core 135 MHz shader 270 MHz memory 135 MHz\n",
            ENODATA,
        },
    };

    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        enum gpu_pstate observed = GPU_PSTATE_MEDIUM;
        CHECK(test_write_text(fixture.root, "pstate", cases[index].listing) == 0);

        errno = 0;
        CHECK(gpu_pstate_read(fixture.pstate_path, &observed) == -1);
        CHECK(errno == cases[index].expected_error);
        CHECK(observed == GPU_PSTATE_MEDIUM);
    }

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/**
 * @brief Verify the commands used to select the low and high states.
 *
 * @return 0 when both commands are correct, or -1 when a check fails.
 */
static int test_writes_selectable_states(void)
{
    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    char command[8];

    CHECK(gpu_pstate_set(fixture.pstate_path, GPU_PSTATE_LOW) == 0);
    CHECK(test_read_text(fixture.root, "pstate", command, sizeof(command)) == 0);
    CHECK(strcmp(command, "03\n") == 0);

    CHECK(test_write_text(fixture.root, "pstate", "") == 0);
    CHECK(gpu_pstate_set(fixture.pstate_path, GPU_PSTATE_HIGH) == 0);
    CHECK(test_read_text(fixture.root, "pstate", command, sizeof(command)) == 0);
    CHECK(strcmp(command, "0f\n") == 0);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/**
 * @brief Verify that unsupported state selections are rejected without writes.
 *
 * @return 0 when invalid selections are rejected, or -1 when a check fails.
 */
static int test_rejects_unsupported_selections(void)
{
    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    char contents[8];

    errno = 0;
    CHECK(gpu_pstate_set(fixture.pstate_path, GPU_PSTATE_MEDIUM) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_pstate_set(fixture.pstate_path, (enum gpu_pstate)99) == -1);
    CHECK(errno == EINVAL);

    CHECK(test_read_text(fixture.root, "pstate", contents, sizeof(contents)) == 0);
    CHECK(strcmp(contents, "") == 0);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

/**
 * @brief Verify argument validation and missing-file errors.
 *
 * @return 0 when each failure is reported correctly, or -1 on failure.
 */
static int test_reports_argument_and_file_errors(void)
{
    struct gpu_pstate_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    enum gpu_pstate observed = GPU_PSTATE_MEDIUM;

    errno = 0;
    CHECK(gpu_pstate_read(NULL, &observed) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_pstate_read(fixture.pstate_path, NULL) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_pstate_set(NULL, GPU_PSTATE_LOW) == -1);
    CHECK(errno == EINVAL);

    test_remove_file(fixture.root, "pstate");

    errno = 0;
    CHECK(gpu_pstate_read(fixture.pstate_path, &observed) == -1);
    CHECK(errno == ENOENT);
    CHECK(observed == GPU_PSTATE_MEDIUM);

    errno = 0;
    CHECK(gpu_pstate_set(fixture.pstate_path, GPU_PSTATE_LOW) == -1);
    CHECK(errno == ENOENT);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"reads supported active states", test_reads_supported_states},
        {"matches unmarked current clocks", test_matches_unmarked_current_clocks},
        {"rejects invalid listings", test_rejects_invalid_listings},
        {"writes selectable states", test_writes_selectable_states},
        {"rejects unsupported selections", test_rejects_unsupported_selections},
        {"reports argument and file errors", test_reports_argument_and_file_errors},
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

    printf("All GPU pstate tests passed\n");
    return 0;
}
