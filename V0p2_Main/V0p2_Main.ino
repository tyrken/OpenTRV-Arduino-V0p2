/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2013--2015
*/

/*
  V0p2 (V0.2) core.

  DHD20130417: hardware setup on bare board.
    * 1MHz CPU clock (from 8MHz internal RC clock with /8 prescaler) ATmega328P running at 1.8V--5V (typically 2V--3.3V).
    * Fuse set for BOD-managed additional clock settle time, ie as fast a restart from sleep as possible.
    * All unused pins unconnected and nominally floating (though driven low as output where possible).
    * 32768Hz xtal between pins XTAL1 and XTAL2, async timer 2, for accurate timekeeping and low-power sleep.
    * All unused system modules turned off.

  Basic AVR power consumption ticking an (empty) control loop at ~0.5Hz should be ~1uA.
 */

// Arduino libraries imported here (even for use in other .cpp files).
#include <SPI.h>
#include <Wire.h>
//#ifdef REQUIRES_ONEWIRE22_LIB // Requires V2.2 of OneWire lib.
//#include <OneWire.h>
//#endif
#include <OTRadioLink.h>

#include <util/crc16.h>
#include <avr/eeprom.h>

#include "V0p2_Main.h"

#include "V0p2_Generic_Config.h"
#include "V0p2_Board_IO_Config.h" // I/O pin allocation: include ahead of I/O module headers.
#include "V0p2_Sensors.h"

#include "Control.h"
#include "EEPROM_Utils.h"
#include "FHT8V_Wireless_Rad_Valve.h"
#include "RTC_Support.h"
#include "Power_Management.h"
#include "PRNG.h"
#include "RFM22_Radio.h"
#include "Security.h"
#include "Serial_IO.h"
#include "UI_Minimal.h"


// Indicate that the system is broken in an obvious way (distress flashing the main LED).
// DOES NOT RETURN.
// Tries to turn off most stuff safely that will benefit from doing so, but nothing too complex.
// Tries not to use lots of energy so as to keep distress beacon running for a while.
void panic()
  {
#ifdef USE_MODULE_RFM22RADIOSIMPLE
  // Reset radio and go into low-power mode.
  RFM22PowerOnInit();
#endif
  // Power down almost everything else...
  minimisePowerWithoutSleep();
#ifdef LED_HEATCALL
  pinMode(LED_HEATCALL, OUTPUT);
#else
  pinMode(LED_HEATCALL_L, OUTPUT);
#endif
  for( ; ; )
    {
    LED_HEATCALL_ON();
    tinyPause();
    LED_HEATCALL_OFF();
    bigPause();
    }
  }

// Panic with fixed message.
void panic(const __FlashStringHelper *s)
  {
  serialPrintlnAndFlush(s); // May fail.
  panic();  
  }


// Compute a CRC of all of SRAM as a hash that should contain some entropy, especially after power-up.
#if !defined(RAMSTART)
#define RAMSTART (0x100)
#endif
static uint16_t sramCRC()
  {
  uint16_t result = ~0U;
  for(uint8_t *p = (uint8_t *)RAMSTART; p <= (uint8_t *)RAMEND; ++p)
    { result = _crc_ccitt_update(result, *p); }
  return(result);
  }
// Compute a CRC of all of EEPROM as a hash that may contain some entropy, particularly across restarts.
static uint16_t eeCRC()
  {
  uint16_t result = ~0U;
  for(uint8_t *p = (uint8_t *)0; p <= (uint8_t *)E2END; ++p)
    {
    const uint8_t v = eeprom_read_byte(p);
    result = _crc_ccitt_update(result, v);
    }
  return(result);
  }

