/**
 * @file thermal.c
 * @brief Nouveau hwmon discovery and temperature-reading implementation.
 */

#define _POSIX_C_SOURCE 200809L

#include "thermal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    /** Maximum supported length of a textual sysfs value, including its terminator. */
    SYSFS_VALUE_CAPACITY = 64
};

/**
 * @brief Construct a path from a directory and filename.
 *
 * @param[out] destination Buffer that receives the constructed path.
 * @param[in] capacity Size of @p destination in bytes.
 * @param[in] directory Directory portion of the path.
 * @param[in] filename Final path component.
 *
 * @return 0 on success, or -1 with @c errno set to @c ENAMETOOLONG if the
 *         resulting path does not fit.
 */
static int build_path(
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

/**
 * @brief Read a small text file into a null-terminated buffer.
 *
 * Interrupted reads are retried. A value that fills the available buffer is
 * rejected rather than returned as potentially truncated data.
 *
 * @param[in] path Path of the file to read.
 * @param[out] buffer Buffer that receives the file contents.
 * @param[in] capacity Size of @p buffer in bytes, including the terminator.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
static int read_text_file(
    const char *path,
    char *buffer,
    size_t capacity)
{
    if (capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) {
        return -1;
    }

    ssize_t length;
    do {
        length = read(descriptor, buffer, capacity - 1);
    } while (length == -1 && errno == EINTR);

    int read_error = errno;
    if (close(descriptor) == -1 && length >= 0) {
        return -1;
    }

    if (length < 0) {
        errno = read_error;
        return -1;
    }

    if ((size_t)length == capacity - 1) {
        errno = EOVERFLOW;
        return -1;
    }

    buffer[length] = '\0';
    return 0;
}

/**
 * @brief Read a decimal integer from a sysfs text file.
 *
 * Trailing whitespace is accepted, but trailing non-whitespace characters and
 * values outside the range of @c int are rejected.
 *
 * @param[in] path Path of the file to read.
 * @param[out] value Destination for the parsed integer.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
static int read_integer_file(const char *path, int *value)
{
    char buffer[SYSFS_VALUE_CAPACITY];
    if (read_text_file(path, buffer, sizeof(buffer)) == -1) {
        return -1;
    }

    errno = 0;
    char *remainder;
    long parsed = strtol(buffer, &remainder, 10);
    if (errno == ERANGE || remainder == buffer || parsed < INT_MIN || parsed > INT_MAX) {
        errno = ERANGE;
        return -1;
    }

    while (isspace((unsigned char)*remainder)) {
        remainder++;
    }

    if (*remainder != '\0') {
        errno = EINVAL;
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

/**
 * @brief Determine whether an hwmon device is managed by Nouveau.
 *
 * @param[in] device_path Path to an hwmon device directory.
 *
 * @retval 1 The device name is `nouveau`.
 * @retval 0 The device is managed by another driver.
 * @retval -1 The device name could not be read; @c errno is set.
 */
static int device_is_nouveau(const char *device_path)
{
    char name_path[PATH_MAX];
    if (build_path(name_path, sizeof(name_path), device_path, "name") == -1) {
        return -1;
    }

    char name[SYSFS_VALUE_CAPACITY];
    if (read_text_file(name_path, name, sizeof(name)) == -1) {
        return -1;
    }

    char *end = name + strlen(name);
    while (end > name && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    return strcmp(name, "nouveau") == 0;
}

/**
 * @brief Find Nouveau's device directory beneath an hwmon root.
 *
 * Unrelated and unreadable hwmon entries are skipped so that one bad entry
 * does not prevent discovery of a later Nouveau device.
 *
 * @param[in] hwmon_root Directory containing hwmon device directories.
 * @param[out] device_path Buffer that receives the discovered device path.
 * @param[in] capacity Size of @p device_path in bytes.
 *
 * @return 0 on success, or -1 on failure with @c errno set. If no matching
 *         device exists, @c errno is set to @c ENODEV.
 */
static int find_nouveau_device(
    const char *hwmon_root,
    char *device_path,
    size_t capacity)
{
    DIR *directory = opendir(hwmon_root);
    if (directory == NULL) {
        return -1;
    }

    int result = -1;
    int saved_errno = ENODEV;
    struct dirent *entry;

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) {
            continue;
        }

        if (build_path(device_path, capacity, hwmon_root, entry->d_name) == -1) {
            saved_errno = errno;
            break;
        }

        int matches = device_is_nouveau(device_path);
        if (matches == 1) {
            result = 0;
            break;
        }

        errno = 0;
    }

    if (entry == NULL && errno != 0) {
        saved_errno = errno;
    }

    if (closedir(directory) == -1 && result == 0) {
        return -1;
    }

    if (result == -1) {
        errno = saved_errno;
    }

    return result;
}

int thermal_sensor_read(
    const struct thermal_sensor *sensor,
    int *temperature_millidegrees)
{
    if (sensor == NULL || temperature_millidegrees == NULL) {
        errno = EINVAL;
        return -1;
    }

    return read_integer_file(sensor->input_path, temperature_millidegrees);
}

int thermal_sensor_discover(
    const char *hwmon_root,
    struct thermal_sensor *sensor,
    int *initial_temperature_millidegrees)
{
    if (hwmon_root == NULL || sensor == NULL || initial_temperature_millidegrees == NULL) {
        errno = EINVAL;
        return -1;
    }

    char device_path[PATH_MAX];
    if (find_nouveau_device(hwmon_root, device_path, sizeof(device_path)) == -1) {
        return -1;
    }

    struct thermal_sensor discovered = {0};
    char value_path[PATH_MAX];

    if (build_path(value_path, sizeof(value_path), device_path, "temp1_max") == -1
        || read_integer_file(value_path, &discovered.max_millidegrees) == -1
        || build_path(value_path, sizeof(value_path), device_path, "temp1_max_hyst") == -1
        || read_integer_file(value_path, &discovered.max_hyst_millidegrees) == -1
        || build_path(
               discovered.input_path,
               sizeof(discovered.input_path),
               device_path,
               "temp1_input") == -1) {
        return -1;
    }

    if (discovered.max_millidegrees <= 0
        || discovered.max_hyst_millidegrees < 0
        || discovered.max_hyst_millidegrees >= discovered.max_millidegrees) {
        errno = EINVAL;
        return -1;
    }

    int initial_temperature;
    if (thermal_sensor_read(&discovered, &initial_temperature) == -1) {
        return -1;
    }

    *sensor = discovered;
    *initial_temperature_millidegrees = initial_temperature;
    return 0;
}
