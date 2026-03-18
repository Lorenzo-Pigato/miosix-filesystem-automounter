/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#pragma once

#ifdef WITH_FILESYSTEM

#include <atomic>
#include "kernel/sync.h"
#include "filesystem/devfs/devfs.h"

namespace miosix
{

    /**
     * \brief Polling-based SD card automounter.
     *
     * This module periodically checks whether an SD card is present, applies a
     * simple debounce filter, and mounts/unmounts /sd on insertion/removal.
     *
     * The BSP must provide a CardDetectFn function that returns true if the card
     * is present.
     */
    class SdAutomounter
    {
    public:
        typedef bool (*CardDetectFn)();

        /**
         * \return singleton instance
         */
        static SdAutomounter &instance();

        /**
         * Configure. Call once from BSP before enable().
         *
         * \param storage storage device used to mount the filesystem
         * \param detect card detect function (polling). If nullptr -> always present.
         * \param pollMs polling period (ms)
         * \param requiredStableSamples consecutive number of stable samples to accept a change
         */
        void configure(
                        intrusive_ref_ptr<Device> storage, 
                        CardDetectFn detect,
                        int pollMs = 200, 
                        int requiredStableSamples = 3);

        /**
         * Enable/disable at runtime.
         */
        void enable();
        void disable();

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

        /**
         * \brief detect if the card is physically present with the
         * provided `detect` function, if any.
         *
         * Boards equipped with a card detection pin should provide a
         * `detect` function that reads it.
         *
         * If no `detect` function is provided, this method assumes
         * that the card is always physically inserted.
         */
        bool detectPhysicalPresence() const;

        /**
         * \brief Integration-based debounce filter.
         *
         * Each sample increments an internal counter towards the sampled direction
         * instead of resetting it on a single glitch.  
         * The debounced state flips only when the counter saturates, providing
         * better noise tolerance than a simpler debounce filter implementation.
         *
         * \param raw current raw card-detect reading
         * \return debounced card-present state
         */
        bool checkStablePresence(bool raw);

        /**
         * \brief Ensure that the /sd mount point exists.
         *
         * This method resolves the root filesystem and creates the "sd"
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
        CardDetectFn detect;
        int pollMs;
        int requiredStableSamples;

        std::atomic<bool> enabled;
        std::atomic<bool> configured;

        Mutex mtx;
        ConditionVariable cv;

        Thread *worker;

        // debounce state
        bool stablePresent;
        bool lastPresent;
        int samplesCount;
        bool sdMounted;
    };

} // namespace miosix

#endif // WITH_FILESYSTEM
