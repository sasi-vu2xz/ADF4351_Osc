// oled_menu.h
// ---------------------------------------------------------------------
// Local control panel: rotary encoder (A/B + push switch) plus whichever
// I2C display is actually connected - a 0.96" SSD1306 128x64 OLED, or a
// 16x2 character LCD on a PCF8574 I2C backpack. Both the OLED and the
// LCD share the same SDA/SCL bus and GPIOs; oledMenuInit() probes known
// I2C addresses at boot (0x3C/0x3D for the OLED, 0x27/0x3F for the LCD
// backpack - no overlap between the two ranges, so address alone tells
// them apart) and sets s_dispType accordingly. Every draw function below
// then branches on s_dispType, so the menu's behaviour (state machine,
// encoder, button, step sizes, flash messages) is identical either way -
// only the on-screen layout differs, and a 16x2 LCD has far less room,
// so its RUN screen auto-cycles through pages instead of showing
// everything at once (see lcdDrawRun()). If neither responds,
// s_dispType stays DISP_NONE and the menu still runs the same state
// machine, it just never draws anything - harmless, just not visible.
//
// Pins (confirmed against Schematic_QO-100_LO_V2.pdf):
//   GPIO3  - Encoder A (RA)
//   GPIO4  - Encoder B (RB)
//   GPIO20 - Encoder push switch (PS), active low, internal pull-up
//   GPIO1  - OLED/LCD SDA
//   GPIO21 - OLED/LCD SCL
//
// Requires the "Adafruit SSD1306" and "Adafruit GFX Library" libraries
// (Library Manager pulls in "Adafruit BusIO" automatically) for the
// OLED, and "LiquidCrystal I2C" (the common Frank de Brabander/
// marcoschwartz library - Library Manager search: "LiquidCrystal I2C")
// for the 16x2 LCD. Both can be installed even if only one display is
// ever plugged in - the unused one's library just never gets used.
//
// Behaviour (digit-cursor tuning replaced with a plain step-size picker -
// simpler to use, at the cost of needing an explicit step choice instead
// of a movable per-digit cursor):
//   RUN mode (default)  - status screen: TX_LO/RX_LO/Ref/power, PTT
//                          state, AP SSID/IP (OLED); frequency + TX/RX +
//                          power (LCD). Rotating the encoder right here
//                          tunes whichever LO is actually active (TX_LO
//                          while keyed, RX_LO while idle) by the current
//                          step size (see STEP below) - live, no "enter
//                          tuning" step needed - and every single detent
//                          is recomputed into registers, saved to flash,
//                          and pushed to the synthesizer immediately
//                          (see oledMenuLoop()'s ST_RUN case).
//   2s+ continuous hold  - opens MENU (below). Deliberately a long,
//                          continuous hold (see oledButtonHeldForStep())
//                          rather than a quick press/release - a plain
//                          short press wasn't registering reliably, so
//                          this fires the instant the hold crosses
//                          STEP_ENTER_HOLD_MS (2s - shorter than the
//                          main sketch's PROGRAM-button 5s hold, since
//                          this one's reached far more often).
//   MENU                - a 5-item list (MENU_ITEMS): "Tuning Steps",
//                          "Tx Frequency", "Rx Power", "Tx Power",
//                          "Exit". Rotating
//                          moves the selection, WRAPPING at both ends
//                          (past the last item lands back on the
//                          first, and vice versa - unlike STEP/power
//                          below, which clamp instead); a short press
//                          enters the selected item's own screen below,
//                          or - for Exit - leaves the menu system
//                          entirely, straight back to RUN. Reopening
//                          MENU (the 2s+ hold from RUN) lands back on
//                          whichever item was last selected/visited,
//                          not always item 0. 20s of no activity falls
//                          back to RUN.
//   STEP                - shows the current step size (DIGIT_PLACE_LABELS,
//                          "1Hz".."1GHz" - 1Hz by default/on first boot).
//                          Rotating scrolls through all ten sizes
//                          (clamped, not wrapped); a short press confirms
//                          the one currently shown and returns to the
//                          MENU list (not all the way out to RUN - only
//                          MENU's own Exit item, or its timeout, does
//                          that) - this screen only ever sets
//                          s_stepIndex for the RUN screen's rotation to
//                          use afterward. 20s of no activity here falls
//                          back to MENU too (same as a confirm, keeping
//                          whatever step was already selected) - total
//                          idle time still eventually lands on RUN via
//                          MENU's own timeout, just not skipping the
//                          list on the way.
//   TX FREQUENCY        - lets you fine-tune TX_LO directly from inside
//                          the menu, without going back out to RUN:
//                          identical tuning behavior to the RUN screen's
//                          own live rotation (same grid-snap-by-current-
//                          step-size, same immediate recompute/save/
//                          push), just always targeting TX_LO regardless
//                          of PTT state, since this screen exists
//                          specifically to dial in TX_LO. A short press
//                          (or 20s idle) returns to the MENU list, same
//                          as every other MENU item below.
//   RX POWER/TX POWER   - shows the selected side's current output level
//                          (OLED_POWER_LABELS, "-4dBm".."+5dBm"). Rotating
//                          changes it by one step (clamped 0-3) - like
//                          RUN-screen frequency tuning, every detent is
//                          recomputed into registers, saved to flash,
//                          and pushed live immediately if that side is
//                          the one actually driving the synthesizer
//                          right now. A short press (or 20s of no
//                          activity) returns to the MENU list, same
//                          reasoning as STEP above.
//
// The single status LED (GPIO0) is normally driven by applyPttState()
// in the main sketch; while this menu is active it instead spells out
// "V" in Morse code (8 WPM, see updateMenuLed() below), so the front
// panel visibly shows "something other than normal RX/TX is happening"
// even without looking at the display. oledMenuIsActive() lets the main
// loop know when to stay out of the LED's way.
#pragma once
#include <stdio.h>
#include <math.h>   // llround() - used to snap RUN-screen tuning onto the step grid
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LiquidCrystal_I2C.h>
#include "lo_config.h"

#define PIN_ENC_A     3
#define PIN_ENC_B     4
#define PIN_ENC_SW    20
#define PIN_OLED_SDA  1
#define PIN_OLED_SCL  21

#define OLED_W        128
#define OLED_H        64
// SSD1306 OLED modules commonly expose 0x3C, occasionally 0x3D.
#define OLED_ADDR_A   0x3C
#define OLED_ADDR_B   0x3D
// 16x2 LCD I2C backpacks (PCF8574-based) commonly sit at 0x27, some at
// 0x3F - neither overlaps the OLED's range above, so a plain address
// probe is enough to tell the two apart without reading any registers.
#define LCD_ADDR_A    0x27
#define LCD_ADDR_B    0x3F
#define LCD_COLS      16
#define LCD_ROWS      2

enum DisplayType { DISP_NONE, DISP_OLED, DISP_LCD1602 };
static DisplayType s_dispType = DISP_NONE;
static LiquidCrystal_I2C *g_lcd = nullptr;

#define MENU_TIMEOUT_MS   20000UL
#define BTN_DEBOUNCE_MS   30UL
#define BTN_LONG_MS       600UL

// How fast the value blinks while actively being edited on STEP/RX
// POWER/TX POWER's own screens (ST_STEP/ST_RXPWR/ST_TXPWR) - toggled
// on a timer in oledMenuLoop() regardless of encoder/button activity,
// since nothing else would otherwise force a redraw while just sitting
// there deciding on a value. Once confirmed or timed out back to the
// MENU list, the same value is shown steady (not blinking) - see
// menuItemValue()/dispDrawMenu(), which never consults this.
#define VALUE_BLINK_MS    400UL
static unsigned long s_valueBlinkMs = 0;
static bool          s_valueBlinkOn = true;

