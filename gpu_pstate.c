/**
 * @file gpu_pstate.c
 * @brief Nouveau debugfs access for reading and selecting GPU pstates.
 */

#define _POSIX_C_SOURCE 200809L

#include "gpu_pstate.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief Fixed clocks reported for one listed or currently active pstate. */
struct gpu_pstate_clocks {
    int core_mhz;
    int shader_mhz;
    int memory_mhz;
};

/** Parse the three fixed clocks used by the target Nouveau pstate format. */
static int parse_pstate_clocks(const char *description, struct gpu_pstate_clocks *clocks)
{
    struct gpu_pstate_clocks parsed;
    int consumed = -1;

    if (sscanf(
            description,
            " core %d MHz shader %d MHz memory %d MHz %n",
            &parsed.core_mhz,
            &parsed.shader_mhz,
            &parsed.memory_mhz,
            &consumed) != 3
        || consumed < 0
        || parsed.core_mhz <= 0
        || parsed.shader_mhz <= 0
        || parsed.memory_mhz <= 0)
        return 0;

    const char *remainder = description + consumed;
    if (*remainder == '*')
        remainder++;

    while (isspace((unsigned char)*remainder))
        remainder++;

    if (*remainder != '\0')
        return 0;

    *clocks = parsed;
    return 1;
}

/** Parse a supported two-character state label at the start of a line. */
static int parse_supported_state_label(const char *position, enum gpu_pstate *pstate)
{
    if (strlen(position) < 3 || position[0] != '0' || position[2] != ':')
        return 0;

    if (position[1] == '3')
        *pstate = GPU_PSTATE_LOW;
    else if (position[1] == '7')
        *pstate = GPU_PSTATE_MEDIUM;
    else if (position[1] == 'f' || position[1] == 'F')
        *pstate = GPU_PSTATE_HIGH;
    else
        return 0;

    return 1;
}

/** Parse fixed clocks from a supported state listing. */
static int parse_listed_state(const char *line, enum gpu_pstate *pstate, struct gpu_pstate_clocks *clocks)
{
    const char *position = line;
    while (isspace((unsigned char)*position))
        position++;

    if (!parse_supported_state_label(position, pstate))
        return 0;

    return parse_pstate_clocks(position + 3, clocks);
}

/** Parse the fixed clocks from Nouveau's current-state AC line. */
static int parse_current_clocks(const char *line, struct gpu_pstate_clocks *clocks)
{
    const char *position = line;
    while (isspace((unsigned char)*position))
        position++;

    if (strlen(position) < 3
        || (position[0] != 'A' && position[0] != 'a')
        || (position[1] != 'C' && position[1] != 'c')
        || position[2] != ':')
        return 0;

    return parse_pstate_clocks(position + 3, clocks);
}

/** Return whether two pstate clock tuples are identical. */
static bool pstate_clocks_equal(const struct gpu_pstate_clocks *left, const struct gpu_pstate_clocks *right)
{
    return left->core_mhz == right->core_mhz
        && left->shader_mhz == right->shader_mhz
        && left->memory_mhz == right->memory_mhz;
}

/**
 * @brief Check one line from Nouveau's list of performance states.
 *
 * A typical line begins with a state label such as "03:" and ends with an
 * asterisk when that state is currently active. Lines without an asterisk can
 * be skipped.
 *
 * @param[in] line One line read from Nouveau's pstate file.
 * @param[out] pstate Destination for the active state when this line contains
 *                    the asterisk.
 *
 * @return 1 when the active state was found, 0 when this line is not active,
 *         or -1 when the active line cannot be understood.
 */
static int parse_active_state(const char *line, enum gpu_pstate *pstate)
{
    // Only the active line matters to BlueMax. Nouveau marks it with an
    // asterisk.
    if (strchr(line, '*') == NULL) {
        return 0;
    }

    const char *position = line;

    // Allow harmless spaces before the state label.
    while (isspace((unsigned char)*position)) {
        position++;
    }

    // The label must contain two hexadecimal characters followed by a colon,
    // for example "03:". Reject the line if that basic shape is missing.
    if (strlen(position) < 3
        || !isxdigit((unsigned char)position[0])
        || !isxdigit((unsigned char)position[1])
        || position[2] != ':') {
        errno = EINVAL;
        return -1;
    }

    // Some Nouveau versions mark only the AC line. Its clocks are matched to
    // the supported state listings after the complete file has been read.
    if ((position[0] == 'A' || position[0] == 'a')
        && (position[1] == 'C' || position[1] == 'c'))
        return 0;

    // Translate Nouveau's labels into names the rest of BlueMax can use
    // without needing to know the text format of the pstate file.
    if (parse_supported_state_label(position, pstate))
        return 1;

    // An asterisk was present, but it identified a state this version of BlueMax
    // does not support. Report that instead of choosing a substitute state.
    errno = EOPNOTSUPP;
    return -1;
}

