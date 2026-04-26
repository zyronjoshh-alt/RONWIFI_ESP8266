#include "coinslot.h"
#include "config.h"

static volatile uint32_t s_pulseCount = 0;
static volatile uint32_t s_lastPulseUs = 0;
static volatile uint32_t s_lastPulseMs = 0;
static CoinState s_state = CS_IDLE;

static IRAM_ATTR void coinPulseISR() {
  uint32_t now = micros();
  uint32_t debounceUs = (uint32_t)g_config.coin.debounce_ms * 1000UL;
  if (now - s_lastPulseUs < debounceUs) return;
  s_lastPulseUs = now;
  s_lastPulseMs = millis();
  s_pulseCount++;
}

static void writeInhibit(bool disabled) {
  // If inhibit is active_low, HIGH = disabled, LOW = enabled
  // If inhibit is active_high, LOW = disabled, HIGH = enabled
  int pin = g_config.pins.coinslot_set_pin;
  if (pin < 0) return;
  bool level;
  if (g_config.pins.set_active_low) {
    level = disabled ? HIGH : LOW;
  } else {
    level = disabled ? LOW : HIGH;
  }
  digitalWrite(pin, level);
}

void coinslotInit() {
  int sigPin = g_config.pins.coinslot_signal_pin;
  int setPin = g_config.pins.coinslot_set_pin;

  if (setPin >= 0) {
    pinMode(setPin, OUTPUT);
    writeInhibit(true); // Disabled by default
  }

  if (sigPin >= 0) {
    // Allan coin slots typically have an internal pull-up or pull-down;
    // use INPUT_PULLUP if active_low, plain INPUT if active_high
    if (g_config.pins.signal_active_low) {
      pinMode(sigPin, INPUT_PULLUP);
      attachInterrupt(digitalPinToInterrupt(sigPin), coinPulseISR, FALLING);
    } else {
      pinMode(sigPin, INPUT);
      attachInterrupt(digitalPinToInterrupt(sigPin), coinPulseISR, RISING);
    }
  }

  s_state = CS_IDLE;
  s_pulseCount = 0;

  Serial.print(F("[coinslot] init sigPin="));
  Serial.print(sigPin);
  Serial.print(F(" setPin="));
  Serial.println(setPin);
}

void coinslotArm() {
  // ✅ FIX: Prevent false pulse from relay activation spike
  noInterrupts();
  s_pulseCount = 0;
  s_lastPulseUs = 0;
  s_lastPulseMs = 0;
  interrupts();
  
  writeInhibit(false);  // Enable relay (creates electrical transient)
  
  // Wait for relay and electrical stabilization (100ms)
  // During this time, any spikes from relay switching will occur
  delay(100);
  
  // Clear any false pulses that occurred during relay activation
  noInterrupts();
  s_pulseCount = 0;
  s_lastPulseUs = 0;
  s_lastPulseMs = 0;
  interrupts();
  
  s_state = CS_ARMED;
  Serial.println(F("[coinslot] armed (relay stabilized, ready for coins)"));
}

void coinslotDisarm() {
  writeInhibit(true);   // Disable slot
  s_state = CS_IDLE;
  Serial.println(F("[coinslot] disarmed"));
}

uint32_t coinslotGetPulseCount() {
  noInterrupts();
  uint32_t c = s_pulseCount;
  interrupts();
  return c;
}

void coinslotResetCount() {
  noInterrupts();
  s_pulseCount = 0;
  interrupts();
}

CoinState coinslotGetState() { return s_state; }
void coinslotSetState(CoinState s) { s_state = s; }

uint32_t coinslotLastPulseMs() {
  noInterrupts();
  uint32_t m = s_lastPulseMs;
  interrupts();
  return m;
}

void coinslotLoop() {
  // Transition ARMED → COUNTING on first pulse
  if (s_state == CS_ARMED && coinslotGetPulseCount() > 0) {
    s_state = CS_COUNTING;
    Serial.println(F("[coinslot] first pulse, state=COUNTING"));
  }
}

void coinslotTestOpen(uint32_t durationMs) {
  Serial.print(F("[coinslot] test open "));
  Serial.print(durationMs);
  Serial.println(F("ms"));
  writeInhibit(false);
  delay(durationMs);
  writeInhibit(true);
}