// RUN-screen tuning (and STEP's grid-snap) clamps TX_LO/RX_LO to this
// range, in Hz - below 35MHz or above 3.00GHz is outside what this
// board/the ADF4351 is meant to be tuned to here, so a detent that
// would cross either edge just stops there instead of going further.
// 35MHz matches adf4351.h's own hard floor (adfComputeRegisters()
// rejects anything below 35000000UL outright), so this clamp can't
// let RUN-screen tuning walk up to a value the chip itself would then
// refuse anyway.
#define TUNE_FREQ_MIN_HZ   35000000.0     // 35 MHz
#define TUNE_FREQ_MAX_HZ   3000000000.0   // 3.00 GHz

static const char *OLED_POWER_LABELS[4] = { "-4dBm", "-1dBm", "+2dBm", "+5dBm" };

// Step-size picker for RUN-screen tuning: the STEP screen lets you pick
// one of these ten sizes (rotate to scroll, short press to confirm),
// and from then on rotating the encoder on the RUN screen changes
// whichever LO is active by exactly that much per detent - e.g. pick
// the hundreds-of-kHz step, confirm, and each detent on RUN is now
// +/-100kHz. s_stepIndex is an index into both arrays (0 = 1Hz step
// ... 9 = 1GHz step); everything here tops out at the 1GHz step because
// no field this firmware edits goes anywhere near 10GHz (TUNE_FREQ_MAX_HZ
// above is 3.00GHz), so a tens-of-GHz step would never move anything
// perceptibly - not worth a selectable entry.
static const double DIGIT_STEP_HZ[10] = {
  1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0,
  1000000.0, 10000000.0, 100000000.0, 1000000000.0
};
static const char *DIGIT_PLACE_LABELS[10] = {
  "1Hz", "10Hz", "100Hz", "1kHz", "10kHz", "100kHz",
  "1MHz", "10MHz", "100MHz", "1GHz"
};
#define STEP_COUNT   10
// NOTE on real-world resolution limits, read before relying on the
// finest places (1Hz/10Hz/100Hz):
//  - LOConfig stores frequency as `double` (see lo_config.h), which has
//    exact-Hz precision at any frequency this firmware handles, so
//    storage itself is no longer a resolution limit the way a 32-bit
//    float used to be.
//  - The ADF4351 itself only hits frequencies on a channel grid of
//    PFD/MOD Hz (PFD = REFIN/R-counter, MOD up to 4095), so even a
//    perfectly-stored target frequency gets rounded to the nearest
//    channel by adfComputeRegisters() - dialing by 10Hz can sometimes
//    produce the *same* RF output until enough detents cross one
//    channel's width. This isn't a firmware bug, it's the synthesizer's
//    real frequency resolution at whatever reference/R-counter is active.

static Adafruit_SSD1306 g_oled(OLED_W, OLED_H, &Wire, -1);

// ---- Shared frequency formatting (both OLED and LCD EDIT screens) -----
// Always the full "GG.MMM.KKK.HHH" (GHz.MHz.kHz.Hz) form, dots at fixed
// character indices 2/6/10, even when the GHz group is all zero - unlike
// formatFreqGrouped() below (used on the RUN screen, where there's no
// cursor to keep aligned and a shorter string reads better). Every Hz
// digit comes from an actual rounded integer Hz value, not truncated
// per-digit, so this doesn't share the old per-field decimal-places
// tradeoff - though the ADF4351's own channel-grid resolution still
// applies regardless of display (see the note above DIGIT_STEP_HZ).
static String formatFreqFull(double freqMhz) {
  uint64_t hz = (uint64_t)(freqMhz * 1000000.0 + 0.5);  // round to nearest Hz
  uint32_t ghz = (uint32_t)((hz / 1000000000ULL) % 100);
  uint32_t mhz = (uint32_t)((hz / 1000000ULL) % 1000);
  uint32_t khz = (uint32_t)((hz / 1000ULL) % 1000);
  uint32_t hzr = (uint32_t)(hz % 1000);
  char buf[15];
  snprintf(buf, sizeof(buf), "%02u.%03u.%03u.%03u", ghz, mhz, khz, hzr);
  return String(buf);
}

// Grouped digits plus a unit suffix, so the value reads as complete at
// a glance instead of a bare number: "xxx.xxx.xxx M" when the GHz group
// is suppressed, "xx.xxx.kkk G" when it isn't - the GHz case drops the
// trailing ".HHH" Hz group (full.substring(0,10) instead of the whole
// string), since neither the OLED's RUN line (label + freq + power all
// on one 128px/21-char row) nor the LCD's 16-column row has room for an
// 11-digit value, its unit, AND anything else alongside it. Both cases
// use a single letter ("M"/"G") rather than spelling out the unit, to
// buy back a few more columns for the frequency digits and power label.
// Shared by both displays' RUN screens - full Hz precision is still
// what the TUNE/EDIT screens show via formatFreqFull() itself,
// unaffected by this. Defined here (ahead of oledDrawRunImpl() below,
// which is the first of several callers) since there's no header
// prototype to forward-declare it with in a single-file, no-.cpp setup
// like this one.
static String formatFreqCompactUnit(double freqMhz) {
  String full = formatFreqFull(freqMhz);
  if (full.startsWith("00.")) {
    return full.substring(3) + " M";         // MMM.KKK.HHH M - 13 chars
  }
  return full.substring(0, 10) + " G";       // GG.MMM.KKK G  - 11 chars
}

// LCD-only frequency format for the RUN screen's row 1 (and, since it's
// the most complete/readable representation available, also for the
// MENU list's "Tx Frequency" row and the TX FREQUENCY edit screen's own
// value) - unlike formatFreqCompactUnit() (shared with the OLED, which
// has a tighter per-line budget once the T/R label and power are
// added), this one is sized to fill the LCD's 16 columns exactly either
// way, so the digit GROUPS always land in the same columns whether the
// value is in the GHz range (TX_LO, ~2.3-2.4GHz) or the MHz range
// (RX_LO, ~700MHz) - switching between them (e.g. on every PTT edge)
// never shifts the dots or the unit letter sideways:
//   "xx.xxx.xxx.xxx G"  - full precision (all 4 groups), GHz range
//   "   xxx.xxx.xxx M"  - 3 leading spaces where "xx." would have been,
//                         MHz range
// Both are exactly 16 characters: 14 (full) + " G" = 16, and 3 +
// 11 (full minus the 3-char "xx." group) + " M" = 16. Defined here
// (ahead of menuItemValue() below, one of its callers) since there's no
// header prototype to forward-declare it with in a single-file, no-.cpp
// setup like this one - same reasoning as formatFreqCompactUnit() just
// above.
static String lcdFormatFreqAligned(double freqMhz) {
  String full = formatFreqFull(freqMhz);           // 14 chars
  if (full.startsWith("00.")) {
    return "   " + full.substring(3) + " M";       // 3+11+2 = 16 chars
  }
  return full + " G";                              // 14+2   = 16 chars
}

// Right-pads s with leading spaces so it ends up exactly `width`
// characters wide with its content flush against the right edge -
// shared by oledDrawRunImpl() below (width = OLED_RUN_LINE_CHARS) and
// lcdRightAlign() further down (width = LCD_COLS), which just calls
// through to this with its own fixed width.
static String padRightAlign(const String &s, uint8_t width) {
  String out;
  for (int i = 0; i < (int)width - (int)s.length(); i++) out += ' ';
  out += s;
  return out;
}

