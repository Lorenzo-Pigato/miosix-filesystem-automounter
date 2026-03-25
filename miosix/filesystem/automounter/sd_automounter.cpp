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

#if AUTOMOUNTER_DEBUG_LOG
#define SD_AUTO_LOG(fmt, ...) printf("[SdAutomounter] " fmt, ##__VA_ARGS__)
#else
#define SD_AUTO_LOG(fmt, ...) do { } while(0)
#endif

namespace miosix
{

#if AUTOMOUNTER_DEBUG_LOG
    namespace
    {
        const char *errnoName(int error)
        {
            if (error < 0)
                error = -error;

            switch (error)
            {
                case 0: return "0";
                case EACCES: return "EACCES";
                case EBUSY: return "EBUSY";
                case EEXIST: return "EEXIST";
                case EINVAL: return "EINVAL";
                case ENODEV: return "ENODEV";
                case ENOENT: return "ENOENT";
                case ENOMEM: return "ENOMEM";
                case ENFILE: return "ENFILE";
                case ENOTTY: return "ENOTTY";
                case EROFS: return "EROFS";
                default: return "UNKNOWN";
            }
        }
    }
#endif

    SdAutomounter &SdAutomounter::instance()
    {
        static SdAutomounter inst;
        return inst;
    }

    SdAutomounter::SdAutomounter()
        : storage(),
          detect(nullptr),
          pollMs(SD_AUTOMOUNTER_POLL_MS),
          reinitBeforeMount(SD_AUTOMOUNTER_REINIT_BEFORE_MOUNT_DEFAULT),
          enabled(false),
          configured(false),
          worker(nullptr),
          pollingState(false),
          sdMounted(false)
    {
    }

