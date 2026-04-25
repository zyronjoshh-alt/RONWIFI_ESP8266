#ifndef RONWIFI_CONFIG_H
#define RONWIFI_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>


// Maps "D0".."D8" and "A0" strings to ESP8266 GPIO numbers
// Returns -1 if name is "none" or unrecognized
int pinNameToGpio(const String& name);
String gpioToPinName(int gpio);

struct NetworkCfg {
  String mode;          // "static" or "dhcp"
  String static_ip;
  String gateway;
  String netmask;
  String dns;
};

struct WifiCfg {
  String ssid;
  String password;
};

struct MqttCfg {
  String host;
  uint16_t port;
  String username;
  String password;
  uint16_t keepalive_seconds;
};

struct MikrotikCfg {
  String host;
  uint16_t port;
  String username;
  String password;
};

struct TimeCfg {
  String backend_url;
  String mikrotik_ntp_host;
  String public_ntp_fallback;
};

struct AdminCfg {
  String username;
  String password;
};

struct CoinCfg {
  uint16_t window_seconds;
  float points_rate;
  uint16_t debounce_ms;
  uint8_t abuse_count;
  uint32_t ban_duration_minutes;
};

struct AutoRestartCfg {
  bool enabled;
  uint32_t interval_minutes;
  bool notify_backend;
};

struct PinsCfg {
  int coinslot_signal_pin;  // GPIO number, -1 if none
  int coinslot_set_pin;
  int led_pin;
  int buzzer_pin;
  bool signal_active_low;
  bool set_active_low;
  bool A0_3V_reset_enabled;
};

struct Config {
  uint16_t version;
  uint16_t vendo_id;
  String vendo_name;
  NetworkCfg network;
  WifiCfg wifi;
  MqttCfg mqtt;
  MikrotikCfg mikrotik;
  TimeCfg time_cfg;
  AdminCfg admin;
  CoinCfg coin;
  AutoRestartCfg auto_restart;
  PinsCfg pins;
  String config_updated_at;
  bool loaded_from_backup;  // true if /config.json failed and we loaded backup
};

extern Config g_config;

bool configLoad();
bool configSave(const String& jsonStr);
bool configExists();
String configToJson();  // masks passwords for admin display
String configToJsonFull();  // full unmasked for backend sync

// Rate tiers stored separately
struct RateTier {
  String label;
  uint32_t pesos;
  uint32_t minutes;
};

bool ratesLoad();
bool ratesSave(const String& jsonStr);
extern RateTier g_rates[16];
extern uint8_t g_rates_count;

#endif