// ---- Quadrature decode (interrupt-driven, full-step table) -----------
// Standard 4x table-based decoder: accumulates quarter-steps in the ISR,
// oledEncoderDelta() below divides by 4 to get whole detents. Robust to
// contact bounce because invalid/half transitions contribute 0.
static volatile int8_t  s_encAccum = 0;
static volatile uint8_t s_encPrevState = 0;
static const int8_t QEM_TABLE[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};

static void IRAM_ATTR oledEncIsr() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t state = (a << 1) | b;
  uint8_t idx = (s_encPrevState << 2) | state;
  s_encAccum += QEM_TABLE[idx & 0x0F];
  s_encPrevState = state;
}

// Returns whole detents moved since the last call (can be negative),
// keeping any leftover quarter-steps for next time.
static int8_t oledEncoderDelta() {
  noInterrupts();
  int8_t accum = s_encAccum;
  interrupts();
  int8_t detents = accum / 4;
  if (detents != 0) {
    noInterrupts();
    s_encAccum -= detents * 4;
    interrupts();
  }
  return detents;
}

// ---- Push switch: non-blocking debounce + short/long classification --
enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };

static int  s_btnLastRaw        = HIGH;
static int  s_btnStable         = HIGH;
static unsigned long s_btnLastRawChangeMs = 0;
static unsigned long s_btnPressedAtMs     = 0;

static BtnEvent oledPollButton() {
  int raw = digitalRead(PIN_ENC_SW);
  unsigned long now = millis();
  if (raw != s_btnLastRaw) {
    s_btnLastRaw = raw;
    s_btnLastRawChangeMs = now;
  }
  if ((now - s_btnLastRawChangeMs) > BTN_DEBOUNCE_MS && raw != s_btnStable) {
    s_btnStable = raw;
    if (s_btnStable == LOW) {
      s_btnPressedAtMs = now;           // press started
    } else {
      unsigned long held = now - s_btnPressedAtMs; // just released
      return (held >= BTN_LONG_MS) ? BTN_LONG : BTN_SHORT;
    }
  }
  return BTN_NONE;
}

// Entering STEP needs a deliberate, continuous hold rather than a quick
// release-based "long press" - short presses were proving unreliable to
// land consistently, so this fires once, WITHOUT waiting for release,
// the instant the button has been held STEP_ENTER_HOLD_MS continuously
// - same pattern as the main sketch's PROGRAM-button 5s hold, just a
// shorter hold here (2s, not 5s) since this is reached far more often
// in normal use than that one-off WiFi-enable gesture. Reuses
// oledPollButton()'s own debounced s_btnStable/s_btnPressedAtMs rather
// than tracking a second, separate debounce state.
#define STEP_ENTER_HOLD_MS 2000UL
static bool s_stepHoldFired = false;

static bool oledButtonHeldForStep() {
  unsigned long now = millis();
  if (s_btnStable == HIGH) {
    s_stepHoldFired = false;   // released - re-arm for the next press
    return false;
  }
  if (!s_stepHoldFired && (now - s_btnPressedAtMs) >= STEP_ENTER_HOLD_MS) {
    s_stepHoldFired = true;
    return true;
  }
  return false;
}

// ---- Menu state ---------------------------------------------------------
// ST_MENU is the 5-item list (including Exit), entered directly from
// ST_RUN on a 2s+ continuous hold (see the top comment block);
// ST_STEP/ST_TXFREQ/ST_RXPWR/ST_TXPWR are that list's first four items'
// own screens, each reached by selecting it from ST_MENU and each
// returning back to ST_MENU (not ST_RUN) on confirm or timeout - only
// MENU's own Exit item (the 5th), or MENU's own timeout, goes back to
// ST_RUN. See MENU's top-comment note for the full reasoning.
enum MenuState { ST_RUN, ST_MENU, ST_STEP, ST_TXFREQ, ST_RXPWR, ST_TXPWR };

// MENU's items, in display/selection order - s_menuIndex is just an
// index into this array (and into the short switch in oledMenuLoop()'s
// ST_MENU case that decides which state a selection moves into).
static const char *MENU_ITEMS[5] = { "Tuning Steps", "Tx Frequency", "Rx Power", "Tx Power", "Exit" };
#define MENU_ITEM_COUNT 5
static uint8_t       s_menuIndex = 0;

static MenuState     s_state = ST_RUN;
// Which step size RUN-screen rotation currently uses - index into
// DIGIT_STEP_HZ/DIGIT_PLACE_LABELS, set by the STEP screen below.
// Defaults to 0 (1Hz) on boot, per spec.
static uint8_t       s_stepIndex = 0;
static unsigned long s_lastActivityMs = 0;
static bool          s_dirty = true;       // forces first draw
static uint8_t       s_ledPin = 0;
static char          s_flashMsg[24] = "";
static unsigned long s_flashUntilMs = 0;

static bool oledMenuIsActive() { return s_state != ST_RUN; }

// ---- Menu-active LED indicator: "V" in Morse code, 8 WPM --------------
// While the local menu is open (any state other than ST_RUN), the
// status LED spells out "V" (dit-dit-dit-dah), repeating, instead of a
// plain blink - so the front panel still visibly shows "something other
// than normal RX/TX is happening" (see oledMenuIsActive() just above),
// just with a distinct, recognizable pattern rather than a generic
// blink. Same PARIS-timed dit/dah scheme and non-blocking state-machine
// approach as the main sketch's own "AP" WiFi-indicator LED (see
// updateApLed() in the .ino) - defined independently here (own struct/
// table/state, not shared with that one) since oled_menu.h is included
// ahead of that code in the concatenated sketch, so there's nothing
// there yet to reuse.
#define MENU_MORSE_WPM      8
#define MENU_MORSE_UNIT_MS (1200UL / MENU_MORSE_WPM)

struct MenuMorseSeg { bool on; uint8_t units; };
static const MenuMorseSeg MENU_MORSE_V[] = {
  { true,  1 }, { false, 1 },   // dit
  { true,  1 }, { false, 1 },   // dit
  { true,  1 }, { false, 1 },   // dit
  { true,  3 }, { false, 7 },   // dah, then word gap before repeat
};
#define MENU_MORSE_V_LEN (sizeof(MENU_MORSE_V) / sizeof(MENU_MORSE_V[0]))

static bool          s_menuMorseRunning = false;
static uint8_t       s_menuMorseIdx = 0;
static unsigned long s_menuMorseSegStartMs = 0;

// Drives s_ledPin through MENU_MORSE_V for as long as the menu is
// active; resets cleanly back to the start of "V" (and the LED off) the
// instant the menu isn't active, so leaving and re-entering the menu
// always starts the message fresh at the first dit rather than
// resuming mid-sequence - same reset-on-gate-fail reasoning as
// updateApLed()'s own s_morseRunning handling.
static void updateMenuLed() {
  if (s_state == ST_RUN) {
    s_menuMorseRunning = false;
    digitalWrite(s_ledPin, LOW);
    return;
  }
  unsigned long now = millis();
  if (!s_menuMorseRunning) {
    s_menuMorseRunning = true;
    s_menuMorseIdx = 0;
    s_menuMorseSegStartMs = now;
    digitalWrite(s_ledPin, MENU_MORSE_V[0].on ? HIGH : LOW);
    return;
  }
  const MenuMorseSeg &seg = MENU_MORSE_V[s_menuMorseIdx];
  if (now - s_menuMorseSegStartMs >= (unsigned long)seg.units * MENU_MORSE_UNIT_MS) {
    s_menuMorseIdx = (s_menuMorseIdx + 1) % MENU_MORSE_V_LEN;
    s_menuMorseSegStartMs = now;
    digitalWrite(s_ledPin, MENU_MORSE_V[s_menuMorseIdx].on ? HIGH : LOW);
  }
}

