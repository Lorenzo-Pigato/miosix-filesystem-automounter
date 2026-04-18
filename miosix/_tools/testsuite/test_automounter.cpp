/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "test_automounter.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miosix.h"
#include "config/miosix_settings.h"
#include "interfaces/poweroff.h"
#include "filesystem/automounter/sd_automounter.h"

using namespace miosix;

#ifdef WITH_FILESYSTEM
#define AM_SENTINEL_DIR  "/sd/automounter_test"
#define AM_SENTINEL      AM_SENTINEL_DIR "/sentinel.txt"

namespace {

const char AM_SENTINEL_CONTENT[] = "miosix automounter sentinel\n";

constexpr unsigned int AM_TIMEOUT_MS   = 5000;
constexpr unsigned int AM_POLL_MS      = 100;
constexpr int          AM_DIR_MODE     = 0755;
constexpr unsigned int AM_SENTINEL_BUF = 64;
constexpr int          AM_FILE_MODE    = 0644;
constexpr int          AM_LOGIC_DEBOUNCE_N = 3;

constexpr unsigned int AM_BUSY_SCENARIO_TIMEOUT_MS = 15000;
constexpr unsigned int AM_BUSY_STALL_TIMEOUT_MS    = 2000;
constexpr unsigned int AM_BUSY_POLL_MS             = 50;
constexpr unsigned int AM_BUSY_WARMUP_TIMEOUT_MS   = 3000;
constexpr unsigned int AM_BUSY_WARMUP_PROGRESS     = 2;
constexpr unsigned int AM_BUSY_IO_CHUNK            = 512;
constexpr unsigned int AM_BUSY_PRECREATE_SIZE      = 64 * 1024;
constexpr unsigned int AM_BUSY_WORKER_STACK        = 2048;

const char AM_BUSY_READ_FILE[]  = AM_SENTINEL_DIR "/busy_read.bin";
const char AM_BUSY_WRITE_FILE[] = AM_SENTINEL_DIR "/busy_write.bin";

constexpr unsigned int estThreadHeapUsage(unsigned int stack)
{
    return (((16 + 32 + stack + sizeof(Thread)) + 8) + 16);
}

bool checkAvailHeap(unsigned int minimum)
{
    unsigned int free = MemoryProfiling::getCurrentFreeHeap();
    if(free < minimum)
    {
        iprintf("Skipping, low heap (%u<%u).\n", free, minimum);
        return false;
    }
    return true;
}

void testName(const char *name)
{
    iprintf("Testing %s... ", name);
    fflush(stdout);
}

void pass()
{
    iprintf("Ok.\n");
}

[[noreturn]] void fail(const char *cause)
{
    // Avoid stack-heavy printing here because some test threads are small.
    write(STDOUT_FILENO, "Failed:\n", 8);
    write(STDOUT_FILENO, cause, strlen(cause));
    write(STDOUT_FILENO, "\n", 1);
    reboot();
    for(;;) ;
}

const char *edgeName(SdAutomounterEdge edge)
{
    switch(edge)
    {
        case SdAutomounterEdge::None:     return "none";
        case SdAutomounterEdge::Inserted: return "inserted";
        case SdAutomounterEdge::Removed:  return "removed";
    }
    return "unknown";
}

bool askYesNo(const char *prompt)
{
    iprintf("%s [y/N] ", prompt);
    for(;;)
    {
        int c = getchar();
        if(c == '\n') continue;
        return c == 'y' || c == 'Y';
    }
}

void waitForAck(const char *prompt)
{
    iprintf("%s Type 'y' when done. ", prompt);
    for(;;)
    {
        int c = getchar();
        if(c == '\n') continue;
        if(c == 'y' || c == 'Y') return;
    }
}

bool isMounted()
{
    struct stat rootStat, sdStat;
    if(stat("/", &rootStat) != 0 || stat("/sd", &sdStat) != 0) return false;
    return rootStat.st_dev != sdStat.st_dev;
}

bool canReadSentinel()
{
    if(!isMounted()) return false;

    FILE *f = fopen(AM_SENTINEL, "rb");
    if(!f) return false;

    char buf[AM_SENTINEL_BUF];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strcmp(buf, AM_SENTINEL_CONTENT) == 0;
}

bool ensureSentinel()
{
    if(!isMounted()) return false;

    mkdir(AM_SENTINEL_DIR, AM_DIR_MODE);
    if(canReadSentinel()) return true;

    FILE *f = fopen(AM_SENTINEL, "wb");
    if(!f) return false;

    size_t expected = strlen(AM_SENTINEL_CONTENT);
    bool ok = fwrite(AM_SENTINEL_CONTENT, 1, expected, f) == expected;
    ok = ok && fclose(f) == 0;
    return ok;
}

bool waitMounted(bool expected, unsigned int timeoutMs)
{
    for(unsigned int t = 0; t <= timeoutMs; t += AM_POLL_MS)
    {
        if(isMounted() == expected) return true;
        Thread::sleep(AM_POLL_MS);
    }
    return false;
}

bool waitSentinel(unsigned int timeoutMs)
{
    for(unsigned int t = 0; t <= timeoutMs; t += AM_POLL_MS)
    {
        if(isMounted() && ensureSentinel() && canReadSentinel())
            return true;
        Thread::sleep(AM_POLL_MS);
    }
    return false;
}

template<int requiredStableSamples>
void checkAdvance(const char *name,
                  SdAutomounterPollingState<requiredStableSamples> state,
                  const bool *samples,
                  const SdAutomounterEdge *expected,
                  unsigned int count,
                  bool expectedStable)
{
    testName(name);
    for(unsigned int i = 0; i < count; i++)
    {
        SdAutomounterEdge got = state.advance(samples[i]);
        if(got != expected[i])
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "sample %u: expected %s, got %s",
                     i, edgeName(expected[i]), edgeName(got));
            fail(buf);
        }
    }
    if(state.isPresent() != expectedStable)
        fail("final stablePresent mismatch");
    pass();
}

