#include "mikrotik_client.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <base64.h>

static String baseUrl() {
  return "http://" + g_config.mikrotik.host + ":" + String(g_config.mikrotik.port);
}

static String basicAuth() {
  String creds = g_config.mikrotik.username + ":" + g_config.mikrotik.password;
  return "Basic " + base64::encode(creds);
}

static String formatDuration(uint32_t minutes) {
  uint32_t days = minutes / 1440;
  uint32_t rem = minutes % 1440;
  uint32_t hours = rem / 60;
  uint32_t mins = rem % 60;
  char buf[32];
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%ud %02u:%02u:00", days, hours, mins);
  } else {
    snprintf(buf, sizeof(buf), "%02u:%02u:00", hours, mins);
  }
  return String(buf);
}

static int doRequest(const String& method, const String& path, const String& body, String& response) {
  WiFiClient client;
  HTTPClient http;
  String url = baseUrl() + path;
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", basicAuth());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code;
  if (method == "POST") code = http.POST(body);
  else if (method == "PUT") code = http.sendRequest("PUT", body);
  else if (method == "PATCH") code = http.sendRequest("PATCH", body);
  else code = http.GET();

  if (code > 0) response = http.getString();
  http.end();
  return code;
}

bool mikrotikPing() {
  return mikrotikPingDetailed().ok;
}

MikrotikPingResult mikrotikPingDetailed() {
  MikrotikPingResult r;
  r.ok = false;
  r.http_code = 0;
  r.url = baseUrl() + "/rest/system/resource";
  r.error = "";
  r.response = "";

  if (g_config.mikrotik.host.length() == 0) {
    r.error = "MikroTik host not configured";
    return r;
  }

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, r.url)) {
    r.http_code = -1;
    r.error = "HTTPClient begin() failed";
    return r;
  }
  http.addHeader("Authorization", basicAuth());
  http.setTimeout(5000);

  int code = http.GET();
  r.http_code = code;

  if (code < 0) {
    // Negative codes are HTTPClient errors
    r.error = HTTPClient::errorToString(code);
    http.end();
    Serial.print(F("[mikrotik] ping ERROR: "));
    Serial.println(r.error);
    return r;
  }

  String resp = http.getString();
  // Truncate long responses for diagnostic display
  if (resp.length() > 800) resp = resp.substring(0, 800) + "... (truncated)";
  r.response = resp;

  if (code >= 200 && code < 300) {
    r.ok = true;
    r.error = "OK";
  } else if (code == 401) {
    r.error = "Authentication failed - check username/password";
  } else if (code == 403) {
    r.error = "Forbidden - user lacks 'rest-api' policy";
  } else if (code == 404) {
    r.error = "Endpoint not found - is /rest/ supported? (RouterOS v7.1+)";
  } else {
    r.error = "HTTP " + String(code);
  }

  http.end();
  Serial.print(F("[mikrotik] ping code="));
  Serial.print(code);
  Serial.print(F(" "));
  Serial.println(r.ok ? F("OK") : r.error);
  return r;
}

bool mikrotikCreateAndLoginUser(const String& macNoColons,
                                const String& macWithColons,
                                const String& ip,
                                uint32_t minutes) {
  String resp;

  // Check if user exists first
  String findPath = "/rest/ip/hotspot/user?name=" + macNoColons;
  int findCode = doRequest("GET", findPath, "", resp);
  bool exists = (findCode >= 200 && findCode < 300 && resp.indexOf(macNoColons) >= 0);

  String dur = formatDuration(minutes);

  if (exists) {
    // Extend existing user (PATCH limit-uptime)
    // Extract .id from response
    int idIdx = resp.indexOf("\".id\"");
    if (idIdx < 0) idIdx = resp.indexOf("\"id\"");
    // For simplicity, use named update via /set
    String body = String("{\"numbers\":\"") + macNoColons +
                  "\",\"limit-uptime\":\"" + dur + "\"}";
    int c = doRequest("POST", "/rest/ip/hotspot/user/set", body, resp);
    Serial.print(F("[mikrotik] extend user code="));
    Serial.println(c);
    if (c < 200 || c >= 300) return false;
  } else {
    // Create user
    String body = String("{") +
      "\"name\":\"" + macNoColons + "\"," +
      "\"password\":\"" + macNoColons + "\"," +
      "\"server\":\"all\"," +
      "\"limit-uptime\":\"" + dur + "\"" +
    "}";
    int c = doRequest("PUT", "/rest/ip/hotspot/user", body, resp);
    Serial.print(F("[mikrotik] create user code="));
    Serial.print(c);
    Serial.print(F(" resp="));
    Serial.println(resp.substring(0, 120));
    if (c < 200 || c >= 300) return false;
  }

  // Active login (force login without redirect)
  // First remove existing active session for this MAC if any
  String remBody = String("{\"numbers\":\"") + macWithColons + "\"}";
  doRequest("POST", "/rest/ip/hotspot/active/remove", remBody, resp);

  // Now login
  String loginBody = String("{") +
    "\"user\":\"" + macNoColons + "\"," +
    "\"password\":\"" + macNoColons + "\"," +
    "\"mac-address\":\"" + macWithColons + "\"," +
    "\"ip\":\"" + ip + "\"" +
  "}";
  int lc = doRequest("POST", "/rest/ip/hotspot/active/login", loginBody, resp);
  Serial.print(F("[mikrotik] login code="));
  Serial.println(lc);
  return (lc >= 200 && lc < 300);
}

bool mikrotikAppendOfflineLog(const String& jsonLine) {
  // MikroTik REST /file doesn't easily support append; we simulate by writing a new file
  // with a unique name then rotating. For simplicity, use /print command via /rest/execute
  // (RouterOS v7.1+). Simplest working approach: overwrite accumulated log stored in RAM.
  // For v0 prototype, we skip this and rely on emergency log in ESP NVS.
  (void)jsonLine;
  return true;
}