static void oledShowFlash(const char *msg, unsigned long ms) {
  strncpy(s_flashMsg, msg, sizeof(s_flashMsg) - 1);
  s_flashMsg[sizeof(s_flashMsg) - 1] = '\0';
  s_flashUntilMs = millis() + ms;
  s_dirty = true;
}

// ---- Drawing --------------------------------------------------------
// Header text/size are fixed, so its on-screen width is known at compile
// time: 10 chars * 12px/char (textSize(2)) = 120px, fitting inside the
// 128px-wide display with a 4px margin on each side - no clipping, seen
// confirmed on actual hardware (the earlier 11-char "ADF4351 Osc" at
// textSize(2) was 132px, 4px over, and right-aligning it put the whole
// overflow on the left edge, visibly clipping the "A"). Centered here so
// the margin is even on both sides. Note: Adafruit_GFX's setTextSize()
// only takes integers, so a non-integer "25% smaller" size isn't an
// option - textSize(2) (this) or textSize(1) (half as tall) are the
// only two real choices, and textSize(2) stays legible as a header.
#define OLED_HEADER_TEXT   "ADF4351 LO"
#define OLED_HEADER_CHARS  10
static void oledDrawRunImpl(const LOConfig &cfg, int pttLevel, bool apRunning,
                             const String &apIp) {
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(2);
  g_oled.setCursor((OLED_W - OLED_HEADER_CHARS * 12) / 2, 0);
  g_oled.print(F(OLED_HEADER_TEXT));
  g_oled.drawFastHLine(4, 16, OLED_W - 8, SSD1306_WHITE);

  // "T"/"R" + compact grouped+unit frequency + power, all on one line,
  // right-aligned so the shorter G-suffix line and the longer M-suffix
  // line both end flush against the same right edge rather than
  // leaving a ragged left edge. Worst case (M-suffix, since "M" is one
  // char wider after grouping than "G"'s shorter digit count) is
  // 1 + 13 + 1 + 5 = 20 chars = 120px, inside the 128px-wide display at
  // textSize(1)'s 6px/char - OLED_RUN_LINE_CHARS below is that width.
  #define OLED_RUN_LINE_CHARS 20
  g_oled.setTextSize(1);
  g_oled.setCursor(0, 18);
  g_oled.println(padRightAlign(String("T") + formatFreqCompactUnit(cfg.tx_freq_mhz) +
                                " " + OLED_POWER_LABELS[cfg.tx_power], OLED_RUN_LINE_CHARS));
  g_oled.println(padRightAlign(String("R") + formatFreqCompactUnit(cfg.rx_freq_mhz) +
                                " " + OLED_POWER_LABELS[cfg.rx_power], OLED_RUN_LINE_CHARS));
  g_oled.print(F("Ref  "));
  g_oled.print(cfg.ref_freq_mhz, 3);
  g_oled.println(F(" MHz"));
  g_oled.print(F("State: "));
  g_oled.println(pttLevel == LOW ? F("TX (keyed)") : F("RX (idle)"));
  if (apRunning) {
    g_oled.print(F("AP "));
    g_oled.println(apIp);
  } else {
    g_oled.println(F("AP off"));
  }
  g_oled.display();
}

// Current value for one MENU_ITEMS[] index, shared by both displays'
// MENU screens so each item's row/line can show its live setting
// alongside its name, not just the name: index 0 (Tuning Steps) reads
// s_stepIndex, 1 (Tx Frequency) reads cfg.tx_freq_mhz at the SAME full
// Hz precision and fixed 16-char layout the RUN screen and TX
// FREQUENCY's own edit screen already use (lcdFormatFreqAligned()) -
// the list row shows the real, complete value rather than a rounded
// summary, 2/3 (Rx/Tx Power) read cfg's power fields. Index 4 (Exit)
// has no value of its own - returns "" (falls through the switch),
// same as any index this function doesn't recognise.
static String menuItemValue(uint8_t idx, const LOConfig &cfg) {
  switch (idx) {
    case 0: return String(DIGIT_PLACE_LABELS[s_stepIndex]);
    case 1: return lcdFormatFreqAligned(cfg.tx_freq_mhz);
    case 2: return String(OLED_POWER_LABELS[cfg.rx_power]);
    case 3: return String(OLED_POWER_LABELS[cfg.tx_power]);
  }
  return String("");
}

// MENU screen: a plain 5-line list (MENU_ITEMS, including Exit), the
// selected one prefixed with ">" rather than inverted/highlighted -
// simpler to draw than a per-character invert block, and still fits
// (5 lines at 8px each - textSize(1)'s own glyph height, no inter-line
// gap - starting at y=24 tops out at exactly 64px, within the 64px-tall
// display) without needing to scroll off-screen. The 4-item list used
// 10px line spacing with room to spare; 5 items no longer fit at that
// spacing (24 + 5*10 = 74, 10px past the bottom edge), hence the
// tighter 8px here.
// Each line also shows that item's current value (menuItemValue()) -
// blank for Exit - name padded to a fixed column width so the values
// all line up: "Tuning Steps"/"Tx Frequency" are the longest names (12
// chars) and "100kHz"/"100MHz" the longest value (6 chars), so worst
// case "> " + 12 + " " + 6 = 21 chars, exactly the 128px/6px-per-char
// budget at textSize(1).
#define MENU_NAME_COLWIDTH 12
#define MENU_LINE_PX        8
static void oledDrawMenuImpl(const LOConfig &cfg) {
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(2);
  g_oled.setCursor(0, 0);
  g_oled.print(F("MENU"));

  g_oled.setTextSize(1);
  for (uint8_t i = 0; i < MENU_ITEM_COUNT; i++) {
    String name = MENU_ITEMS[i];
    while ((int)name.length() < MENU_NAME_COLWIDTH) name += ' ';
    g_oled.setCursor(0, 24 + i * MENU_LINE_PX);
    g_oled.print(i == s_menuIndex ? F("> ") : F("  "));
    g_oled.print(name);
    g_oled.print(F(" "));
    g_oled.println(menuItemValue(i, cfg));
  }
  g_oled.display();
}

// RX/TX POWER screen: shares one implementation parameterized by
// isTx, since the two only differ in title and which cfg field they
// read - shows the current level large, lets you scroll through the
// four presets, confirm with a short press. Mirrors oledDrawStepImpl()'s
// layout for a consistent feel across every MENU item screen.
static void oledDrawPowerImpl(bool isTx, uint8_t powerLevel) {
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(2);
  g_oled.setCursor(0, 0);
  g_oled.print(isTx ? F("TX POWER") : F("RX POWER"));

  g_oled.setTextSize(1);
  g_oled.setCursor(0, 24);
  g_oled.print(F("Level: "));
  // Blinks while this screen is actively being edited (s_valueBlinkOn,
  // toggled on a timer in oledMenuLoop()'s ST_RXPWR/ST_TXPWR case) -
  // clearDisplay() above already blanked this whole frame, so skipping
  // the print on the "off" phase is all blinking needs here. Once
  // confirmed, you're back on the MENU list showing the same value
  // steady (oledDrawMenuImpl(), which never consults this).
  if (s_valueBlinkOn) g_oled.println(OLED_POWER_LABELS[powerLevel]);
  g_oled.setCursor(0, 40);
  g_oled.print(F("rotate = change"));
  g_oled.setCursor(0, 48);
  g_oled.print(F("press  = confirm"));
  g_oled.display();
}

