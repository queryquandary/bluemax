#ifndef BLUEMAX_GPU_MMIO_H
#define BLUEMAX_GPU_MMIO_H

#include <stddef.h>
#include <stdint.h>

/** @brief Snapshot of the graphics and hardware video engine-status registers. */
struct gpu_activity_sample {
    uint32_t pgraph; /**< Graphics engine status. */
    uint32_t pvld;   /**< Video decoder engine status. */
    uint32_t ppdec;  /**< Video parser/decoder engine status. */
    uint32_t pppp;   /**< Video post-processing engine status. */
};

/** @brief Read-only BAR0 mapping and resolved activity-register addresses. */
struct gpu_mmio {
    /** Read-only BAR0 mapping retained until gpu_mmio_unmap() is called. */
    void *bar0_address;

    /** Length of the retained BAR0 mapping, in bytes. */
    size_t bar0_length;

    /** Address of the graphics engine-status register. */
    volatile const uint32_t *pgraph_reg;

    /** Address of the video decoder engine-status register. */
    volatile const uint32_t *pvld_reg;

    /** Address of the video parser/decoder engine-status register. */
    volatile const uint32_t *ppdec_reg;

    /** Address of the video post-processing engine-status register. */
    volatile const uint32_t *pppp_reg;
};

/**
 * @brief Map a GPU's BAR0 resource for read-only activity telemetry.
 *
 * Opens @p resource_path read-only, verifies that it spans the complete BAR0
 * mapping, resolves the activity-register addresses, and closes the underlying
 * file descriptor. The completed mapping is published to @p gpu only after all
 * initialization steps succeed.
 *
 * @param[in] resource_path Path to the GPU's PCI BAR0 resource file.
 * @param[out] gpu Destination for the initialized mapping. It must not already
 *                 own an active mapping.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int gpu_mmio_map(const char *resource_path, struct gpu_mmio *gpu);

/**
 * @brief Read one fresh snapshot of the GPU activity registers.
 *
 * @param[in] gpu Mapping previously initialized by gpu_mmio_map().
 * @param[out] sample Destination for the four freshly loaded register values.
 */
void gpu_mmio_read_activity(
    const struct gpu_mmio *gpu,
    struct gpu_activity_sample *sample);

/**
 * @brief Release a GPU BAR0 mapping.
 *
 * The mapping structure is cleared after a successful unmap.
 *
 * @param[in,out] gpu Mapping previously initialized by gpu_mmio_map().
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int gpu_mmio_unmap(struct gpu_mmio *gpu);

#endif
