// adf4351.h
// ---------------------------------------------------------------------
// ADF4351 register math + bit-bang control for the ADF4351_Osc LO board.
//
// Bus pins (ESP32-C3-SuperMini), confirmed against the schematic:
//   GPIO6  -> CLK
//   GPIO7  -> DATA
//   GPIO10 -> LE
//   CE     -> no GPIO; held high by the board's own R16 pull-up (3.3V)
//
// Register field positions are ported from the datasheet-accurate
// community reference (dfannin/adf4351 Arduino library), converted to
// pure integer arithmetic (no floats/BigNumber) and extended here with
// the AUX (RFOUTB) enable/power/select fields, since this firmware
// needs independent control of which output is active - RFOUTA feeds
// the TX_LO buffer, RFOUTB feeds the RX_LO buffer, and only one side
// is meaningful at a time since both outputs share one VCO/N-divider.
#pragma once
#include <Arduino.h>

#define PIN_ADF_CLK   6
#define PIN_ADF_DATA  7
#define PIN_ADF_LE    10

// PFD frequency should stay at or below this for clean fractional-N
// operation; used to auto-select the R-counter for whatever reference
// frequency the user enters in the config page.
#define ADF_PFD_MAX_HZ   25000000UL
#define ADF_PFD_MIN_HZ   10000UL

// ---- Low level: bit-bang one 32-bit register into the ADF4351 --------
static inline void adfWriteRegister(uint32_t value) {
  for (int8_t i = 31; i >= 0; i--) {
    digitalWrite(PIN_ADF_CLK, LOW);
    digitalWrite(PIN_ADF_DATA, (value >> i) & 0x1UL ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_ADF_CLK, HIGH);   // ADF4351 shifts DATA in on CLK rising edge
    delayMicroseconds(1);
  }
  digitalWrite(PIN_ADF_CLK, LOW);
  digitalWrite(PIN_ADF_LE, HIGH);      // LE rising edge latches the 32 bits
  delayMicroseconds(5);
  digitalWrite(PIN_ADF_LE, LOW);
}

// Writes a full 6-register set in the datasheet-required R5->R0 order.
static inline void adfWriteAllRegisters(const uint32_t regs[6]) {
  for (int8_t i = 5; i >= 0; i--) {
    adfWriteRegister(regs[i]);
    delay(1);
  }
}

static inline void adfBusInit() {
  pinMode(PIN_ADF_DATA, OUTPUT);
  pinMode(PIN_ADF_CLK, OUTPUT);
  pinMode(PIN_ADF_LE, OUTPUT);
  digitalWrite(PIN_ADF_DATA, LOW);
  digitalWrite(PIN_ADF_CLK, LOW);
  digitalWrite(PIN_ADF_LE, LOW);
  // CE is not driven from the ESP32-C3 at all - R16 holds it high.
}