// STEP screen: shows the current step size large, lets you scroll
// through all ten sizes, confirm with a short press. Nothing else is
// edited here - see the top comment block.
static void oledDrawStepImpl() {
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(2);
  g_oled.setCursor(0, 0);
  g_oled.print(F("STEP"));

  g_oled.setTextSize(1);
  g_oled.setCursor(0, 24);
  g_oled.print(F("Size: "));
  // Blinks while actively being edited - same reasoning as
  // oledDrawPowerImpl()'s own value line above.
  if (s_valueBlinkOn) g_oled.println(DIGIT_PLACE_LABELS[s_stepIndex]);
  g_oled.setCursor(0, 40);
  g_oled.print(F("rotate = change"));
  g_oled.setCursor(0, 48);
  g_oled.print(F("press  = confirm"));
  g_oled.display();
}

// TX FREQUENCY screen: shows TX_LO's current value at full Hz precision
// (formatFreqFull() - the same grouped "GG.MMM.KKK.HHH" form the LCD's
// own version of this screen uses below), lets you nudge it live by the
// currently-selected step size, confirm with a short press. Mirrors
// oledDrawStepImpl()/oledDrawPowerImpl()'s layout for a consistent feel.
static void oledDrawTxFreqImpl(double txFreqMhz) {
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(2);
  g_oled.setCursor(0, 0);
  g_oled.print(F("TX FREQ"));

  g_oled.setTextSize(1);
  g_oled.setCursor(0, 24);
  // Blinks while actively being edited - same reasoning as
  // oledDrawStepImpl()/oledDrawPowerImpl()'s own value line.
  if (s_valueBlinkOn) g_oled.println(formatFreqFull(txFreqMhz));
  g_oled.setCursor(0, 40);
  g_oled.print(F("rotate = change"));
  g_oled.setCursor(0, 48);
  g_oled.print(F("press  = confirm"));
  g_oled.display();
}

// ---- LCD1602 (PCF8574 I2C backpack) drawing --------------------------
// Only 2 lines x 16 chars, nowhere near enough to show TX/RX/Ref/state/
// AP at once the way the OLED does, so the RUN screen only ever shows
// whichever LO is actually driving the synthesizer right now - RX_LO
// while idle, TX_LO the instant PTT keys down - never both at once.
// Fixed, simple layout (no page-cycling, nothing that changes on its
// own over time): row 1 is just the frequency; row 2 is the TX/RX
// label flush left and the power level flush right, with blank space
// between. Ref/AP aren't shown on the LCD at all - the OLED (or the
// WiFi portal) is where those live.
//
// Writes exactly (LCD_COLS - col) characters starting at (col,row),
// padding with spaces or truncating as needed, so stale characters from
// a longer previous line never linger - the PCF8574 backpack has no
// per-character or per-line clear, only a full lcd.clear(), which would
// flicker visibly on every redraw if used here instead.
static void lcdPrintPadded(uint8_t col, uint8_t row, const String &s) {
  String t = s;
  int width = LCD_COLS - col;
  while ((int)t.length() < width) t += ' ';
  if ((int)t.length() > width) t = t.substring(0, width);
  g_lcd->setCursor(col, row);
  g_lcd->print(t);
}

// Right-pads s with leading spaces so it ends up exactly LCD_COLS wide
// with its content flush against the right edge - just padRightAlign()
// (defined above, next to formatFreqCompactUnit()) at the LCD's fixed
// width.
static String lcdRightAlign(const String &s) {
  return padRightAlign(s, LCD_COLS);
}

// Builds one LCD_COLS-wide line with `left` flush against the left
// edge and `right` flush against the right edge, spaces filling
// whatever's between - used for every MENU-related screen's top row
// (item number on the left, item name on the right): the MENU list's
// own row 1, and each of STEP/RX POWER/TX POWER's own row 1 (see
// lcdDrawMenuImpl()/lcdDrawStepImpl()/lcdDrawPowerImpl() below).
static String lcdLeftRightLine(const String &left, const String &right) {
  String out = left;
  while ((int)out.length() < LCD_COLS - (int)right.length()) out += ' ';
  out += right;
  return out;
}

static void lcdDrawRunImpl(const LOConfig &cfg, int pttLevel, bool apRunning,
                            const String &apIp) {
  // Whichever LO is actually active right now - PTT LOW means keyed/TX,
  // so the synthesizer is on TX_LO at that instant; otherwise it's
  // sitting on RX_LO. This mirrors applyPttState()'s own choice of
  // register set in the .ino, so this always matches what the ADF4351
  // is actually doing, not a static TX/RX pair.
  // Backlight is turned on once at boot in oledMenuInit(), but it's also
  // reasserted here on every redraw (cheap - it's just one more bit in
  // the same I2C byte the library already writes) so it can't be left
  // dark by a glitched write to the PCF8574 expander or a stray
  // noBacklight() anywhere - it's meant to always be on, full stop, with
  // no firmware path that ever turns it off.
  g_lcd->backlight();
  g_lcd->noCursor(); g_lcd->noBlink(); // only the EDIT freq screen ever turns these on
  bool keyed = (pttLevel == LOW);
  double activeFreq  = keyed ? cfg.tx_freq_mhz : cfg.rx_freq_mhz;
  uint8_t activePwr  = keyed ? cfg.tx_power    : cfg.rx_power;
  (void)apRunning; (void)apIp; // not shown on this display

  // Row 1: just the frequency, in lcdFormatFreqAligned()'s fixed
  // 16-char layout - already exactly LCD_COLS wide either way, so no
  // extra right-align padding is needed here.
  lcdPrintPadded(0, 0, lcdFormatFreqAligned(activeFreq));

  // Row 2: TX/RX label flush left, power level flush right, blank
  // space in between - built as one 16-char string (left part, then
  // spaces padding out to leave exactly enough room for the power
  // label, then the power label itself) so a single lcdPrintPadded()
  // call lays down the whole row and clears any stale characters left
  // over from a longer previous line.
  String label = keyed ? "TX" : "RX";
  String power = OLED_POWER_LABELS[activePwr];
  while ((int)label.length() < LCD_COLS - (int)power.length()) label += ' ';
  lcdPrintPadded(0, 1, label + power);
}

// STEP screen on the LCD: item number (1) flush left / item name
// ("Tuning Steps") flush right on row 1 - same header format as every
// other MENU-related screen (see lcdLeftRightLine()). Row 2 is the
// step size, right-aligned, BLINKING while this screen is actively
// being edited (s_valueBlinkOn, toggled on a timer in oledMenuLoop()'s
// ST_STEP case) - printing nothing (lcdPrintPadded clears the row to
// blanks regardless) on the "off" phase is what makes it blink; once
// confirmed, you're back on the MENU list showing the same value
// steady (see lcdDrawMenuImpl() below, which never blinks).
static void lcdDrawStepImpl() {
  g_lcd->backlight(); // always on - see the comment in lcdDrawRunImpl()
  g_lcd->noCursor(); g_lcd->noBlink(); // no hardware cursor needed here

  lcdPrintPadded(0, 0, lcdLeftRightLine(String(1), MENU_ITEMS[0]));
  lcdPrintPadded(0, 1, s_valueBlinkOn
                         ? lcdRightAlign(String(DIGIT_PLACE_LABELS[s_stepIndex]))
                         : String(""));
}

