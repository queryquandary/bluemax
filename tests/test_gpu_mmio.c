/**
 * @file test_gpu_mmio.c
 * @brief Tests for read-only GPU BAR0 mapping and activity sampling.
 */

#define _POSIX_C_SOURCE 200809L

#include "gpu_mmio.h"
#include "test_helpers.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// These independent test values describe the target hardware contract. They
// intentionally do not reuse private gpu_mmio.c constants.
enum {
    MOCK_BAR0_LENGTH = 16 * 1024 * 1024,
    PGRAPH_TEST_OFFSET = 0x400700,
    PVLD_TEST_OFFSET = 0x084048,
    PPDEC_TEST_OFFSET = 0x08504c,
    PPPP_TEST_OFFSET = 0x08604c
};

/** Mock PCI BAR0 resource owned by one test case. */
struct gpu_mmio_fixture {
    /// Unique temporary directory containing the mock resource.
    char root[PATH_MAX];

    /// Complete path passed to gpu_mmio_map().
    char resource_path[PATH_MAX];
};

/// Evaluate one condition and retain the test's cleanup path on failure.
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(                                                            \
                stderr,                                                         \
                "FAIL %s:%d: %s\n",                                            \
                __func__,                                                       \
                __LINE__,                                                       \
                #condition);                                                    \
            result = -1;                                                        \
            goto cleanup;                                                       \
        }                                                                       \
    } while (0)

/** Remove only the explicitly known mock resource and temporary directory. */
static void fixture_destroy(struct gpu_mmio_fixture *fixture)
{
    test_remove_file(fixture->root, "resource0");

    if (fixture->root[0] != '\0') {
        rmdir(fixture->root);
    }
}

/**
 * @brief Create a sparse 16 MiB file representing the target GPU's BAR0.
 *
 * The file has the required logical size without allocating a 16 MiB memory
 * buffer or writing every byte.
 */
static int fixture_create(struct gpu_mmio_fixture *fixture)
{
    static const char root_template[] = "/tmp/bluemax-gpu-mmio-test-XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->root, root_template, sizeof(root_template));

    if (mkdtemp(fixture->root) == NULL
        || test_build_path(
               fixture->resource_path,
               sizeof(fixture->resource_path),
               fixture->root,
               "resource0") == -1
        || test_create_sized_file(fixture->root, "resource0", MOCK_BAR0_LENGTH) == -1) {
        int setup_error = errno;
        fixture_destroy(fixture);
        errno = setup_error;
        return -1;
    }

    return 0;
}

/** Write one complete set of activity-register values to the mock BAR0. */
static int write_activity(
    const struct gpu_mmio_fixture *fixture,
    const struct gpu_activity_sample *sample)
{
    if (test_write_bytes_at(
            fixture->root,
            "resource0",
            &sample->pgraph,
            sizeof(sample->pgraph),
            PGRAPH_TEST_OFFSET) == -1
        || test_write_bytes_at(
               fixture->root,
               "resource0",
               &sample->pvld,
               sizeof(sample->pvld),
               PVLD_TEST_OFFSET) == -1
        || test_write_bytes_at(
               fixture->root,
               "resource0",
               &sample->ppdec,
               sizeof(sample->ppdec),
               PPDEC_TEST_OFFSET) == -1
        || test_write_bytes_at(
               fixture->root,
               "resource0",
               &sample->pppp,
               sizeof(sample->pppp),
               PPPP_TEST_OFFSET) == -1) {
        return -1;
    }

    return 0;
}

