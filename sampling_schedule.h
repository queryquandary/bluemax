#ifndef BLUEMAX_SAMPLING_SCHEDULE_H
#define BLUEMAX_SAMPLING_SCHEDULE_H

#include "runtime_config.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Absolute deadlines retained by the runtime sampling scheduler. */
struct sampling_schedule {
    /** Next CLOCK_MONOTONIC time at which activity should be sampled. */
    uint64_t next_sample_deadline_ms;

    /** Next CLOCK_MONOTONIC time at which temperature should be polled. */
    uint64_t next_temperature_poll_deadline_ms;
};

/**
 * @brief Initialize absolute sampling deadlines from known startup time.
 *
 * The first activity sample is due immediately. Startup already supplies a
 * valid temperature, so the first temperature poll is due after one complete
 * temperature interval.
 *
 * @param[out] schedule Destination for the initialized deadlines.
 * @param[in] config Validated activity and temperature intervals.
 * @param[in] now_ms Current monotonic time in milliseconds.
 *
 * @return 0 on success, or -1 on invalid input or overflow with @c errno set.
 */
int sampling_schedule_init(struct sampling_schedule *schedule, const struct runtime_config *config, uint64_t now_ms);

/**
 * @brief Determine whether a cycle should perform a temperature poll.
 *
 * The caller is already executing an activity cycle selected from
 * next_sample_deadline_ms. This function decides only whether the independent
 * temperature deadline should piggyback on that cycle.
 *
 * @param[in] schedule Initialized sampling schedule.
 * @param[in] now_ms Monotonic time associated with the activity cycle.
 * @param[out] poll_temperature Whether the temperature deadline is due.
 *
 * @return 0 on success, or -1 on invalid input with @c errno set.
 */
int sampling_schedule_prepare(const struct sampling_schedule *schedule, uint64_t now_ms, bool *poll_temperature);

/**
 * @brief Advance absolute deadlines after one completed activity cycle.
 *
 * Every elapsed activity deadline is skipped. Temperature deadlines are
 * advanced only when this cycle attempted a temperature poll, and every
 * elapsed temperature deadline is likewise skipped. The schedule is unchanged
 * if either deadline cannot be advanced without overflow.
 *
 * @param[in,out] schedule Initialized sampling schedule.
 * @param[in] config Validated activity and temperature intervals.
 * @param[in] completed_ms Monotonic time after the cycle completed.
 * @param[in] temperature_polled Whether this cycle attempted a temperature read.
 *
 * @return 0 on success, or -1 on invalid input or overflow with @c errno set.
 */
int sampling_schedule_complete(struct sampling_schedule *schedule, const struct runtime_config *config, uint64_t completed_ms, bool temperature_polled);

#endif
