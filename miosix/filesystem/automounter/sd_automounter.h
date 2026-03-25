/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#pragma once

#include "miosix.h"
#include "config/sd_automounter_config.h"

#ifdef WITH_FILESYSTEM

#include <algorithm>
#include <atomic>
#include "kernel/sync.h"
#include "filesystem/devfs/devfs.h"

namespace miosix
{

    enum class SdAutomounterEdge { None, Inserted, Removed };

    /**
     * \brief Self-contained polling state for the SD automounter debounce logic.
     *
     * N is the number of consecutive stable samples required to accept a state
     * change (compile-time constant, defaults to SD_AUTOMOUNTER_DEBOUNCE_SAMPLES).
     * A static_assert prevents invalid values at compile time.
     */
    template<int requiredStableSamples = SD_AUTOMOUNTER_DEBOUNCE_SAMPLES>
    class SdAutomounterPollingState
    {
        static_assert(requiredStableSamples > 0, "debounce threshold must be positive");
    public:
        /**
         * \brief Build the initial polling state.
         *
         * If the card is already present at boot, the counter starts saturated
         * and the first advance(true) emits an Inserted edge so the automounter
         * can mount an already-inserted card.
         */
        explicit SdAutomounterPollingState(bool present)
        {
            stablePresent = present;
            lastPresent = false;
            samplesCount = present ? requiredStableSamples : 0;
        }

        SdAutomounterEdge advance(bool raw)
        {
            if (raw)
                samplesCount = std::min(samplesCount + 1, requiredStableSamples);
            else
                samplesCount = std::max(samplesCount - 1, 0);

            if (samplesCount >= requiredStableSamples)
                stablePresent = true;
            else if (samplesCount <= 0)
                stablePresent = false;

            if (stablePresent == lastPresent)
                return SdAutomounterEdge::None;

            lastPresent = stablePresent;
            return stablePresent ? SdAutomounterEdge::Inserted
                                : SdAutomounterEdge::Removed;
        }

        bool isPresent() const { return stablePresent; }

    private:
        bool stablePresent;
        bool lastPresent;
        int samplesCount;
    };

    /**
     * \brief Polling-based SD card automounter.
     *
     * This module periodically checks whether an SD card is present, applies a
     * simple debounce filter, and mounts/unmounts `/sd` on insertion/removal.
     *
     * The BSP must provide a CardProbeFunction function that returns true if the card
     * is present.
     */
    class SdAutomounter
    {
    public:
        typedef bool (*CardProbeFunction)();

        /**
         * \return singleton instance
         */
        static SdAutomounter &instance();

        /**
         * Configure. Call once from BSP before enable().
         *
         * \param storage storage device used to mount the filesystem
         * \param detect card detect function (polling); must not be nullptr
         * \param pollMs polling period (ms)
         * \param reinitBeforeMount true to reinitialize the storage device before mounting
         */
        void configure(
                        intrusive_ref_ptr<Device> storage,
                        CardProbeFunction detect,
                        int pollMs = SD_AUTOMOUNTER_POLL_MS,
                        bool reinitBeforeMount = SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_DEFAULT);

        /**
         * Enable/disable at runtime.
         */
        void enable();
        void disable();

        /**
         * \brief Stop the worker thread permanently.
         *
         * This method is intended for shutdown and reboot paths. It prevents
         * further polling activity, requests cooperative thread termination,
         * and waits until the worker exits.
         *
         * The stop operation is idempotent and one-way: after it returns the
         * automounter is no longer running and must not be restarted.
         */
        void stop();

        bool isEnabled() const { return enabled.load(); }

    private:
        SdAutomounter();
        SdAutomounter(const SdAutomounter &);
        SdAutomounter &operator=(const SdAutomounter &);

        /**
         * \brief Thread entry point for the SD automounter worker.
         *
         * Miosix `Thread::create()` expects a static member function,
         * while `run()` is a non-static member function that needs to
         * access instance members.
         *
         * This helper converts the generic thread argument back to an
         * SdAutomounter object and starts its main loop.
         *
         * \param arg pointer to the SdAutomounter instance
         * \return nullptr when the worker thread exits
         */
        static void *threadTrampoline(void *arg);
        
        void run();

        /** \brief This method polls for the presence of the SD card.
         *
         * This is a wrapper around the configured CardProbeFunction and returns its
         * result
         *
         * \return true if the card is present, false otherwise
         */
        bool probeCardPresence() const;

        /**
         * \brief Ensure that the /sd mount point exists.
         *
         * This method resolves the root filesystem and creates the `/sd`
         * directory in it if needed. If the directory already exists, that
         * condition is treated as success.
         *
         * \return true if /sd exists or is created successfully, false otherwise
         */
        bool ensureSdMountpoint();

        bool mountSd();
        void unmountSd();
        bool tryMountFat32(intrusive_ref_ptr<FileBase>& disk);
        bool tryMountLittleFs(intrusive_ref_ptr<FileBase>& disk);

        /**
         * \brief Open the configured storage device.
         *
         * This method opens the underlying block device used for SD mounting
         * and stores the resulting handle in the output parameter.
         *
         * \param disk output reference that receives the opened device handle
         * \return 0 on success, or a negative error code on failure
         */
        int openDisk(intrusive_ref_ptr<FileBase>& disk);

        intrusive_ref_ptr<Device> storage;
        CardProbeFunction detect;
        int pollMs;
        bool reinitBeforeMount;

        std::atomic<bool> enabled;
        std::atomic<bool> configured;

        Mutex mtx;
        ConditionVariable cv;

        Thread *worker;

        SdAutomounterPollingState<> pollingState;
        bool sdMounted;
    };

} // namespace miosix

#endif // WITH_FILESYSTEM
