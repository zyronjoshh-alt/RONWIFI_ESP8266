#include "config.h"
#include <LittleFS.h>

Config g_config;
RateTier g_rates[16];
uint8_t g_rates_count = 0;

// ---------- Pin name mapping for NodeMCU v3 ----------
int pinNameToGpio(const String& n) {
  String name = n;
  name.toUpperCase();
  name.trim();
  if (name == "NONE" || name == "") return -1;
  if (name == "D0") return 16;
  if (name == "D1") return 5;
  if (name == "D2") return 4;
  if (name == "D3") return 0;
  if (name == "D4") return 2;
  if (name == "D5") return 14;
  if (name == "D6") return 12;
  if (name == "D7") return 13;
  if (name == "D8") return 15;
  if (name == "A0") return A0;
  // Accept raw GPIO numbers too
  if (name.toInt() >= 0 && name.toInt() <= 16) return name.toInt();
  return -1;
}

String gpioToPinName(int gpio) {
  switch (gpio) {
    case 16: return "D0";
    case 5:  return "D1";
    case 4:  return "D2";
    case 0:  return "D3";
    case 2:  return "D4";
    case 14: return "D5";
    case 12: return "D6";
    case 13: return "D7";
    case 15: return "D8";
    case A0: return "A0";
    case -1: return "none";
    default: return String(gpio);
  }
}

// ---------- Parsing ----------
static bool parseJson(const String& json, JsonDocument& doc) {
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print(F("[config] JSON parse error: "));
    Serial.println(err.c_str());
    return false;
  }
  return true;
}

static bool applyConfig(JsonDocument& doc) {
  if (!doc.containsKey("vendo_id") || !doc.containsKey("mqtt") ||
      !doc.containsKey("mikrotik") || !doc.containsKey("network")) {
    Serial.println(F("[config] Missing required fields"));
    return false;
  }

  g_config.version = doc["version"] | 1;
  g_config.vendo_id = doc["vendo_id"] | 0;
  g_config.vendo_name = doc["vendo_name"] | "";

  JsonObject net = doc["network"];
  g_config.network.mode = net["mode"] | "static";
  g_config.network.static_ip = net["static_ip"] | "";
  g_config.network.gateway = net["gateway"] | "";
  g_config.network.netmask = net["netmask"] | "255.255.255.0";
  g_config.network.dns = net["dns"] | "";

  JsonObject wifi = doc["wifi"];
  g_config.wifi.ssid = wifi["ssid"] | "";
  g_config.wifi.password = wifi["password"] | "";

  JsonObject mqtt = doc["mqtt"];
  g_config.mqtt.host = mqtt["host"] | "";
  g_config.mqtt.port = mqtt["port"] | 1883;
  g_config.mqtt.username = mqtt["username"] | "";
  g_config.mqtt.password = mqtt["password"] | "";
  g_config.mqtt.keepalive_seconds = mqtt["keepalive_seconds"] | 60;

  JsonObject mt = doc["mikrotik"];
  g_config.mikrotik.host = mt["host"] | "";
  g_config.mikrotik.port = mt["port"] | 80;
  g_config.mikrotik.username = mt["username"] | "";
  g_config.mikrotik.password = mt["password"] | "";

  JsonObject tm = doc["time"];
  g_config.time_cfg.backend_url = tm["backend_url"] | "";
  g_config.time_cfg.mikrotik_ntp_host = tm["mikrotik_ntp_host"] | "";
  g_config.time_cfg.public_ntp_fallback = tm["public_ntp_fallback"] | "pool.ntp.org";

  JsonObject adm = doc["admin"];
  g_config.admin.username = adm["username"] | "admin";
  g_config.admin.password = adm["password"] | "admin";

  JsonObject coin = doc["coin_settings"];
  g_config.coin.window_seconds = coin["window_seconds"] | 60;
  g_config.coin.points_rate = coin["points_rate"] | 0.20;
  g_config.coin.debounce_ms = coin["debounce_ms"] | 30;
  g_config.coin.abuse_count = coin["abuse_count"] | 3;
  g_config.coin.ban_duration_minutes = coin["ban_duration_minutes"] | 15;

  JsonObject ar = doc["auto_restart"];
  g_config.auto_restart.enabled = ar["enabled"] | false;
  g_config.auto_restart.interval_minutes = ar["interval_minutes"] | 1440;
  g_config.auto_restart.notify_backend = ar["notify_backend"] | true;

  JsonObject pins = doc["pins"];
  g_config.pins.coinslot_signal_pin = pinNameToGpio(pins["coinslot_signal_pin"] | "D2");
  g_config.pins.coinslot_set_pin = pinNameToGpio(pins["coinslot_set_pin"] | "D1");
  g_config.pins.led_pin = pinNameToGpio(pins["led_pin"] | "none");
  g_config.pins.buzzer_pin = pinNameToGpio(pins["buzzer_pin"] | "none");
  g_config.pins.signal_active_low = pins["signal_active_low"] | true;
  g_config.pins.set_active_low = pins["set_active_low"] | true;
  g_config.pins.A0_3V_reset_enabled = pins["A0_3V_reset_enabled"] | true;

  g_config.config_updated_at = doc["config_updated_at"] | "";

  return true;
}

