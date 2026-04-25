#ifndef RONWIFI_MIKROTIK_CLIENT_H
#define RONWIFI_MIKROTIK_CLIENT_H

#include <Arduino.h>

// Ping result with full diagnostic info
struct MikrotikPingResult {
  bool ok;
  int http_code;           // 0 = no response, -1 = couldn't begin, -2 = timeout
  String url;              // what URL was called
  String error;            // human-readable error
  String response;         // server response body (if any)
};

// Test connectivity — returns detailed info
MikrotikPingResult mikrotikPingDetailed();

// Legacy bool wrapper (for internal use)
bool mikrotikPing();

// Login a hotspot user by creating the user and doing active/login.
// username and password = MAC without colons (uppercase)
// macWithColons = AA:BB:CC:DD:EE:FF format
// minutes = session duration
bool mikrotikCreateAndLoginUser(const String& macNoColons,
                                const String& macWithColons,
                                const String& ip,
                                uint32_t minutes);

// Append a transaction line to /file/ronwifi-offline-vendo-{id}.log
bool mikrotikAppendOfflineLog(const String& jsonLine);

#endif
