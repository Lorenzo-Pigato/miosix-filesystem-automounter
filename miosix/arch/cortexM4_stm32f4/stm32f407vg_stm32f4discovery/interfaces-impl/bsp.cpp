/***************************************************************************
 *   Copyright (C) 2012, 2013, 2014 by Terraneo Federico                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   As a special exception, if other files instantiate templates or use   *
 *   macros or inline functions from this file, or you compile this file   *
 *   and link it with other works to produce a work based on this file,    *
 *   this file does not by itself cause the resulting work to be covered   *
 *   by the GNU General Public License. However the source code for this   *
 *   file must still be made available in accordance with the GNU General  *
 *   Public License. This exception does not invalidate any other reasons  *
 *   why a work based on this file might be covered by the GNU General     *
 *   Public License.                                                       *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/ 

/***********************************************************************
* bsp.cpp Part of the Miosix Embedded OS.
* Board support package, this file initializes hardware.
************************************************************************/

#include <cstdlib>
#include <inttypes.h>
#include <sys/ioctl.h>
#include "interfaces/bsp.h"
#include "interfaces_private/bsp_private.h"
#include "kernel/thread.h"
#include "kernel/sync.h"
#include "interfaces/delays.h"
#include "interfaces/poweroff.h"
#include "interfaces/arch_registers.h"
#include "config/miosix_settings.h"
#include "kernel/logging.h"
#include "filesystem/file_access.h"
#include "filesystem/console/console_device.h"
#include "drivers/serial.h"
#include "drivers/sd_stm32f2_f4_f7.h"
#include "board_settings.h"
#include "filesystem/automounter/sd_automounter.h"

#if SD_AUTOMOUNTER_DEBUG_LOG
#define SD_AUTO_BSP_LOG(fmt, ...) printf("[SdAutomounter/BSP] " fmt, ##__VA_ARGS__)
#else
#define SD_AUTO_BSP_LOG(fmt, ...) do { } while(0)
#endif

