/**
 * @file test_governor_policy.c
 * @brief Tests for deterministic GPU governor policy decisions.
 */

#include "governor_policy.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/** Evaluate one test condition and return a failure from the current test. */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(                                                            \
                stderr,                                                         \
                "FAIL %s:%d: %s\n",                                            \
                __func__,                                                       \
                __LINE__,                                                       \
                #condition);                                                    \
            return -1;                                                          \
        }                                                                       \
    } while (0)

enum {
    TEMPERATURE_MAX_MILLIDEGREES = 95000,
    TEMPERATURE_HYSTERESIS_MILLIDEGREES = 3000,
    SAFE_TEMPERATURE_MILLIDEGREES = 51000
};

/**
 * @brief Verify initialization of each supported startup pstate.
 *
 * @return 0 when every case passes, or -1 when a check fails.
 */
static int test_initializes_supported_pstates(void)
{
    static const struct {
        enum gpu_pstate initial;
        enum gpu_pstate expected;
    } cases[] = {
        {GPU_PSTATE_LOW, GPU_PSTATE_LOW},
        {GPU_PSTATE_MEDIUM, GPU_PSTATE_LOW},
        {GPU_PSTATE_HIGH, GPU_PSTATE_HIGH},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        struct governor_policy policy;
        CHECK(governor_policy_init(
                  &policy,
                  cases[index].initial,
                  TEMPERATURE_MAX_MILLIDEGREES,
                  TEMPERATURE_HYSTERESIS_MILLIDEGREES,
                  SAFE_TEMPERATURE_MILLIDEGREES,
                  1234) == 0);
        CHECK(policy.desired_pstate == cases[index].expected);
        CHECK(policy.high_since_ms == 1234);
        CHECK(policy.last_activity_ms == 1234);
        CHECK(!policy.thermal_limit_active);
        CHECK(!policy.temperature_failure_pending);
        CHECK(!policy.temperature_fault_active);
        CHECK(policy.graphics_history == 0);
        CHECK(policy.video_history == 0);
    }

    return 0;
}

/**
 * @brief Verify that an unsafe startup temperature forces LOW.
 *
 * @return 0 when the safety state is initialized correctly, or -1 on failure.
 */
static int test_applies_thermal_limit_at_startup(void)
{
    static const int temperatures[] = {95000, 96000};

    for (size_t index = 0;
         index < sizeof(temperatures) / sizeof(temperatures[0]);
         index++) {
        struct governor_policy policy;
        CHECK(governor_policy_init(
                  &policy,
                  GPU_PSTATE_HIGH,
                  TEMPERATURE_MAX_MILLIDEGREES,
                  TEMPERATURE_HYSTERESIS_MILLIDEGREES,
                  temperatures[index],
                  2000) == 0);
        CHECK(policy.desired_pstate == GPU_PSTATE_LOW);
        CHECK(policy.thermal_limit_active);
    }

    return 0;
}

/**
 * @brief Verify rejection of invalid input without modifying the destination.
 *
 * @return 0 when every invalid input is rejected, or -1 on failure.
 */
static int test_rejects_invalid_inputs(void)
{
    struct governor_policy policy;
    struct governor_policy unchanged;
    memset(&policy, 0xa5, sizeof(policy));
    unchanged = policy;

    errno = 0;
    CHECK(governor_policy_init(
              NULL,
              GPU_PSTATE_LOW,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              SAFE_TEMPERATURE_MILLIDEGREES,
              0) == -1);
    CHECK(errno == EINVAL);

    static const struct {
        enum gpu_pstate pstate;
        int maximum;
        int hysteresis;
    } cases[] = {
        {(enum gpu_pstate)99, 95000, 3000},
        {GPU_PSTATE_LOW, 0, 0},
        {GPU_PSTATE_LOW, 95000, -1},
        {GPU_PSTATE_LOW, 95000, 95000},
        {GPU_PSTATE_LOW, 95000, 96000},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        errno = 0;
        CHECK(governor_policy_init(
                  &policy,
                  cases[index].pstate,
                  cases[index].maximum,
                  cases[index].hysteresis,
                  SAFE_TEMPERATURE_MILLIDEGREES,
                  0) == -1);
        CHECK(errno == EINVAL);
        CHECK(memcmp(&policy, &unchanged, sizeof(policy)) == 0);
    }

    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_initializes_supported_pstates() != 0;
    failures += test_applies_thermal_limit_at_startup() != 0;
    failures += test_rejects_invalid_inputs() != 0;

    if (failures != 0) {
        fprintf(stderr, "%d governor policy test group(s) failed\n", failures);
        return 1;
    }

    printf("All governor policy tests passed\n");
    return 0;
}
