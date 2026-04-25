#ifndef RONWIFI_RATES_H
#define RONWIFI_RATES_H

#include <Arduino.h>

// Given inserted pesos, compute total minutes using greedy fill.
// Uses g_rates tiers sorted by pesos descending.
// Example: tiers = [₱1/10m, ₱5/60m, ₱10/180m]
// Insert ₱7 → ₱5/60m + ₱1/10m + ₱1/10m = 80 min (no leftover)
uint32_t calculateMinutes(uint32_t pesos);

// Returns leftover pesos after greedy fill (should be 0 if ₱1 tier exists)
uint32_t calculateLeftoverPesos(uint32_t pesos);

#endif
