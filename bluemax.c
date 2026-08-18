#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum {
    // Activity is sampled at 100 Hz and temperature is sampled at 1 Hz.
    SAMPLE_INTERVAL_MS = 10,
    TEMPERATURE_POLL_INTERVAL_MS = 1000,

    // HIGH must be held briefly, while downshifts require sustained idleness.
    HIGH_MIN_RESIDENCY_MS = 500,
    IDLE_DOWNSHIFT_DELAY_MS = 2000,

    // Missing temperature telemetry becomes a safety fault after this delay.
    TEMPERATURE_FAULT_TIMEOUT_MS = 3000,

    // Video activity triggers HIGH after two active samples out of three.
    VIDEO_TRIGGER_WINDOW = 3,
    VIDEO_TRIGGER_COUNT = 2,

    // Graphics activity triggers HIGH after three active samples out of five.
    GRAPHICS_TRIGGER_WINDOW = 5,
    GRAPHICS_TRIGGER_COUNT = 3
};

// Each mask retains only the samples evaluated by its activity trigger.
#define VIDEO_TRIGGER_MASK ((1ULL << VIDEO_TRIGGER_WINDOW) - 1ULL)
#define GRAPHICS_TRIGGER_MASK ((1ULL << GRAPHICS_TRIGGER_WINDOW) - 1ULL)

// Byte offsets of the read-only engine-status registers within NVIDIA BAR0.
enum {
    PGRAPH_REGISTER_OFFSET = 0x400700,
    PVLD_REGISTER_OFFSET = 0x084048,
    PPDEC_REGISTER_OFFSET = 0x08504c,
    PPPP_REGISTER_OFFSET = 0x08604c
};

// BlueMax selects LOW or HIGH automatically; MEDIUM is observation-only at this time
enum gpu_pstate {
    GPU_PSTATE_LOW,
    GPU_PSTATE_MEDIUM,
    GPU_PSTATE_HIGH
};

// Snapshot of the graphics and hardware video engine-status registers.
struct gpu_activity_sample {
    uint32_t pgraph;
    uint32_t pvld;
    uint32_t ppdec;
    uint32_t pppp;
};

// Runtime state and resources owned by the single-threaded governor.
struct governor_context {
    enum gpu_pstate pstate;

    // Bit 0 is the newest sample; graphics and video histories stay separate.
    uint64_t graphics_history;
    uint64_t video_history;

    // Monotonic timestamps used to enforce residency and inactivity delays.
    struct timespec high_since;
    struct timespec last_activity_time;

    // A persistent read failure forces LOW after the configured timeout.
    bool temperature_fault_active;
    struct timespec temperature_fault_since;

    // Nouveau's cached thermal limit and hysteresis, in millidegrees Celsius.
    int temp_max_millidegrees;
    int temp_max_hyst_millidegrees;

    // BAR0 is mapped read-only once and kept for the daemon's lifetime.
    void *bar0_address;

    // Register addresses are resolved at startup to keep sampling inexpensive.
    volatile const uint32_t *pgraph_reg;
    volatile const uint32_t *pvld_reg;
    volatile const uint32_t *ppdec_reg;
    volatile const uint32_t *pppp_reg;
};

int main(void)
{
    return 0;
}
