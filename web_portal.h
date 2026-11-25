// web_portal.h
// ---------------------------------------------------------------------
// AP-mode captive portal: SSID ADF4351_OSC, open (no password) - anyone
// in range can join and reach the config page directly, no credential
// to share or type on a phone. Serves a single
// configuration page (TX_LO, RX_LO, reference, per-side output power),
// and on Save & Apply computes the ADF4351 register sets, writes the
// whole config to flash, and restarts the board so it comes back up
// in normal run mode with the new values already active.
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "lo_config.h"

#define AP_SSID "ADF4351_OSC"
// No AP_PASS - softAP() is called with no password below, so the
// network is open. Protection for the config page itself (if any is
// ever wanted) would need to be added at the HTTP layer, not WiFi
// WPA2, since that's what gated who could even reach the portal.

static WebServer   portalServer(80);
static DNSServer   portalDns;
static const byte  DNS_PORT = 53;

static const char *POWER_LABELS[4] = { "-4 dBm", "-1 dBm", "+2 dBm", "+5 dBm" };

static String powerOptions(uint8_t selected) {
  String s;
  for (uint8_t i = 0; i < 4; i++) {
    s += "<option value='" + String(i) + "'";
    if (i == selected) s += " selected";
    s += ">" + String(POWER_LABELS[i]) + "</option>";
  }
  return s;
}

static String buildConfigPage(const LOConfig &cfg) {
  String h;
  h += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" AP_SSID " setup</title><style>";
  h += "body{font-family:sans-serif;max-width:360px;margin:0 auto;padding:16px;color:#222}";
  h += "h1{font-size:18px;margin:4px 0}";
  h += "p.sub{font-size:13px;color:#666;margin:4px 0 20px}";
  h += "label{font-size:13px;display:block;margin:12px 0 4px}";
  h += "input,select{width:100%;box-sizing:border-box;padding:8px;font-size:14px;";
  h += "border:1px solid #ccc;border-radius:6px}";
  h += "button{width:100%;margin-top:20px;padding:10px;font-size:15px;font-weight:bold;";
  h += "background:#1f6feb;color:#fff;border:none;border-radius:6px}";
  h += ".note{font-size:12px;color:#888;text-align:center;margin-top:10px}";
  h += ".cur{background:#f4f4f4;border-radius:6px;padding:8px 10px;font-size:12px;color:#555;margin:10px 0 16px}";
  h += ".sect{font-size:11px;font-weight:bold;color:#888;text-transform:uppercase;margin:16px 0 4px}";
  h += "</style></head><body>";
  h += "<h1>Transverter LO setup</h1>";
  h += "<p class='sub'>Set the local oscillator frequencies and output levels, then apply.</p>";

  h += "<div class='cur'><b>Currently saved:</b><br>";
  h += "TX_LO " + String(cfg.tx_freq_mhz, 3) + " MHz &middot; ";
  h += "RX_LO " + String(cfg.rx_freq_mhz, 3) + " MHz &middot; ";
  h += "Ref " + String(cfg.ref_freq_mhz, 3) + " MHz &middot; ";
  h += "TX " + String(POWER_LABELS[cfg.tx_power]) + " &middot; ";
  h += "RX " + String(POWER_LABELS[cfg.rx_power]) + "</div>";

  h += "<form method='POST' action='/save'>";
  h += "<div class='sect'>Local oscillator</div>";
  h += "<label>TX_LO frequency (MHz)</label>";
  h += "<input type='number' step='0.001' name='tx_freq' value='" + String(cfg.tx_freq_mhz, 3) + "' required>";
  h += "<label>RX_LO frequency (MHz)</label>";
  h += "<input type='number' step='0.001' name='rx_freq' value='" + String(cfg.rx_freq_mhz, 3) + "' required>";

  // Reference frequency is intentionally NOT editable from this page -
  // removed per request. cfg.ref_freq_mhz still exists and is still
  // read/saved as part of LOConfig (and still shown, read-only, in the
  // "Currently saved" summary above), it's just no longer a form field
  // here: handlePortalSave() below only touches it when a "ref_freq"
  // POST field is present, so omitting the input here leaves whatever
  // value is already on flash untouched on every Save & apply.

  h += "<div class='sect'>Output power</div>";
  h += "<label>TX_LO level</label><select name='tx_power'>" + powerOptions(cfg.tx_power) + "</select>";
  h += "<label>RX_LO level</label><select name='rx_power'>" + powerOptions(cfg.rx_power) + "</select>";

  h += "<button type='submit'>Save &amp; apply</button>";
  h += "</form>";
  h += "<p class='note'>Registers are calculated, saved to flash, and the board restarts into normal operation with Wi-Fi off.</p>";
  h += "</body></html>";
  return h;
}

static const char *SAVED_PAGE =
  "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Saved</title><style>body{font-family:sans-serif;max-width:360px;margin:60px auto;"
  "text-align:center;color:#222}</style></head><body>"
  "<h2>Saved</h2><p>New configuration written to flash.<br>The board is restarting now.</p></body></html>";

static const char *ERROR_PAGE =
  "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Invalid values</title><style>body{font-family:sans-serif;max-width:360px;margin:60px auto;"
  "text-align:center;color:#222}</style></head><body>"
  "<h2>Could not apply</h2><p>One or more values are outside what the ADF4351/reference "
  "chain can produce. Go back and check the frequencies.</p>"
  "<p><a href='/'>Back</a></p></body></html>";

