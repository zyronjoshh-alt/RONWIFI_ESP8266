#ifndef RONWIFI_SETUP_AP_H
#define RONWIFI_SETUP_AP_H

#include <Arduino.h>

// Starts the AP, serves setup UI, blocks until user submits valid config
// then saves to LittleFS and reboots.
void runSetupMode();

#endif