template<int requiredStableSamples>
SdAutomounterPollingState<requiredStableSamples> stableState(bool present)
{
    SdAutomounterPollingState<requiredStableSamples> state(present);
    if(present) state.advance(true);
    return state;
}

enum class BusyWorkerMode
{
    SequentialRead,
    SyncWrite
};

struct BusyWorkerState
{
    std::atomic<unsigned int> progressCounter;
    std::atomic<long long> lastProgressNs;
    std::atomic<bool> finished;
    std::atomic<bool> sawIoError;
    std::atomic<int> lastError;

    BusyWorkerState()
        : progressCounter(0), lastProgressNs(0), finished(false),
          sawIoError(false), lastError(0) {}
};

struct BusyWorkerContext
{
    BusyWorkerState *state;
    BusyWorkerMode mode;
    const char *path;
};

bool writeFull(int fd, const void *buf, size_t count)
{
    const unsigned char *ptr = reinterpret_cast<const unsigned char*>(buf);
    while(count > 0)
    {
        ssize_t written = write(fd, ptr, count);
        if(written < 0) return false;
        if(written == 0)
        {
            errno = EIO;
            return false;
        }
        ptr += written;
        count -= written;
    }
    return true;
}

bool preparePatternFile(const char *path, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, AM_FILE_MODE);
    if(fd < 0) return false;

    unsigned char buffer[AM_BUSY_IO_CHUNK];
    for(unsigned int i = 0; i < sizeof(buffer); i++)
        buffer[i] = static_cast<unsigned char>(i & 0xff);

    size_t remaining = size;
    while(remaining > 0)
    {
        size_t chunk = std::min<size_t>(sizeof(buffer), remaining);
        if(!writeFull(fd, buffer, chunk))
        {
            close(fd);
            return false;
        }
        remaining -= chunk;
    }

    return close(fd) == 0;
}

void bestEffortRemove(const char *path)
{
    if(unlink(path) < 0 && errno != ENOENT)
        iprintf("Warning: could not remove %s (%d)\n", path, errno);
}

void ensureMountedForBusyTest()
{
    if(waitSentinel(AM_TIMEOUT_MS)) return;
    fail("busy extraction: card not mounted before scenario");
}

void waitForBusyReinsertion()
{
    waitForAck("Reinsert the SD card now.");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("busy extraction: card did not remount after reinsertion");
}

void busySetProgress(BusyWorkerState& state)
{
    state.progressCounter.fetch_add(1);
    state.lastProgressNs.store(getTime());
}

void busySetIoError(BusyWorkerState& state, int err)
{
    state.lastError.store(err > 0 ? err : EIO);
    state.sawIoError.store(true);
    state.finished.store(true);
}

