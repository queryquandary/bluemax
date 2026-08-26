/**
 * @file shutdown_signal.c
 * @brief Process signal handling for orderly BlueMax shutdown.
 */

#define _POSIX_C_SOURCE 200809L

#include "shutdown_signal.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>

static volatile sig_atomic_t shutdown_requested;
static struct sigaction previous_sigint_action;
static struct sigaction previous_sigterm_action;
static bool handlers_installed;

/** Record a shutdown request without performing work in signal context. */
static void request_shutdown(int signal_number)
{
    // POSIX signal handlers must accept an int parameter containing 
    // the signal number, but this handler treats SIGINT and SIGTERM 
    // identically. Casting it to void prevents an -Wunused-parameter 
    // warning while documenting that ignoring it is deliberate. 
    // It generates no runtime behavior or machine code in an optimized build.
    (void)signal_number;
    shutdown_requested = 1;
}

int shutdown_signal_install(void)
{
    if (handlers_installed)
        return 0;

    struct sigaction action = {
        .sa_handler = request_shutdown,
    };

    if (sigemptyset(&action.sa_mask) == -1)
        return -1;

    shutdown_requested = 0;

    if (sigaction(SIGINT, &action, &previous_sigint_action) == -1)
        return -1;

    if (sigaction(SIGTERM, &action, &previous_sigterm_action) == -1)
    {
        int install_error = errno;
        sigaction(SIGINT, &previous_sigint_action, NULL);
        errno = install_error;
        return -1;
    }

    handlers_installed = true;
    return 0;
}

bool shutdown_signal_requested(void)
{
    return shutdown_requested != 0;
}

int shutdown_signal_restore(void)
{
    if (!handlers_installed)
        return 0;

    int first_error = 0;

    if (sigaction(SIGTERM, &previous_sigterm_action, NULL) == -1)
        first_error = errno;

    if (sigaction(SIGINT, &previous_sigint_action, NULL) == -1 && first_error == 0)
        first_error = errno;

    if (first_error != 0)
    {
        errno = first_error;
        return -1;
    }

    handlers_installed = false;
    return 0;
}
