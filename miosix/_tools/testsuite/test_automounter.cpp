/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

// [!] WARNING: this file must be included from testsuite.cpp, do not compile directly


#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "filesystem/automounter/sd_automounter.h"
#include "filesystem/ioctl.h"

#ifdef WITH_FILESYSTEM
#define AM_SENTINEL_DIR  "/sd/automounter_test"
#define AM_SENTINEL      AM_SENTINEL_DIR "/sentinel.txt"

namespace {

// --------------------------- Constants -------------------------------------

const char AM_SENTINEL_CONTENT[] = "miosix automounter sentinel\n";

constexpr unsigned int AM_TIMEOUT_MS  = 5000; ///< general wait timeout
constexpr unsigned int AM_POLL_MS     = 100;  ///< general polling interval
constexpr int          AM_DIR_MODE    = 0755; ///< mkdir permissions
constexpr unsigned int AM_SENTINEL_BUF = 64;  ///< sentinel read buffer size
constexpr int          AM_FILE_MODE   = 0644; ///< regular file permissions

/// Debounce threshold for logic tests
constexpr int AM_LOGIC_DEBOUNCE_N = 3;

// --------------------------- Helpers -------------------------------------

const char *edgeName(SdAutomounterEdge edg)
{
    switch(edg)
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

    // return false upon a stat failure
    if(stat("/", &rootStat) != 0 || stat("/sd", &sdStat) != 0) return false;
    
    // return true if the root and /sd are on different devices.
    // This implies /sd is a mounted filesystem and not just a 
    // directory inside root fs
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
    
    mkdir(AM_SENTINEL_DIR, AM_DIR_MODE); // ignore EEXIST
    
    if(canReadSentinel()) 
        return true;
    
    // otherwise, try creating the sentinel file
    FILE *f = fopen(AM_SENTINEL, "wb");
    if(!f) return false;
    
    size_t expected = strlen(AM_SENTINEL_CONTENT);
    
    // fwrite writes a single byte at a time (1)
    bool ok = (fwrite(AM_SENTINEL_CONTENT, 1, expected, f) == expected && fclose(f) == 0);
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

#include "test_automounter_busy.cpp"

// ------------------------- LOGIC TESTS ---------------------------
// Test the correctness of the debounce state machine
// No hardware needed

template<int requiredStableSamples> 
void checkAdvance(const char *name,
                  SdAutomounterPollingState<requiredStableSamples> state,
                  const bool *samples,
                  const SdAutomounterEdge *expected,
                  unsigned int count,
                  bool expectedStable)
{
    test_name(name);
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

/**
 * \brief Verify that a card already present at boot emits one and only one insertion edge.
 */
void logicTest1()
{
    const bool samps[] = {true, true, true};
    const SdAutomounterEdge exp[] = {
        SdAutomounterEdge::Inserted,
        SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    
    checkAdvance("[logic] [1] boot present",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(true), samps, exp, 3, true);
}

/**
 * \brief Verify that insertion requires enough stable present samples.
 */
void logicTest2()
{
    const bool samps[] = {true, false, true, true, true};
    const SdAutomounterEdge exp[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Inserted};
    
    checkAdvance("[logic] [2] insert debounce",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(false), samps, exp, 5, true);
}

/**
 * \brief Verify that removal requires enough stable absent samples.
 */
void logicTest3()
{
    const bool samps[] = {false, true, false, false, false};
    const SdAutomounterEdge exp[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Removed};
    
    SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N> running =
        stableState<AM_LOGIC_DEBOUNCE_N>(true);
    
    checkAdvance("[logic] [3] remove debounce", running, samps, exp, 5, false);
}

/**
 * \brief Verify that a stable present state does not emit duplicate insertions.
 */
void logicTest4()
{
    const bool samps[] = {true, true, true};
    const SdAutomounterEdge exp[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    
    SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N> running =
        stableState<AM_LOGIC_DEBOUNCE_N>(true);
    
    checkAdvance("[logic] [4] no duplicate edges", running, samps, exp, 3, true);
}

/**
 * \brief Verify that alternating samples below threshold do not emit edges.
 */
void logicTest5()
{
    const bool samps[] = {true, false, true, false, true};
    const SdAutomounterEdge exp[] = {
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    
    checkAdvance("[logic] [5] glitch rejection",
        SdAutomounterPollingState<AM_LOGIC_DEBOUNCE_N>(false), samps, exp, 5, false);
}

// ------------------------- HARDWARE TESTS ------------------------
// Require a real SD card and user interaction


/**
 * \brief Verify that a card inserted before boot is mounted automatically.
 *
 * \note Manual hardware test. The SD card must already be inserted at boot.
 */
void hwBootWithCard()
{
    test_name("[hw] [1] boot with card");
    
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after boot");
    pass();
}

/**
 * \brief Verify that a manually inserted card is mounted automatically.
 *
 * \note Manual hardware test. The board must boot without the SD card.
 */
void hwInsertCard()
{
    test_name("[hw] [2] insert card");
    
    if(isMounted())
        fail("already mounted — reboot without card to run this test");
    
    waitForAck("Insert the SD card now.");
    
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after insertion");
    pass();
}

/**
 * \brief Verify that removing the card unmounts `/sd`.
 *
 * \note Manual hardware test. The sentinel file must become unreadable.
 */
void hwRemoveCard()
{
    test_name("[hw] [3] remove card");
    
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted before removal test");
    
    waitForAck("Remove the SD card now.");
    
    if(!waitMounted(false, AM_TIMEOUT_MS))
        fail("/sd still mounted after removal");
    if(canReadSentinel())
        fail("sentinel still readable after removal");
    pass();
}

/**
 * \brief Verify that reinserting the card mounts `/sd` again.
 *
 * \note Manual hardware test.
 */
void hwReinsertCard()
{
    test_name("[hw] [4] reinsert card");
    waitForAck("Reinsert the SD card now.");
    if(!waitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after reinsertion");
    pass();
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point — outside the anonymous namespace so it matches the forward
// declaration `static void test_automounter()` in testsuite.cpp.
// ---------------------------------------------------------------------------

/**
 * \brief Run SD automounter logic tests and optional hardware tests.
 */
static void test_automounter()
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

#else // WITH_FILESYSTEM

static void test_automounter()
{
    iprintf("Automounter tests skipped, filesystem support is disabled\n");
}

#endif // WITH_FILESYSTEM
