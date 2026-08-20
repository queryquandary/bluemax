#ifndef BLUEMAX_GPU_PSTATE_H
#define BLUEMAX_GPU_PSTATE_H

/**
 * @brief Names BlueMax uses for the GPU's supported performance states.
 *
 * Higher states provide more performance but generally use more power and
 * produce more heat. BlueMax may choose LOW or HIGH automatically. MEDIUM is
 * recognized when another program selects it, but BlueMax does not select it.
 */
enum gpu_pstate {
    GPU_PSTATE_LOW,    /**< Low-power state 03. */
    GPU_PSTATE_MEDIUM, /**< Intermediate state 07. */
    GPU_PSTATE_HIGH    /**< High-performance state 0f. */
};

/**
 * @brief Find which GPU performance state is currently active.
 *
 * Nouveau lists the available states in a text file and marks the active one
 * with an asterisk. This function reads that list and returns the matching
 * BlueMax state name.
 *
 * @param[in] pstate_path Path to Nouveau's pstate file.
 * @param[out] pstate Destination for the active supported state.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int gpu_pstate_read(const char *pstate_path, enum gpu_pstate *pstate);

/**
 * @brief Ask Nouveau to change the GPU's performance state.
 *
 * Only GPU_PSTATE_LOW and GPU_PSTATE_HIGH are valid choices. The control file
 * is opened only long enough to send the requested state.
 *
 * @param[in] pstate_path Path to Nouveau's pstate file.
 * @param[in] pstate State to select.
 *
 * @return 0 on success, or -1 on failure with @c errno set.
 */
int gpu_pstate_set(const char *pstate_path, enum gpu_pstate pstate);

#endif
