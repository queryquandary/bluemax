#ifndef BLUEMAX_RUNTIME_H
#define BLUEMAX_RUNTIME_H

#include "governor_policy.h"
#include "gpu_mmio.h"
#include "gpu_pstate.h"
#include "runtime_config.h"
#include "thermal.h"

/** @brief System paths used to initialize the BlueMax runtime. */
struct runtime_paths {
    /** Root directory containing Linux hwmon devices. */
    const char *hwmon_root;

    /** Nouveau debugfs file containing the available and active pstates. */
    const char *pstate_path;

    /** Linux PCI sysfs file representing the GPU's BAR0 resource. */
    const char *bar0_resource_path;
};

/** @brief Runtime state and resources owned by the single-threaded governor. */
struct governor_context {
    /** Validated runtime intervals supplied through the command line. */
    struct runtime_config config;

    /** Discovered thermal sensor retained for fresh temperature reads. */
    struct thermal_sensor thermal;

    /** Last performance state successfully read from or applied to the GPU. */
    enum gpu_pstate applied_pstate;

    /** Workload and thermal policy state. */
    struct governor_policy policy;

    /** Read-only GPU BAR0 mapping and resolved activity registers. */
    struct gpu_mmio gpu;
};

/** @brief Identifies the result and, on failure, the stage of runtime startup. */
enum runtime_startup_result {
    RUNTIME_STARTUP_OK,
    RUNTIME_STARTUP_INVALID_ARGUMENT,
    RUNTIME_STARTUP_THERMAL_ERROR,
    RUNTIME_STARTUP_PSTATE_ERROR,
    RUNTIME_STARTUP_MMIO_ERROR,
    RUNTIME_STARTUP_CLOCK_ERROR,
    RUNTIME_STARTUP_POLICY_ERROR
};

/**
 * @brief Initialize all hardware resources and policy state used at runtime.
 *
 * Startup is transactional: @p context is changed only after every stage
 * succeeds. Any BAR0 mapping acquired before a later failure is released while
 * the original failure remains available through @c errno.
 *
 * @param[out] context Destination for the completed runtime context.
 * @param[in] config Validated runtime configuration to retain.
 * @param[in] paths System paths used for hardware discovery and access.
 *
 * @return RUNTIME_STARTUP_OK on success, or the failed startup stage with
 *         @c errno set.
 */
enum runtime_startup_result runtime_start(struct governor_context *context, const struct runtime_config *config, const struct runtime_paths *paths);

/**
 * @brief Return a concise description of a runtime startup result.
 *
 * @param[in] result Result returned by runtime_start().
 *
 * @return Static text suitable for an error diagnostic.
 */
const char *runtime_startup_result_description(enum runtime_startup_result result);

/**
 * @brief Release resources retained by a runtime context.
 *
 * Cleanup is idempotent. A context without an active BAR0 mapping is already
 * considered clean.
 *
 * @param[in,out] context Runtime context to clean up.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int runtime_cleanup(struct governor_context *context);

#endif
