/***************************************************************************
 *   Copyright (C) 2012-2021 by Terraneo Federico                          *
 *   Copyright (C) 2024 by Daniele Cattaneo                                *
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

#pragma once

#include "interfaces/gpio.h"

/**
 * \internal
 * Versioning for board_settings.h for out of git tree projects
 */
#define BOARD_SETTINGS_VERSION 300

namespace miosix {

/**
 * \addtogroup Settings
 * \{
 */

/// Size of stack for main().
/// The C standard library is stack-heavy (iprintf requires 1KB) but the
/// STM32F407VG only has 192KB of RAM so there is room for a big 4K stack.
const unsigned int MAIN_STACK_SIZE=4*1024;

/// Serial port
/// Serial ports 1 to 6 are available
const unsigned int defaultSerial=3;
const unsigned int defaultSerialSpeed=115200;
const bool defaultSerialFlowctrl=false;
const bool defaultSerialDma=true;
// Default serial 1 pins (uncomment when using serial 1)
// Note: on this board, pins PA9-12 are in use by the user USB port, and PB6 is
// connected to the Cirrus audio chip
//using defaultSerialTxPin = Gpio<PB,6>;
//using defaultSerialRxPin = Gpio<PB,7>;
//using defaultSerialRtsPin = Gpio<PA,12>;
//using defaultSerialCtsPin = Gpio<PA,11>;
// Default serial 2 pins (uncomment when using serial 2)
//using defaultSerialTxPin = Gpio<PA,2>;
//using defaultSerialRxPin = Gpio<PA,3>;
//using defaultSerialRtsPin = Gpio<PA,1>;
//using defaultSerialCtsPin = Gpio<PA,0>;
// Default serial 3 pins (uncomment when using serial 3)
using defaultSerialTxPin = Gpio<PB,10>;
using defaultSerialRxPin = Gpio<PB,11>;
using defaultSerialRtsPin = Gpio<PB,14>;
using defaultSerialCtsPin = Gpio<PB,13>;

// Aux serial port
// Uncomment AUX_SERIAL to enable. The device will appear as /dev/auxtty.
//#define AUX_SERIAL "auxtty"
const unsigned int auxSerial=2;
const unsigned int auxSerialSpeed=9600;
const bool auxSerialFlowctrl=false;
//Disable DMA for serial 2 because it conflicts with I2S driver in examples
const bool auxSerialDma=false;
// Default aux serial 1 pins (uncomment when using serial 1)
// Note: on this board, pins PA9-12 are in use by the user USB port, and PB6 is
// connected to the Cirrus audio chip
//using auxSerialTxPin = Gpio<PB,6>;
//using auxSerialRxPin = Gpio<PB,7>;
//using auxSerialRtsPin = Gpio<PA,12>;
//using auxSerialCtsPin = Gpio<PA,11>;
// Default aux serial 2 pins (uncomment when using serial 2)
using auxSerialTxPin = Gpio<PA,2>;
using auxSerialRxPin = Gpio<PA,3>;
using auxSerialRtsPin = Gpio<PA,1>;
using auxSerialCtsPin = Gpio<PA,0>;
// Default aux serial 3 pins (uncomment when using serial 3)
//using auxSerialTxPin = Gpio<PB,10>;
//using auxSerialRxPin = Gpio<PB,11>;
//using auxSerialRtsPin = Gpio<PB,14>;
//using auxSerialCtsPin = Gpio<PB,13>;

//SD card driver
static const unsigned char sdVoltage=30; //Board powered @ 3.0V
#define SD_ONE_BIT_DATABUS //Can't use 4 bit databus due to pin conflicts

//
// SD automounter hardware wiring
//
// Behavior and timing settings live in config/sd_automounter_config.h.
// Detection mode selector:
// - 0: SDIO software probing
// - 1: Hardware card-detect pin (CD)
#ifndef WITH_SD_CD_PIN
#define WITH_SD_CD_PIN 0
#endif

#if WITH_SD_CD_PIN!=0 && WITH_SD_CD_PIN!=1
#error "SD_AUTOMOUNTER_HARDWARE must be 0 (SDIO) or 1 (CD pin)"
#endif

// Hardware CD logic configuration.
#define SD_AUTOMOUNTER_CD_ACTIVE_LOW  0
#define SD_AUTOMOUNTER_CD_ACTIVE_HIGH 1
#ifndef SD_AUTOMOUNTER_CD_POLARITY
#define SD_AUTOMOUNTER_CD_POLARITY SD_AUTOMOUNTER_CD_ACTIVE_LOW
#endif

// Hardware CD input mode.
#define SD_AUTOMOUNTER_CD_PULL_NONE  0
#define SD_AUTOMOUNTER_CD_PULL_UP    1
#define SD_AUTOMOUNTER_CD_PULL_DOWN  2
#ifndef SD_AUTOMOUNTER_CD_PULL
#define SD_AUTOMOUNTER_CD_PULL SD_AUTOMOUNTER_CD_PULL_UP
#endif

// Optional short form to configure the CD pin with one macro:
// #define SD_AUTOMOUNTER_CD_GPIO Gpio<PC,0>
//
// Legacy form (still supported):
// #define SD_AUTOMOUNTER_CD_PORT PC
// #define SD_AUTOMOUNTER_CD_PIN  0
#if WITH_SD_CD_PIN
#if defined(SD_AUTOMOUNTER_CD_GPIO)
using sdAutomounterCardDetectPin = SD_AUTOMOUNTER_CD_GPIO;
#elif defined(SD_AUTOMOUNTER_CD_PORT) && defined(SD_AUTOMOUNTER_CD_PIN)
using sdAutomounterCardDetectPin = Gpio<SD_AUTOMOUNTER_CD_PORT, SD_AUTOMOUNTER_CD_PIN>;
#else
class SdAutomounterInvalidCdPin
{
public:
    static void mode(Mode) {}
    static int value() { return 0; }
};
using sdAutomounterCardDetectPin = SdAutomounterInvalidCdPin;
#error "Define SD_AUTOMOUNTER_CD_GPIO (recommended) or SD_AUTOMOUNTER_CD_PORT+SD_AUTOMOUNTER_CD_PIN when SD_AUTOMOUNTER_HARDWARE is 1"
#endif
#endif

/**
 * \}
 */

} //namespace miosix
