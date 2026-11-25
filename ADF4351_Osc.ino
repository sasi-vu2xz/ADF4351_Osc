// ADF4351_Osc.ino
// ---------------------------------------------------------------------
// ESP32-C3-SuperMini control firmware for the ADF4351_Osc transverter LO
// board. Programs the ADF4351 synthesizer, retunes it live between a
// TX_LO and an RX_LO register set on PTT transitions, drives a single
// status LED, serves a WiFi AP with a captive-portal config page, and
// now also drives a local rotary-encoder + display menu (auto-detects
// an OLED or a 16x2 LCD, whichever is wired) as an on-board alternative
// to the web portal for quick tweaks.
//
// Pin map (confirmed against the ADF4351_Osc board schematic):
//   GPIO5  - PTT in, active low (via J1/D1)
//   GPIO6  - ADF4351 CLK           (see adf4351.h)
//   GPIO7  - ADF4351 DATA          (see adf4351.h)
//   GPIO10 - ADF4351 LE            (see adf4351.h)
//   (CE has no GPIO - held high by the board's own R16 pull-up)
//   GPIO0  - single status LED (R23): ON while keyed/TX, OFF while
//            idle/RX; spelling out "V" in Morse code (8 WPM) while the
//            local menu is open (see oled_menu.h's updateMenuLed()), OR
//            spelling out "AP" in Morse code (8 WPM) for as long as the
//            WiFi AP is up (whichever of the two is true takes over the
//            LED - see applyPttState() and updateApLed() below).
//            ALSO shared with SW1 ("PROGRAM" on the schematic), which
//            ties this same pin to GND when pressed - see
//            readProgramButtonHeld() below for how the firmware
//            time-multiplexes an LED output and a button input on one
//            physical pin.
//   GPIO3  - rotary encoder A (RA)          (see oled_menu.h)
//   GPIO4  - rotary encoder B (RB)          (see oled_menu.h)
//   GPIO20 - rotary encoder push switch     (see oled_menu.h)
//   GPIO1  - display SDA (OLED or 16x2 LCD) (see oled_menu.h)
//   GPIO21 - display SCL (OLED or 16x2 LCD) (see oled_menu.h)
//   GPIO8, GPIO9, GPIO2 - deliberately left unconnected in firmware;
//            broken out to header H1 only, for programming/debug access
//            without risking the boot-strapping behaviour of these pins.
//
// WiFi AP (ADF4351_OSC, open/no password) is OFF by default at boot and
// stays off unless deliberately enabled - not something that starts
// transmitting on every power-up. Two independent gestures enable it,
// and they are NOT the same thing - one replaces normal operation
// entirely, the other runs alongside it:
//   - Holding the rotary encoder's own push switch (GPIO20) down WHILE
//     POWERING UP the board (sampled once, early in setup(), before
//     anything else - a press that started before power-on and is
//     still held a moment after is what's checked) boots straight into
//     a dedicated AP-ONLY MODE (g_apOnlyMode): the ADF4351 is never
//     programmed, PTT is never read, and the local menu never runs,
//     for the rest of that power cycle - see g_apOnlyMode's own
//     comment and loop()'s early-return. The display shows a
//     PERMANENT two-line screen (dispDrawApOnly() in oled_menu.h): the
//     AP's SSID on row 1, "AP MODE ON" on row 2 - nothing else ever
//     draws over it, since oledMenuLoop() itself is never called. The
//     status LED spells out "AP" in Morse code (8 WPM), repeating, for
//     as long as the AP stays up (updateApLed() below). The only way
//     out is a reboot without the switch held.
//   - Holding SW1 (the "PROGRAM" button, GPIO0) for 5s at any time
//     while running normally (see readProgramButtonHeld() below) turns
//     the AP on ALONGSIDE everything else - tuning, PTT, the local
//     menu and its RUN screen all keep working exactly as before; only
//     the RUN screen's AP/IP line (OLED only) and the status LED's
//     Morse-code indicator (while the AP is up and the local menu
//     isn't also open) change. This is the original gesture, unchanged,
//     and is a different, lesser thing than AP-only mode above.
// The local menu (encoder + its own push switch, GPIO20) is otherwise
// unrelated to either of these and always available for TX_LO/RX_LO/
// reference/power changes once the board has finished a NORMAL boot
// (i.e. whenever it did NOT enter AP-only mode).
//
// Display: either an SSD1306 128x64 OLED or a 16x2 LCD on a PCF8574 I2C
// backpack can be wired to the same SDA/SCL pins - oled_menu.h probes
// the I2C bus at boot and uses whichever one answers, adapting the menu
// layout to it automatically (see oled_menu.h's top comment for how).
//
// Requires: "Adafruit SSD1306" + "Adafruit GFX Library" (for the OLED)
// and "LiquidCrystal I2C" (for the 16x2 LCD) - Library Manager also
// pulls in "Adafruit BusIO" automatically for the first pair. Both can
// be installed regardless of which single display you actually use.