// ---- Register calculation ---------------------------------------------
// Computes one complete 6-register ADF4351 configuration.
//
//   refin_hz      - reference input frequency actually present at REFIN
//   freq_hz       - desired RFOUTA/RFOUTB frequency (both outputs sit
//                   at the same frequency; which one matters is chosen
//                   by enable_primary / enable_aux below)
//   power_level   - 0-3, primary (RFOUTA) output power (-4/-1/+2/+5 dBm)
//   enable_primary- true to enable RFOUTA (used for the TX_LO register set)
//   aux_power_level - 0-3, AUX (RFOUTB) output power
//   enable_aux    - true to enable RFOUTB (used for the RX_LO register set)
//   out_regs      - uint32_t[6] receiving R0..R5
//
// Returns true on success, false if the requested frequency/reference
// combination falls outside what the chip (or this calculator) supports.
static inline bool adfComputeRegisters(uint32_t refin_hz, uint32_t freq_hz,
                                        uint8_t power_level, bool enable_primary,
                                        uint8_t aux_power_level, bool enable_aux,
                                        uint32_t out_regs[6]) {
  if (freq_hz < 35000000UL || freq_hz > 4294000000UL) return false;
  if (refin_hz < 1000000UL || refin_hz > 250000000UL) return false;

  // Auto-select the R-counter (1-1023) so the phase detector frequency
  // stays within a sane range regardless of what reference the user enters.
  uint16_t rcounter = 1;
  while (refin_hz / rcounter > ADF_PFD_MAX_HZ && rcounter < 1023) rcounter++;
  uint32_t pfd_freq = refin_hz / rcounter;
  if (pfd_freq < ADF_PFD_MIN_HZ || pfd_freq > ADF_PFD_MAX_HZ) return false;

  // 1. Output divider: keep the VCO fundamental inside 2.2-4.4 GHz
  uint32_t localosc_ratio = 2200000000UL / freq_hz;
  uint32_t outdiv = 1;
  uint8_t  rfDivSel = 0;
  while (outdiv <= localosc_ratio && outdiv <= 64) {
    outdiv *= 2;
    rfDivSel++;
  }
  // uint64_t, not uint32_t: freq_hz*outdiv can reach ~4.3-4.4GHz (e.g.
  // freq_hz just above 2^31 Hz with outdiv=2), which is ABOVE what a
  // 32-bit unsigned product can hold (UINT32_MAX is only ~4.295GHz) -
  // computing this in 32-bit arithmetic silently wrapped to a small/
  // zero value right at that boundary, which is exactly what was
  // rejecting every tune attempt above ~2.1475GHz (2^31 Hz) as
  // "invalid": the wrapped vco_freq fed bogus numbers into the
  // prescaler/N_INT math below, which then failed its own (correct)
  // range checks on what looked like a completely different, invalid
  // frequency.
  uint64_t vco_freq = (uint64_t)freq_hz * outdiv;

  // 2. Prescaler: 4/5 below 3.6 GHz fundamental, 8/9 above
  uint8_t prescaler = (vco_freq > 3600000000ULL) ? 1 : 0;

  // 3. INT / FRAC / MOD (mod fixed at 1000, reduced by GCD if possible)
  uint32_t mod = 1000;
  uint64_t total = (uint64_t)vco_freq * mod;
  uint64_t n_scaled = total / pfd_freq;
  uint64_t rem = total % pfd_freq;
  if (rem * 2 >= pfd_freq) n_scaled++;        // round to nearest

  // Check the N_INT range BEFORE truncating to uint16_t - casting
  // first would silently wrap a too-large value instead of catching it.
  uint64_t n_int64 = n_scaled / mod;
  if (n_int64 > 65535) return false;
  uint16_t n_int = (uint16_t)n_int64;
  uint32_t frac  = (uint32_t)(n_scaled % mod);

  if (frac != 0) {
    uint32_t a = frac, b = mod, t;
    while (b) { t = a % b; a = b; b = t; }
    if (a > 1) { frac /= a; mod /= a; }
  }

  if (prescaler == 0 && (n_int < 23 || n_int > 65535)) return false;
  if (prescaler == 1 && (n_int < 75 || n_int > 65535)) return false;
  if (mod < 2 || mod > 4095) return false;

  // 4. Pack registers (field positions per ADF4351 datasheet)
  uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;

  r0 |= (frac & 0xFFFUL) << 3;
  r0 |= ((uint32_t)n_int & 0xFFFFUL) << 15;

  r1 |= 1UL;
  r1 |= (mod & 0xFFFUL) << 3;
  r1 |= 1UL << 15;                            // phase = 1 (datasheet recommended)
  r1 |= ((uint32_t)prescaler & 0x1UL) << 27;

  r2 |= 2UL;
  r2 |= 1UL << 6;                             // PD polarity = positive
  if (frac == 0) {
    r2 |= 1UL << 7;                           // LDP, int-N
    r2 |= 1UL << 8;                           // LDF, int-N
  }
  r2 |= 7UL << 9;                             // charge pump current
  r2 |= ((uint32_t)rcounter & 0x3FFUL) << 14;
  // RD2refdouble / RD1Rdiv2 left at 0 (no doubler, no /2)

  r3 |= 3UL;
  r3 |= (150UL & 0xFFFUL) << 3;               // clock divider value
  if (frac == 0) {
    r3 |= 1UL << 21;                          // charge cancel, int-N
    r3 |= 1UL << 22;                          // ABP, int-N
  }
  r3 |= 1UL << 23;                            // band select clock mode

  r4 |= 4UL;
  r4 |= ((uint32_t)power_level & 0x3UL) << 3;      // RFOUTA power
  r4 |= (enable_primary ? 1UL : 0UL) << 5;         // RFOUTA enable
  r4 |= ((uint32_t)aux_power_level & 0x3UL) << 6;  // AUX (RFOUTB) power
  r4 |= (enable_aux ? 1UL : 0UL) << 8;             // AUX enable
  r4 |= 0UL << 9;                                  // AUX select: 0 = divided (matches RFOUTA)
  r4 |= (80UL & 0xFFUL) << 12;                     // band select clock divider
  r4 |= ((uint32_t)rfDivSel & 0x7UL) << 20;        // RF divider select
  r4 |= 1UL << 23;                                 // feedback select = fundamental

  r5 |= 5UL;
  r5 |= 3UL << 19;                            // reserved, must be set to 11
  r5 |= 1UL << 22;                            // LD pin mode = digital lock detect

  out_regs[0] = r0; out_regs[1] = r1; out_regs[2] = r2;
  out_regs[3] = r3; out_regs[4] = r4; out_regs[5] = r5;
  return true;
}
