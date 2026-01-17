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

#ifdef WITH_FILESYSTEM

#include <atomic>
#include "kernel/sync.h"

namespace miosix
{

    /**
     * \brief Polling-based SD card presence monitor (dummy automounter).
     *
     * This module only detects insertion/removal using polling and prints events.
     * It is intentionally filesystem-agnostic in this dummy phase.
     *
     * The BSP must provide a CardDetectFn function that returns true if the card
     * is present. The module applies a simple debounce filter.
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
         * \param detect card detect function (polling). If nullptr -> always present.
         * \param pollMs polling period (ms)
         * \param debounceSamples consecutive equal samples to accept a change
         */
        void configure(CardDetectFn detect, int pollMs = 200, int debounceSamples = 3);

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

        static void *threadTrampoline(void *arg);
        void run();

        bool readPresentRaw() const;
        bool updateDebounced(bool raw);

        void onInserted();
        void onRemoved();

        CardDetectFn detect;
        int pollMs;
        int debounceSamples;

        std::atomic<bool> enabled;
        std::atomic<bool> configured;

        Mutex mtx;
        ConditionVariable cv;

        Thread *worker;

        // debounce state
        bool lastRaw;
        bool stablePresent;
        bool lastPresent;
        int stableCount;
    };

} // namespace miosix

#endif // WITH_FILESYSTEM