// ---------- Public API ----------
bool configExists() {
  return LittleFS.exists("/config.json");
}

bool configLoad() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println(F("[config] /config.json not found, trying backup"));
    f = LittleFS.open("/config.backup.json", "r");
    if (!f) {
      Serial.println(F("[config] no backup either"));
      return false;
    }
    g_config.loaded_from_backup = true;
  } else {
    g_config.loaded_from_backup = false;
  }

  String json = f.readString();
  f.close();

  StaticJsonDocument<2048> doc;
  if (!parseJson(json, doc)) {
    if (!g_config.loaded_from_backup) {
      Serial.println(F("[config] main corrupt, trying backup"));
      File b = LittleFS.open("/config.backup.json", "r");
      if (b) {
        String jb = b.readString();
        b.close();
        StaticJsonDocument<2048> doc2;
        if (parseJson(jb, doc2) && applyConfig(doc2)) {
          g_config.loaded_from_backup = true;
          Serial.println(F("[config] loaded from backup"));
          return true;
        }
      }
    }
    return false;
  }

  if (!applyConfig(doc)) return false;

  Serial.print(F("[config] loaded vendo_id="));
  Serial.println(g_config.vendo_id);
  return true;
}

bool configSave(const String& jsonStr) {
  // Validate first
  StaticJsonDocument<2048> doc;
  if (!parseJson(jsonStr, doc)) return false;

  // Rotate backup
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.backup.json");
    LittleFS.rename("/config.json", "/config.backup.json");
  }

  // Atomic write via tmp file
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) {
    Serial.println(F("[config] failed to open tmp for write"));
    return false;
  }
  f.print(jsonStr);
  f.close();

  // Verify
  File v = LittleFS.open("/config.json.tmp", "r");
  if (!v) return false;
  String check = v.readString();
  v.close();
  if (check.length() != jsonStr.length()) {
    Serial.println(F("[config] tmp size mismatch after write"));
    LittleFS.remove("/config.json.tmp");
    return false;
  }

  // Atomic rename
  LittleFS.remove("/config.json");
  if (!LittleFS.rename("/config.json.tmp", "/config.json")) {
    Serial.println(F("[config] rename failed"));
    return false;
  }

  // Reload into g_config
  return configLoad();
}

static String maskPassword(const String& p) {
  if (p.length() <= 2) return "**";
  return String(p.charAt(0)) + "****" + String(p.charAt(p.length() - 1));
}

