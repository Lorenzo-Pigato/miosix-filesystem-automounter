# Miosix Filesystem Automounter

This repository contains an SD card filesystem automounter developed for the
[Miosix](https://miosix.org) kernel.

The project adds runtime SD hotplug support: when a card is inserted, the
automounter detects it, recovers the SDIO driver state if needed, mounts the
filesystem under `/sd`, and exposes the raw block device as `/dev/sda`. When
the card is removed, it unmounts `/sd` and recovers from busy extraction cases
without requiring a board reset.

The maintained implementation, full technical overview, testsuite notes, and
patch deliverables are available on the
[`automounter`](https://github.com/Lorenzo-Pigato/miosix-filesystem-automounter/tree/automounter)
branch.
