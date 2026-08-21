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
 * @brief Initialize a thermally safe LOW policy for an activity test.
 *
 * @param[out] policy Destination for the initialized policy.
 * @param[in] now_ms Initial monotonic time in milliseconds.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
static int initialize_low_policy(
    struct governor_policy *policy,
    uint64_t now_ms)
{
    return governor_policy_init(
        policy,
        GPU_PSTATE_LOW,
        TEMPERATURE_MAX_MILLIDEGREES,
        TEMPERATURE_HYSTERESIS_MILLIDEGREES,
        SAFE_TEMPERATURE_MILLIDEGREES,
        now_ms);
}

/**
 * @brief Apply one activity sample without polling temperature.
 *
 * @param[in,out] policy Policy that receives the sample.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] graphics_activity_detected Whether graphics activity is present.
 * @param[in] video_activity_detected Whether hardware video activity is present.
 *
 * @return Result produced by the governor policy.
 */
static struct governor_policy_result apply_activity(
    struct governor_policy *policy,
    uint64_t now_ms,
    bool graphics_activity_detected,
    bool video_activity_detected)
{
    const struct governor_policy_input input = {
        .now_ms = now_ms,
        .graphics_activity_detected = graphics_activity_detected,
        .video_activity_detected = video_activity_detected,
        .temperature_observation = GOVERNOR_TEMPERATURE_NOT_POLLED,
        .temperature_millidegrees = 0,
    };

    return governor_policy_step(policy, &input);
}

/**
 * @brief Apply one valid temperature sample with optional activity.
 *
 * @param[in,out] policy Policy that receives the sample.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] temperature_millidegrees Fresh temperature sample.
 * @param[in] graphics_activity_detected Whether graphics activity is present.
 * @param[in] video_activity_detected Whether hardware video activity is present.
 *
 * @return Result produced by the governor policy.
 */
static struct governor_policy_result apply_temperature(
    struct governor_policy *policy,
    uint64_t now_ms,
    int temperature_millidegrees,
    bool graphics_activity_detected,
    bool video_activity_detected)
{
    const struct governor_policy_input input = {
        .now_ms = now_ms,
        .graphics_activity_detected = graphics_activity_detected,
        .video_activity_detected = video_activity_detected,
        .temperature_observation = GOVERNOR_TEMPERATURE_VALID,
        .temperature_millidegrees = temperature_millidegrees,
    };

    return governor_policy_step(policy, &input);
}

/**
 * @brief Apply one failed temperature read with optional activity.
 *
 * @param[in,out] policy Policy that receives the failed observation.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] graphics_activity_detected Whether graphics activity is present.
 * @param[in] video_activity_detected Whether hardware video activity is present.
 *
 * @return Result produced by the governor policy.
 */
static struct governor_policy_result apply_temperature_failure(
    struct governor_policy *policy,
    uint64_t now_ms,
    bool graphics_activity_detected,
    bool video_activity_detected)
{
    const struct governor_policy_input input = {
        .now_ms = now_ms,
        .graphics_activity_detected = graphics_activity_detected,
        .video_activity_detected = video_activity_detected,
        .temperature_observation = GOVERNOR_TEMPERATURE_READ_FAILED,
        .temperature_millidegrees = 0,
    };

    return governor_policy_step(policy, &input);
}

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
        CHECK(policy.target_pstate == cases[index].expected);
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
        CHECK(policy.target_pstate == GPU_PSTATE_LOW);
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

/**
 * @brief Verify that two active video samples among three request HIGH.
 *
 * @return 0 when the video trigger behaves correctly, or -1 on failure.
 */
static int test_video_activity_requests_high(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 1000) == 0);

    struct governor_policy_result result =
        apply_activity(&policy, 1010, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 1020, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 1030, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT);
    CHECK(policy.high_since_ms == 1030);
    CHECK(policy.last_activity_ms == 1030);

    return 0;
}