// Signal position in basic POST sequence as a small positive integer, or zero for done/none.
// Simple count of position in ON flashes.
// LED is assumed to be ON upon entry, and is left ON at exit.
//
// See video which shows the boot sequence: http://gallery.hd.org/_c/energy-matters/_more2013/_more12/OpenTRV-V0p2-breadboard-POST-Power-On-Self-Test-LED-sequence-as-of-20131202-bootloader-then-five-sections-then-flicker-for-SPI-drive-of-RFM23-1-DHD.mov.html
//   * Two quick flashes from the Arduino bootloader then the LED comes on.
//   * Each of the 5 main sections of Power On Self Test is 1 second LED on, 0.5 second off, n short flashes separated by 0.25s off, then 0.5s off, then 1s on.
//     The value of n is 1, 2, 3, 4, 5.
//   * The LED should then go off except for optional faint flickers as the radio is being driven if set up to do so.
#define PP_OFF_MS 250
static void posPOST(const uint8_t position, const __FlashStringHelper *s)
  {
  sleepLowPowerMs(1000);
#ifdef DEBUG
  DEBUG_SERIAL_PRINT_FLASHSTRING("posPOST: "); // Can only be used once serial is set up.
  DEBUG_SERIAL_PRINT(position);
  DEBUG_SERIAL_PRINT_FLASHSTRING(": ");
  DEBUG_SERIAL_PRINT(s);
  DEBUG_SERIAL_PRINTLN();
#else
  serialPrintlnAndFlush(s);
#endif
//  pinMode(LED_HEATCALL, OUTPUT);
  LED_HEATCALL_OFF();

  // Skip much of lightshow if '0'/end/none position.
  if(position > 0)
    {
    sleepLowPowerMs(2*PP_OFF_MS); // TODO: use this time to gather entropy.
    for(int i = position; --i >= 0; )
      {
      LED_HEATCALL_ON();
      tinyPause();
      LED_HEATCALL_OFF();
      sleepLowPowerMs(PP_OFF_MS); // TODO: use this time to gather entropy.
      }
    }

  sleepLowPowerMs(PP_OFF_MS); // TODO: use this time to gather entropy.
  LED_HEATCALL_ON();
  sleepLowPowerMs(1000); // TODO: use this time to gather entropy.
  }

// Rearrange date into sensible most-significant-first order.  (Would like it also to be fully numeric, but whatever...)
// FIXME: would be better to have this in PROGMEM (Flash) rather than RAM.
static const char _YYYYMmmDD[] =
  {
  __DATE__[7], __DATE__[8], __DATE__[9], __DATE__[10],
  '/',
  __DATE__[0], __DATE__[1], __DATE__[2],
  '/',
  __DATE__[4], __DATE__[5],
  '\0'
  };
// Version (code/board) information printed as one line to serial (with line-end, and flushed); machine- and human- parseable.
// Format: "board VXXXX REVY; code YYYY/Mmm/DD HH:MM:SS".
void serialPrintlnBuildVersion()
  {
  serialPrintAndFlush(F("board V0.2 REV"));
  serialPrintAndFlush(V0p2_REV);
  serialPrintAndFlush(F("; code $Id: V0p2_Main.ino 4788 2015-07-14 17:52:27Z damonhd $ ")); // Expect SVN to substitute the Id keyword here with svn:keywords property set.
  serialPrintAndFlush(_YYYYMmmDD);
  serialPrintAndFlush(F(" " __TIME__));
  serialPrintlnAndFlush();
  }