#include "adf4351.h"
#include "lo_config.h"
#include "web_portal.h"
#include "oled_menu.h"

#define PIN_PTT        5
#define PIN_LED_STATUS 0

static LOConfig g_cfg;
static int g_lastPtt = HIGH;          // HIGH = idle/RX, LOW = keyed/TX
static bool g_apRunning = false;

// True for the rest of this power cycle whenever the board booted with
// the encoder push switch held down (see setup()'s own bootApRequested
// sampling) - the board never leaves this mode once it's entered; the
// only way out is a reboot without the switch held. While true, loop()
// skips normal operation ENTIRELY (no PTT handling, no ADF4351 register
// writes, no local menu) - see loop()'s own early-return for this.
static bool g_apOnlyMode = false;

// Always retunes the ADF4351 on a PTT edge (safety/correctness critical
// - must happen regardless of what the local menu UI is doing). Only
// touches the status LED when neither the local menu NOR the WiFi AP
// is using it for its own blink feedback, so none of the three ever
// fight over the same pin - the AP-active blink (see loop()'s own
// block below) and the local menu's own blink both take priority over
// this PTT-driven solid on/off.
static void applyPttState(int pttLevel) {
  if (pttLevel == LOW) {
    adfWriteAllRegisters(g_cfg.tx_regs);
  } else {
    adfWriteAllRegisters(g_cfg.rx_regs);
  }
  if (!oledMenuIsActive() && !g_apRunning) {
    digitalWrite(PIN_LED_STATUS, (pttLevel == LOW) ? HIGH : LOW);
  }
}

// ---- WiFi-AP-active LED indicator: "AP" in Morse code, 8 WPM ----------
// Independent of the local menu's own blink (which only runs while a
// menu screen is actually open, oled_menu.h's own s_state != ST_RUN) -
// this one runs continuously for as long as g_apRunning is true,
// regardless of what the local menu is doing, so the LED is the
// at-a-glance "WiFi is on" indicator even if nobody's looking at a
// display - spelling out "AP" rather than just blinking plainly makes
// that identification unambiguous (a generic blink could be mistaken
// for the local menu's own, or for a PTT-driven flicker). Gated on
// !oledMenuIsActive() so this and the local menu's own blink never
// both try to drive the pin in the same loop() iteration - whichever
// is active "wins" that iteration.
//
// Standard PARIS timing at MORSE_WPM words per minute: one "unit" is
// 1200/WPM ms; a dit is 1 unit, a dah is 3 units, the gap between
// symbols within one letter is 1 unit, the gap between letters is 3
// units, and the gap before the whole message repeats (a "word gap")
// is 7 units. MORSE_AP[] below is "A" (dit-dah) + letter gap + "P"
// (dit-dah-dah-dit) + word gap, spelled out as explicit ON/OFF
// segments (each segment's OFF already accounts for whatever gap
// follows it, so segments are not summed with a separate inter-symbol
// gap on top) rather than computed from a generic Morse encoder, since
// the message here is permanently just "AP" - no need for the
// generality.
#define MORSE_WPM      8
#define MORSE_UNIT_MS  (1200UL / MORSE_WPM)

