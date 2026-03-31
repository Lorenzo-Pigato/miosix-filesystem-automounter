// [!] WARNING: this file must be included from test_automounter.cpp

/// Busy extraction hardware test tuning
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
            break;

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
            break;
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
    // Do not ask the user to remove the card until we know that file I/O
    // is already running.
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
    // If the SDIO path hard-locks below the scheduler, the harness cannot
    // recover locally: the best it can do is detect the loss of progress and
    // fail the scenario on timeout/stall.
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
    test_name(name);
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

/**
 * \brief Verify that hot-removing the SD during active file I/O does not hang or crash the system.
 *
 * \note Manual hardware test. File integrity is not checked; only kernel robustness
 * and eventual worker termination are required.
 */
void hwBusyExtraction()
{
    CHECK_AVAIL_HEAP(EST_THREAD_HEAP_USAGE(AM_BUSY_WORKER_STACK) * 2);

    ensureMountedForBusyTest();

    prepareBusyReadScenario();
    runBusyScenario("[hw] [5.1] busy extraction read loop",
                    BusyWorkerMode::SequentialRead, AM_BUSY_READ_FILE);
    waitForBusyReinsertion();

    prepareBusyWriteScenario(AM_BUSY_WRITE_FILE);
    runBusyScenario("[hw] [5.2] busy extraction sync write loop",
                    BusyWorkerMode::SyncWrite, AM_BUSY_WRITE_FILE);
}