int gpu_pstate_read(const char *pstate_path, enum gpu_pstate *pstate)
{
    // One value tells us which file to read; the other gives us somewhere to
    // return the performance state that we find.
    if (pstate_path == NULL || pstate == NULL) {
        errno = EINVAL;
        return -1;
    }

    // Nouveau provides its performance-state information in a text file. Open
    // it read-only because checking the current state must not change the GPU.
    int descriptor = open(pstate_path, O_RDONLY | O_CLOEXEC);
    
    if (descriptor == -1) {
        return -1;
    }

    // Prepare the file to be read one complete line at a time.
    FILE *stream = fdopen(descriptor, "r");

    if (stream == NULL) {
        // Preparation failed, so close the file opened above. Keep the first
        // error so cleanup does not hide the reason this operation failed.
        int stream_error = errno;
        close(descriptor);
        errno = stream_error;
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    bool active_state_found = false;
    enum gpu_pstate active_state = GPU_PSTATE_LOW;
    struct gpu_pstate_clocks listed_clocks[GPU_PSTATE_HIGH + 1] = {0};
    bool listed_state_found[GPU_PSTATE_HIGH + 1] = {false};
    struct gpu_pstate_clocks current_clocks = {0};
    bool current_clocks_found = false;
    int result = 0;

    // Each line describes an available performance state. Nouveau puts an
    // asterisk beside the state currently in use, so inspect every line to find
    // it.
    while (getline(&line, &capacity, stream) != -1) {
        enum gpu_pstate listed_state;
        struct gpu_pstate_clocks clocks;

        if (parse_listed_state(line, &listed_state, &clocks))
        {
            listed_clocks[listed_state] = clocks;
            listed_state_found[listed_state] = true;
        }

        if (parse_current_clocks(line, &clocks))
        {
            current_clocks = clocks;
            current_clocks_found = true;
        }

        enum gpu_pstate parsed_state;

        int parsed = parse_active_state(line, &parsed_state);
        
        // Stop if the marked line does not contain a state BlueMax recognizes.
        if (parsed == -1) {
            result = -1;
            break;
        }

        if (parsed == 1) {
            // Only one state can be active. Two marked lines would be
            // contradictory, so report invalid data instead of guessing.
            if (active_state_found) {
                errno = EINVAL;
                result = -1;
                break;
            }

            active_state = parsed_state;
            active_state_found = true;
        }
    }

    // Finishing the file is normal. Check whether the loop ended because some
    // part of the file could not be read instead.
    if (result == 0 && ferror(stream)) {

        if (errno == 0) {
            errno = EIO;
        }

        result = -1;
    }

    // Older Nouveau versions omit the marker and report only the current AC
    // clocks. Match that tuple to exactly one supported state listing.
    if (result == 0 && !active_state_found && current_clocks_found)
    {
        unsigned int matches = 0;

        for (enum gpu_pstate state = GPU_PSTATE_LOW; state <= GPU_PSTATE_HIGH; state++)
        {
            if (listed_state_found[state] && pstate_clocks_equal(&listed_clocks[state], &current_clocks))
            {
                active_state = state;
                matches++;
            }
        }

        active_state_found = matches == 1;
    }

    if (result == 0 && !active_state_found)
    {
        errno = ENODATA;
        result = -1;
    }

    // Remember any error found above before releasing temporary data and
    // closing the file, since cleanup can produce a different error.
    int operation_error = errno;
    free(line);

    // The complete listing has been checked, so the file is no longer needed.
    if (fclose(stream) == EOF && result == 0) {
        return -1;
    }

    // On failure, report the original problem and do not change the caller's
    // output value.
    if (result == -1) {
        errno = operation_error;
        return -1;
    }

    // Everything was valid, so it is now safe to return the active state.
    *pstate = active_state;
    return 0;
}

int gpu_pstate_set(const char *pstate_path, enum gpu_pstate pstate)
{
    // A path is required so BlueMax knows which GPU control file to use.
    if (pstate_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    const char *command;
    
    // Convert BlueMax's state name into the short command Nouveau expects.
    // BlueMax can observe the medium state, but its automatic policy is only
    // allowed to choose the low and high states.
    switch (pstate) {
        case GPU_PSTATE_LOW:
            command = "03\n";
            break;

        case GPU_PSTATE_HIGH:
            command = "0f\n";
            break;

        case GPU_PSTATE_MEDIUM:
        default:
            errno = EINVAL;
            return -1;
    }

    // Open the control file for writing only when a change is requested. This
    // file receives a command; it is not a normal document that should be
    // erased or rewritten in full.
    int descriptor = open(pstate_path, O_WRONLY | O_CLOEXEC);

    if (descriptor == -1) {
        return -1;
    }

    size_t remaining = strlen(command);
    const char *position = command;

    // Keep writing until the complete command has been accepted. Although the
    // command is short, a write can be interrupted or accept only part of it.
    while (remaining > 0) {
        ssize_t written = write(descriptor, position, remaining);

        if (written == -1) {
            // An interruption is temporary, so try the same bytes again.
            if (errno == EINTR) {
                continue;
            }

            // A real write failure ends the operation. Preserve that error
            // while closing the file so the caller receives the useful cause.
            int write_error = errno;
            close(descriptor);
            errno = write_error;
            return -1;
        }

        // Making no progress would otherwise leave this loop running forever.
        if (written == 0) {
            close(descriptor);
            errno = EIO;
            return -1;
        }

        position += written;
        remaining -= (size_t)written;
    }

    // The command has been delivered, so close the file immediately. A close
    // failure is also reported because the requested change may not be safe to
    // treat as complete.
    return close(descriptor);
}