// Optional Power-On Self Test routines.
// Aborts with a call to panic() if a test fails.
void optionalPOST()
  {
  // Capture early sub-cycle time to help ensure that the 32768Hz async clock is actually running.
  const uint8_t earlySCT = getSubCycleTime();

//  posPOST(1, F("about to test radio module"));

#ifdef USE_MODULE_RFM22RADIOSIMPLE
#if !defined(RFM22_IS_ACTUALLY_RFM23) && defined(DEBUG) && !defined(MIN_ENERGY_BOOT)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("(Using RFM22.)");
#endif
  // Initialise the radio, if configured, ASAP because it can suck a lot of power until properly initialised.
  RFM22PowerOnInit();
  // Check that the radio is correctly connected; panic if not...
  if(!RFM22CheckConnected()) { panic(); }
  // Configure the radio.
  RFM22RegisterBlockSetup(FHT8V_RFM22_Reg_Values);
  // Put the radio in low-power standby mode.
  RFM22ModeStandbyAndClearState();
#endif

//  posPOST(1, F("Radio OK, checking buttons/sensors and xtal"));

  // Check buttons not stuck in the activated position.
  if((fastDigitalRead(BUTTON_MODE_L) == LOW)
#if defined(BUTTON_LEARN_L)
     || (fastDigitalRead(BUTTON_LEARN_L) == LOW)
#endif
#if defined(BUTTON_LEARN2_L)
     || (fastDigitalRead(BUTTON_LEARN2_L) == LOW)
#endif
    )
    { panic(F("button stuck")); }

#if defined(WAKEUP_32768HZ_XTAL)
  // Check that the 32768Hz async clock is actually running having done significant CPU-intensive work.
  const uint8_t laterSCT = getSubCycleTime();
  if(laterSCT == earlySCT)
    {
#if defined(WAKEUP_32768HZ_XTAL)
    // Allow extra time for 32768Hz crystal to start reliably, see: http://www.atmel.com/Images/doc1259.pdf
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("Sleeping to let 32768Hz clock start...");
#endif
    // Time spent here should not be a whole multiple of basic cycle time to avoid a spuriously-stationary async clock reading!
    // Allow several seconds to start.
    // Attempt to capture some entropy while waiting, implicitly from oscillator start-up time if nothing else.
    for(uint8_t i = 255; (--i > 0) && (earlySCT == getSubCycleTime()); )
      {
      addEntropyToPool(clockJitterWDT() ^ noisyADCRead(), 1); // Conservatively hope for at least 1 bit from combined sources!
      nap(WDTO_15MS); // Ensure lower mount of ~3s until loop finishes.
      captureEntropy1(); // Have other fun, though likely largely ineffective at this stage.
      }
#endif
    const uint8_t latestSCT = getSubCycleTime();
    if(latestSCT == earlySCT)
      {
#if 0 && defined(DEBUG)
      DEBUG_SERIAL_PRINTLN_FLASHSTRING("32768Hz clock may not be running!");
#endif
      panic(F("Xtal dead")); // Async clock not running.
      }
    }
//  posPOST(2, F("slow RTC clock OK"));
#else
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("(No xtal.)");
#endif

  // Single POST checkpoint for speed.
#if defined(WAKEUP_32768HZ_XTAL)
  posPOST(0, F("Radio, xtal, buttons OK"));
#else
  posPOST(0, F("Radio, buttons OK"));
#endif
  }


