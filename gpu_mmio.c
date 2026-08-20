/**
 * @file gpu_mmio.c
 * @brief Read-only NVIDIA BAR0 access for GPU activity telemetry.
 */

#define _POSIX_C_SOURCE 200809L

#include "gpu_mmio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/** Size of the target GPU's BAR0 resource (16 MiB). */
#define BAR0_MAPPING_LENGTH ((size_t)16 * 1024 * 1024)

/** Byte offsets of the read-only engine-status registers within NVIDIA BAR0. */
enum {
    PGRAPH_REGISTER_OFFSET = 0x400700,
    PVLD_REGISTER_OFFSET = 0x084048,
    PPDEC_REGISTER_OFFSET = 0x08504c,
    PPPP_REGISTER_OFFSET = 0x08604c
};

/**
 * @brief Resolve a 32-bit MMIO register at a byte offset within BAR0.
 *
 * The returned address is read-only to BlueMax and volatile because the GPU
 * can change the register value independently of the program.
 *
 * @param[in] bar0_address Base address of the mapped BAR0 resource.
 * @param[in] offset Byte offset of the register within BAR0.
 *
 * @return Address of the requested read-only MMIO register.
 */
static volatile const uint32_t *register_at(void *bar0_address, size_t offset)
{
    // Resolve byte offsets during startup so the sampling path only dereferences
    // the cached register pointers.
    return (volatile const uint32_t *)((const uint8_t *)bar0_address + offset);
}

int gpu_mmio_map(const char *resource_path, struct gpu_mmio *gpu)
{
    if (resource_path == NULL || gpu == NULL) {
        errno = EINVAL;
        return -1;
    }

    int descriptor = open(resource_path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) {
        return -1;
    }

    struct stat resource_status;
    if (fstat(descriptor, &resource_status) == -1) {
        int status_error = errno;
        close(descriptor);
        errno = status_error;
        return -1;
    }

    if (resource_status.st_size < (off_t)BAR0_MAPPING_LENGTH) {
        close(descriptor);
        errno = EINVAL;
        return -1;
    }

    // Map the resource into the process's virtual address space as read-only.
    void *bar0_address = mmap(
        NULL,
        BAR0_MAPPING_LENGTH,
        PROT_READ,
        MAP_SHARED,
        descriptor,
        0);

    if (bar0_address == MAP_FAILED) {
        int mapping_error = errno;
        close(descriptor);
        errno = mapping_error;
        return -1;
    }

    if (close(descriptor) == -1) {
        int close_error = errno;
        munmap(bar0_address, BAR0_MAPPING_LENGTH);
        errno = close_error;
        return -1;
    }

    // Publish the mapping and its resolved register addresses as one unit only
    // after every fallible initialization operation has succeeded.
    struct gpu_mmio mapped = {
        .bar0_address = bar0_address,
        .bar0_length = BAR0_MAPPING_LENGTH,
        .pgraph_reg = register_at(bar0_address, PGRAPH_REGISTER_OFFSET),
        .pvld_reg = register_at(bar0_address, PVLD_REGISTER_OFFSET),
        .ppdec_reg = register_at(bar0_address, PPDEC_REGISTER_OFFSET),
        .pppp_reg = register_at(bar0_address, PPPP_REGISTER_OFFSET),
    };

    *gpu = mapped;
    return 0;
}

void gpu_mmio_read_activity(
    const struct gpu_mmio *gpu,
    struct gpu_activity_sample *sample)
{
    sample->pgraph = *gpu->pgraph_reg;
    sample->pvld = *gpu->pvld_reg;
    sample->ppdec = *gpu->ppdec_reg;
    sample->pppp = *gpu->pppp_reg;
}

int gpu_mmio_unmap(struct gpu_mmio *gpu)
{
    if (gpu == NULL || gpu->bar0_address == NULL || gpu->bar0_length == 0) {
        errno = EINVAL;
        return -1;
    }

    if (munmap(gpu->bar0_address, gpu->bar0_length) == -1) {
        return -1;
    }

    *gpu = (struct gpu_mmio){0};
    return 0;
}
