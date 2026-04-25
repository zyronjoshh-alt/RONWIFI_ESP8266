#include "wifi_manager.h"
#include "config.h"
#include <ESP8266WiFi.h>

static uint32_t s_disconnectedSinceMs = 0;

bool wifiConnectStation() {
  if (g_config.wifi.ssid.length() == 0) {
    Serial.println(F("[wifi] no SSID configured"));
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);

  // Apply static IP if configured
  if (g_config.network.mode == "static") {
    IPAddress ip, gw, mask, dns;
    if (ip.fromString(g_config.network.static_ip) &&
        gw.fromString(g_config.network.gateway) &&
        mask.fromString(g_config.network.netmask)) {
      dns.fromString(g_config.network.dns.length() ? g_config.network.dns : g_config.network.gateway);
      WiFi.config(ip, gw, mask, dns);
    }
  }

  Serial.print(F("[wifi] connecting to "));
  Serial.println(g_config.wifi.ssid);
  WiFi.begin(g_config.wifi.ssid.c_str(), g_config.wifi.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[wifi] connected, IP="));
    Serial.println(WiFi.localIP().toString());
    s_disconnectedSinceMs = 0;
    return true;
  }

  Serial.println(F("[wifi] connect FAILED"));
  return false;
}

void wifiStartSetupAP() {
  String macStr = WiFi.macAddress();
  macStr.replace(":", "");
  String last4 = macStr.substring(macStr.length() - 4);
  String ssid = "RONwifi-Setup-" + last4;

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                    IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str());

  Serial.print(F("[wifi] AP started: "));
  Serial.println(ssid);
  Serial.print(F("[wifi] AP IP: 192.168.4.1"));
  Serial.println();
}

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String wifiGetLocalIp() {
  return WiFi.localIP().toString();
}

int wifiGetRssi() {
  return WiFi.RSSI();
}

void wifiWatchdogLoop() {
  if (WiFi.status() == WL_CONNECTED) {
    s_disconnectedSinceMs = 0;
    return;
  }
  if (s_disconnectedSinceMs == 0) {
    s_disconnectedSinceMs = millis();
    return;
  }
  if (millis() - s_disconnectedSinceMs > 120000UL) {
    Serial.println(F("[wifi] 2min disconnected, restarting..."));
    delay(500);
    ESP.restart();
  }
}
