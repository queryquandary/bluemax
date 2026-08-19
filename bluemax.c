/**
 * @file bluemax.c
 * @brief GPU performance-state governor policy and runtime state.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum {
    /** Interval between GPU activity samples. */
    SAMPLE_INTERVAL_MS = 10,

    /** Interval between temperature samples. */
    TEMPERATURE_POLL_INTERVAL_MS = 1000,

    /** Minimum time the governor must remain in the HIGH state. */
    HIGH_MIN_RESIDENCY_MS = 500,

    /** Required period of inactivity before the governor may downshift. */
    IDLE_DOWNSHIFT_DELAY_MS = 2000,

    /** Time without temperature telemetry before a safety fault is declared. */
    TEMPERATURE_FAULT_TIMEOUT_MS = 3000,

    /** Number of recent samples considered by the video activity trigger. */
    VIDEO_TRIGGER_WINDOW = 3,

    /** Active video samples required to trigger the HIGH state. */
    VIDEO_TRIGGER_COUNT = 2,

    /** Number of recent samples considered by the graphics activity trigger. */
    GRAPHICS_TRIGGER_WINDOW = 5,

    /** Active graphics samples required to trigger the HIGH state. */
    GRAPHICS_TRIGGER_COUNT = 3
};

/** Mask for the samples evaluated by the video activity trigger. */
#define VIDEO_TRIGGER_MASK ((1ULL << VIDEO_TRIGGER_WINDOW) - 1ULL)

/** Mask for the samples evaluated by the graphics activity trigger. */
#define GRAPHICS_TRIGGER_MASK ((1ULL << GRAPHICS_TRIGGER_WINDOW) - 1ULL)

/** Byte offsets of the read-only engine-status registers within NVIDIA BAR0. */
enum {
    PGRAPH_REGISTER_OFFSET = 0x400700,
    PVLD_REGISTER_OFFSET = 0x084048,
    PPDEC_REGISTER_OFFSET = 0x08504c,
    PPPP_REGISTER_OFFSET = 0x08604c
};

/**
 * @brief GPU performance states observed or selected by the governor.
 *
 * BlueMax selects LOW or HIGH automatically. MEDIUM is currently observed but
 * never selected directly.
 */
enum gpu_pstate {
    GPU_PSTATE_LOW,    /**< Low-power state selected while the GPU is idle. */
    GPU_PSTATE_MEDIUM, /**< Intermediate state observed but not selected. */
    GPU_PSTATE_HIGH    /**< High-performance state selected during activity. */
};

/** @brief Snapshot of the graphics and hardware video engine-status registers. */
struct gpu_activity_sample {
    uint32_t pgraph; /**< Graphics engine status. */
    uint32_t pvld;   /**< Video decoder engine status. */
    uint32_t ppdec;  /**< Video parser/decoder engine status. */
    uint32_t pppp;   /**< Video post-processing engine status. */
};

/** @brief Runtime state and resources owned by the single-threaded governor. */
struct governor_context {
    enum gpu_pstate pstate; /**< Current GPU performance state. */

    /** Graphics activity history, bitfield with the newest sample in bit 0. */
    uint64_t graphics_history;

    /** Video activity history, bitfield with the newest sample in bit 0. */
    uint64_t video_history;

    /** Monotonic time at which the governor entered the HIGH state. */
    struct timespec high_since;

    /** Monotonic time of the most recent graphics or video activity. */
    struct timespec last_activity_time;

    /** Whether temperature telemetry is currently unavailable. */
    bool temperature_fault_active;

    /** Monotonic time at which the current telemetry failure began. */
    struct timespec temperature_fault_since;

    /** Nouveau's cached maximum temperature, in millidegrees Celsius. */
    int temp_max_millidegrees;

    /** Nouveau's cached thermal hysteresis, in millidegrees Celsius. */
    int temp_max_hyst_millidegrees;

    /** Read-only BAR0 mapping retained for the daemon's lifetime. */
    void *bar0_address;

    /** Address of the graphics engine-status register. */
    volatile const uint32_t *pgraph_reg;

    /** Address of the video decoder engine-status register. */
    volatile const uint32_t *pvld_reg;

    /** Address of the video parser/decoder engine-status register. */
    volatile const uint32_t *ppdec_reg;

    /** Address of the video post-processing engine-status register. */
    volatile const uint32_t *pppp_reg;
};

int main(void)
{
    return 0;
}
