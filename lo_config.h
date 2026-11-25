// lo_config.h
// ---------------------------------------------------------------------
// Flash-backed (NVS) storage for the LO configuration. The register
// sets are computed once, when the user saves the config page, and
// stored alongside the raw values - so normal run mode never has to
// repeat the INT/FRAC/MOD math, it just loads and writes six uint32_t
// values per side.
#pragma once
#include <Preferences.h>
#include "adf4351.h"

#define LO_CONFIG_MAGIC 0x41444631UL  // 'ADF1'

// Converts a MHz value (double) to whole Hz (uint32_t) WITHOUT ever
// routing through a 32-bit *signed* cast. `(uint32_t)(someDouble)`
// looks safe but on this RISC-V ESP32-C3 toolchain the compiler-emitted
// double->uint32 conversion saturates at INT32_MAX (2,147,483,647)
// instead of giving the correct value for anything from ~2.1475GHz up
// to 4.29GHz - confirmed by the local panel being unable to tune past
// exactly 2.147483647 GHz. Rounding through `llround()` first forces a
// double->int64 conversion (a path every toolchain implements
// correctly, since it's ubiquitous), and int64_t comfortably covers
// any Hz value this board will ever see before the final narrowing
// truncation to uint32_t, which is safe once the value is known to
// fit (freq_hz is always well under 4.3GHz here).
static inline uint32_t hzFromMhz(double mhz) {
  return (uint32_t)(uint64_t)llround(mhz * 1e6);
}

// tx_freq_mhz/rx_freq_mhz/ref_freq_mhz are `double`, not `float` - a
// 32-bit float only carries ~7 significant decimal digits, which near
// TX_LO's ~2.4GHz (a 4-5 digit MHz value) leaves just ~100-300Hz of
// real resolution: tuning with a coarse step (e.g. 10kHz or 100kHz on
// the local panel - see oled_menu.h's STEP screen) would snap the
// value exactly onto that step's grid in integer Hz, only to have it
// come back out of 32-bit float storage with a little noise in the
// finer digits, which then showed up as the display's 1Hz/10Hz/100Hz
// places drifting even though no finer digit was ever actually being
// adjusted. `double` (~15-17 significant digits) has more than enough
// headroom for exact-Hz precision at any frequency this firmware
// handles, so the stored value round-trips cleanly. NOTE: this changes
// sizeof(LOConfig), so loConfigLoad()'s size check below will reject
// any config saved by an older build and fall back to defaults - a
// one-time reset back to factory TX_LO/RX_LO/Ref after flashing this.
struct LOConfig {
  uint32_t magic;
  double   tx_freq_mhz;
  double   rx_freq_mhz;
  double   ref_freq_mhz;
  uint8_t  tx_power;      // 0-3
  uint8_t  rx_power;      // 0-3
  uint32_t tx_regs[6];
  uint32_t rx_regs[6];
};

// Sensible factory defaults: 10m-band pair (28.5 MHz IF), 25 MHz
// reference, -1dBm on both outputs - used only if nothing has been
// saved to flash yet (first boot).
static inline void loConfigDefaults(LOConfig &cfg) {
  cfg.magic = LO_CONFIG_MAGIC;
  cfg.tx_freq_mhz = 2371.5f;
  cfg.rx_freq_mhz = 711.0f;
  cfg.ref_freq_mhz = 25.0f;
  cfg.tx_power = 1;
  cfg.rx_power = 1;
  uint32_t refin_hz = hzFromMhz(cfg.ref_freq_mhz);
  adfComputeRegisters(refin_hz, hzFromMhz(cfg.tx_freq_mhz),
                       cfg.tx_power, true, 0, false, cfg.tx_regs);
  adfComputeRegisters(refin_hz, hzFromMhz(cfg.rx_freq_mhz),
                       0, false, cfg.rx_power, true, cfg.rx_regs);
}

static inline bool loConfigLoad(LOConfig &cfg) {
  Preferences prefs;
  prefs.begin("lo_cfg", true);
  size_t n = prefs.getBytesLength("cfg");
  bool ok = false;
  if (n == sizeof(LOConfig)) {
    prefs.getBytes("cfg", &cfg, sizeof(LOConfig));
    ok = (cfg.magic == LO_CONFIG_MAGIC);
  }
  prefs.end();
  if (!ok) loConfigDefaults(cfg);
  return ok;
}

static inline void loConfigSave(const LOConfig &cfg) {
  Preferences prefs;
  prefs.begin("lo_cfg", false);
  prefs.putBytes("cfg", &cfg, sizeof(LOConfig));
  prefs.end();
}

// Recomputes both register sets from the raw frequency/power fields -
// call this right after the web form values are parsed, before saving.
static inline bool loConfigRecompute(LOConfig &cfg) {
  uint32_t refin_hz = hzFromMhz(cfg.ref_freq_mhz);
  bool okTx = adfComputeRegisters(refin_hz, hzFromMhz(cfg.tx_freq_mhz),
                                   cfg.tx_power, true, 0, false, cfg.tx_regs);
  bool okRx = adfComputeRegisters(refin_hz, hzFromMhz(cfg.rx_freq_mhz),
                                   0, false, cfg.rx_power, true, cfg.rx_regs);
  cfg.magic = LO_CONFIG_MAGIC;
  return okTx && okRx;
}

// Recomputes and validates ONLY the TX or RX side, leaving the other
// side's stored registers untouched. The local panel's live tuning
// (RUN-screen rotate, Rx/Tx Power screens) uses this instead of
// loConfigRecompute() above, so that adjusting one side is never
// rejected because of whatever the *other* side currently happens to
// be sitting at - only the web portal's full Save & Apply needs both
// sides valid at once, since it's replacing both in a single form
// submit.
static inline bool loConfigRecomputeSide(LOConfig &cfg, bool isTx) {
  uint32_t refin_hz = hzFromMhz(cfg.ref_freq_mhz);
  bool ok;
  if (isTx) {
    ok = adfComputeRegisters(refin_hz, hzFromMhz(cfg.tx_freq_mhz),
                              cfg.tx_power, true, 0, false, cfg.tx_regs);
  } else {
    ok = adfComputeRegisters(refin_hz, hzFromMhz(cfg.rx_freq_mhz),
                              0, false, cfg.rx_power, true, cfg.rx_regs);
  }
  cfg.magic = LO_CONFIG_MAGIC;
  return ok;
}
