#ifndef BLUEMAX_TEST_HELPERS_H
#define BLUEMAX_TEST_HELPERS_H

#include <stddef.h>
#include <sys/types.h>

/**
 * @brief Join a directory and filename without allowing silent truncation.
 *
 * @param[out] destination Buffer that receives the resulting path.
 * @param[in] capacity Size of @p destination in bytes.
 * @param[in] directory Directory portion of the path.
 * @param[in] filename Final path component.
 *
 * @return 0 on success, or -1 with @c errno set to @c ENAMETOOLONG if the path
 *         does not fit.
 */
int test_build_path(
    char *destination,
    size_t capacity,
    const char *directory,
    const char *filename);

/**
 * @brief Create or replace a text file used by a test fixture.
 *
 * Interrupted and partial writes are handled so the complete value is stored.
 *
 * @param[in] directory Directory that will contain the file.
 * @param[in] filename Name of the file to create or replace.
 * @param[in] text Complete text to write to the file.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int test_write_text(
    const char *directory,
    const char *filename,
    const char *text);

/**
 * @brief Create or replace a fixture file with an exact logical size.
 *
 * This is useful for sparse mock resources whose addressable size matters but
 * whose contents do not need to consume equivalent physical storage.
 *
 * @param[in] directory Directory that will contain the file.
 * @param[in] filename Name of the file to create or replace.
 * @param[in] size Required logical size in bytes.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int test_create_sized_file(
    const char *directory,
    const char *filename,
    off_t size);

/**
 * @brief Write bytes at a specific offset in an existing fixture file.
 *
 * Interrupted and partial writes are handled so the complete value is stored.
 *
 * @param[in] directory Directory containing the file.
 * @param[in] filename Name of the existing file.
 * @param[in] data Bytes to write.
 * @param[in] length Number of bytes to write.
 * @param[in] offset Byte offset at which writing begins.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int test_write_bytes_at(
    const char *directory,
    const char *filename,
    const void *data,
    size_t length,
    off_t offset);

/**
 * @brief Remove one known test-fixture file on a best-effort basis.
 *
 * Missing files and path-construction failures are intentionally ignored so
 * this helper remains safe to call after partial fixture setup.
 */
void test_remove_file(const char *directory, const char *filename);

#endif
