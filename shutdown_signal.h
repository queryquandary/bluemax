#ifndef BLUEMAX_SHUTDOWN_SIGNAL_H
#define BLUEMAX_SHUTDOWN_SIGNAL_H

#include <stdbool.h>

/**
 * @brief Install orderly-shutdown handlers for SIGINT and SIGTERM.
 *
 * The handlers only record that shutdown was requested. Previous signal
 * dispositions are retained for shutdown_signal_restore().
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int shutdown_signal_install(void);

/** Return whether an installed handler has received SIGINT or SIGTERM. */
bool shutdown_signal_requested(void);

/**
 * @brief Restore the signal dispositions retained during installation.
 *
 * Calling this function when no handlers are installed succeeds without
 * changing signal dispositions.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int shutdown_signal_restore(void);

#endif