namespace miosix {

typedef Gpio<PD,4>  cs43l22reset;

//
// Initialization
//

void IRQbspInit()
{
    //Enable all gpios
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
                    RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN |
                    RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOHEN;
    RCC_SYNC();
    GPIOA->OSPEEDR=0xaaaaaaaa; //Default to 50MHz speed for all GPIOS
    GPIOB->OSPEEDR=0xaaaaaaaa;
    GPIOC->OSPEEDR=0xaaaaaaaa;
    GPIOD->OSPEEDR=0xaaaaaaaa;
    GPIOE->OSPEEDR=0xaaaaaaaa;
    GPIOH->OSPEEDR=0xaaaaaaaa;
    
    _led::mode(Mode::OUTPUT);
    ledOn();
    delayMs(100);
    ledOff();

    // On stm32f4discovery some of the SDIO pins conflict with the
    // audio output chip, so keep it permanently reset to avoid issues
    cs43l22reset::mode(Mode::OUTPUT);
    cs43l22reset::low();
    // Initialize default serial
    DefaultConsole::instance().IRQset(
        STM32SerialBase::get<defaultSerialTxPin,defaultSerialRxPin,
        defaultSerialRtsPin,defaultSerialCtsPin>(
            defaultSerial,defaultSerialSpeed,
            defaultSerialFlowctrl,defaultSerialDma));

    #if defined(WITH_AUTOMOUNTER) && WITH_SD_CD_PIN
    #if SD_AUTOMOUNTER_CD_PULL==SD_AUTOMOUNTER_CD_PULL_UP
    sdAutomounterCardDetectPin::mode(Mode::INPUT_PULL_UP);
    #elif SD_AUTOMOUNTER_CD_PULL==SD_AUTOMOUNTER_CD_PULL_DOWN
    sdAutomounterCardDetectPin::mode(Mode::INPUT_PULL_DOWN);
    #elif SD_AUTOMOUNTER_CD_PULL==SD_AUTOMOUNTER_CD_PULL_NONE
    sdAutomounterCardDetectPin::mode(Mode::INPUT);
    #else
    #error "Invalid SD_AUTOMOUNTER_CD_PULL value"
    #endif
    #endif
}

#ifdef WITH_AUTOMOUNTER
#if WITH_SD_CD_PIN==0
static bool sdCardPresentBySdio()
{
    // SDIODriver::readBlock requires a full 512-byte block
    static unsigned char buf[512];
    intrusive_ref_ptr<SDIODriver> sd = SDIODriver::instance();

    // Backoff: try to reinit at most once every ~5 seconds when card is absent
    static int reinitCountdown = 0;
    if (reinitCountdown > 0)
        reinitCountdown--;

    ssize_t r = sd->readBlock(buf, sizeof(buf), 0);
    if (r == 512) return true;

    if (reinitCountdown == 0)
    {
        sd->ioctl(IOCTL_REINIT, nullptr);
        reinitCountdown = 7; // ~5.6s at 200ms poll
    }

    return false;
}
#endif

#if WITH_SD_CD_PIN
static bool sdCardPresentByCd()
{
    bool cd = sdAutomounterCardDetectPin::value() != 0;
    #if SD_AUTOMOUNTER_CD_POLARITY==SD_AUTOMOUNTER_CD_ACTIVE_LOW
    return !cd;
    #elif SD_AUTOMOUNTER_CD_POLARITY==SD_AUTOMOUNTER_CD_ACTIVE_HIGH
    return cd;
    #else
    #error "Invalid SD_AUTOMOUNTER_CD_POLARITY value"
    #endif
}
#endif
#endif //WITH_AUTOMOUNTER

void bspInit2()
{
    #ifdef WITH_FILESYSTEM
    #ifdef WITH_AUTOMOUNTER
    {
        intrusive_ref_ptr<SDIODriver> sd = SDIODriver::instance();
        intrusive_ref_ptr<DevFs> devFs=basicFilesystemSetup(intrusive_ref_ptr<Device>());
        #ifdef AUX_SERIAL
        devFs->addDevice(AUX_SERIAL,
            STM32SerialBase::get<auxSerialTxPin,auxSerialRxPin,
            auxSerialRtsPin,auxSerialCtsPin>(
                auxSerial,auxSerialSpeed,auxSerialFlowctrl,auxSerialDma));
        #endif //AUX_SERIAL

        #if WITH_SD_CD_PIN
        SdAutomounter::instance().configure(sd, &sdCardPresentByCd, 200, 3);
        SD_AUTO_BSP_LOG("Detection mode: hardware CD\n");
        #else
        SdAutomounter::instance().configure(sd, &sdCardPresentBySdio, 200, 3);
        SD_AUTO_BSP_LOG("Detection mode: SDIO software probing\n");
        #endif

        SdAutomounter::instance().enable();
        SD_AUTO_BSP_LOG("SD card automounter enabled\n");
    }

    #else //WITH_AUTOMOUNTER
    #ifdef AUX_SERIAL
    {
        intrusive_ref_ptr<DevFs> devFs=basicFilesystemSetup(SDIODriver::instance());
        devFs->addDevice(AUX_SERIAL,
            STM32SerialBase::get<auxSerialTxPin,auxSerialRxPin,
            auxSerialRtsPin,auxSerialCtsPin>(
                auxSerial,auxSerialSpeed,auxSerialFlowctrl,auxSerialDma));
    }
    #else //AUX_SERIAL
    basicFilesystemSetup(SDIODriver::instance());
    #endif //AUX_SERIAL
    #endif //WITH_AUTOMOUNTER
    #endif //WITH_FILESYSTEM
}

//
// Shutdown and reboot
//

/**
This function disables filesystem (if enabled), serial port (if enabled) and
puts the processor in deep sleep mode.<br>
Wakeup occurs when PA.0 goes high, but instead of sleep(), a new boot happens.
<br>This function does not return.<br>
WARNING: close all files before using this function, since it unmounts the
filesystem.<br>
When in shutdown mode, power consumption of the miosix board is reduced to ~
5uA??, however, true power consumption depends on what is connected to the GPIO
pins. The user is responsible to put the devices connected to the GPIO pin in the
minimal power consumption mode before calling shutdown(). Please note that to
minimize power consumption all unused GPIO must not be left floating.
*/
void shutdown()
{
    ioctl(STDOUT_FILENO,IOCTL_SYNC,0);

    #ifdef WITH_FILESYSTEM
    FilesystemManager::instance().umountAll();
    #endif //WITH_FILESYSTEM

    FastGlobalIrqLock::lock();
    for(;;) ;
}

void reboot()
{
    ioctl(STDOUT_FILENO,IOCTL_SYNC,0);
    
    #ifdef WITH_FILESYSTEM
    FilesystemManager::instance().umountAll();
    #endif //WITH_FILESYSTEM

    FastGlobalIrqLock::lock();
    IRQsystemReboot();
}

} //namespace miosix
