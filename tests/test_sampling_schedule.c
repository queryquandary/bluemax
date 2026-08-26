/**
 * @file test_sampling_schedule.c
 * @brief Tests for deterministic activity and temperature deadlines.
 */

#include "sampling_schedule.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** Evaluate one condition and return failure from the current test. */
#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
            return -1;                                                          \
        }                                                                       \
    } while (0)

/** Verify startup schedules activity now and temperature one interval later. */
static int test_initializes_startup_deadlines(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1000);
    CHECK(schedule.next_temperature_poll_deadline_ms == 2000);
    return 0;
}

/** Verify temperature becomes due exactly at its absolute deadline. */
static int test_identifies_temperature_deadline(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;
    bool poll_temperature = true;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(sampling_schedule_prepare(&schedule, 1999, &poll_temperature) == 0);
    CHECK(!poll_temperature);
    CHECK(sampling_schedule_prepare(&schedule, 2000, &poll_temperature) == 0);
    CHECK(poll_temperature);
    CHECK(sampling_schedule_prepare(&schedule, 2500, &poll_temperature) == 0);
    CHECK(poll_temperature);
    return 0;
}

/** Verify ordinary completion advances from the absolute sample deadline. */
static int test_advances_without_deadline_drift(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(sampling_schedule_complete(&schedule, &config, 1007, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1010);
    CHECK(schedule.next_temperature_poll_deadline_ms == 2000);

    CHECK(sampling_schedule_complete(&schedule, &config, 1018, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1020);
    CHECK(schedule.next_temperature_poll_deadline_ms == 2000);
    return 0;
}

/** Verify every elapsed activity deadline is skipped rather than replayed. */
static int test_skips_missed_activity_deadlines(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(sampling_schedule_complete(&schedule, &config, 1047, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1050);

    CHECK(sampling_schedule_complete(&schedule, &config, 1049, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1050);
    return 0;
}

/** Verify one poll skips all elapsed temperature deadlines. */
static int test_advances_polled_temperature_deadlines(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;
    bool poll_temperature;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(sampling_schedule_prepare(&schedule, 3500, &poll_temperature) == 0);
    CHECK(poll_temperature);
    CHECK(sampling_schedule_complete(&schedule, &config, 3500, true) == 0);
    CHECK(schedule.next_sample_deadline_ms == 3510);
    CHECK(schedule.next_temperature_poll_deadline_ms == 4000);

    CHECK(sampling_schedule_prepare(&schedule, 3500, &poll_temperature) == 0);
    CHECK(!poll_temperature);
    return 0;
}

/** Verify a deadline crossed without polling remains due next cycle. */
static int test_retains_unpolled_temperature_deadline(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;
    bool poll_temperature;

    CHECK(sampling_schedule_init(&schedule, &config, 1000) == 0);
    CHECK(sampling_schedule_complete(&schedule, &config, 2005, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 2010);
    CHECK(schedule.next_temperature_poll_deadline_ms == 2000);
    CHECK(sampling_schedule_prepare(&schedule, 2010, &poll_temperature) == 0);
    CHECK(poll_temperature);
    return 0;
}

/** Verify independent deadlines when intervals are not evenly divisible. */
static int test_schedules_non_divisible_intervals(void)
{
    const struct runtime_config config = {333, 1000, false};
    struct sampling_schedule schedule;
    bool poll_temperature;

    CHECK(sampling_schedule_init(&schedule, &config, 0) == 0);
    CHECK(sampling_schedule_complete(&schedule, &config, 0, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 333);
    CHECK(sampling_schedule_complete(&schedule, &config, 333, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 666);
    CHECK(sampling_schedule_complete(&schedule, &config, 666, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 999);
    CHECK(sampling_schedule_prepare(&schedule, 999, &poll_temperature) == 0);
    CHECK(!poll_temperature);

    // The 1000 ms temperature deadline falls between activity samples, so the
    // first cycle able to service it is the independently scheduled 1332 ms one.
    CHECK(sampling_schedule_complete(&schedule, &config, 999, false) == 0);
    CHECK(schedule.next_sample_deadline_ms == 1332);
    CHECK(sampling_schedule_prepare(&schedule, 1332, &poll_temperature) == 0);
    CHECK(poll_temperature);
    CHECK(sampling_schedule_complete(&schedule, &config, 1332, true) == 0);
    CHECK(schedule.next_temperature_poll_deadline_ms == 2000);
    return 0;
}

/** Verify arithmetic failures leave the destination state unchanged. */
static int test_rejects_deadline_overflow_transactionally(void)
{
    const struct runtime_config config = {10, 1000, false};
    struct sampling_schedule schedule;
    struct sampling_schedule unchanged;

    memset(&schedule, 0xa5, sizeof(schedule));
    unchanged = schedule;
    errno = 0;
    CHECK(sampling_schedule_init(&schedule, &config, UINT64_MAX - 500) == -1);
    CHECK(errno == EOVERFLOW);
    CHECK(memcmp(&schedule, &unchanged, sizeof(schedule)) == 0);

    schedule = (struct sampling_schedule){UINT64_MAX - 5, UINT64_MAX};
    unchanged = schedule;
    errno = 0;
    CHECK(sampling_schedule_complete(&schedule, &config, UINT64_MAX - 1, false) == -1);
    CHECK(errno == EOVERFLOW);
    CHECK(memcmp(&schedule, &unchanged, sizeof(schedule)) == 0);

    // Here activity can advance, but temperature cannot. The private candidate
    // must prevent the successful activity calculation from leaking through.
    schedule = (struct sampling_schedule){UINT64_MAX, UINT64_MAX - 5};
    unchanged = schedule;
    errno = 0;
    CHECK(sampling_schedule_complete(&schedule, &config, UINT64_MAX - 1, true) == -1);
    CHECK(errno == EOVERFLOW);
    CHECK(memcmp(&schedule, &unchanged, sizeof(schedule)) == 0);

    // Exercise overflow in the skipped-interval count itself, before deadline
    // multiplication is reached.
    const struct runtime_config one_ms_config = {1, 100, false};
    schedule = (struct sampling_schedule){0, UINT64_MAX};
    unchanged = schedule;
    errno = 0;
    CHECK(sampling_schedule_complete(&schedule, &one_ms_config, UINT64_MAX, false) == -1);
    CHECK(errno == EOVERFLOW);
    CHECK(memcmp(&schedule, &unchanged, sizeof(schedule)) == 0);
    return 0;
}

/** Verify null pointers and unusable intervals are rejected. */
static int test_rejects_invalid_arguments(void)
{
    const struct runtime_config valid = {10, 1000, false};
    static const struct runtime_config invalid[] = {
        {0, 1000, false},
        {10, 0, false},
        {1000, 100, false},
    };
    struct sampling_schedule schedule = {0};
    bool poll_temperature;

    errno = 0;
    CHECK(sampling_schedule_init(NULL, &valid, 0) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(sampling_schedule_init(&schedule, NULL, 0) == -1);
    CHECK(errno == EINVAL);

    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        errno = 0;
        CHECK(sampling_schedule_init(&schedule, &invalid[index], 0) == -1);
        CHECK(errno == EINVAL);
    }

    errno = 0;
    CHECK(sampling_schedule_prepare(NULL, 0, &poll_temperature) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(sampling_schedule_prepare(&schedule, 0, NULL) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(sampling_schedule_complete(NULL, &valid, 0, false) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(sampling_schedule_complete(&schedule, NULL, 0, false) == -1);
    CHECK(errno == EINVAL);
    return 0;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"initializes startup deadlines", test_initializes_startup_deadlines},
        {"identifies temperature deadline", test_identifies_temperature_deadline},
        {"advances without deadline drift", test_advances_without_deadline_drift},
        {"skips missed activity deadlines", test_skips_missed_activity_deadlines},
        {"advances polled temperature deadlines", test_advances_polled_temperature_deadlines},
        {"retains unpolled temperature deadline", test_retains_unpolled_temperature_deadline},
        {"schedules non-divisible intervals", test_schedules_non_divisible_intervals},
        {"rejects deadline overflow transactionally", test_rejects_deadline_overflow_transactionally},
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

    printf("All sampling schedule tests passed\n");
    return 0;
}
