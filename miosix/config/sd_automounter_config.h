/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#pragma once

/**
 * \file sd_automounter_config.h
 * \brief Build-time policy settings for the SD automounter.
 *
 * Hardware-specific card-detect wiring stays in the board's
 * board_settings.h. This header only collects behavior and tuning options
 * that are shared by the automounter implementation and the BSP.
 */

/// Enable automounter debug logs.
#ifndef AUTOMOUNTER_DEBUG_LOG
#define AUTOMOUNTER_DEBUG_LOG 0
#endif

/// Polling period for card detection, in milliseconds.
#ifndef SD_AUTOMOUNTER_POLL_MS
#define SD_AUTOMOUNTER_POLL_MS 200
#endif

/// Number of consecutive stable samples required to accept a state change.
#ifndef SD_AUTOMOUNTER_DEBOUNCE_SAMPLES
#define SD_AUTOMOUNTER_DEBOUNCE_SAMPLES 3
#endif

/// Default configure() value for reinitBeforeMount when no explicit value is passed.
#ifndef SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_DEFAULT
#define SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_DEFAULT 0
#endif

/// Reinitialize storage before mount when using a hardware card-detect pin.
#ifndef SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_WITH_CD
#define SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_WITH_CD 1
#endif

/// Reinitialize storage before mount when using SDIO software probing.
#ifndef SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_WITH_SDIO_PROBE
#define SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_WITH_SDIO_PROBE 0
#endif

/// Number of mount attempts after an insertion edge.
#ifndef SD_AUTOMOUNTER_MOUNT_RETRY_COUNT
#define SD_AUTOMOUNTER_MOUNT_RETRY_COUNT 3
#endif

/// Delay between mount retries, in milliseconds.
#ifndef SD_AUTOMOUNTER_MOUNT_RETRY_DELAY_MS
#define SD_AUTOMOUNTER_MOUNT_RETRY_DELAY_MS 500
#endif

/// Number of graceful unmount attempts before forcing the unmount.
#ifndef SD_AUTOMOUNTER_UNMOUNT_RETRY_COUNT
#define SD_AUTOMOUNTER_UNMOUNT_RETRY_COUNT 3
#endif

/// Delay between graceful unmount retries, in milliseconds.
#ifndef SD_AUTOMOUNTER_UNMOUNT_RETRY_DELAY_MS
#define SD_AUTOMOUNTER_UNMOUNT_RETRY_DELAY_MS 200
#endif

/// Number of polling cycles to wait before retrying SDIO reinit after a failed probe.
#ifndef SD_AUTOMOUNTER_SDIO_REINIT_BACKOFF_POLLS
#define SD_AUTOMOUNTER_SDIO_REINIT_BACKOFF_POLLS 7
#endif
