/***************************************************************************
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

// WARNING: this file must be included from testsuite.cpp

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "filesystem/automounter/sd_automounter.h"

#ifdef WITH_FILESYSTEM

using namespace miosix;

static const char AM_SENTINEL[]="/sd/automounter_test/sentinel.txt";
static const char AM_SENTINEL_CONTENT[]="miosix automounter sentinel\n";
static const unsigned int AM_TIMEOUT_MS=5000;
static const unsigned int AM_POLL_MS=100;

static const char *edgeName(SdAutomounterEdge e)
{
    switch(e)
    {
        case SdAutomounterEdge::None:     return "none";
        case SdAutomounterEdge::Inserted: return "inserted";
        case SdAutomounterEdge::Removed:  return "removed";
    }
    return "unknown";
}

static bool automounterAskYesNo(const char *prompt)
{
    iprintf("%s [y/N]\t",prompt);
    for(;;)
    {
        int c=getchar();
        if(c=='\n') continue;
        return c=='y' || c=='Y';
    }
}

static void automounterWaitForAck(const char *prompt)
{
    iprintf("%s Type 'y' when ready.\t",prompt);
    for(;;)
    {
        int c=getchar();
        if(c=='\n') continue;
        if(c=='y' || c=='Y') return;
    }
}

static bool automounterIsMounted()
{
    struct stat rootSt, sdSt;
    if(stat("/",&rootSt)!=0 || stat("/sd",&sdSt)!=0) return false;
    return rootSt.st_dev!=sdSt.st_dev;
}

static bool automounterCanReadSentinel()
{
    if(!automounterIsMounted()) return false;
    
    FILE *f=fopen(AM_SENTINEL,"rb");
    if(!f) return false;
    char buf[64];
    size_t n=fread(buf,1,sizeof(buf)-1,f);
    buf[n]='\0';
    fclose(f);
    return strcmp(buf,AM_SENTINEL_CONTENT)==0;
}

static bool automounterEnsureSentinel()
{
    if(!automounterIsMounted()) return false;
    mkdir("/sd/automounter_test",0755); // ignore EEXIST
    if(automounterCanReadSentinel()) return true;
    FILE *f=fopen(AM_SENTINEL,"wb");
    if(!f) return false;
    size_t expected=strlen(AM_SENTINEL_CONTENT);
    bool ok=(fwrite(AM_SENTINEL_CONTENT,1,expected,f)==expected && fclose(f)==0);
    return ok;
}

static bool automounterWaitMounted(bool expected, unsigned int timeoutMs)
{
    for(unsigned int t=0; t<=timeoutMs; t+=AM_POLL_MS)
    {
        if(automounterIsMounted()==expected) return true;
        Thread::sleep(AM_POLL_MS);
    }
    return false;
}

static bool automounterWaitSentinel(unsigned int timeoutMs)
{
    for(unsigned int t=0; t<=timeoutMs; t+=AM_POLL_MS)
    {
        if(automounterIsMounted() && automounterEnsureSentinel()
           && automounterCanReadSentinel())
            return true;
        Thread::sleep(AM_POLL_MS);
    }
    return false;
}

//
// Logic tests — exercise the debounce state machine without hardware
//

template<int N>
static void automounterCheckAdvance(const char *name,
                                    SdAutomounterPollingState<N> state,
                                    const bool *samples,
                                    const SdAutomounterEdge *expected,
                                    unsigned int count,
                                    bool expectedStable)
{
    test_name(name);
    for(unsigned int i=0; i<count; i++)
    {
        SdAutomounterEdge got=state.advance(samples[i]);
        if(got!=expected[i])
        {
            char buf[128];
            snprintf(buf,sizeof(buf),"sample %u: expected %s, got %s",
                     i,edgeName(expected[i]),edgeName(got));
            fail(buf);
        }
    }
    if(state.isPresent()!=expectedStable)
        fail("final stablePresent mismatch");
    pass();
}

template<int N>
static SdAutomounterPollingState<N> automounterStableState(bool present)
{
    SdAutomounterPollingState<N> state(present);
    if(present) state.advance(true);
    return state;
}

static void automounterLogicTest1()
{
    // Boot with card already present: first advance triggers Inserted
    const bool sam[]={true,true,true};
    const SdAutomounterEdge exp[]={
        SdAutomounterEdge::Inserted,
        SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    automounterCheckAdvance("logic_1 boot present",
        SdAutomounterPollingState<3>(true),sam,exp,3,true);
}

static void automounterLogicTest2()
{
    // Insert debounce: glitch resets, then 3 stable samples trigger Inserted
    const bool sam[]={true,false,true,true,true};
    const SdAutomounterEdge exp[]={
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Inserted};
    automounterCheckAdvance("logic_2 insert debounce",
        SdAutomounterPollingState<3>(false),sam,exp,5,true);
}

static void automounterLogicTest3()
{
    // Remove debounce: glitch resets, then 3 stable absent samples trigger Removed
    const bool sam[]={false,true,false,false,false};
    const SdAutomounterEdge exp[]={
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::Removed};
    SdAutomounterPollingState<3> running=
        automounterStableState<3>(true);
    automounterCheckAdvance("logic_3 remove debounce",
        running,sam,exp,5,false);
}

static void automounterLogicTest4()
{
    // Already stable present: no duplicate Inserted edge
    const bool sam[]={true,true,true};
    const SdAutomounterEdge exp[]={
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    SdAutomounterPollingState<3> running=
        automounterStableState<3>(true);
    automounterCheckAdvance("logic_4 no duplicate edges",
        running,sam,exp,3,true);
}

static void automounterLogicTest5()
{
    // Alternating true/false never reaches threshold → no edge
    const bool sam[]={true,false,true,false,true};
    const SdAutomounterEdge exp[]={
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None, SdAutomounterEdge::None,
        SdAutomounterEdge::None};
    automounterCheckAdvance("logic_5 glitch rejection",
        SdAutomounterPollingState<3>(false),sam,exp,5,false);
}

//
// Hardware tests — require a real SD card
//

static void automounterHwBootWithCard()
{
    test_name("hw_1 boot with card");
    if(!automounterWaitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after boot");
    pass();
}

static void automounterHwInsertCard()
{
    test_name("hw_2 insert card");
    if(automounterIsMounted())
        fail("already mounted — reboot without card to run this test");
    automounterWaitForAck("Insert the SD card now.");
    if(!automounterWaitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after insertion");
    pass();
}

static void automounterHwRemoveCard()
{
    test_name("hw_3 remove card");
    if(!automounterWaitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted before removal test");
    automounterWaitForAck("Remove the SD card now.");
    if(!automounterWaitMounted(false,AM_TIMEOUT_MS))
        fail("/sd still mounted after removal");
    if(automounterCanReadSentinel())
        fail("sentinel still readable after removal");
    pass();
}

static void automounterHwReinsertCard()
{
    test_name("hw_4 reinsert card");
    automounterWaitForAck("Reinsert the SD card now.");
    if(!automounterWaitSentinel(AM_TIMEOUT_MS))
        fail("card not mounted after reinsertion");
    pass();
}

static void test_automounter()
{
    // Pure logic tests (no hardware needed)
    automounterLogicTest1();
    automounterLogicTest2();
    automounterLogicTest3();
    automounterLogicTest4();
    automounterLogicTest5();

    #ifndef WITH_AUTOMOUNTER
    iprintf("Automounter hardware tests skipped, WITH_AUTOMOUNTER is disabled\n");
    return;
    #else
    if(!automounterAskYesNo("Run interactive hardware automounter tests now?"))
    {
        iprintf("Interactive hardware automounter tests skipped by user\n");
        return;
    }

    if(automounterAskYesNo("Was the board booted with the SD card already inserted?"))
        automounterHwBootWithCard();
    else
        automounterHwInsertCard();

    if(!automounterIsMounted())
    {
        automounterWaitForAck("Insert the SD card to continue.");
        if(!automounterWaitSentinel(AM_TIMEOUT_MS))
            fail("could not mount card for removal/reinsert tests");
    }

    automounterHwRemoveCard();
    automounterHwReinsertCard();
    #endif
}

#else // WITH_FILESYSTEM

static void test_automounter()
{
    iprintf("Automounter tests skipped, filesystem support is disabled\n");
}

#endif // WITH_FILESYSTEM