// TX FREQUENCY screen on the LCD: item number (2) flush left / item
// name ("Tx Frequency") flush right on row 1, same header format as
// STEP above. Row 2 is TX_LO's value in the same fixed 16-char aligned
// form the RUN screen itself uses (lcdFormatFreqAligned() - already
// exactly LCD_COLS wide, so lcdRightAlign() is a no-op here but kept
// for consistency with the other edit screens), blinking while
// actively being edited - identical reasoning to lcdDrawStepImpl()'s
// own row 2.
static void lcdDrawTxFreqImpl(double txFreqMhz) {
  g_lcd->backlight();
  g_lcd->noCursor(); g_lcd->noBlink();

  lcdPrintPadded(0, 0, lcdLeftRightLine(String(2), MENU_ITEMS[1]));
  lcdPrintPadded(0, 1, s_valueBlinkOn
                         ? lcdRightAlign(lcdFormatFreqAligned(txFreqMhz))
                         : String(""));
}

// MENU screen on the LCD: only 2 rows, nowhere near enough to show all
// 5 items at once the way the OLED does, so row 1 shows the
// currently-selected item's number flush left and its name flush right
// (lcdLeftRightLine()) - still reads as "item N of 4" while scrolling,
// without the number and name crowding together. Row 2 is that item's
// current value, right-aligned and steady (never blinking - this is
// just browsing the list, not actively editing a value).
static void lcdDrawMenuImpl(const LOConfig &cfg) {
  g_lcd->backlight();
  g_lcd->noCursor(); g_lcd->noBlink();

  lcdPrintPadded(0, 0, lcdLeftRightLine(String(s_menuIndex + 1), MENU_ITEMS[s_menuIndex]));
  lcdPrintPadded(0, 1, lcdRightAlign(menuItemValue(s_menuIndex, cfg)));
}

// RX/TX POWER screen on the LCD: item number (3/4) flush left / item
// name ("Rx Power"/"Tx Power") flush right on row 1, same header
// format as STEP above. Row 2 is the power level, right-aligned,
// blinking while actively being edited - identical reasoning to
// lcdDrawStepImpl()'s own row 2.
static void lcdDrawPowerImpl(bool isTx, uint8_t powerLevel) {
  g_lcd->backlight();
  g_lcd->noCursor(); g_lcd->noBlink();

  lcdPrintPadded(0, 0, lcdLeftRightLine(String(isTx ? 4 : 3), MENU_ITEMS[isTx ? 3 : 2]));
  lcdPrintPadded(0, 1, s_valueBlinkOn
                         ? lcdRightAlign(String(OLED_POWER_LABELS[powerLevel]))
                         : String(""));
}

// Longer flash messages (e.g. "Saved. Restarting", 18 chars) get split
// across both lines rather than silently truncated to 16.
static void lcdDrawFlashImpl(const char *msg) {
  g_lcd->backlight(); // always on - see the comment in lcdDrawRunImpl()
  g_lcd->noCursor(); g_lcd->noBlink(); // only the EDIT freq screen ever turns these on
  String s(msg);
  lcdPrintPadded(0, 0, lcdRightAlign(s.substring(0, LCD_COLS)));
  // Row 2 is left blank for flash messages too - a message longer than
  // 16 chars (e.g. "Saved. Restarting") loses its overflow rather than
  // wrapping onto row 2.
  lcdPrintPadded(0, 1, String(""));
}

// ---- Display dispatch --------------------------------------------------
// Every call site below branches once on s_dispType instead of the menu
// logic needing to know which physical display is present.
static void dispDrawRun(const LOConfig &cfg, int pttLevel, bool apRunning, const String &apIp) {
  if (s_dispType == DISP_OLED) oledDrawRunImpl(cfg, pttLevel, apRunning, apIp);
  else if (s_dispType == DISP_LCD1602) lcdDrawRunImpl(cfg, pttLevel, apRunning, apIp);
}
static void dispDrawStep() {
  if (s_dispType == DISP_OLED) oledDrawStepImpl();
  else if (s_dispType == DISP_LCD1602) lcdDrawStepImpl();
}
static void dispDrawMenu(const LOConfig &cfg) {
  if (s_dispType == DISP_OLED) oledDrawMenuImpl(cfg);
  else if (s_dispType == DISP_LCD1602) lcdDrawMenuImpl(cfg);
}
static void dispDrawPower(bool isTx, uint8_t powerLevel) {
  if (s_dispType == DISP_OLED) oledDrawPowerImpl(isTx, powerLevel);
  else if (s_dispType == DISP_LCD1602) lcdDrawPowerImpl(isTx, powerLevel);
}
static void dispDrawTxFreq(double txFreqMhz) {
  if (s_dispType == DISP_OLED) oledDrawTxFreqImpl(txFreqMhz);
  else if (s_dispType == DISP_LCD1602) lcdDrawTxFreqImpl(txFreqMhz);
}
// For code that writes directly into g_oled's buffer outside the draw
// functions above (just the confirm/save flash in oledMenuLoop) and
// needs the OLED's buffer actually pushed to the panel - the LCD
// backpack has no equivalent buffering, so this is a no-op there.
static void dispFlush() {
  if (s_dispType == DISP_OLED) g_oled.display();
}
// Renders s_flashMsg immediately (not through the timed overlay check
// in oledMenuLoop below) - used right before the Save&Restart reboot,
// where there's no further loop() iteration left to let the normal
// per-loop flash handling draw it.
static void dispDrawFlashNow() {
  if (s_dispType == DISP_OLED) {
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 24);
    g_oled.println(s_flashMsg);
    g_oled.display();
  } else if (s_dispType == DISP_LCD1602) {
    lcdDrawFlashImpl(s_flashMsg);
  }
}

// Draws a fixed, PERMANENT two-line screen - used only for the board's
// dedicated "AP-only" boot mode (see the .ino's top comment and
// setup()'s own bootApRequested handling): when the encoder push
// switch is held through power-up, the .ino skips normal operation
// entirely (no ADF4351 programming, no PTT handling, no local menu -
// oledMenuLoop() itself is never called at all while in this mode), so
// unlike every other screen in this file, nothing ever redraws over or
// clears this one - it's drawn once, right after the AP comes up, and
// stays on screen for as long as the board stays in this mode (until
// the next reboot). line1/line2 are plain C strings so the .ino can
// pass AP_SSID (a #define in web_portal.h) directly.
static void dispDrawApOnly(const char *line1, const char *line2) {
  if (s_dispType == DISP_OLED) {
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 0);
    g_oled.println(line1);
    g_oled.println(line2);
    g_oled.display();
  } else if (s_dispType == DISP_LCD1602) {
    g_lcd->backlight();
    g_lcd->noCursor(); g_lcd->noBlink();
    lcdPrintPadded(0, 0, String(line1));
    lcdPrintPadded(0, 1, String(line2));
  }
}