    void SdAutomounter::configure(intrusive_ref_ptr<Device> storage, CardProbeFunction detect,
                                  int pollMs, bool reinitBeforeMount)
    {
        assert(!configured.load());
        assert(detect != nullptr);

        Lock<Mutex> l(mtx);

        this->storage = storage;
        this->detect = detect;
        this->pollMs = pollMs > 0 ? pollMs : SD_AUTOMOUNTER_POLL_MS;
        this->reinitBeforeMount = reinitBeforeMount;

        // Init the integration counter so it starts saturated in the current
        // direction, avoiding a spurious transition at the first poll cycle.
        pollingState = SdAutomounterPollingState<>(probeCardPresence());
        
        // Force a fresh edge evaluation when the worker wakes up. 
        // This allows mounting an already-inserted card at boot.
        sdMounted = false;

        SD_AUTO_LOG("Configured: poll: %dms\tdebounce: %d\tstate: %s\n",
                    this->pollMs,
                    SD_AUTOMOUNTER_DEBOUNCE_SAMPLES,
                    pollingState.isPresent() ? "present" : "absent");

        if (worker == nullptr)
        {
            worker = Thread::create(threadTrampoline, 2048, 1, this, Thread::JOINABLE);
            if (worker == nullptr)
            {
                SD_AUTO_LOG("Failed to create worker thread\n");
                return;
            }
            SD_AUTO_LOG("Worker thread created\n");
        }

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

    void SdAutomounter::stop()
    {
        Thread *localWorker = worker;
        if (localWorker == nullptr)
            return;

        enabled.store(false);
        SD_AUTO_LOG("Stopping worker thread\n");
        {
            Lock<Mutex> l(mtx);
            cv.signal();
        }

        localWorker->terminate();
        localWorker->join();
        worker = nullptr;
        SD_AUTO_LOG("Worker thread stopped\n");
    }
    
    void *SdAutomounter::threadTrampoline(void *automounterInstance)
    {   
        reinterpret_cast<SdAutomounter *>(automounterInstance)->run();
        return nullptr;
    }

    bool SdAutomounter::probeCardPresence() const
    {
        return detect();
    }

    bool SdAutomounter::ensureSdMountpoint()
    {
        std::string root("/");
        ResolvedPath resolved = FilesystemManager::instance().resolvePath(root, false);
        if (resolved.result < 0 || !resolved.fs)
        {
            SD_AUTO_LOG("Cannot resolve root filesystem (%s)\n", errnoName(resolved.result));
            return false;
        }

        StringPart sd("sd");
        int result = resolved.fs->mkdir(sd, 0755);
        if (result != 0 && result != -EEXIST)
            SD_AUTO_LOG("Cannot create /sd mountpoint (%s)\n", errnoName(result));
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
        if(result<0)
            SD_AUTO_LOG("Cannot open storage device (%s)\n", errnoName(result));
        return result;
    }

    bool SdAutomounter::tryMountFat32(intrusive_ref_ptr<FileBase>& disk)
    {
        #ifdef WITH_FATFS
        intrusive_ref_ptr<Fat32Fs> fs(new Fat32Fs(disk));
        if (fs->mountFailed())
            return false;

        int result = FilesystemManager::instance().kmount("/sd", fs);
        if (result == -EBUSY)
            SD_AUTO_LOG("/sd is already mounted (EBUSY)\n");
        else if (result < 0)
            SD_AUTO_LOG("FAT32 mount failed (%s)\n", errnoName(result));
        return result == 0 || result == -EBUSY;
        #else
        (void)disk;
        return false;
        #endif
    }

    bool SdAutomounter::tryMountLittleFs(intrusive_ref_ptr<FileBase>& disk)
    {
        #ifdef WITH_LITTLEFS
        intrusive_ref_ptr<LittleFS> fs(new LittleFS(disk));
        if (fs->mountFailed())
            return false;

        int result = FilesystemManager::instance().kmount("/sd", fs);
        if (result == -EBUSY)
            SD_AUTO_LOG("/sd is already mounted (EBUSY)\n");
        else if (result < 0)
            SD_AUTO_LOG("LittleFS mount failed (%s)\n", errnoName(result));
        return result == 0 || result == -EBUSY;
        #else
        (void)disk;
        return false;
        #endif
    }

    bool SdAutomounter::mountSd()
    {
        if (Thread::testTerminate())
            return false;
        if (sdMounted)
        {
            SD_AUTO_LOG("Card already mounted, skipping mount\n");
            return true;
        }
        if (!ensureSdMountpoint())
            return false;
        if (Thread::testTerminate())
            return false;
        if (reinitBeforeMount && storage)
        {
            // Before probing a filesystem, bring the card back to a known
            // transfer state and let the driver recalibrate its final bus
            // width/clock. Raw presence probing alone is not enough to
            // guarantee that the subsequent mount sees consistent data.
            int reinitResult = storage->ioctl(IOCTL_REINIT, nullptr);
            if (reinitResult < 0)
                SD_AUTO_LOG("Storage reinit failed (%s)\n",
                            errnoName(reinitResult));
        }
        if (Thread::testTerminate())
            return false;

        intrusive_ref_ptr<FileBase> disk;
        if (openDisk(disk) < 0)
            return false;
        if (Thread::testTerminate())
            return false;

        // Try all enabled filesystems in the same order used at boot.
        if (tryMountFat32(disk))
        {
            sdMounted = true;
            SD_AUTO_LOG("Mounted /sd using FAT32\n");
            return true;
        }
        if (Thread::testTerminate())
            return false;
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
        const int retries = SD_AUTOMOUNTER_UNMOUNT_RETRY_COUNT > 0
            ? SD_AUTOMOUNTER_UNMOUNT_RETRY_COUNT
            : 1;
        for (int i = 0; i < retries; i++)
        {
            int result = fsm.umount("/sd", false);
            if (result == 0 || result == -EINVAL)
            {
                SD_AUTO_LOG("Unmounted /sd\n");
                return;
            }
            if (result != -EBUSY)
            {
                SD_AUTO_LOG("Unmount failed (%s)\n", errnoName(result));
                break;
            }
            SD_AUTO_LOG("Unmount busy, retrying (%d/%d)\n", i + 1, retries);
            Thread::sleep(SD_AUTOMOUNTER_UNMOUNT_RETRY_DELAY_MS);
        }

        // Forced umount is a last resort used when handles are still open.
        int forced=fsm.umount("/sd", true);
        if (forced == 0 || forced == -EINVAL)
            SD_AUTO_LOG("Forced unmount completed\n");
        else
            SD_AUTO_LOG("Forced unmount failed (%s)\n", errnoName(forced));
    }

    void SdAutomounter::run()
    {
        #if AUTOMOUNTER_DEBUG_LOG
        bool lastRawForLog = pollingState.isPresent();
        #endif
        
        while (!Thread::testTerminate())
        {
            {
                Lock<Mutex> l(mtx);
                while (!Thread::testTerminate() &&
                       (!configured.load() || !enabled.load()))
                {
                    cv.wait(mtx);
                }
            }
            if (Thread::testTerminate())
                break;

            bool raw = probeCardPresence();
            
            #if AUTOMOUNTER_DEBUG_LOG
            if (raw != lastRawForLog)
            {
                SD_AUTO_LOG("Physical detect changed: %s\n",
                            raw ? "present" : "absent");
                lastRawForLog = raw;
            }
            #endif
            
            SdAutomounterEdge edge = pollingState.advance(raw);

            // Edge detection on the debounced state:
            if (edge != SdAutomounterEdge::None)
            {
                if (edge == SdAutomounterEdge::Inserted)
                {
                    SD_AUTO_LOG("Debounced insertion detected\n");
                    //Retry mount a few times. Bus-level CRC errors can
                    //cause intermittent read failures, but retrying
                    //usually succeeds.
                    const int retries = SD_AUTOMOUNTER_MOUNT_RETRY_COUNT > 0
                        ? SD_AUTOMOUNTER_MOUNT_RETRY_COUNT
                        : 1;
                    for (int attempt = 0; attempt < retries; attempt++)
                    {
                        if (mountSd()) break;
                        SD_AUTO_LOG("Mount attempt %d/%d failed, retrying\n",
                                    attempt + 1, retries);
                        if (Thread::testTerminate()) break;
                        Thread::sleep(SD_AUTOMOUNTER_MOUNT_RETRY_DELAY_MS);
                    }
                }
                else
                {
                    SD_AUTO_LOG("Debounced removal detected\n");
                    unmountSd();
                }
            }

            if (Thread::testTerminate())
                break;
            Thread::sleep(pollMs);
        }
    }

} // namespace miosix

#endif // WITH_FILESYSTEM
