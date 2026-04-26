#ifndef RONWIFI_WIFI_MANAGER_H
#define RONWIFI_WIFI_MANAGER_H

#include <Arduino.h>

// Connect to configured WiFi. Returns true if connected within 30s.
// Call repeatedly; manages retries internally.
bool wifiConnectStation();

// Start AP mode for setup
void wifiStartSetupAP();

bool wifiIsConnected();
String wifiGetLocalIp();
int wifiGetRssi();

// Watchdog: if not connected for 2 min straight, ESP.restart()
void wifiWatchdogLoop();

#endif
