# Miosix SD Automounter

This repository contains an SD card filesystem automounter developed for
[Miosix](https://miosix.org), a real-time operating system for 32-bit
microcontrollers.

The work is based on the official Miosix `unstable` branch and adds runtime SD
card hotplug support. The final integration is maintained on the `automounter`
branch, while the original pre-port development history is preserved on
`automounter-legacy` and tagged as `automounter-v1.0`.

## Overview

The project adds a kernel-side SD automounter that detects card insertion and
removal at runtime, mounts the card under `/sd`, unmounts it after extraction,
and exposes the raw SD block device through DevFs as `/dev/sda`.

Before this work, the SD card was effectively expected to be present during
boot. The new design makes the storage stack recover from later insertion,
removal, and reinsertion without requiring a full board reset.

Two detection strategies are supported:

- Hardware card-detect pin, when the board provides a usable CD signal.
- SDIO software probing, for boards or adapters without a reliable CD pin.

The current board integration targets the STM32F407VG Discovery board, but the
automounter logic is kept separate from the BSP so that other boards can reuse
the same runtime code with only board-specific wiring changes.

## Main Components

- `miosix/filesystem/automounter/` contains the `SdAutomounter` runtime,
  debounce logic, mount and unmount flow, probe selection, and DevFs device
  registration helpers.
- `miosix/config/sd_automounter_config.h` contains automounter timing and
  behavior parameters.
- `miosix/arch/drivers/sdmmc/stm32f2_f4_f7_sd.cpp` contains the SDIO driver
  changes needed for hotplug recovery and read-path correctness.
- `miosix/arch/board/stm32f407vg_stm32f4discovery/interfaces-impl/bsp.cpp`
  connects the generic automounter to the board-specific SD driver instance.
- `miosix/config/board/stm32f407vg_stm32f4discovery/board_settings.h` contains
  the board-local CD-pin and timing-pin settings.
- `tools/testsuite/test_automounter.cpp` provides deterministic automounter
  tests and interactive hardware tests.
- `examples/fs_automounter_shell/` provides a small manual validation shell.

## Runtime Behavior

The automounter runs as a dedicated kernel thread. The thread remains inactive
until it is configured and enabled by the BSP, then periodically samples raw
card presence and feeds the result into a debounce state machine.

On stable insertion, the automounter:

- creates the `/sd` mountpoint if needed
- optionally reinitializes the SD driver through `IOCTL_REINIT`
- opens the raw SD block device
- tries FAT32 first
- tries LittleFS if FAT32 is not detected

On stable removal, the automounter first attempts a normal unmount. If the
filesystem remains busy for repeated attempts, it falls back to a forced
unmount. This keeps normal filesystem semantics in the common case while still
recovering when the card is physically removed during active I/O.

The raw device node `/dev/sda` is intentionally separate from `/sd`.
`/dev/sda` represents the block device, while `/sd` represents a mounted
filesystem on top of that device.

## SDIO Driver Work

The automounter required the SDIO driver to support reliable reinitialization
after card removal and reinsertion. The driver now exposes an `IOCTL_REINIT`
path that restores the SD card state and, when requested, recalibrates the bus
speed.

The driver also fixes a read-completion race in the STM32 SDIO path. A DMA
transfer-complete interrupt can arrive before the SDIO peripheral has completed
its final protocol-side checks. The updated driver tracks DMA completion and
SDIO completion separately, and wakes the waiting thread only when the transfer
has reached a valid final state or when an error has been latched.

Clock calibration remains owned by `ClockController`, keeping the final driver
closer to upstream Miosix. The calibration logic is stronger than the original
one: candidate speeds are accepted only if a probe block matches a reference
block read at a conservative low speed. A recursive driver mutex avoids the
deadlock that would otherwise occur when reinitialization calls back into
calibration and calibration performs block reads.

The SDIO initialization path is also split between one-time setup and reset-only
reinitialization. GPIO setup, clock enable, and IRQ registration are performed
once, while hotplug recovery reuses only the peripheral reset and card
initialization sequence.

## Board Integration

The STM32F407VG Discovery BSP performs only board-specific work:

- selects the concrete SD driver instance
- configures the optional card-detect pin
- selects hardware CD or SDIO software probing
- starts the automounter during board initialization

The card-detect configuration uses typed board-local settings for pin,
polarity, and input mode. The coarse `WITH_SD_CD_PIN` macro is still retained
because it allows the whole CD-specific path to be compiled out when unused.

An optional timing GPIO can be enabled from board settings. It produces a pulse
around insertion or removal handling and is useful for oscilloscope-based timing
checks.

## Tests

The automounter testsuite is integrated into `tools/testsuite` and is available
from the testsuite menu through the automounter entry.

The tests cover:

- debounce behavior without hardware dependencies
- boot with card already inserted
- hot insertion
- hot removal
- reinsertion
- busy extraction during active reads
- busy extraction during synchronous writes

The busy extraction tests are the most important stress tests. A worker thread
keeps reading or writing while the card is physically removed. The expected
result is not preservation of the interrupted operation, but a controlled I/O
failure without deadlock, livelock, or a permanently mounted filesystem.

## Manual Example

The example in `examples/fs_automounter_shell/` is a lightweight interactive
environment for manual checks. It can be used to inspect `/sd`, inspect
`/dev/sda`, perform basic file operations, and enable or disable the
automounter during runtime experiments.

The shell is a validation and demonstration tool. It is not part of the core
automounter architecture.

## Build Checks

Useful build commands for the validated board are:

```sh
make -C tools/testsuite -j4 main OPT_BOARD=stm32f407vg_stm32f4discovery
make -C tools/testsuite -j4 image OPT_BOARD=stm32f407vg_stm32f4discovery
make -C templates/simple -j4 main OPT_BOARD=stm32f407vg_stm32f4discovery
make -C examples/fs_automounter_shell -j4 main OPT_BOARD=stm32f407vg_stm32f4discovery
```

The final branch was rebased on the current official Miosix `unstable` branch
available from:

https://miosix.org/git-public/miosix-kernel.git

## Patch Deliverables

Generated patch files are kept under `patches/`:

- `automounter.patch` contains the complete automounter diff.
- `automounter-no-example.patch` excludes the shell example.
- `automounter-no-test.patch` excludes both the shell example and the
  automounter testsuite changes.

These patches are generated as plain diffs against `upstream/unstable`, so they
describe the final file changes rather than the internal development history.

## Miosix References

General information about Miosix is available at:

https://miosix.org

Miosix implements the fluid-kernel model described in:

F. Terraneo and D. Cattaneo, "Fluid Kernels: Seamlessly Conquering the Embedded
Computing Continuum," IEEE Transactions on Computers, vol. 74, no. 12, Dec.
2025. DOI: https://doi.org/10.1109/TC.2025.3605745.

The Miosix scheduler subsystem is related to:

M. Maggio, F. Terraneo, and A. Leva, "Task scheduling: A control-theoretical
viewpoint for a general and flexible solution," ACM Transactions on Embedded
Computing Systems, vol. 13, no. 4, Nov. 2014.
DOI: https://doi.org/10.1145/2560015.
