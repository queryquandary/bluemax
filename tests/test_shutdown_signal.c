/**
 * @file test_shutdown_signal.c
 * @brief Tests for orderly-shutdown signal handling.
 */

#define _POSIX_C_SOURCE 200809L

#include "shutdown_signal.h"

#include <signal.h>
#include <stddef.h>
#include <stdio.h>

/** Evaluate one condition and return failure from the current test. */
#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
            return -1;                                                          \
        }                                                                       \
    } while (0)

/** Verify SIGINT records a request without terminating the process. */
static int test_handles_sigint(void)
{
    CHECK(shutdown_signal_install() == 0);
    CHECK(!shutdown_signal_requested());
    CHECK(raise(SIGINT) == 0);
    CHECK(shutdown_signal_requested());
    CHECK(shutdown_signal_restore() == 0);
    return 0;
}

/** Verify a new installation clears the prior request before handling SIGTERM. */
static int test_handles_sigterm(void)
{
    CHECK(shutdown_signal_install() == 0);
    CHECK(!shutdown_signal_requested());
    CHECK(raise(SIGTERM) == 0);
    CHECK(shutdown_signal_requested());
    CHECK(shutdown_signal_restore() == 0);
    return 0;
}

/** Verify installation and restoration preserve prior signal dispositions. */
static int test_restores_previous_dispositions(void)
{
    struct sigaction previous_sigint;
    struct sigaction previous_sigterm;
    struct sigaction restored_sigint;
    struct sigaction restored_sigterm;

    CHECK(sigaction(SIGINT, NULL, &previous_sigint) == 0);
    CHECK(sigaction(SIGTERM, NULL, &previous_sigterm) == 0);
    CHECK(shutdown_signal_install() == 0);
    CHECK(shutdown_signal_restore() == 0);
    CHECK(sigaction(SIGINT, NULL, &restored_sigint) == 0);
    CHECK(sigaction(SIGTERM, NULL, &restored_sigterm) == 0);
    CHECK(restored_sigint.sa_handler == previous_sigint.sa_handler);
    CHECK(restored_sigterm.sa_handler == previous_sigterm.sa_handler);
    return 0;
}

/** Verify restoration is harmless when no handlers are installed. */
static int test_restore_is_idempotent(void)
{
    CHECK(shutdown_signal_restore() == 0);
    CHECK(shutdown_signal_restore() == 0);
    return 0;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"handles SIGINT", test_handles_sigint},
        {"handles SIGTERM", test_handles_sigterm},
        {"restores previous dispositions", test_restores_previous_dispositions},
        {"restoration is idempotent", test_restore_is_idempotent},
    };

    int failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
    {
        if (tests[index].run() == 0)
            printf("PASS: %s\n", tests[index].name);
        else
            failures++;
    }

    if (failures != 0)
    {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All shutdown signal tests passed\n");
    return 0;
}
