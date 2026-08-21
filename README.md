# BlueMax

**BlueMax** is a lightweight automatic GPU pstate governor for Linux systems using the Nouveau driver on legacy NVIDIA hardware.

It monitors real GPU engine activity and dynamically changes performance states so the GPU can remain at a low-power pstate while idle and quickly move to a high-performance pstate when graphics or hardware video decoding activity is detected.

The initial target platform is:

* Linux Mint 22.2 Xfce
* Lenovo ThinkPad W510
* NVIDIA Quadro FX 880M / GT216GLM
* Nouveau driver

BlueMax is intended to behave like a traditional automatic GPU performance governor: **set it and forget it**.

## Motivation

On the Quadro FX 880M, Nouveau exposes several manual performance states through:

```text
/sys/kernel/debug/dri/0000:01:00.0/pstate
```

Available states on the target hardware are:

```text
03  core 135 MHz, shader 270 MHz, memory 135 MHz
07  core 405 MHz, shader 810 MHz, memory 324 MHz
0f  core 550 MHz, shader 1210 MHz, memory 790 MHz
```

Nouveau's built-in `auto` mode does not provide useful dynamic scaling on this GPU. In testing, it tends to remain at a high pstate rather than reliably returning to a low-power state after activity ends.

BlueMax provides a small userspace governor specifically for this use case.

## Design Goals

BlueMax is designed around a few principles:

* Keep the daemon lightweight and lean.
* Minimize CPU wakeups, memory use, filesystem I/O, and other system overhead.
* Prefer simple designs over unnecessary abstraction or bookkeeping.
* Make decisions from current hardware telemetry whenever possible.
* Avoid duplicating functionality already provided by Nouveau or the hardware.
* Treat temperature as a safety signal, not as a workload detector.
* Never compromise GPU thermal safety in pursuit of performance.
* Avoid manual configuration during normal use.

## Workload Detection

BlueMax monitors four known GPU engine-status registers through NVIDIA BAR0 MMIO:

```text
PGRAPH  0x400700
PVLD    0x084048
PPDEC   0x08504c
PPPP    0x08604c
```

The current activity model is:

```text
graphics_activity_detected =
    PGRAPH != 0

video_activity_detected =
    PVLD != 0
    OR PPDEC != 0
    OR PPPP != 0
```

This provides useful separation between graphics workloads and hardware video decoding.

For example, testing has shown:

```text
OpenGL graphics workload:
    PGRAPH -> active
    PVLD   -> idle

H.264 VDPAU decoding:
    PVLD   -> 0x00000003
    PPDEC  -> intermittently 0x000001ff
    PPPP   -> intermittently 0x0000002f
```

## Current Governor Policy

The initial policy uses only the lowest and highest pstates:

```text
LOW   -> 03
HIGH  -> 0f
```

The intermediate `07` state is recognized but is not currently selected by the automatic policy.

Activity is sampled every:

```text
10 ms
```

Two 64-bit rolling histories track graphics and video activity.

The current upshift rules are:

```text
Video:
    2 active samples among the most recent 3

Graphics:
    3 active samples among the most recent 5
```

Once HIGH is entered:

```text
minimum HIGH residency: 500 ms
```

The GPU returns to LOW only after:

```text
2 seconds of continuous inactivity
```

Any valid graphics or video activity resets the inactivity timer.

## Thermal Safety

BlueMax discovers the Nouveau hwmon interface at startup and reads the GPU thermal thresholds once.

On the current target hardware these are:

```text
temp1_max:        95 C
temp1_max_hyst:    3 C
temp1_crit:      105 C
temp1_emergency: 135 C
```

The governor uses Nouveau's configured maximum temperature and hysteresis values rather than hard-coding them.

If the GPU reaches `temp1_max`, BlueMax forces the LOW pstate.

Normal automatic operation resumes only after the GPU temperature falls below the recovery threshold.

GPU temperature is sampled approximately once per second.

If temperature telemetry remains unavailable for three seconds, BlueMax forces the LOW pstate. Normal operation resumes when valid telemetry returns, subject to the maximum-temperature limit and hysteresis.

BlueMax does not modify Nouveau's thermal thresholds or attempt to replace the driver's existing thermal-management functionality.

## Governor Policy Module

`governor_policy.c/.h` implements the governor decisions as a pure, deterministic state machine.

Each policy step receives explicit activity, temperature-observation, and monotonic-time inputs. It updates the rolling histories and safety state, then returns the recommended pstate and flags describing meaningful events such as workload upshifts, idle downshifts, thermal limiting, and telemetry faults or recoveries.

The policy module performs no file access, MMIO access, clock reads, sleeping, logging, or direct pstate writes. This keeps it independently testable and allows a future console mode to display meaningful decisions without reporting every 10 ms sample.

## MMIO Access

BlueMax uses the Linux PCI sysfs resource interface:

```text
/sys/bus/pci/devices/0000:01:00.0/resource0
```

BAR0 is:

```text
16 MiB
```

and is mapped once at startup using a read-only mapping.

The daemon:

* opens BAR0 with `O_RDONLY`;
* maps it with `PROT_READ`;
* performs only 32-bit reads from the four known status registers;
* performs no MMIO writes.

The BAR mapping consumes virtual address space but does not allocate a corresponding 16 MiB RAM buffer.

## Pstate Control

Pstate changes are made through Nouveau's debugfs interface:

```text
/sys/kernel/debug/dri/0000:01:00.0/pstate
```

The pstate file is opened only when a transition is required and closed immediately afterward.

BlueMax does not continuously poll the pstate interface.

## Execution Model

BlueMax is designed as a single-threaded daemon.

The main loop uses:

```text
CLOCK_MONOTONIC
```

with absolute 10 ms sampling deadlines.

Missed sampling deadlines are skipped rather than replayed, because historical GPU activity cannot be reconstructed accurately.

Temperature polling and other slow work piggyback on the main sampling loop rather than requiring additional worker threads or timers.

## Hardware Video Decode Testing

Hardware VDPAU decoding has been confirmed on the target GPU for:

* MPEG-1
* MPEG-2
* H.264
* VC-1
* MPEG-4 Part 2

A 1276x960 60 fps H.264 High-profile test video demonstrated a significant dependency on GPU pstate:

```text
pstate 03 -> approximately 4.3 fps
pstate 0f -> approximately 85-86 fps
```

This is one of the primary reasons BlueMax explicitly monitors the hardware video-decode engines.

## Project Status

BlueMax is currently under development.

Thermal telemetry, read-only GPU activity telemetry, Nouveau pstate control, and the pure governor policy module are implemented and covered by unit tests using synthetic files and inputs. The tests do not access the real GPU, `/sys`, or debugfs.

The daemon loop, runtime hardware integration, signal handling, console output, and systemd service are not yet implemented.

## Scope

The first version is intentionally simple.

It will:

* operate automatically;
* choose between `03` and `0f`;
* monitor graphics and video-decode activity;
* enforce thermal safety;
* run as a `systemd` service.

Possible future work may include:

* use of the `07` intermediate pstate;
* additional telemetry and diagnostics;
* an unprivileged Xfce tray application;
* display of current pstate and GPU temperature;
* optional manual overrides.

These are intentionally outside the scope of the initial implementation.

## Warning

BlueMax performs direct read-only MMIO access to NVIDIA GPU registers while Nouveau is active.

Although the specific status registers used by BlueMax have been experimentally tested on the target hardware, direct MMIO access is inherently low-level and hardware-specific.

This software should currently be considered experimental and specific to the supported GPU/platform combination.

## License

License to be determined.
