/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "sd_automounter.h"

#ifdef WITH_FILESYSTEM

#include <cstdio>

namespace miosix
{

    SdAutomounter &SdAutomounter::instance()
    {
        static SdAutomounter inst;
        return inst;
    }

    SdAutomounter::SdAutomounter()
        : detect(nullptr),
          pollMs(200),
          debounceSamples(3),
          enabled(false),
          configured(false),
          worker(nullptr),
          lastRaw(false),
          stablePresent(false),
          lastPresent(false),
          stableCount(0)
    {
    }

    void SdAutomounter::configure(CardDetectFn detect, int pollMs, int debounceSamples)
    {
        Lock<Mutex> l(mtx);

        this->detect = detect;
        this->pollMs = pollMs > 0 ? pollMs : 200;
        this->debounceSamples = debounceSamples > 0 ? debounceSamples : 3;

        lastRaw = readPresentRaw();
        stablePresent = lastRaw;
        stableCount = 0;

        if (worker == nullptr)
        {
            worker = Thread::create(threadTrampoline, 2048, 1, this, Thread::JOINABLE);
            if (worker == nullptr)
                return;
        }

        configured.store(true);
        cv.signal();
    }

    void SdAutomounter::enable()
    {
        enabled.store(true);
        Lock<Mutex> l(mtx);
        cv.signal();
    }

    void SdAutomounter::disable()
    {
        enabled.store(false);
        Lock<Mutex> l(mtx);
        cv.signal();
    }

    void *SdAutomounter::threadTrampoline(void *arg)
    {
        reinterpret_cast<SdAutomounter *>(arg)->run();
        return nullptr;
    }

    bool SdAutomounter::readPresentRaw() const
    {
        if (detect == nullptr)
            return true;
        return detect();
    }

    bool SdAutomounter::updateDebounced(bool raw)
    {
        if (raw != lastRaw)
        {
            lastRaw = raw;
            stableCount = 0;
            return stablePresent;
        }

        if (stableCount < debounceSamples)
            stableCount++;
        if (stableCount >= debounceSamples)
            stablePresent = raw;

        return stablePresent;
    }

    void SdAutomounter::onInserted()
    {
        printf("SdAutomounter: card inserted\n");
    }

    void SdAutomounter::onRemoved()
    {
        printf("SdAutomounter: card removed\n");
    }

    void SdAutomounter::run()
    {
        while (true)
        {
            {
                Lock<Mutex> l(mtx);
                while (!configured.load() || !enabled.load())
                {
                    cv.wait(mtx);
                }
            }

            bool raw = readPresentRaw();
            bool present = updateDebounced(raw);

            // Edge detection on the debounced state:
            if (present != lastPresent)
            {
                lastPresent = present;
                if (present)
                    onInserted();
                else
                    onRemoved();
            }

            Thread::sleep(pollMs);
        }
    }

} // namespace miosix

#endif // WITH_FILESYSTEM