// ---- I2C auto-detect ---------------------------------------------------
// A bare address probe (STOP with no data) is enough to tell the OLED
// and LCD apart since their address ranges don't overlap - no need to
// read back chip-specific registers.
static bool i2cPingAddr(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

// ---- Setup -----------------------------------------------------------
// Returns true if a supported display (OLED or LCD1602) responded on
// the I2C bus. Tries the OLED addresses first, then the LCD backpack
// addresses; whichever answers sets s_dispType and is the only one used
// from then on - if both happened to be wired at once, only the OLED
// would be driven.
static bool oledMenuInit(uint8_t ledPin) {
  s_ledPin = ledPin;

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  s_encPrevState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), oledEncIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), oledEncIsr, CHANGE);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  if (i2cPingAddr(OLED_ADDR_A) || i2cPingAddr(OLED_ADDR_B)) {
    uint8_t addr = i2cPingAddr(OLED_ADDR_A) ? OLED_ADDR_A : OLED_ADDR_B;
    if (g_oled.begin(SSD1306_SWITCHCAPVCC, addr)) {
      s_dispType = DISP_OLED;
    }
  }
  if (s_dispType == DISP_NONE) {
    uint8_t lcdAddr = 0;
    if (i2cPingAddr(LCD_ADDR_A)) lcdAddr = LCD_ADDR_A;
    else if (i2cPingAddr(LCD_ADDR_B)) lcdAddr = LCD_ADDR_B;
    if (lcdAddr != 0) {
      g_lcd = new LiquidCrystal_I2C(lcdAddr, LCD_COLS, LCD_ROWS);
      g_lcd->init();
      g_lcd->backlight();
      s_dispType = DISP_LCD1602;
    }
  }

  if (s_dispType == DISP_OLED) {
    g_oled.clearDisplay();
    g_oled.setTextSize(1);
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setCursor(0, 0);
    g_oled.println(F("ADF4351_Osc"));
    g_oled.println(F("starting..."));
    g_oled.display();
  } else if (s_dispType == DISP_LCD1602) {
    g_lcd->clear();
    g_lcd->setCursor(0, 0);
    g_lcd->print(F("ADF4351_Osc"));
    g_lcd->setCursor(0, 1);
    g_lcd->print(F("starting..."));
  }
  return s_dispType != DISP_NONE;
}