/**
 * @brief Verify that three active graphics samples among five request HIGH.
 *
 * @return 0 when the graphics trigger behaves correctly, or -1 on failure.
 */
static int test_graphics_activity_requests_high(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 2000) == 0);

    static const bool samples[] = {true, false, true, false, true};
    struct governor_policy_result result;

    for (size_t index = 0; index < sizeof(samples) / sizeof(samples[0]); index++) {
        result = apply_activity(
            &policy,
            2010 + (uint64_t)index * 10,
            samples[index],
            false);

        if (index < 4) {
            CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
            CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
        }
    }

    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT);
    CHECK(policy.high_since_ms == 2050);
    CHECK(policy.last_activity_ms == 2050);

    return 0;
}

/**
 * @brief Verify that graphics and video samples are counted independently.
 *
 * @return 0 when mixed activity does not combine, or -1 on failure.
 */
static int test_keeps_activity_histories_separate(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 3000) == 0);

    struct governor_policy_result result =
        apply_activity(&policy, 3010, true, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);

    result = apply_activity(&policy, 3020, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK((policy.graphics_history & 3U) == 2U);
    CHECK((policy.video_history & 3U) == 1U);

    return 0;
}

/**
 * @brief Verify that simultaneous trigger causes are both reported.
 *
 * @return 0 when both event flags are present, or -1 on failure.
 */
static int test_reports_simultaneous_activity_triggers(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 4000) == 0);

    apply_activity(&policy, 4010, true, false);
    apply_activity(&policy, 4020, true, false);
    apply_activity(&policy, 4030, false, false);
    apply_activity(&policy, 4040, false, true);

    struct governor_policy_result result =
        apply_activity(&policy, 4050, true, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events
          == (GOVERNOR_POLICY_EVENT_GRAPHICS_UPSHIFT
              | GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT));

    return 0;
}

/**
 * @brief Verify that HIGH does not repeatedly report activity upshifts.
 *
 * @return 0 when repeated activity produces no new event, or -1 on failure.
 */
static int test_does_not_repeat_upshift_events(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 5000) == 0);

    apply_activity(&policy, 5010, false, true);
    struct governor_policy_result result =
        apply_activity(&policy, 5020, false, true);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT);

    result = apply_activity(&policy, 5030, true, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.high_since_ms == 5020);
    CHECK(policy.last_activity_ms == 5030);

    return 0;
}

/**
 * @brief Verify that existing thermal inhibition blocks activity upshifts.
 *
 * @return 0 when the policy remains LOW, or -1 on failure.
 */
static int test_thermal_limit_blocks_activity_upshift(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_LOW,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              TEMPERATURE_MAX_MILLIDEGREES,
              6000) == 0);

    apply_activity(&policy, 6010, true, true);
    struct governor_policy_result result =
        apply_activity(&policy, 6020, true, true);
    result = apply_activity(&policy, 6030, true, true);

    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.graphics_history != 0);
    CHECK(policy.video_history != 0);
    CHECK(policy.last_activity_ms == 6030);

    return 0;
}

/**
 * @brief Verify idle downshift immediately before and at its time boundary.
 *
 * @return 0 when two seconds of inactivity are required, or -1 on failure.
 */
static int test_downshifts_after_continuous_inactivity(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 7000) == 0);

    apply_activity(&policy, 7010, false, true);
    struct governor_policy_result result =
        apply_activity(&policy, 7020, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(policy.high_since_ms == 7020);
    CHECK(policy.last_activity_ms == 7020);

    result = apply_activity(&policy, 9019, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 9020, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_IDLE_DOWNSHIFT);

    result = apply_activity(&policy, 9030, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    return 0;
}

/**
 * @brief Verify that activity restarts the continuous-inactivity interval.
 *
 * @return 0 when the downshift uses the latest activity time, or -1 on failure.
 */
static int test_activity_resets_inactivity_timer(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_HIGH,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              SAFE_TEMPERATURE_MILLIDEGREES,
              10000) == 0);

    struct governor_policy_result result =
        apply_activity(&policy, 11999, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);

    result = apply_activity(&policy, 12000, true, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.last_activity_ms == 12000);

    result = apply_activity(&policy, 13999, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 14000, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_IDLE_DOWNSHIFT);

    return 0;
}