struct MorseSeg { bool on; uint8_t units; };
static const MorseSeg MORSE_AP[] = {
  { true,  1 }, { false, 1 }, { true,  3 }, { false, 3 },  // A: dit dah
  { true,  1 }, { false, 1 }, { true,  3 }, { false, 1 },
  { true,  3 }, { false, 1 }, { true,  1 }, { false, 7 },  // P: dit dah dah dit
};
#define MORSE_AP_LEN (sizeof(MORSE_AP) / sizeof(MORSE_AP[0]))

static bool          s_morseRunning = false;
static uint8_t       s_morseIdx = 0;
static unsigned long s_morseSegStartMs = 0;

static void updateApLed() {
  if (!g_apRunning || oledMenuIsActive()) {
    s_morseRunning = false;  // re-arm - next time starts the message over from "A", not mid-way through
    return;
  }
  unsigned long now = millis();
  if (!s_morseRunning) {
    s_morseRunning = true;
    s_morseIdx = 0;
    s_morseSegStartMs = now;
    digitalWrite(PIN_LED_STATUS, MORSE_AP[0].on ? HIGH : LOW);
    return;
  }
  const MorseSeg &seg = MORSE_AP[s_morseIdx];
  if (now - s_morseSegStartMs >= (unsigned long)seg.units * MORSE_UNIT_MS) {
    s_morseIdx = (s_morseIdx + 1) % MORSE_AP_LEN;
    s_morseSegStartMs = now;
    digitalWrite(PIN_LED_STATUS, MORSE_AP[s_morseIdx].on ? HIGH : LOW);
  }
}

// ---- SW1 "PROGRAM" button, shared with the GPIO0 status LED output ----
// GPIO0 can't be an output (driving the LED) and an input (reading SW1)
// at the same instant, so this briefly flips it to INPUT_PULLUP, samples
// it, then flips it straight back to OUTPUT and re-asserts whatever
// level it was already driving (read back via digitalRead() while still
// in OUTPUT mode - on the ESP32 this reflects the pin's own driven
// level, not an external read, so no extra state variable is needed to
// remember "what the LED was doing"). While the pin is briefly INPUT_
// PULLUP, the internal pull-up (not SW1, not a hard driven level) is
// the only thing holding it high, so a tiny trickle current runs
// through R23/the LED to ground for that instant - not enough to look
// "on", but running this every single loop() iteration (loop() turns
// over far faster than this button needs - it only has to be sampled
// often enough to catch a 5-SECOND hold) made that trickle frequent
// enough to read as a faint, continuous flicker on the LED even while
// it's meant to be solidly off (RX/idle). PROGRAM_BTN_SAMPLE_MS below
// rate-limits the actual pin flip/sample to once per that interval -
// still far finer than needed for a human press-and-hold gesture - so
// the rest of loop() runs exactly as before but the LED only sees that
// brief pull-up a few dozen times a second instead of thousands,
// cutting the average trickle current enough to no longer be visible.
#define PROGRAM_BTN_DEBOUNCE_MS 30UL
#define PROGRAM_BTN_HOLD_MS     5000UL
#define PROGRAM_BTN_SAMPLE_MS   50UL

static int  s_pgmRawLevel      = HIGH;
static int  s_pgmStableLevel   = HIGH;
static unsigned long s_pgmLastRawChangeMs = 0;
static unsigned long s_pgmPressedAtMs     = 0;
static unsigned long s_pgmLastSampleMs    = 0;
static bool s_pgmHoldFired     = false;