static int test_maps_reads_fresh_values_and_unmaps(void)
{
    struct gpu_mmio_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    struct gpu_mmio gpu = {0};
    const struct gpu_activity_sample initial = {
        .pgraph = 0x00000101,
        .pvld = 0x00000003,
        .ppdec = 0x000001ff,
        .pppp = 0x0000002f
    };

    CHECK(write_activity(&fixture, &initial) == 0);
    CHECK(gpu_mmio_map(fixture.resource_path, &gpu) == 0);
    CHECK(gpu.bar0_address != NULL);
    CHECK(gpu.bar0_length == MOCK_BAR0_LENGTH);

    // Confirm initialization resolved each independent hardware offset.
    CHECK(
        (const volatile uint8_t *)gpu.pgraph_reg
        == (const volatile uint8_t *)gpu.bar0_address + PGRAPH_TEST_OFFSET);
    CHECK(
        (const volatile uint8_t *)gpu.pvld_reg
        == (const volatile uint8_t *)gpu.bar0_address + PVLD_TEST_OFFSET);
    CHECK(
        (const volatile uint8_t *)gpu.ppdec_reg
        == (const volatile uint8_t *)gpu.bar0_address + PPDEC_TEST_OFFSET);
    CHECK(
        (const volatile uint8_t *)gpu.pppp_reg
        == (const volatile uint8_t *)gpu.bar0_address + PPPP_TEST_OFFSET);

    struct gpu_activity_sample observed;
    gpu_mmio_read_activity(&gpu, &observed);
    CHECK(observed.pgraph == initial.pgraph);
    CHECK(observed.pvld == initial.pvld);
    CHECK(observed.ppdec == initial.ppdec);
    CHECK(observed.pppp == initial.pppp);

    const struct gpu_activity_sample updated = {
        .pgraph = 0,
        .pvld = 0,
        .ppdec = 0x0000007f,
        .pppp = 0
    };

    // Updating the backing file verifies that sampling performs fresh loads.
    CHECK(write_activity(&fixture, &updated) == 0);
    gpu_mmio_read_activity(&gpu, &observed);
    CHECK(observed.pgraph == updated.pgraph);
    CHECK(observed.pvld == updated.pvld);
    CHECK(observed.ppdec == updated.ppdec);
    CHECK(observed.pppp == updated.pppp);

    CHECK(gpu_mmio_unmap(&gpu) == 0);
    CHECK(gpu.bar0_address == NULL);
    CHECK(gpu.bar0_length == 0);
    CHECK(gpu.pgraph_reg == NULL);
    CHECK(gpu.pvld_reg == NULL);
    CHECK(gpu.ppdec_reg == NULL);
    CHECK(gpu.pppp_reg == NULL);

cleanup:
    if (gpu.bar0_address != NULL) {
        gpu_mmio_unmap(&gpu);
    }
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_undersized_resource_without_publishing(void)
{
    struct gpu_mmio_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    CHECK(test_create_sized_file(fixture.root, "resource0", MOCK_BAR0_LENGTH - 1) == 0);

    struct gpu_mmio gpu = {0};
    errno = 0;
    CHECK(gpu_mmio_map(fixture.resource_path, &gpu) == -1);
    CHECK(errno == EINVAL);
    CHECK(gpu.bar0_address == NULL);
    CHECK(gpu.bar0_length == 0);
    CHECK(gpu.pgraph_reg == NULL);
    CHECK(gpu.pvld_reg == NULL);
    CHECK(gpu.ppdec_reg == NULL);
    CHECK(gpu.pppp_reg == NULL);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_reports_missing_resource(void)
{
    struct gpu_mmio_fixture fixture;
    if (fixture_create(&fixture) == -1) {
        perror("fixture_create");
        return -1;
    }

    int result = 0;
    test_remove_file(fixture.root, "resource0");

    struct gpu_mmio gpu = {0};
    errno = 0;
    CHECK(gpu_mmio_map(fixture.resource_path, &gpu) == -1);
    CHECK(errno == ENOENT);

cleanup:
    fixture_destroy(&fixture);
    return result;
}

static int test_rejects_invalid_arguments(void)
{
    int result = 0;
    struct gpu_mmio gpu = {0};

    errno = 0;
    CHECK(gpu_mmio_map(NULL, &gpu) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_mmio_map("unused", NULL) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_mmio_unmap(NULL) == -1);
    CHECK(errno == EINVAL);

    errno = 0;
    CHECK(gpu_mmio_unmap(&gpu) == -1);
    CHECK(errno == EINVAL);

cleanup:
    return result;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"maps, reads fresh values, and unmaps", test_maps_reads_fresh_values_and_unmaps},
        {"rejects an undersized resource", test_rejects_undersized_resource_without_publishing},
        {"reports a missing resource", test_reports_missing_resource},
        {"rejects invalid arguments", test_rejects_invalid_arguments}
    };

    int failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (tests[index].run() == 0) {
            printf("PASS: %s\n", tests[index].name);
        } else {
            failures++;
        }
    }

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All GPU MMIO tests passed\n");
    return 0;
}
