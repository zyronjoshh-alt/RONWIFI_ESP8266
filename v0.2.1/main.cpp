/* ==========================================================
   RONwifi ESP8266 Firmware - Main Entry Point

   Boot sequence:
   1. Hardware init, LittleFS mount
   2. A0+3V3 detection (10s window) for forced AP mode
   3. Load /config.json (or /config.backup.json)
      - Missing/corrupt: enter AP mode (Setup)
   4. WiFi connect (static or DHCP)
   5. MikroTik ping (informational)
   6. HTTP server up
   7. Main loop: handle HTTP, coinslot, watchdogs

   v0 SCOPE: Offline mode (no backend, no MQTT)
   ========================================================== */

#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "wifi_manager.h"
#include "coinslot.h"
#include "mikrotik_client.h"
#include "http_server.h"
#include "setup_ap.h"

// Auto-restart tracking
static uint32_t s_bootTimeMs = 0;

// Check if A0 is shorted to 3.3V for 10 seconds → force AP mode
static bool shouldForceAP() {
  Serial.println(F("[boot] Checking A0+3V3 reset trigger (10s)..."));
  pinMode(A0, INPUT);
  uint32_t start = millis();
  uint32_t highSince = 0;
  while (millis() - start < 10000) {
    int v = analogRead(A0);
    // ESP8266 A0 max = 1024 at ~1.0V (with internal divider on NodeMCU it scales to 3.3V)
    // A value > 900 indicates ~3V input
    if (v > 900) {
      if (highSince == 0) highSince = millis();
      else if (millis() - highSince > 9000) {
        Serial.println(F("[boot] A0 held high 9s+ → forcing AP mode"));
        return true;
      }
    } else {
      highSince = 0;
    }
    delay(100);
    yield();
  }
  Serial.println(F("[boot] A0 check complete, normal boot"));
  return false;
}

// Check if currently "busy" — active coin session in progress
static bool isBusy() {
  CoinState s = coinslotGetState();
  return (s == CS_ARMED || s == CS_COUNTING || s == CS_FINALIZING);
}

// ----------------------------------------------------------
// SETUP
// ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F("RONwifi ESP8266 Firmware " FIRMWARE_VERSION));
  Serial.println(F("====================================="));

  s_bootTimeMs = millis();

  // Mount filesystem
  if (!LittleFS.begin()) {
    Serial.println(F("[boot] LittleFS mount failed, formatting..."));
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println(F("[boot] LittleFS mounted"));

  // Check A0+3V3 reset trigger (10s window)
  // Only check if config exists — first boot always goes to AP mode naturally
  if (configExists()) {
    if (shouldForceAP()) {
      Serial.println(F("[boot] Entering forced AP setup mode"));
      runSetupMode();  // blocks forever, reboots on save
      return;
    }
  }

  // Try to load config
  if (!configLoad()) {
    Serial.println(F("[boot] No valid config found, entering AP setup mode"));
    runSetupMode();  // blocks forever
    return;
  }

  if (g_config.loaded_from_backup) {
    Serial.println(F("[boot] WARNING: main config was bad, loaded from backup"));
  }

  // Load rates
  ratesLoad();

  // Initialize coin slot (GPIOs safe, inhibit asserted)
  coinslotInit();

  // WiFi connect
  if (!wifiConnectStation()) {
    Serial.println(F("[boot] Initial WiFi connect failed, will retry in loop"));
    // Continue anyway — HTTP server starts, retries happen in loop
  }

  // MikroTik ping (informational)
  if (wifiIsConnected()) {
    mikrotikPing();
  }

  // Start HTTP server
  httpServerInit();

  // Done
  Serial.println(F("[boot] ========= READY ========="));
  Serial.print(F("[boot] vendo_id=")); Serial.println(g_config.vendo_id);
  Serial.print(F("[boot] vendo_name=")); Serial.println(g_config.vendo_name);
  Serial.print(F("[boot] IP=")); Serial.println(WiFi.localIP().toString());
  Serial.print(F("[boot] admin URL: http://")); Serial.print(WiFi.localIP().toString()); Serial.println(F("/admin"));
}

// ----------------------------------------------------------
// LOOP
// ----------------------------------------------------------
void loop() {
  // Serve HTTP requests
  httpServerLoop();

  // Coin slot state updates
  coinslotLoop();

  // Coin session inactivity timeout
  static CoinState s_lastState = CS_IDLE;
  static uint32_t s_armTimeMs = 0;
  static uint32_t s_armWindowEndsMs = 0;  // ✅ FIX #4: Track when window expires (for resume)

  CoinState currentState = coinslotGetState();

  // Track exactly when the coinslot entered the ARMED state
  if (currentState == CS_ARMED && s_lastState != CS_ARMED) {
    s_armTimeMs = millis();
    s_armWindowEndsMs = millis() + ((uint32_t)g_config.coin.window_seconds * 1000UL);  // ✅ FIX #4
    httpSetArmWindowEnds(s_armWindowEndsMs);  // ✅ FIX #4: Pass to http_server for resume logic
  }
  s_lastState = currentState;

  if (currentState == CS_ARMED || currentState == CS_COUNTING) {
    // If counting, use last pulse. If armed, use the initial arm time.
    uint32_t lastActivity = (currentState == CS_COUNTING) ? coinslotLastPulseMs() : s_armTimeMs;
    
    if (millis() > s_armWindowEndsMs) {  // ✅ FIX #4: Use tracked deadline instead of computing offset
       httpAutoFinalizeSession();
    }
  }

  // WiFi watchdog (reboot after 2min offline)
  static uint32_t lastWdt = 0;
  if (millis() - lastWdt > 5000) {
    lastWdt = millis();
    wifiWatchdogLoop();

    // Retry WiFi if disconnected
    if (!wifiIsConnected()) {
      static uint32_t lastRetry = 0;
      if (millis() - lastRetry > 15000) {
        lastRetry = millis();
        Serial.println(F("[loop] WiFi disconnected, retrying..."));
        wifiConnectStation();
      }
    }
  }

  // Auto-restart check (if configured)
  if (g_config.auto_restart.enabled && !isBusy()) {
    uint32_t restartAt = (uint32_t)g_config.auto_restart.interval_minutes * 60UL * 1000UL;
    if (millis() - s_bootTimeMs > restartAt) {
      Serial.println(F("[loop] Auto-restart interval reached, restarting..."));
      delay(500);
      ESP.restart();
    }
  }

  yield();
  delay(1);
}