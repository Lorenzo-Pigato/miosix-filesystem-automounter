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

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include "filesystem/fat32/fat32.h"
#include "filesystem/file_access.h"
#include "filesystem/file.h"
#include "filesystem/ioctl.h"
#include "filesystem/stringpart.h"
#include "filesystem/littlefs/lfs_miosix.h"

#if SD_AUTOMOUNTER_DEBUG_LOG
#define SD_AUTO_LOG(fmt, ...) printf("[SdAutomounter] " fmt, ##__VA_ARGS__)
#else
#define SD_AUTO_LOG(fmt, ...) do { } while(0)
#endif

namespace miosix
{

    SdAutomounter &SdAutomounter::instance()
    {
        static SdAutomounter inst;
        return inst;
    }

    SdAutomounter::SdAutomounter()
        : storage(),
          detect(nullptr),
          pollMs(200),
          requiredStableSamples(3),
          enabled(false),
          configured(false),
          worker(nullptr),
          stablePresent(false),
          lastPresent(false),
          samplesCount(0),
          sdMounted(false)
    {
    }

    void SdAutomounter::configure(intrusive_ref_ptr<Device> storage, CardDetectFn detect,
                                  int pollMs, int requiredStableSamples)
    {
        assert(!configured.load());
        Lock<Mutex> l(mtx);

        this->storage = storage;
        this->detect = detect;
        this->pollMs = pollMs > 0 ? pollMs : 200;
        this->requiredStableSamples = requiredStableSamples > 0 ? requiredStableSamples : 3;

        // Init the integration counter so it starts saturated in the current
        // direction, avoiding a spurious transition at the first poll cycle.
        stablePresent = detectPhysicalPresence();
        samplesCount = stablePresent ? this->requiredStableSamples : 0;
        
        // Force a fresh edge evaluation when the worker wakes up. 
        // This allows mounting an already-inserted card at boot.
        lastPresent = false;
        sdMounted = false;

        SD_AUTO_LOG("Configured: poll=%dms debounce=%d initialPresent=%d\n",
                    this->pollMs, 
                    this->requiredStableSamples,
                    static_cast<int>(stablePresent));

        if (worker == nullptr)
        {
            worker = Thread::create(threadTrampoline, 2048, 1, this, Thread::DETACHED);
            if (worker == nullptr)
                return;
            SD_AUTO_LOG("Worker thread created\n");
        }

        // Set configured flag and wake up the worker thread
        configured.store(true);
        cv.signal();
    }

    void SdAutomounter::enable()
    {
        enabled.store(true);
        SD_AUTO_LOG("Enabled\n");
        
        Lock<Mutex> l(mtx);
        cv.signal();
    }

    void SdAutomounter::disable()
    {
        enabled.store(false);
        SD_AUTO_LOG("Disabled\n");
        
        Lock<Mutex> l(mtx);
        cv.signal();
    }
    
    void *SdAutomounter::threadTrampoline(void *automounterInstance)
    {   
        reinterpret_cast<SdAutomounter *>(automounterInstance)->run();
        return nullptr;
    }

    bool SdAutomounter::detectPhysicalPresence() const
    {
        if (detect == nullptr)
            return true;
        return detect();
    }

    bool SdAutomounter::checkStablePresence(bool raw)
    {
        if (raw)
            samplesCount = std::min(samplesCount + 1, requiredStableSamples);
        else
            samplesCount = std::max(samplesCount - 1, 0);

        if (samplesCount >= requiredStableSamples)
            stablePresent = true;
        else if (samplesCount <= 0)
            stablePresent = false;

        return stablePresent;
    }

    bool SdAutomounter::ensureSdMountpoint()
    {
        std::string root("/");
        ResolvedPath resolved = FilesystemManager::instance().resolvePath(root, false);
        if (resolved.result < 0 || !resolved.fs)
        {
            SD_AUTO_LOG("Cannot resolve root filesystem, result=%d\n", resolved.result);
            return false;
        }

        StringPart sd("sd");
        int result = resolved.fs->mkdir(sd, 0755);
        if (result != 0 && result != -EEXIST)
            SD_AUTO_LOG("Cannot create /sd mountpoint, result=%d\n", result);
        return result == 0 || result == -EEXIST;
    }

    int SdAutomounter::openDisk(intrusive_ref_ptr<FileBase>& disk)
    {
        if (!storage)
        {
            SD_AUTO_LOG("No storage device configured\n");
            return -ENODEV;
        }
        int result=storage->open(disk, intrusive_ref_ptr<FilesystemBase>(), O_RDWR, 0);
        if(result<0) SD_AUTO_LOG("Cannot open storage device, result=%d\n", result);
        return result;
    }