/**
 * @brief Verify that minimum HIGH residency independently delays a downshift.
 *
 * @return 0 when the 500 ms residency boundary is enforced, or -1 on failure.
 */
static int test_enforces_minimum_high_residency(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_HIGH,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              SAFE_TEMPERATURE_MILLIDEGREES,
              15000) == 0);

    // Model activity that occurred long before this HIGH residency began.
    policy.last_activity_ms = 10000;

    struct governor_policy_result result =
        apply_activity(&policy, 15499, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 15500, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_IDLE_DOWNSHIFT);

    return 0;
}

/**
 * @brief Verify that reaching the maximum temperature immediately forces LOW.
 *
 * @return 0 when thermal limiting overrides HIGH residency, or -1 on failure.
 */
static int test_thermal_limit_immediately_forces_low(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_HIGH,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              SAFE_TEMPERATURE_MILLIDEGREES,
              16000) == 0);

    struct governor_policy_result result = apply_temperature(
        &policy,
        16010,
        TEMPERATURE_MAX_MILLIDEGREES,
        true,
        true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_THERMAL_LIMIT);
    CHECK(policy.target_pstate == GPU_PSTATE_LOW);
    CHECK(policy.thermal_limit_active);

    result = apply_temperature(&policy, 16020, 96000, true, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.thermal_limit_active);

    return 0;
}

/**
 * @brief Verify strict hysteresis before thermal limiting recovers.
 *
 * @return 0 when recovery occurs only below its threshold, or -1 on failure.
 */
static int test_thermal_limit_uses_strict_hysteresis(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_LOW,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              TEMPERATURE_MAX_MILLIDEGREES,
              17000) == 0);

    struct governor_policy_result result =
        apply_temperature(&policy, 17010, 92000, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.thermal_limit_active);

    result = apply_temperature(&policy, 17020, 91999, false, false);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_THERMAL_RECOVERY);
    CHECK(!policy.thermal_limit_active);

    result = apply_temperature(&policy, 17030, 91000, false, false);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    return 0;
}

/**
 * @brief Verify workload upshift on the same step as thermal recovery.
 *
 * @return 0 when both meaningful events are reported, or -1 on failure.
 */
static int test_activity_upshifts_when_thermal_limit_recovers(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_LOW,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              TEMPERATURE_MAX_MILLIDEGREES,
              18000) == 0);

    struct governor_policy_result result =
        apply_activity(&policy, 18010, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_temperature(&policy, 18020, 91999, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events
          == (GOVERNOR_POLICY_EVENT_THERMAL_RECOVERY
              | GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT));
    CHECK(!policy.thermal_limit_active);
    CHECK(policy.high_since_ms == 18020);

    return 0;
}

/**
 * @brief Verify that valid telemetry clears a temporary failure interval.
 *
 * @return 0 when a short failure does not become a fault, or -1 on failure.
 */
static int test_temporary_temperature_failure_recovers(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 19000) == 0);

    struct governor_policy_result result =
        apply_temperature_failure(&policy, 19010, false, false);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.temperature_failure_pending);
    CHECK(policy.temperature_failure_since_ms == 19010);
    CHECK(!policy.temperature_fault_active);

    result = apply_temperature_failure(&policy, 21010, false, false);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(policy.temperature_failure_since_ms == 19010);

    result = apply_temperature(
        &policy,
        22009,
        SAFE_TEMPERATURE_MILLIDEGREES,
        false,
        false);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(!policy.temperature_failure_pending);
    CHECK(!policy.temperature_fault_active);

    result = apply_activity(&policy, 23010, false, false);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(!policy.temperature_fault_active);

    return 0;
}

