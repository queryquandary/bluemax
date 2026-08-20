/**
 * @file test_helpers.c
 * @brief Reusable filesystem helpers for BlueMax tests.
 */

#define _POSIX_C_SOURCE 200809L

#include "test_helpers.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int test_build_path(
    char *destination,
    size_t capacity,
    const char *directory,
    const char *filename)
{
    int length = snprintf(destination, capacity, "%s/%s", directory, filename);
    if (length < 0 || (size_t)length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int test_write_text(
    const char *directory,
    const char *filename,
    const char *text)
{
    char path[PATH_MAX];
    if (test_build_path(path, sizeof(path), directory, filename) == -1) {
        return -1;
    }

    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor == -1) {
        return -1;
    }

    size_t remaining = strlen(text);
    const char *position = text;
    while (remaining > 0) {
        ssize_t written = write(descriptor, position, remaining);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }

            int write_error = errno;
            close(descriptor);
            errno = write_error;
            return -1;
        }

        position += written;
        remaining -= (size_t)written;
    }

    return close(descriptor);
}

int test_create_sized_file(
    const char *directory,
    const char *filename,
    off_t size)
{
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }

    char path[PATH_MAX];
    if (test_build_path(path, sizeof(path), directory, filename) == -1) {
        return -1;
    }

    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor == -1) {
        return -1;
    }

    if (ftruncate(descriptor, size) == -1) {
        int truncate_error = errno;
        close(descriptor);
        errno = truncate_error;
        return -1;
    }

    return close(descriptor);
}

int test_write_bytes_at(
    const char *directory,
    const char *filename,
    const void *data,
    size_t length,
    off_t offset)
{
    if (data == NULL || offset < 0) {
        errno = EINVAL;
        return -1;
    }

    char path[PATH_MAX];
    if (test_build_path(path, sizeof(path), directory, filename) == -1) {
        return -1;
    }

    int descriptor = open(path, O_WRONLY | O_CLOEXEC);
    if (descriptor == -1) {
        return -1;
    }

    const unsigned char *position = data;
    size_t remaining = length;
    off_t write_offset = offset;

    while (remaining > 0) {
        ssize_t written = pwrite(descriptor, position, remaining, write_offset);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }

            int write_error = errno;
            close(descriptor);
            errno = write_error;
            return -1;
        }

        if (written == 0) {
            close(descriptor);
            errno = EIO;
            return -1;
        }

        position += written;
        remaining -= (size_t)written;
        write_offset += written;
    }

    return close(descriptor);
}

void test_remove_file(const char *directory, const char *filename)
{
    char path[PATH_MAX];
    if (test_build_path(path, sizeof(path), directory, filename) == 0) {
        unlink(path);
    }
}