// Setup routine: runs once after reset.
// Does some limited board self-test and will panic() if anything is obviously broken.
void setup()
  {
  // Set appropriate low-power states, interrupts, etc, ASAP.
  powerSetup();

#if defined(MIN_ENERGY_BOOT)
  nap(WDTO_120MS); // Sleep to let power supply recover a little.
#endif

  // IO setup for safety, and to avoid pins floating.
  IOSetup();

#if defined(MIN_ENERGY_BOOT)
  nap(WDTO_120MS); // Sleep to let power supply recover a little.
#endif

#if !defined(MIN_ENERGY_BOOT)
  // Restore previous RTC state if available.
  restoreRTC();
  // TODO: consider code to calibrate the internal RC oscillator against the xtal, eg to keep serial comms happy, eg http://www.avrfreaks.net/index.php?name=PNphpBB2&file=printview&t=36237&start=0
#endif

#if !defined(MIN_ENERGY_BOOT)
  serialPrintAndFlush(F("\r\nOpenTRV: ")); // Leading CRLF to clear leading junk, eg from bootloader.
    serialPrintlnBuildVersion();
#endif

#if !defined(MIN_ENERGY_BOOT)
  // Count resets to detect unexpected crashes/restarts.
  const uint8_t oldResetCount = eeprom_read_byte((uint8_t *)EE_START_RESET_COUNT);
  eeprom_write_byte((uint8_t *)EE_START_RESET_COUNT, 1 + oldResetCount);
#endif

#if defined(DEBUG) && !defined(MIN_ENERGY_BOOT)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("DEBUG");
#endif

#if defined(DEBUG) && !defined(MIN_ENERGY_BOOT)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Resets: ");
  DEBUG_SERIAL_PRINT(oldResetCount);
  DEBUG_SERIAL_PRINTLN();
#if !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
  const uint8_t overruns = (~eeprom_read_byte((uint8_t *)EE_START_OVERRUN_COUNTER)) & 0xff;
  if(0 != overruns)
    {
    DEBUG_SERIAL_PRINT_FLASHSTRING("Overruns: ");
    DEBUG_SERIAL_PRINT(overruns);
    DEBUG_SERIAL_PRINTLN();
    }
#endif
#if 0 && defined(DEBUG)
  // Compute approx free RAM: see http://jeelabs.org/2011/05/22/atmega-memory-use/
  DEBUG_SERIAL_PRINT_FLASHSTRING("Free RAM: ");
  extern int __heap_start, *__brkval;
  int x;
  DEBUG_SERIAL_PRINT((int) &x - (__brkval == 0 ? (int) &__heap_start : (int) __brkval));
  DEBUG_SERIAL_PRINTLN();
#endif
#if defined(ALT_MAIN_LOOP)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("ALTERNATE MAIN LOOP...");
#elif defined(UNIT_TESTS)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("UNIT TESTS...");
#endif
#endif

// Do not do normal POST if running alternate main loop.
// POST may take too long and do unwanted things,
// especially for non-standard hardware setup.
#if defined(ALT_MAIN_LOOP)
  POSTalt(); // Do alternate POST and setup if required.
#else
  optionalPOST();
#endif

  // Collect full set of environmental values before entering loop().
  // This should also help ensure that sensors are properly initialised.

  // No external sensors are *assumed* present if running alt main loop.
  // This may mean that the alt loop/POST will have to initialise them explicitly,
  // and the initial seed entropy may be marginally reduced also.
#if !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
  const int light = AmbLight.read();
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("L: ");
  DEBUG_SERIAL_PRINT(light);
  DEBUG_SERIAL_PRINTLN();
#endif
//  // Assume 0 or full-scale values unlikely.
//  if((0 == light) || (light >= 1023)) { panic(F("LDR fault")); }
  const int heat = TemperatureC16.read();
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("T: ");
  DEBUG_SERIAL_PRINT(heat);
  DEBUG_SERIAL_PRINTLN();
#endif
#ifdef HUMIDITY_SENSOR_SUPPORT
  const uint8_t rh = RelHumidity.read();
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("RH%: ");
  DEBUG_SERIAL_PRINT(rh);
  DEBUG_SERIAL_PRINTLN();
#endif
#endif
#if defined(TEMP_POT_AVAILABLE)
  const int tempPot = TempPot.read();
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("temp pot: ");
  DEBUG_SERIAL_PRINT(tempPot);
  DEBUG_SERIAL_PRINTLN();
#endif
#endif
#endif


#if !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
  // Get current power supply voltage (internal sensor).
  const uint16_t Vcc = Supply_mV.read();
#if 1 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Vcc: ");
  DEBUG_SERIAL_PRINT(Vcc);
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("mV");
#endif
  // Get internal temperature measurement (internal sensor).
  const int intTempC16 = readInternalTemperatureC16();
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Int temp: ");
  DEBUG_SERIAL_PRINT((intTempC16 + 8) >> 4);
  DEBUG_SERIAL_PRINT_FLASHSTRING("C / ");
  DEBUG_SERIAL_PRINT(intTempC16);
  DEBUG_SERIAL_PRINTLN();
#endif

#if !defined(MIN_ENERGY_BOOT)
  // Seed PRNG(s) with available environmental values and clock time/jitter for some entropy.
  // Also sweeps over SRAM and EEPROM (see RAMEND and E2END), especially for non-volatile state and uninitialised areas of SRAM.
  // TODO: add better PRNG with entropy pool (eg for crypto).
  // TODO: add RFM22B WUT clock jitter, RSSI, temperature and battery voltage measures.
  const uint16_t srseed = sramCRC();
  const uint16_t eeseed = eeCRC();
  // DHD20130430: maybe as much as 16 bits of entropy on each reset in seed1, concentrated in the least-significant bits.
  const uint16_t s16 = (__DATE__[5]) ^
                       Vcc ^
                       (intTempC16 << 1) ^
#if !defined(ALT_MAIN_LOOP)
                       (heat << 2) ^
#if defined(TEMP_POT_AVAILABLE)
                       ((((uint16_t)tempPot) << 3) + tempPot) ^
#endif
                       (light << 4) ^
#if defined(HUMIDITY_SENSOR_SUPPORT)
                       ((((uint16_t)rh) << 8) - rh) ^
#endif
#endif
                       (getMinutesSinceMidnightLT() << 5) ^
                       (((uint16_t)getSubCycleTime()) << 6);
  //const long seed1 = ((((long) clockJitterRTC()) << 13) ^ (((long)clockJitterWDT()) << 21) ^ (((long)(srseed^eeseed)) << 16)) + s16;
  // Seed simple/fast/small built-in PRNG.  (Smaller and faster than srandom()/random().)
  const uint8_t nar1 = noisyADCRead();
  seedRNG8(nar1 ^ (uint8_t) s16, oldResetCount - (uint8_t)((s16+eeseed) >> 8), clockJitterWDT() ^ (uint8_t)srseed);
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("nar ");
  DEBUG_SERIAL_PRINTFMT(nar1, BIN);
  DEBUG_SERIAL_PRINTLN();
#endif
  // TODO: seed other/better PRNGs.
  // Feed in mainly persistent/nonvolatile state explicitly. 
  addEntropyToPool(oldResetCount ^ eeseed, 0);
  addEntropyToPool((uint8_t)(eeseed >> 8) + nar1, 0);
  addEntropyToPool((uint8_t)s16 ^ (uint8_t)(s16 >> 8), 0);
  for(uint8_t i = 0; i < EE_LEN_SEED; ++i)
    { addEntropyToPool(eeprom_read_byte((uint8_t *)(EE_START_SEED + i)), 0); }
  addEntropyToPool(noisyADCRead(), 1); // Conservative first push of noise into pool.
  // Carry a few bits of entropy over a reset by picking one of the four designated EEPROM bytes at random;
  // if zero, erase to 0xff, else AND in part of the seed including some of the previous EEPROM hash (and write).
  // This amounts to about a quarter of an erase/write cycle per reset/restart per byte, or 400k restarts endurance!
  // These 4 bytes should be picked up as part of the hash/CRC of EEPROM above, next time,
  // essentially forming a longish-cycle (poor) PRNG even with little new real entropy each time.
  seedRNG8(nar1 ^ (uint8_t) s16, oldResetCount - (uint8_t)((s16+eeseed) >> 8), clockJitterWDT() ^ (uint8_t)srseed);
  uint8_t *const erp = (uint8_t *)(EE_START_SEED + (3&((s16)^((eeseed>>8)+(__TIME__[7]))))); // Use some new and some eeseed bits to choose which byte to updated.
  const uint8_t erv = eeprom_read_byte(erp);
  if(0 == erv) { eeprom_smart_erase_byte(erp); }
  else
    {
    eeprom_smart_clear_bits(erp,
#if !defined(NO_clockJitterEntropyByte)
      clockJitterEntropyByte()
#else
      (clockJitterWDT() ^ nar1) // Less good fall-back when clockJitterEntropyByte() not available with more actual entropy.
#endif
      + ((uint8_t)eeseed)); // Nominally include disjoint set of eeseed bits in choice of which to clear.
    }
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("srseed ");
  DEBUG_SERIAL_PRINTFMT(srseed, BIN);
  DEBUG_SERIAL_PRINTLN();
  DEBUG_SERIAL_PRINT_FLASHSTRING("eeseed ");
  DEBUG_SERIAL_PRINTFMT(eeseed, BIN);
  DEBUG_SERIAL_PRINTLN();
  DEBUG_SERIAL_PRINT_FLASHSTRING("RNG8 ");
  DEBUG_SERIAL_PRINTFMT(randRNG8(), BIN);
  DEBUG_SERIAL_PRINTLN();
  DEBUG_SERIAL_PRINT_FLASHSTRING("erv ");
  DEBUG_SERIAL_PRINTFMT(erv, BIN);
  DEBUG_SERIAL_PRINTLN();
#endif
#endif
#endif


#if !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("Computing initial target/demand...");
#endif
  // Update targets, output to TRV and boiler, etc, to be sensible before main loop starts.
  NominalRadValve.read();
#if defined(USE_MODULE_FHT8VSIMPLE)
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("Creating initial FHT8V frame...");
#endif
  // Unconditionally ensure that a valid FHT8V TRV command frame has been computed and stored
  // in case this unit is actually controlling a local valve.
  FHT8VCreateValveSetCmdFrame();
#endif
#endif


  // Ensure that the unique node ID is set up (mainly on first use).
  // Have one attempt (don't want to stress an already failing EEPROM) to force-reset if not good, then panic.
  // Needs to have had entropy gathered, etc.
  if(!ensureIDCreated())
    {
    if(!ensureIDCreated(true)) // Force reset.
      { panic(F("!Bad ID: can't reset")); }
    }


  // Initialised: turn heatcall UI LED off.
//  pinMode(LED_HEATCALL, OUTPUT);
  LED_HEATCALL_OFF();
  
#if defined(SUPPORT_CLI) && !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
  // Help user get to CLI.
  serialPrintlnAndFlush(F("? at CLI prompt for help"));
#endif

#if !defined(ALT_MAIN_LOOP) && !defined(UNIT_TESTS)
  // Report initial status.
  serialStatusReport();
  // Do OpenTRV-specific (late) setup.
  setupOpenTRV();
#endif
  }