static void handlePortalRoot() {
  LOConfig cfg;
  loConfigLoad(cfg);
  portalServer.send(200, "text/html", buildConfigPage(cfg));
}

static void handlePortalSave() {
  LOConfig cfg;
  loConfigLoad(cfg);

  if (portalServer.hasArg("tx_freq"))  cfg.tx_freq_mhz  = portalServer.arg("tx_freq").toFloat();
  if (portalServer.hasArg("rx_freq"))  cfg.rx_freq_mhz  = portalServer.arg("rx_freq").toFloat();
  if (portalServer.hasArg("ref_freq")) cfg.ref_freq_mhz = portalServer.arg("ref_freq").toFloat();
  if (portalServer.hasArg("tx_power")) cfg.tx_power = (uint8_t)portalServer.arg("tx_power").toInt();
  if (portalServer.hasArg("rx_power")) cfg.rx_power = (uint8_t)portalServer.arg("rx_power").toInt();

  if (!loConfigRecompute(cfg)) {
    portalServer.send(200, "text/html", ERROR_PAGE);
    return;
  }

  loConfigSave(cfg);
  portalServer.send(200, "text/html", SAVED_PAGE);
  delay(1500);
  ESP.restart();
}

// Catch-all for anything not explicitly routed - redirect to the
// config page. Used for requests this list doesn't specifically know
// about; the OS-specific captive-portal probes below are handled
// separately (see registerCaptivePortalProbes()), since iOS in
// particular needs a direct 200 response rather than a redirect to be
// recognized as "there is a captive portal here" and reliably avoid
// disconnecting the station after its validation check fails.
static void handlePortalNotFound() {
  portalServer.sendHeader("Location", "/", true);
  portalServer.send(302, "text/plain", "");
}

// iOS/macOS's Captive Network Assistant probes
// captive.apple.com/hotspot-detect.html (and a couple of related
// paths) right after joining a network. If it doesn't get a response
// it specifically recognizes, newer iOS versions mark the network as
// "no internet, unverified" and will actively disconnect after a
// validation timeout - which is what was producing the consistent
// ~30s drop seen only on iPhone, never on Android (Android's handling
// is more lenient about an ambiguous response). Serving the actual
// config page directly (200 OK) rather than a redirect is the
// documented fix: it reads as "a captive portal is present and this
// is it," which iOS accepts and opens in its own mini in-app browser
// without later deciding the network is bad.
static void registerCaptivePortalProbes() {
  const char *probePaths[] = {
    "/hotspot-detect.html",      // iOS / macOS
    "/library/test/success.html", // older iOS / macOS
    "/generate_204",              // Android
    "/gen_204",                   // Android (newer)
    "/connecttest.txt",           // Windows
    "/ncsi.txt",                  // Windows
    "/canonical.html",            // Firefox
  };
  for (size_t i = 0; i < sizeof(probePaths) / sizeof(probePaths[0]); i++) {
    portalServer.on(probePaths[i], HTTP_GET, handlePortalRoot);
  }
}

// Returns true only if the AP genuinely came up - check this rather
// than assuming success, since WiFi.softAP() fails silently on bad
// inputs (e.g. a too-short WPA2 password) unless its return value is
// actually inspected.
static bool webPortalBegin() {
  // NOTE: custom IP (192.168.29.73) temporarily removed - isolating a
  // bug where WiFi.softAPConfig() right after softAP() was making the
  // SSID invisible on a scan, confirmed by a bare WiFi.softAP() test
  // (no config call) working fine. Using the ESP32 default AP address
  // for now; revisit the custom IP as its own isolated step later.
  WiFi.mode(WIFI_AP);
  // Disable WiFi modem sleep - a well-known cause of an AP flickering
  // in and out even while the rest of the firmware stays healthy.
  WiFi.setSleep(false);
  // ~50% of max transmit power (19.5dBm -> 17dBm is a 3dB cut, which
  // is exactly half the power: 89mW -> ~50mW). Reduced from max after
  // the board was running hot - trading a bit of range/margin for
  // less heat and lower current draw.
  WiFi.setTxPower(WIFI_POWER_17dBm);
  // Open network - passing no password (or NULL) makes softAP() skip
  // WPA2 entirely, instead of merely using a blank one (which ESP32
  // WiFi still rejects as too short).
  bool apOk = WiFi.softAP(AP_SSID);
  if (!apOk) {
    Serial.println(F("softAP() FAILED to start - check SSID validity"));
    return false;
  }
  delay(100);
  portalDns.start(DNS_PORT, "*", WiFi.softAPIP());

  portalServer.on("/", HTTP_GET, handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  registerCaptivePortalProbes();
  portalServer.onNotFound(handlePortalNotFound);
  portalServer.begin();
  return true;
}

// Called repeatedly from loop() while in AP/config mode.
static void webPortalLoop() {
  portalDns.processNextRequest();
  portalServer.handleClient();
  // Small yield so the single-core ESP32-C3's WiFi housekeeping (beacon
  // frames etc.) reliably gets scheduled alongside our own handling.
  delay(2);
}
