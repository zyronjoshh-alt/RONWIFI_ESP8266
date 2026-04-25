#ifndef RONWIFI_COINSLOT_H
#define RONWIFI_COINSLOT_H

#include <Arduino.h>

enum CoinState {
  CS_IDLE,       // Coin slot disabled (inhibit active)
  CS_ARMED,      // Slot enabled, waiting for first pulse
  CS_COUNTING,   // Pulses being received
  CS_FINALIZING, // Transaction in progress (post-Done)
  CS_ERROR
};

void coinslotInit();
void coinslotArm();                 // Enable coin slot, start accepting
void coinslotDisarm();              // Disable coin slot
uint32_t coinslotGetPulseCount();   // Current pulse count since arm
void coinslotResetCount();
CoinState coinslotGetState();
void coinslotSetState(CoinState s);
uint32_t coinslotLastPulseMs();     // millis() of last pulse
void coinslotLoop();                // Must be called from main loop

// For admin UI: manually test the slot
void coinslotTestOpen(uint32_t durationMs);

#endif