/**
 * @brief Verify fault activation immediately before and at its timeout.
 *
 * @return 0 when three seconds of failure force LOW once, or -1 on failure.
 */
static int test_temperature_failure_timeout_forces_low(void)
{
    struct governor_policy policy;
    CHECK(governor_policy_init(
              &policy,
              GPU_PSTATE_HIGH,
              TEMPERATURE_MAX_MILLIDEGREES,
              TEMPERATURE_HYSTERESIS_MILLIDEGREES,
              SAFE_TEMPERATURE_MILLIDEGREES,
              24000) == 0);

    struct governor_policy_result result =
        apply_temperature_failure(&policy, 24010, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    result = apply_activity(&policy, 27009, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);
    CHECK(!policy.temperature_fault_active);

    result = apply_activity(&policy, 27010, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_TEMPERATURE_FAULT);
    CHECK(policy.temperature_failure_pending);
    CHECK(policy.temperature_fault_active);

    result = apply_temperature_failure(&policy, 27020, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    return 0;
}

/**
 * @brief Verify workload upshift when valid telemetry clears a fault.
 *
 * @return 0 when recovery and activity events are both reported, or -1 on
 *         failure.
 */
static int test_activity_upshifts_when_temperature_fault_recovers(void)
{
    struct governor_policy policy;
    CHECK(initialize_low_policy(&policy, 28000) == 0);

    apply_temperature_failure(&policy, 28010, false, true);
    struct governor_policy_result result =
        apply_activity(&policy, 31010, false, true);
    CHECK(result.recommended_pstate == GPU_PSTATE_LOW);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_TEMPERATURE_FAULT);

    result = apply_temperature(
        &policy,
        31020,
        SAFE_TEMPERATURE_MILLIDEGREES,
        false,
        true);
    CHECK(result.recommended_pstate == GPU_PSTATE_HIGH);
    CHECK(result.events
          == (GOVERNOR_POLICY_EVENT_TEMPERATURE_RECOVERY
              | GOVERNOR_POLICY_EVENT_VIDEO_UPSHIFT));
    CHECK(!policy.temperature_failure_pending);
    CHECK(!policy.temperature_fault_active);
    CHECK(policy.high_since_ms == 31020);

    result = apply_temperature(
        &policy,
        31030,
        SAFE_TEMPERATURE_MILLIDEGREES,
        false,
        true);
    CHECK(result.events == GOVERNOR_POLICY_EVENT_NONE);

    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_initializes_supported_pstates() != 0;
    failures += test_applies_thermal_limit_at_startup() != 0;
    failures += test_rejects_invalid_inputs() != 0;
    failures += test_video_activity_requests_high() != 0;
    failures += test_graphics_activity_requests_high() != 0;
    failures += test_keeps_activity_histories_separate() != 0;
    failures += test_reports_simultaneous_activity_triggers() != 0;
    failures += test_does_not_repeat_upshift_events() != 0;
    failures += test_thermal_limit_blocks_activity_upshift() != 0;
    failures += test_downshifts_after_continuous_inactivity() != 0;
    failures += test_activity_resets_inactivity_timer() != 0;
    failures += test_enforces_minimum_high_residency() != 0;
    failures += test_thermal_limit_immediately_forces_low() != 0;
    failures += test_thermal_limit_uses_strict_hysteresis() != 0;
    failures += test_activity_upshifts_when_thermal_limit_recovers() != 0;
    failures += test_temporary_temperature_failure_recovers() != 0;
    failures += test_temperature_failure_timeout_forces_low() != 0;
    failures += test_activity_upshifts_when_temperature_fault_recovers() != 0;

    if (failures != 0) {
        fprintf(stderr, "%d governor policy test group(s) failed\n", failures);
        return 1;
    }

    printf("All governor policy tests passed\n");
    return 0;
}
