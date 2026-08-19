#ifndef BLUEMAX_THERMAL_H
#define BLUEMAX_THERMAL_H

#include <limits.h>

/**
 * @brief Cached information for a discovered thermal sensor.
 *
 * The information remains valid after Nouveau's hwmon device is discovered.
 */
struct thermal_sensor {
    /** Path to the file containing the current temperature. */
    char input_path[PATH_MAX];

    /** Maximum temperature limit, in millidegrees Celsius. */
    int max_millidegrees;

    /** Hysteresis threshold, in millidegrees Celsius. */
    int max_hyst_millidegrees;
};

/**
 * @brief Locate Nouveau's hwmon device and read its initial temperature.
 *
 * Discovers the device and caches its temperature limits in @p sensor.
 *
 * @param[in] hwmon_root
 *     Path to the root directory containing hwmon devices.
 * @param[out] sensor
 *     Destination for the discovered sensor information.
 * @param[out] initial_temperature_millidegrees
 *     Destination for the initial temperature, in millidegrees Celsius.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int thermal_sensor_discover(
    const char *hwmon_root,
    struct thermal_sensor *sensor,
    int *initial_temperature_millidegrees);

/**
 * @brief Read the current temperature from a previously discovered sensor.
 *
 * @param[in] sensor
 *     Sensor previously initialized by thermal_sensor_discover().
 * @param[out] temperature_millidegrees
 *     Destination for the current temperature, in millidegrees Celsius.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int thermal_sensor_read(
    const struct thermal_sensor *sensor,
    int *temperature_millidegrees);

#endif