String configToJson() {
  StaticJsonDocument<2048> doc;
  doc["version"] = g_config.version;
  doc["vendo_id"] = g_config.vendo_id;
  doc["vendo_name"] = g_config.vendo_name;

  JsonObject net = doc.createNestedObject("network");
  net["mode"] = g_config.network.mode;
  net["static_ip"] = g_config.network.static_ip;
  net["gateway"] = g_config.network.gateway;
  net["netmask"] = g_config.network.netmask;
  net["dns"] = g_config.network.dns;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = g_config.wifi.ssid;
  wifi["password"] = maskPassword(g_config.wifi.password);

  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["host"] = g_config.mqtt.host;
  mqtt["port"] = g_config.mqtt.port;
  mqtt["username"] = g_config.mqtt.username;
  mqtt["password"] = maskPassword(g_config.mqtt.password);
  mqtt["keepalive_seconds"] = g_config.mqtt.keepalive_seconds;

  JsonObject mt = doc.createNestedObject("mikrotik");
  mt["host"] = g_config.mikrotik.host;
  mt["port"] = g_config.mikrotik.port;
  mt["username"] = g_config.mikrotik.username;
  mt["password"] = maskPassword(g_config.mikrotik.password);

  JsonObject tm = doc.createNestedObject("time");
  tm["backend_url"] = g_config.time_cfg.backend_url;
  tm["mikrotik_ntp_host"] = g_config.time_cfg.mikrotik_ntp_host;
  tm["public_ntp_fallback"] = g_config.time_cfg.public_ntp_fallback;

  JsonObject adm = doc.createNestedObject("admin");
  adm["username"] = g_config.admin.username;
  adm["password"] = maskPassword(g_config.admin.password);

  JsonObject coin = doc.createNestedObject("coin_settings");
  coin["window_seconds"] = g_config.coin.window_seconds;
  coin["points_rate"] = g_config.coin.points_rate;
  coin["debounce_ms"] = g_config.coin.debounce_ms;
  coin["abuse_count"] = g_config.coin.abuse_count;
  coin["ban_duration_minutes"] = g_config.coin.ban_duration_minutes;

  JsonObject ar = doc.createNestedObject("auto_restart");
  ar["enabled"] = g_config.auto_restart.enabled;
  ar["interval_minutes"] = g_config.auto_restart.interval_minutes;
  ar["notify_backend"] = g_config.auto_restart.notify_backend;

  JsonObject pins = doc.createNestedObject("pins");
  pins["coinslot_signal_pin"] = gpioToPinName(g_config.pins.coinslot_signal_pin);
  pins["coinslot_set_pin"] = gpioToPinName(g_config.pins.coinslot_set_pin);
  pins["led_pin"] = gpioToPinName(g_config.pins.led_pin);
  pins["buzzer_pin"] = gpioToPinName(g_config.pins.buzzer_pin);
  pins["signal_active_low"] = g_config.pins.signal_active_low;
  pins["set_active_low"] = g_config.pins.set_active_low;
  pins["A0_3V_reset_enabled"] = g_config.pins.A0_3V_reset_enabled;

  doc["config_updated_at"] = g_config.config_updated_at;
  doc["firmware_version"] = FIRMWARE_VERSION;

  String out;
  serializeJsonPretty(doc, out);
  return out;
}

