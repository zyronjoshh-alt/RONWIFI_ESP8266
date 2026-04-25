#include "rates.h"
#include "config.h"

// Greedy fill: take largest applicable tier first, repeat until pesos exhausted
uint32_t calculateMinutes(uint32_t pesos) {
  if (pesos == 0 || g_rates_count == 0) return 0;

  // Sort indices by pesos descending (simple insertion sort, N<=16)
  uint8_t order[16];
  for (uint8_t i = 0; i < g_rates_count; i++) order[i] = i;
  for (uint8_t i = 1; i < g_rates_count; i++) {
    uint8_t k = order[i];
    int j = i - 1;
    while (j >= 0 && g_rates[order[j]].pesos < g_rates[k].pesos) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }

  uint32_t minutes = 0;
  uint32_t remaining = pesos;

  while (remaining > 0) {
    bool applied = false;
    for (uint8_t i = 0; i < g_rates_count; i++) {
      RateTier& t = g_rates[order[i]];
      if (t.pesos > 0 && t.pesos <= remaining) {
        minutes += t.minutes;
        remaining -= t.pesos;
        applied = true;
        break;
      }
    }
    if (!applied) break;  // No tier fits remaining amount
  }

  return minutes;
}

uint32_t calculateLeftoverPesos(uint32_t pesos) {
  if (pesos == 0 || g_rates_count == 0) return pesos;

  uint8_t order[16];
  for (uint8_t i = 0; i < g_rates_count; i++) order[i] = i;
  for (uint8_t i = 1; i < g_rates_count; i++) {
    uint8_t k = order[i];
    int j = i - 1;
    while (j >= 0 && g_rates[order[j]].pesos < g_rates[k].pesos) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }

  uint32_t remaining = pesos;
  while (remaining > 0) {
    bool applied = false;
    for (uint8_t i = 0; i < g_rates_count; i++) {
      RateTier& t = g_rates[order[i]];
      if (t.pesos > 0 && t.pesos <= remaining) {
        remaining -= t.pesos;
        applied = true;
        break;
      }
    }
    if (!applied) break;
  }
  return remaining;
}
