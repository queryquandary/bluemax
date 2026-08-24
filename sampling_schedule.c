/**
 * @file sampling_schedule.c
 * @brief Deterministic activity and temperature deadline scheduling.
 */

#include "sampling_schedule.h"

#include <errno.h>
#include <stdint.h>

/** Validate the interval relationship required by the sampling loop. */
static int validate_config(const struct runtime_config *config)
{
    if (config == NULL
        || config->sample_interval_ms == 0
        || config->temperature_poll_interval_ms == 0
        || config->temperature_poll_interval_ms < config->sample_interval_ms)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/** Advance one absolute deadline to the first interval strictly after now. */
static int advance_deadline(uint64_t *deadline_ms, unsigned int interval_ms, uint64_t now_ms)
{
    // A future deadline needs no adjustment. This also lets an early cycle
    // completion preserve the already scheduled wakeup.
    if (*deadline_ms > now_ms)
        return 0;

    // Count every deadline at or before now, including the current one. Adding
    // that many whole intervals keeps the schedule anchored to its original
    // timeline instead of drifting with cycle completion time.
    uint64_t elapsed_ms = now_ms - *deadline_ms;
    uint64_t elapsed_intervals = elapsed_ms / interval_ms;

    // One interval beyond the elapsed count selects the first deadline strictly
    // after now. Guard that addition separately because UINT64_MAX + 1 would
    // wrap before the later multiplication check could detect the overflow.
    if (elapsed_intervals == UINT64_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }

    uint64_t intervals_to_advance = elapsed_intervals + 1U;

    if (intervals_to_advance > (UINT64_MAX - *deadline_ms) / interval_ms)
    {
        errno = EOVERFLOW;
        return -1;
    }

    *deadline_ms += intervals_to_advance * interval_ms;
    return 0;
}

int sampling_schedule_init(struct sampling_schedule *schedule, const struct runtime_config *config, uint64_t now_ms)
{
    if (schedule == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (validate_config(config) == -1)
        return -1;

    if ((uint64_t)config->temperature_poll_interval_ms > UINT64_MAX - now_ms)
    {
        errno = EOVERFLOW;
        return -1;
    }

    const struct sampling_schedule initialized = {
        .next_sample_deadline_ms = now_ms,
        .next_temperature_poll_deadline_ms = now_ms + config->temperature_poll_interval_ms,
    };

    *schedule = initialized;
    return 0;
}

int sampling_schedule_prepare(const struct sampling_schedule *schedule, uint64_t now_ms, bool *poll_temperature)
{
    if (schedule == NULL || poll_temperature == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *poll_temperature = now_ms >= schedule->next_temperature_poll_deadline_ms;
    return 0;
}

int sampling_schedule_complete(struct sampling_schedule *schedule, const struct runtime_config *config, uint64_t completed_ms, bool temperature_polled)
{
    if (schedule == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (validate_config(config) == -1)
        return -1;

    // Advance a private copy so overflow in either independent timeline cannot
    // leave the caller with only one deadline updated.
    struct sampling_schedule advanced = *schedule;
    if (advance_deadline(&advanced.next_sample_deadline_ms, config->sample_interval_ms, completed_ms) == -1)
        return -1;

    // A failed temperature read still counts as a poll attempt. Advancing its
    // deadline prevents repeated filesystem reads on every activity cycle.
    if (temperature_polled)
    {
        if (advance_deadline(&advanced.next_temperature_poll_deadline_ms, config->temperature_poll_interval_ms, completed_ms) == -1)
            return -1;
    }

    *schedule = advanced;
    return 0;
}