    bool SdAutomounter::tryMountFat32(intrusive_ref_ptr<FileBase>& disk)
    {
        #ifdef WITH_FATFS
        SD_AUTO_LOG("Trying FAT32 mount on /sd\n");
        intrusive_ref_ptr<Fat32Fs> fs(new Fat32Fs(disk));
        if (fs->mountFailed())
        {
            SD_AUTO_LOG("FAT32 mount probe failed\n");
            return false;
        }

        int result = FilesystemManager::instance().kmount("/sd", fs);
        if (result == -EBUSY)
            SD_AUTO_LOG("Warning: /sd already mounted (EBUSY)\n");
        else
            SD_AUTO_LOG("FAT32 kmount result=%d\n", result);
        return result == 0 || result == -EBUSY;
        #else
        (void)disk;
        return false;
        #endif
    }

    bool SdAutomounter::tryMountLittleFs(intrusive_ref_ptr<FileBase>& disk)
    {
        #ifdef WITH_LITTLEFS
        SD_AUTO_LOG("Trying LittleFS mount on /sd\n");
        intrusive_ref_ptr<LittleFS> fs(new LittleFS(disk));
        if (fs->mountFailed())
        {
            SD_AUTO_LOG("LittleFS mount probe failed\n");
            return false;
        }

        int result = FilesystemManager::instance().kmount("/sd", fs);
        if (result == -EBUSY)
            SD_AUTO_LOG("Warning: /sd already mounted (EBUSY)\n");
        else
            SD_AUTO_LOG("LittleFS kmount result=%d\n", result);
        return result == 0 || result == -EBUSY;
        #else
        (void)disk;
        return false;
        #endif
    }

    bool SdAutomounter::mountSd()
    {
        if (sdMounted)
        {
            SD_AUTO_LOG("Card already mounted, skipping mount\n");
            return true;
        }
        if (!ensureSdMountpoint())
            return false;
        if (storage)
        {
            // Reinit is needed for hardware CD pin detection mode, where the
            // detect function is a simple GPIO read and does not touch the SDIO
            // driver. For SDIO software probing this is redundant but harmless.
            int reinitResult = storage->ioctl(IOCTL_REINIT, nullptr);
            SD_AUTO_LOG("Storage reinit result=%d\n", reinitResult);
            (void)reinitResult;
        }

        intrusive_ref_ptr<FileBase> disk;
        if (openDisk(disk) < 0)
            return false;

        // Try all enabled filesystems in the same order used at boot.
        if (tryMountFat32(disk))
        {
            sdMounted = true;
            SD_AUTO_LOG("Mounted /sd using FAT32\n");
            return true;
        }
        if (tryMountLittleFs(disk))
        {
            sdMounted = true;
            SD_AUTO_LOG("Mounted /sd using LittleFS\n");
            return true;
        }
        SD_AUTO_LOG("No supported filesystem detected on card\n");
        return false;
    }

    void SdAutomounter::unmountSd()
    {
        // Clearing before the actual umount is safe: mount and unmount only
        // run on the single worker thread, so no insertion edge can race here.
        sdMounted = false;

        FilesystemManager& fsm = FilesystemManager::instance();
        const int retries = 3;
        for (int i = 0; i < retries; i++)
        {
            int result = fsm.umount("/sd", false);
            SD_AUTO_LOG("Graceful umount attempt %d/%d result=%d\n",
                        i+1, retries, result);
            if (result == 0 || result == -EINVAL)
                return;
            if (result != -EBUSY)
                break;
            Thread::sleep(200);
        }

        // Forced umount is a last resort used when handles are still open.
        int forced=fsm.umount("/sd", true);
        SD_AUTO_LOG("Forced umount result=%d\n", forced);
        (void)forced;
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

            bool raw = detectPhysicalPresence();
            bool present = checkStablePresence(raw);

            // Edge detection on the debounced state:
            if (present != lastPresent)
            {
                lastPresent = present;
                if (present)
                {
                    SD_AUTO_LOG("Insertion edge detected\n");
                    if (!mountSd())
                        SD_AUTO_LOG("Mount failed\n");
                }
                else
                {
                    SD_AUTO_LOG("Removal edge detected\n");
                    unmountSd();
                }
            }

            Thread::sleep(pollMs);
        }
    }

} // namespace miosix

#endif // WITH_FILESYSTEM