void *busyIoWorker(void *arg)
{
    BusyWorkerContext *ctx = reinterpret_cast<BusyWorkerContext*>(arg);
    BusyWorkerState& state = *ctx->state;

    unsigned char buffer[AM_BUSY_IO_CHUNK];
    for(unsigned int i = 0; i < sizeof(buffer); i++)
        buffer[i] = static_cast<unsigned char>(0x30 + (i % 40));

    state.lastProgressNs.store(getTime());

    int fd = -1;
    size_t bytesWrittenSinceReset = 0;

    switch(ctx->mode)
    {
        case BusyWorkerMode::SequentialRead:
            fd = open(ctx->path, O_RDONLY, 0);
            if(fd < 0)
            {
                busySetIoError(state, errno);
                return nullptr;
            }
            // Keep reading the same file forever. Card removal should turn
            // this loop into a normal I/O error, not a crash or hang.
            for(;;)
            {
                ssize_t n = read(fd, buffer, sizeof(buffer));
                if(n > 0)
                {
                    busySetProgress(state);
                    continue;
                }
                if(n == 0)
                {
                    if(lseek(fd, 0, SEEK_SET) < 0)
                    {
                        int err = errno;
                        close(fd);
                        busySetIoError(state, err);
                        return nullptr;
                    }
                    continue;
                }
                {
                    int err = errno;
                    close(fd);
                    busySetIoError(state, err);
                    return nullptr;
                }
            }

        case BusyWorkerMode::SyncWrite:
            fd = open(ctx->path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, AM_FILE_MODE);
            if(fd < 0)
            {
                busySetIoError(state, errno);
                return nullptr;
            }
            // Write synchronously so storage is really busy when the user
            // pulls the card. Truncate periodically to keep the file bounded.
            for(;;)
            {
                if(!writeFull(fd, buffer, sizeof(buffer)))
                {
                    int err = errno;
                    close(fd);
                    busySetIoError(state, err);
                    return nullptr;
                }
                busySetProgress(state);
                bytesWrittenSinceReset += sizeof(buffer);
                if(bytesWrittenSinceReset >= AM_BUSY_PRECREATE_SIZE)
                {
                    close(fd);
                    fd = open(ctx->path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, AM_FILE_MODE);
                    if(fd < 0)
                    {
                        busySetIoError(state, errno);
                        return nullptr;
                    }
                    bytesWrittenSinceReset = 0;
                }
            }
    }

    busySetIoError(state, EINVAL);
    return nullptr;
}

void busyFail(const char *scenario, const char *reason, int err = 0)
{
    char buf[192];
    if(err > 0)
        snprintf(buf, sizeof(buf), "%s: %s (errno=%d)", scenario, reason, err);
    else
        snprintf(buf, sizeof(buf), "%s: %s", scenario, reason);
    fail(buf);
}

void busyCheckWarmupState(const char *scenario, const BusyWorkerState& state)
{
    if(state.sawIoError.load())
        busyFail(scenario, "worker reported I/O error before removal", state.lastError.load());
}

void waitBusyWarmup(const char *scenario, BusyWorkerState& state)
{
    // Do not ask the user to remove the card until file I/O is already running.
    const long long deadline = getTime() + AM_BUSY_WARMUP_TIMEOUT_MS * 1000000LL;
    while(getTime() < deadline)
    {
        busyCheckWarmupState(scenario, state);

        if(state.finished.load())
            busyFail(scenario, "worker finished before removal");

        if(state.progressCounter.load() >= AM_BUSY_WARMUP_PROGRESS)
            return;

        const unsigned int progress = state.progressCounter.load();
        const long long lastNs = state.lastProgressNs.load();
        if(progress > 0 && lastNs != 0 &&
           getTime() - lastNs > AM_BUSY_STALL_TIMEOUT_MS * 1000000LL)
        {
            busyFail(scenario, "worker stalled before removal");
        }

        Thread::sleep(AM_BUSY_POLL_MS);
    }
    busyFail(scenario, "worker did not reach warmup state");
}

void waitBusyCompletionAfterRemoval(const char *scenario, BusyWorkerState& state)
{
    // If the SDIO path hard-locks below the scheduler, the harness can only
    // detect the stall and fail on timeout.
    // After removal, an I/O error is fine. A stuck worker is not.
    const long long deadline = getTime() + AM_BUSY_SCENARIO_TIMEOUT_MS * 1000000LL;
    while(getTime() < deadline)
    {
        if(state.finished.load()) return;

        const long long lastNs = state.lastProgressNs.load();
        if(lastNs != 0 &&
           getTime() - lastNs > AM_BUSY_STALL_TIMEOUT_MS * 1000000LL)
        {
            busyFail(scenario, "worker stalled after removal");
        }
        Thread::sleep(AM_BUSY_POLL_MS);
    }
    busyFail(scenario, "worker did not finish after removal");
}

void joinBusyWorker(const char *scenario, Thread *thread)
{
    if(thread == nullptr || thread->join() == false)
        busyFail(scenario, "could not join worker thread");
}

void runBusyScenario(const char *name, BusyWorkerMode mode, const char *path)
{
    testName(name);
    ensureMountedForBusyTest();

    BusyWorkerState state;
    BusyWorkerContext context = {&state, mode, path};
    Thread *thread = Thread::create(busyIoWorker, AM_BUSY_WORKER_STACK,
                                    DEFAULT_PRIORITY, &context, Thread::JOINABLE);
    if(thread == nullptr)
        busyFail(name, "could not create worker thread");

    waitBusyWarmup(name, state);
    waitForAck("Remove the SD card now.");
    waitBusyCompletionAfterRemoval(name, state);
    joinBusyWorker(name, thread);
    pass();
}