// Returns true exactly once per press, the instant the hold crosses
// PROGRAM_BTN_HOLD_MS (doesn't wait for release). Won't fire again
// until SW1 is released and pressed again.
static bool readProgramButtonHeld() {
  unsigned long now = millis();

  // Only actually touch GPIO0 at most once every PROGRAM_BTN_SAMPLE_MS -
  // see the comment above for why (LED flicker). Between samples, just
  // re-check the hold-duration condition against whatever s_pgmStableLevel
  // was last measured, so a 5s hold that started mid-interval still
  // fires on time.
  if (now - s_pgmLastSampleMs >= PROGRAM_BTN_SAMPLE_MS) {
    s_pgmLastSampleMs = now;

    bool ledLevel = digitalRead(PIN_LED_STATUS);  // still OUTPUT here
    pinMode(PIN_LED_STATUS, INPUT_PULLUP);
    delayMicroseconds(10);
    int raw = digitalRead(PIN_LED_STATUS);
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_STATUS, ledLevel);

    if (raw != s_pgmRawLevel) {
      s_pgmRawLevel = raw;
      s_pgmLastRawChangeMs = now;
    }
  }

  if ((now - s_pgmLastRawChangeMs) > PROGRAM_BTN_DEBOUNCE_MS && s_pgmRawLevel != s_pgmStableLevel) {
    s_pgmStableLevel = s_pgmRawLevel;
    if (s_pgmStableLevel == LOW) {
      s_pgmPressedAtMs = now;
      s_pgmHoldFired = false;            // armed for a fresh hold
    }
  }
  if (s_pgmStableLevel == LOW && !s_pgmHoldFired &&
      (now - s_pgmPressedAtMs) >= PROGRAM_BTN_HOLD_MS) {
    s_pgmHoldFired = true;
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // __DATE__/__TIME__ are filled in by the compiler at build time, so
  // this always reflects the moment you last hit Upload - no manual
  // version string to remember to bump.
  Serial.println(F("====================================="));
  Serial.print(F("Firmware built: "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));
  Serial.println(F("====================================="));

  pinMode(PIN_PTT, INPUT_PULLUP);
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, LOW);

  // Sample the rotary encoder's push switch (PIN_ENC_SW, defined in
  // oled_menu.h) early - before the 5s OCXO delay below, and before
  // oledMenuInit() re-configures this same pin for its own normal use
  // - so holding it down WHILE POWERING UP the board is what's
  // detected, not a press made later once the board's already running
  // (that's the local menu's own job, via its 2s+ hold). A short
  // settle delay after pinMode() lets the internal pull-up actually
  // reach HIGH before the read, in case the switch is released right
  // at this instant rather than held all the way through boot.
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  delay(20);
  g_apOnlyMode = (digitalRead(PIN_ENC_SW) == LOW);

  if (g_apOnlyMode) {
    // ---- AP-only boot mode: skips EVERYTHING to do with normal
    // operation - no OCXO settle delay (nothing is about to program
    // the synth), no adfBusInit(), no loConfigLoad()/applyPttState(),
    // and loop() below will never touch the ADF4351, PTT, or the local
    // menu for the rest of this power cycle (see g_apOnlyMode's own
    // comment above and loop()'s early-return). Still brings up the
    // display (oledMenuInit() - cheap, and dispDrawApOnly() below needs
    // it) and the WiFi AP, then leaves a permanent two-line screen up:
    // the SSID on row 1, "AP MODE ON" on row 2, and the status LED
    // spelling out "AP" in Morse code (updateApLed(), gated on g_apRunning).
    Serial.println(F("Encoder push switch held through power-up - entering AP-ONLY mode"));
    Serial.println(F("(normal ADF4351/PTT operation and the local menu are disabled for this power cycle)"));

    oledMenuInit(PIN_LED_STATUS);

    g_apRunning = webPortalBegin();
    if (g_apRunning) {
      Serial.print(F("WiFi AP enabled: "));
      Serial.println(AP_SSID);
      Serial.print(F("Portal IP: "));
      Serial.println(WiFi.softAPIP());
      dispDrawApOnly(AP_SSID, "AP MODE ON");
    } else {
      Serial.println(F("WiFi AP enable FAILED - see reason above"));
      dispDrawApOnly("WiFi AP", "FAILED");
    }
    return;  // skip the rest of setup() entirely - no synth/PTT setup
  }

  // Let the OCXO and the ADF4351's own PLL settle before the first
  // register write. On cold power-up the OCXO reference isn't
  // instantly at frequency/amplitude, and the ADF4351's internal PLL
  // needs its own lock time - programming it immediately against a
  // reference that's still settling risks an initial lock onto the
  // wrong point. 5s here costs nothing (it's once, at boot) and
  // gives both a clean start, though it covers less of the OCXO's
  // warm-up spec than the earlier 15s did - if frequency looks off
  // right after power-up, that's the tradeoff to revisit first.
  Serial.println(F("Waiting 5s for OCXO/PLL to settle before first tune..."));
  delay(5000);

  adfBusInit();

  bool haveSavedConfig = loConfigLoad(g_cfg);
  Serial.println(haveSavedConfig ? F("Loaded saved LO config") : F("No saved config - using defaults"));

  g_lastPtt = digitalRead(PIN_PTT);
  applyPttState(g_lastPtt);

  bool dispOk = oledMenuInit(PIN_LED_STATUS);
  Serial.println(dispOk ? F("Display found and initialised (OLED or 16x2 LCD, auto-detected)")
                         : F("NO display found on I2C bus - check wiring/address"));

  // WiFi AP stays off here - the only way to reach AP-only mode is
  // holding the encoder push switch through power-up (handled above,
  // which returns early and never reaches this point). The PROGRAM
  // button's 5s hold (readProgramButtonHeld(), checked every loop())
  // is still available to enable the AP later ALONGSIDE normal
  // operation, which is a different thing from AP-only mode above -
  // that one keeps tuning/PTT/the local menu all working as normal.
  Serial.println(F("WiFi AP off - hold the PROGRAM button 5s to enable it alongside normal operation, "
                    "or hold the encoder push switch through the next power-up for AP-only mode"));
}