//========================================
// MAIN LOOP
//========================================

void loop()
  {
#if defined(EST_CPU_DUTYCYCLE)
  const unsigned long usStart = micros();
#endif


#if defined(UNIT_TESTS) // Run unit tests *instead* of normal loop() code. 
  loopUnitTest();
#elif defined(ALT_MAIN_LOOP) // Run alternative main loop.
  loopAlt();
#else // Normal OpenTRV usage.
  loopOpenTRV();
#endif

#if defined(EST_CPU_DUTYCYCLE)
  const unsigned long usEnd = micros();
  // Nominal loop time should be 2s x 1MHz clock, ie 2,000,000 if CPU running all the time.
  // Should generally be <2000 (<0.1%) for leaf, <20000 (<1%) for hub.
  const unsigned long usApparentTaken = usEnd - usStart;
#if 1 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("us apparent: ");
  DEBUG_SERIAL_PRINT(usApparentTaken);
  DEBUG_SERIAL_PRINTLN();
// Leaf sample 2014/11/22...
//us apparent: 4544
//us apparent: 25280
//us apparent: 9280
//us apparent: 68416
//=F0%@19C7;IC2E0;V3230;X0;R81;T8 1 W255 0 F255 0 W255 0 F255 0;S12 12 16 e;o34
//
//>
//us apparent: 339136
//us apparent: 1600
//us apparent: 2112
//us apparent: 2880
//us apparent: 3008
//us apparent: 1408
//us apparent: 1344
//Bare stats TX
//us apparent: 74368
//{"@":"c2e0","T|C16":311,"H|%":81,"L":238,"B|cV":323}
//us apparent: 119232
//us apparent: 1408
//...
//us apparent: 3008
//us apparent: 1344
//us apparent: 3264
//us apparent: 1408
//us apparent: 3776
//us apparent: 2816
#endif
#endif
  }