void prepareBusyReadScenario()
{
    if(!preparePatternFile(AM_BUSY_READ_FILE, AM_BUSY_PRECREATE_SIZE))
        fail("busy extraction: could not prepare sequential read file");
}

void prepareBusyWriteScenario(const char *path)
{
    bestEffortRemove(path);
}

void logicTest1()
{
    const bool samples[] = {true, true, true};
    const SdAutomounterEdge expected[] = {
        SdAutomounterEdge::Inserted,
        SdAutomounterEdge::None,
        SdAutomounterEdge::None};

    checkAdvance("[logic] [1] boot present",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(true),
        samples, expected, 3, true);
}

void logicTest2()
{
    const bool samples[] = {true, false, true, true, true};
    const SdAutomounterEdge expected[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Inserted};

    checkAdvance("[logic] [2] insert debounce",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(false),
        samples, expected, 5, true);
}

void logicTest3()
{
    const bool samples[] = {false, true, false, false, false};
    const SdAutomounterEdge expected[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Removed};

    SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N> running =
        stableState<AM_LOGIC_DEBOUNCE_N>(true);

    checkAdvance("[logic] [3] remove debounce",
        running, samples, expected, 5, false);
}

void logicTest4()
{
    const bool samples[] = {true, true, true};
    const SdAutomounterEdge expected[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};

    SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N> running =
        stableState<AM_LOGIC_DEBOUNCE_N>(true);

    checkAdvance("[logic] [4] no duplicate edges",
        running, samples, expected, 3, true);
}

void logicTest5()
{
    const bool samples[] = {true, false, true, false, true};
    const SdAutomounterEdge expected[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};

    checkAdvance("[logic] [5] glitch rejection",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(false),
        samples, expected, 5, false);
}

void hwBootWithCard()
{
    testName("[hw] [1] boot with card");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after boot");
    pass();
}

void hwInsertCard()
{
    testName("[hw] [2] insert card");
    if(isMounted())
        fail("already mounted — reboot without card to run this test");

    waitForAck("Insert the SD card now.");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after insertion");
    pass();
}

void hwRemoveCard()
{
    testName("[hw] [3] remove card");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted before removal test");

    waitForAck("Remove the SD card now.");
    if(!waitMounted(false, AM_TIMEOUT_MS))
        fail("/sd still mounted after removal");
    if(canReadSentinel())
        fail("sentinel still readable after removal");
    pass();
}

void hwReinsertCard()
{
    testName("[hw] [4] reinsert card");
    waitForAck("Reinsert the SD card now.");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after reinsertion");
    pass();
}

void hwBusyExtraction()
{
    if(!checkAvailHeap(estThreadHeapUsage(AM_BUSY_WORKER_STACK) * 2))
        return;

    ensureMountedForBusyTest();

    prepareBusyReadScenario();
    runBusyScenario("[hw] [5.1] busy extraction read loop",
                    BusyWorkerMode::SequentialRead, AM_BUSY_READ_FILE);
    waitForBusyReinsertion();

    prepareBusyWriteScenario(AM_BUSY_WRITE_FILE);
    runBusyScenario("[hw] [5.2] busy extraction sync write loop",
                    BusyWorkerMode::SyncWrite, AM_BUSY_WRITE_FILE);
}

} // namespace

void test_automounter()
{
    logicTest1();
    logicTest2();
    logicTest3();
    logicTest4();
    logicTest5();

    #ifndef WITH_AUTOMOUNTER
    iprintf("Automounter hardware tests skipped, WITH_AUTOMOUNTER is disabled\n");
    return;
    #else
    if(!askYesNo("Run interactive hardware automounter tests now?"))
    {
        iprintf("Interactive hardware automounter tests skipped by user\n");
        return;
    }

    if(askYesNo("Was the board booted with the SD card already inserted?"))
        hwBootWithCard();
    else
        hwInsertCard();

    if(!isMounted())
    {
        waitForAck("Insert the SD card to continue.");
        if(!waitSentinel(AM_TIMEOUT_MS))
            fail("could not mount card for removal/reinsert tests");
    }

    hwRemoveCard();
    hwReinsertCard();

    if(askYesNo("Run busy extraction hardware test now?"))
        hwBusyExtraction();
    else
        iprintf("Busy extraction hardware test skipped by user\n");
    #endif
}

#else

void test_automounter()
{
    iprintf("Automounter tests skipped, filesystem support is disabled\n");
}

#endif