// Full unmasked config — for admin form editing only (requires auth)
String configToJsonFull() {
  StaticJsonDocument<2048> doc;
  doc["version"] = g_config.version;
  doc["vendo_id"] = g_config.vendo_id;
  doc["vendo_name"] = g_config.vendo_name;

  JsonObject net = doc.createNestedObject("network");
  net["mode"] = g_config.network.mode;
  net["static_ip"] = g_config.network.static_ip;
  net["gateway"] = g_config.network.gateway;
  net["netmask"] = g_config.network.netmask;
  net["dns"] = g_config.network.dns;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = g_config.wifi.ssid;
  wifi["password"] = g_config.wifi.password;

  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["host"] = g_config.mqtt.host;
  mqtt["port"] = g_config.mqtt.port;
  mqtt["username"] = g_config.mqtt.username;
  mqtt["password"] = g_config.mqtt.password;
  mqtt["keepalive_seconds"] = g_config.mqtt.keepalive_seconds;

  JsonObject mt = doc.createNestedObject("mikrotik");
  mt["host"] = g_config.mikrotik.host;
  mt["port"] = g_config.mikrotik.port;
  mt["username"] = g_config.mikrotik.username;
  mt["password"] = g_config.mikrotik.password;

  JsonObject tm = doc.createNestedObject("time");
  tm["backend_url"] = g_config.time_cfg.backend_url;
  tm["mikrotik_ntp_host"] = g_config.time_cfg.mikrotik_ntp_host;
  tm["public_ntp_fallback"] = g_config.time_cfg.public_ntp_fallback;

  JsonObject adm = doc.createNestedObject("admin");
  adm["username"] = g_config.admin.username;
  adm["password"] = g_config.admin.password;

  JsonObject coin = doc.createNestedObject("coin_settings");
  coin["window_seconds"] = g_config.coin.window_seconds;
  coin["points_rate"] = g_config.coin.points_rate;
  coin["debounce_ms"] = g_config.coin.debounce_ms;
  coin["abuse_count"] = g_config.coin.abuse_count;

  JsonObject ar = doc.createNestedObject("auto_restart");
  ar["enabled"] = g_config.auto_restart.enabled;
  ar["interval_minutes"] = g_config.auto_restart.interval_minutes;
  ar["notify_backend"] = g_config.auto_restart.notify_backend;

  JsonObject pins = doc.createNestedObject("pins");
  pins["coinslot_signal_pin"] = gpioToPinName(g_config.pins.coinslot_signal_pin);
  pins["coinslot_set_pin"] = gpioToPinName(g_config.pins.coinslot_set_pin);
  pins["led_pin"] = gpioToPinName(g_config.pins.led_pin);
  pins["buzzer_pin"] = gpioToPinName(g_config.pins.buzzer_pin);
  pins["signal_active_low"] = g_config.pins.signal_active_low;
  pins["set_active_low"] = g_config.pins.set_active_low;
  pins["A0_3V_reset_enabled"] = g_config.pins.A0_3V_reset_enabled;

  doc["config_updated_at"] = g_config.config_updated_at;

  String out;
  serializeJson(doc, out);
  return out;
}

// ---------- Rates ----------
bool ratesLoad() {
  File f = LittleFS.open("/rates.json", "r");
  if (!f) {
    Serial.println(F("[rates] /rates.json not found, using defaults"));
    // Default rates
    g_rates[0] = {"₱1 / 10min", 1, 10};
    g_rates[1] = {"₱5 / 1hr", 5, 60};
    g_rates[2] = {"₱10 / 3hr", 10, 180};
    g_rates[3] = {"₱20 / 1day", 20, 1440};
    g_rates[4] = {"₱50 / 3days", 50, 4320};
    g_rates[5] = {"₱100 / 7days", 100, 10080};
    g_rates[6] = {"₱300 / 30days", 300, 43200};
    g_rates_count = 7;
    return true;
  }

  String json = f.readString();
  f.close();

  StaticJsonDocument<1024> doc;
  if (!parseJson(json, doc)) return false;

  JsonArray tiers = doc["tiers"];
  g_rates_count = 0;
  for (JsonVariant t : tiers) {
    if (g_rates_count >= 16) break;
    g_rates[g_rates_count].label = t["label"] | "";
    g_rates[g_rates_count].pesos = t["pesos"] | 0;
    g_rates[g_rates_count].minutes = t["minutes"] | 0;
    g_rates_count++;
  }

  Serial.print(F("[rates] loaded "));
  Serial.print(g_rates_count);
  Serial.println(F(" tiers"));
  return true;
}

bool ratesSave(const String& jsonStr) {
  StaticJsonDocument<1024> doc;
  if (!parseJson(jsonStr, doc)) return false;

  File f = LittleFS.open("/rates.json.tmp", "w");
  if (!f) return false;
  f.print(jsonStr);
  f.close();

  LittleFS.remove("/rates.json");
  LittleFS.rename("/rates.json.tmp", "/rates.json");
  return ratesLoad();
}