void loop() {
  // AP-ONLY mode (see setup()'s own g_apOnlyMode handling): normal
  // operation is disabled entirely for the rest of this power cycle -
  // no PTT handling, no ADF4351 register writes, no local menu, no
  // re-drawing the display (dispDrawApOnly() already drew its one,
  // permanent screen back in setup()). Just keep the portal served and
  // the status LED spelling out "AP" in Morse code for as long as the
  // AP stays up.
  if (g_apOnlyMode) {
    if (g_apRunning) webPortalLoop();  // guarded - webPortalBegin() may have failed
    updateApLed();
    return;
  }

  // --- PTT-driven live retune (always runs, independent of the menu) ---
  int ptt = digitalRead(PIN_PTT);
  if (ptt != g_lastPtt) {
    delay(5); // light debounce
    ptt = digitalRead(PIN_PTT);
    if (ptt != g_lastPtt) {
      g_lastPtt = ptt;
      applyPttState(ptt);
    }
  }

  // --- Web config portal, serviced only once the AP has actually been
  // enabled - here, that can only be via the PROGRAM button's 5s hold
  // just below (the OTHER way to enable WiFi, the boot-time encoder-
  // switch hold, diverts into AP-only mode and never reaches this far
  // down in loop() at all - see the early-return above). Calling
  // WebServer/DNSServer methods before softAP() has run would do
  // nothing useful anyway, and skipping them while off saves the
  // per-loop overhead.
  if (g_apRunning) {
    webPortalLoop();
  }
  updateApLed();

  // --- 5s hold of SW1 ("PROGRAM", GPIO0) enables WiFi (one-shot; see
  // readProgramButtonHeld() above). Only acts if the AP isn't already
  // up, so re-holding past 5s again later doesn't try to re-start it.
  if (!g_apRunning && readProgramButtonHeld()) {
    g_apRunning = webPortalBegin();
    if (g_apRunning) {
      Serial.print(F("WiFi AP enabled via 5s PROGRAM-button hold: "));
      Serial.println(AP_SSID);
      Serial.print(F("Portal IP: "));
      Serial.println(WiFi.softAPIP());
      oledShowFlash("WiFi ON", 1200);
    } else {
      Serial.println(F("WiFi AP enable FAILED - see reason above"));
      oledShowFlash("WiFi FAILED", 1500);
    }
  }

  // --- Local encoder + OLED menu ---
  bool wasMenuActive = oledMenuIsActive();
  String apIp = g_apRunning ? WiFi.softAPIP().toString() : String("");
  oledMenuLoop(g_cfg, g_lastPtt, g_apRunning, apIp);
  // If the menu just closed (Exit, timeout, or a Save that didn't
  // restart because validation failed before reaching ESP.restart()),
  // resync the status LED to the real PTT state - it may have been left
  // mid-blink.
  if (wasMenuActive && !oledMenuIsActive()) {
    applyPttState(g_lastPtt);
  }
}