// ---- Per-loop update ---------------------------------------------------
// cfg        - the live, currently-applied config. In ST_RUN, rotating
//              the encoder writes straight into whichever LO is active
//              (tx_freq_mhz/rx_freq_mhz), recomputes both register sets,
//              saves the whole thing to flash, and pushes the active
//              side's registers live - all on every single detent (see
//              the ST_RUN case below). Read-only in ST_STEP.
// pttLevel   - current PTT GPIO level, for the status screen and to
//              pick which LO is "active" for RUN-screen tuning
// apRunning  - whether the web portal AP is currently up
// apIp       - AP IP as a String, for the status screen
static void oledMenuLoop(LOConfig &cfg, int pttLevel, bool apRunning, const String &apIp) {
  int8_t delta = oledEncoderDelta();
  BtnEvent btn = oledPollButton();
  bool stepHeld = oledButtonHeldForStep(); // fires once per 2s+ hold, no release needed
  unsigned long now = millis();

  // LED: spell "V" in Morse while the local menu is active, otherwise
  // leave it alone (the main sketch drives it directly from PTT state
  // in RUN mode) - see updateMenuLed() above.
  updateMenuLed();

  // A flash message (e.g. "Invalid values!") takes over the display
  // briefly regardless of state, then clears itself.
  if (s_flashUntilMs != 0) {
    if (now < s_flashUntilMs) {
      dispDrawFlashNow();
      return;
    } else {
      s_flashUntilMs = 0;
      s_dirty = true;
    }
  }

  switch (s_state) {
    case ST_RUN: {
      // Rotating right here, with no press needed, tunes whichever LO
      // is actually active right now (same side the RUN screen itself
      // is showing) by the current step size - live, and saved every
      // single detent per spec. Each detent SNAPS the frequency onto
      // the selected step's grid (nearest whole multiple of that step,
      // in Hz) rather than just adding stepMhz to whatever float value
      // was already there - e.g. picking the 1kHz step should tune
      // ONLY the kHz place, leaving the finer 100Hz/10Hz/1Hz digits at
      // zero, not drifting along with whatever sub-kHz remainder the
      // value happened to carry from earlier (finer-step) tuning. Also
      // relies on LOConfig's frequency fields being `double`, not
      // `float` (see lo_config.h) - a 32-bit float can't round-trip
      // exact Hz values this large without a little noise of its own,
      // which used to show up as the same kind of unwanted drift in
      // the finer digits even though the grid math here was exact.
      if (delta != 0) {
        bool txActive = (pttLevel == LOW);
        double *target = txActive ? &cfg.tx_freq_mhz : &cfg.rx_freq_mhz;
        double prev = *target;
        double stepHz = DIGIT_STEP_HZ[s_stepIndex];
        double curHz = *target * 1000000.0;
        int64_t steps = (int64_t)llround(curHz / stepHz) + delta;
        double newHz = (double)steps * stepHz;
        if (newHz < TUNE_FREQ_MIN_HZ) newHz = TUNE_FREQ_MIN_HZ;
        if (newHz > TUNE_FREQ_MAX_HZ) newHz = TUNE_FREQ_MAX_HZ;
        *target = newHz / 1000000.0;
        // Validate/recompute ONLY the side being tuned - using the full
        // loConfigRecompute() here would also re-validate the *other*
        // side's untouched value and reject this detent if that other
        // side happened to be sitting somewhere invalid, even though
        // nothing about it just changed. See loConfigRecomputeSide()'s
        // comment in lo_config.h.
        if (loConfigRecomputeSide(cfg, txActive)) {
          loConfigSave(cfg);
          adfWriteAllRegisters(txActive ? cfg.tx_regs : cfg.rx_regs);
        } else {
          *target = prev;
          loConfigRecomputeSide(cfg, txActive);  // restore valid registers for prev
          oledShowFlash("Invalid value!", 800);
        }
        s_dirty = true;
      }

      if (stepHeld) {
        // Open MENU - a deliberate 5s continuous hold (see
        // oledButtonHeldForStep()), not a quick press/release, since
        // short presses weren't registering reliably. btn's own
        // BTN_SHORT/BTN_LONG (release-based, ~600ms) stay unused on RUN.
        // Deliberately does NOT reset s_menuIndex here - the menu
        // reopens on whichever item was last selected/visited, not
        // always back at item 0, so going straight back in to nudge
        // the same setting again doesn't need a re-scroll past
        // everything before it every single time.
        // Returns immediately rather than falling into the RUN redraw
        // below: switch(s_state) only re-evaluates on the NEXT call, so
        // without this return the code below would still run this same
        // pass, redraw the RUN screen (since s_dirty is now true) and
        // clear s_dirty right back to false - leaving MENU's own draw
        // in the ST_MENU case nothing to trigger on, so it never
        // actually appeared even though the state had changed (only the
        // LED's s_state != ST_RUN blink gave it away).
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      // Status screen only needs to refresh when PTT changes, a
      // detent just tuned something (s_dirty), or on a gentle fixed
      // cadence otherwise; the main sketch calling this every loop
      // would otherwise hammer the I2C bus for no reason.
      static int s_lastDrawnPtt = -1;
      static unsigned long s_lastRunDraw = 0;
      if (pttLevel != s_lastDrawnPtt || s_dirty || (now - s_lastRunDraw) > 1000) {
        s_lastDrawnPtt = pttLevel;
        s_lastRunDraw = now;
        dispDrawRun(cfg, pttLevel, apRunning, apIp);
        s_dirty = false;
      }
      return;
    }

    case ST_STEP: {
      // Ticks the value's blink on a fixed timer regardless of
      // encoder/button activity - otherwise nothing would force a
      // redraw while just sitting here deciding on a size.
      if (now - s_valueBlinkMs >= VALUE_BLINK_MS) {
        s_valueBlinkMs = now;
        s_valueBlinkOn = !s_valueBlinkOn;
        s_dirty = true;
      }

      if (delta != 0) {
        int16_t d = (int16_t)s_stepIndex + delta;
        if (d < 0) d = 0;
        if (d > STEP_COUNT - 1) d = STEP_COUNT - 1;
        s_stepIndex = (uint8_t)d;
        s_lastActivityMs = now;
        s_dirty = true;
      }

      if (btn == BTN_SHORT) {
        // Confirm the step shown and return to the MENU list (not all
        // the way out to RUN) - nothing else changes here, s_stepIndex
        // is already set above. Lets you set one item then immediately
        // check/adjust another without re-doing the hold gesture; MENU
        // itself is how you actually leave (its own Exit item, or its
        // timeout). btn == BTN_LONG is reserved for a future use - no-op
        // for now.
        // Returns immediately (same reasoning as RUN's own stepHeld
        // handling above): without this, the dispDrawStep() call just
        // below would still run this same pass since s_dirty is now
        // true, clearing it right back to false before ST_MENU's own
        // case ever gets a turn to draw.
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (now - s_lastActivityMs > MENU_TIMEOUT_MS) {
        // No activity - fall back to MENU (same effect as a confirm),
        // keeping whatever step was already selected. Not RUN directly:
        // MENU has its own timeout, so total idle time still eventually
        // lands back on RUN, just via the list rather than skipping it.
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (s_dirty) { dispDrawStep(); s_dirty = false; }
      return;
    }

    case ST_TXFREQ: {
      // Same grid-snap tuning mechanics as ST_RUN's own rotation
      // handling above (identical math, identical immediate recompute/
      // save/push-if-active policy) - the only difference is this
      // screen always targets TX_LO, regardless of PTT state, since
      // that's the whole point of a dedicated "Tx Frequency" menu item
      // (RUN-screen rotation instead follows PTT, tuning whichever LO
      // is actually active at that instant).
      if (now - s_valueBlinkMs >= VALUE_BLINK_MS) {
        s_valueBlinkMs = now;
        s_valueBlinkOn = !s_valueBlinkOn;
        s_dirty = true;
      }

      if (delta != 0) {
        double prev = cfg.tx_freq_mhz;
        double stepHz = DIGIT_STEP_HZ[s_stepIndex];
        double curHz = cfg.tx_freq_mhz * 1000000.0;
        int64_t steps = (int64_t)llround(curHz / stepHz) + delta;
        double newHz = (double)steps * stepHz;
        if (newHz < TUNE_FREQ_MIN_HZ) newHz = TUNE_FREQ_MIN_HZ;
        if (newHz > TUNE_FREQ_MAX_HZ) newHz = TUNE_FREQ_MAX_HZ;
        cfg.tx_freq_mhz = newHz / 1000000.0;
        if (loConfigRecomputeSide(cfg, true)) {
          loConfigSave(cfg);
          if (pttLevel == LOW) adfWriteAllRegisters(cfg.tx_regs);
        } else {
          cfg.tx_freq_mhz = prev;
          loConfigRecomputeSide(cfg, true);  // restore valid registers for prev
          oledShowFlash("Invalid value!", 800);
        }
        s_lastActivityMs = now;
        s_dirty = true;
      }

      if (btn == BTN_SHORT) {
        // Confirm and return to the MENU list - same as every other
        // MENU item's own confirm handling.
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (now - s_lastActivityMs > MENU_TIMEOUT_MS) {
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (s_dirty) { dispDrawTxFreq(cfg.tx_freq_mhz); s_dirty = false; }
      return;
    }

    case ST_MENU: {
      if (delta != 0) {
        // Wraps at both ends (rotating past the last item lands back
        // on the first, and vice versa) rather than stopping - unlike
        // STEP/power, which clamp instead, since a short, fixed-length
        // top-level list reads more like a dial you can spin all the
        // way around than a value you'd want to hit a hard limit on.
        int16_t d = ((int16_t)s_menuIndex + delta) % MENU_ITEM_COUNT;
        if (d < 0) d += MENU_ITEM_COUNT;
        s_menuIndex = (uint8_t)d;
        s_lastActivityMs = now;
        s_dirty = true;
      }

      if (btn == BTN_SHORT) {
        // Enter the selected item's own screen - index order matches
        // MENU_ITEMS[] (defined next to s_menuIndex above): 0 = Tuning
        // Steps, 1 = Tx Frequency, 2 = Rx Power, 3 = Tx Power, 4 = Exit
        // (the only item that leaves the menu system entirely, straight
        // back to RUN - everything else now returns to THIS list on
        // confirm, not all the way out, see
        // ST_STEP/ST_TXFREQ/ST_RXPWR/ST_TXPWR's own confirm handling
        // below).
        s_lastActivityMs = now;
        s_dirty = true;
        // Always starts visible (not mid-"off" blink) on entry to an
        // editable item's screen - purely cosmetic, so the value isn't
        // momentarily invisible the instant you step into it.
        s_valueBlinkOn = true;
        s_valueBlinkMs = now;
        switch (s_menuIndex) {
          case 0: s_state = ST_STEP;   break;
          case 1: s_state = ST_TXFREQ; break;
          case 2: s_state = ST_RXPWR;  break;
          case 3: s_state = ST_TXPWR;  break;
          case 4: s_state = ST_RUN;    break;
        }
      }

      if (now - s_lastActivityMs > MENU_TIMEOUT_MS) {
        s_state = ST_RUN;
        s_dirty = true;
      }

      if (s_dirty) { dispDrawMenu(cfg); s_dirty = false; }
      return;
    }

    case ST_RXPWR:
    case ST_TXPWR: {
      // Shared between both sides - only which cfg field/register set
      // gets touched differs, same pattern as oledDrawPowerImpl()'s
      // isTx parameter. Rotating adjusts the level live, every detent:
      // recomputed, saved to flash, and pushed to the synthesizer
      // immediately if that side is the one actually driving it right
      // now - mirrors RUN-screen frequency tuning's own "every detent"
      // policy, just on a 0-3 integer instead of a Hz grid.
      bool isTx = (s_state == ST_TXPWR);
      uint8_t *power = isTx ? &cfg.tx_power : &cfg.rx_power;

      // Same fixed-timer blink tick as ST_STEP above.
      if (now - s_valueBlinkMs >= VALUE_BLINK_MS) {
        s_valueBlinkMs = now;
        s_valueBlinkOn = !s_valueBlinkOn;
        s_dirty = true;
      }

      if (delta != 0) {
        int16_t p = (int16_t)(*power) + delta;
        if (p < 0) p = 0;
        if (p > 3) p = 3;
        uint8_t prev = *power;
        *power = (uint8_t)p;
        if (loConfigRecomputeSide(cfg, isTx)) {
          loConfigSave(cfg);
          bool txActive = (pttLevel == LOW);
          if (isTx == txActive) {
            adfWriteAllRegisters(isTx ? cfg.tx_regs : cfg.rx_regs);
          }
        } else {
          *power = prev;
          loConfigRecomputeSide(cfg, isTx);
          oledShowFlash("Invalid value!", 800);
        }
        s_lastActivityMs = now;
        s_dirty = true;
      }

      if (btn == BTN_SHORT) {
        // Confirm and return to the MENU list - same as STEP's own
        // confirm above, not straight out to RUN. Returns immediately,
        // same reasoning as ST_STEP's own confirm: otherwise
        // dispDrawPower() just below would still run this pass and
        // clear s_dirty before ST_MENU's case gets a turn to draw.
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (now - s_lastActivityMs > MENU_TIMEOUT_MS) {
        // Same reasoning as STEP's timeout above: back to MENU, not
        // RUN directly, so MENU's own timeout is what ultimately gets
        // you all the way out if left idle.
        s_state = ST_MENU;
        s_lastActivityMs = now;
        s_dirty = true;
        return;
      }

      if (s_dirty) { dispDrawPower(isTx, *power); s_dirty = false; }
      return;
    }
  }
}
