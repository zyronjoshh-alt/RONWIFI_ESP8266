
#include "http_server.h"
#include "config.h"
#include "coinslot.h"
#include "rates.h"
#include "mikrotik_client.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Updater.h>

static ESP8266WebServer server(80);

// Session state for current coin transaction
static String s_currentSessionId = "";
static String s_currentMac = "";
static String s_currentIp = "";
static uint32_t s_sessionStartMs = 0;

// --- ANTI-ABUSE LEDGER ---
struct AbuseRecord {
  String mac;
  uint8_t strikes;
  uint32_t ban_expires_ms;
};
static AbuseRecord s_abuseLedger[10]; // Max 10 tracked offenders

static AbuseRecord* getLedgerEntry(const String& mac) {
  for (int i = 0; i < 10; i++) {
    if (s_abuseLedger[i].mac == mac) return &s_abuseLedger[i];
  }
  return nullptr;
}

// ---------- Helpers ----------
static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

static void sendJsonOk(const String& dataJson) {
  sendJson(200, String("{\"ok\":true,\"data\":") + dataJson + "}");
}

static void sendJsonError(int code, const String& errCode, const String& msg) {
  String body = String("{\"ok\":false,\"error\":{\"code\":\"") + errCode +
                "\",\"message\":\"" + msg + "\"}}";
  sendJson(code, body);
}

static bool requireAdminAuth() {
  if (!server.authenticate(g_config.admin.username.c_str(), g_config.admin.password.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ---------- CORS preflight ----------
static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ---------- PORTAL FALLBACK ENDPOINTS ----------
static void handlePing() {
  String body = String("{\"vendo_id\":") + g_config.vendo_id +
                ",\"vendo_name\":\"" + g_config.vendo_name + "\"" +
                ",\"uptime_s\":" + (millis() / 1000) +
                ",\"firmware\":\"" FIRMWARE_VERSION "\"}";
  sendJsonOk(body);
}

static void handleCoinStart() {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJsonError(400, "bad_json", "Invalid JSON body");
    return;
  }

  String mac = doc["mac"] | "";
  String ip = doc["ip"] | "";
  if (mac.length() < 12 || ip.length() < 7) {
    sendJsonError(400, "bad_params", "mac and ip required");
    return;
  }
  mac.toUpperCase();

  // 1. CHECK THE LEDGER (Are they banned?)
  AbuseRecord* record = getLedgerEntry(mac);
  if (record && record->strikes >= g_config.coin.abuse_count) {
    if (millis() < record->ban_expires_ms) {
      uint32_t remainingMins = (record->ban_expires_ms - millis()) / 60000;
      sendJsonError(429, "banned", "Too many attempts. Please wait " + String(remainingMins + 1) + " minutes.");
      return;
    } else {
      // Ban expired, reset their strikes
      record->strikes = 0;
      record->ban_expires_ms = 0;
    }
  }

  CoinState cs = coinslotGetState();

  // 2. THE RESUME LOGIC
  if (cs == CS_ARMED || cs == CS_COUNTING || cs == CS_FINALIZING) {
    if (s_currentMac == mac) {
      // It's the same user! Just return the active session ID (Resume)
      String body = String("{\"session_id\":\"") + s_currentSessionId + "\",\"state\":\"" + 
                    (cs == CS_ARMED ? "armed" : "counting") + "\"}";
      sendJsonOk(body);
      return;
    } else {
      // It's a different user trying to interrupt
      sendJsonError(409, "busy", "Coinslot busy");
      return;
    }
  }

  // 3. START NEW SESSION
  // Pre-check MikroTik login
  bool mikrotikOk = false;
  for (int i = 0; i < 3; i++) {
    if (mikrotikPing()) { mikrotikOk = true; break; }
    delay(500);
  }
  if (!mikrotikOk) {
    sendJsonError(503, "mikrotik_unreachable", "Could not reach MikroTik");
    return;
  }

  // 4. APPLY A STRIKE TO THE LEDGER
  if (!record) {
    // Find an empty slot
    for (int i = 0; i < 10; i++) {
      if (s_abuseLedger[i].mac == "") { record = &s_abuseLedger[i]; break; }
    }
    // If array is full, trigger Limbo state!
    if (!record) {
      sendJsonError(503, "limbo", "System preparing to restart. Please try again in 2 minutes.");
      return; 
    }
    record->mac = mac;
    record->strikes = 0;
  }
  
  record->strikes += 1;
  if (record->strikes >= g_config.coin.abuse_count) {
    record->ban_expires_ms = millis() + (g_config.coin.ban_duration_minutes * 60000UL);
  }

  // Initialize Session
  s_currentSessionId = String(millis(), HEX);
  s_currentMac = mac;
  s_currentIp = ip;
  s_sessionStartMs = millis();

  coinslotArm();

  String body = String("{\"session_id\":\"") + s_currentSessionId + "\",\"state\":\"armed\"}";
  sendJsonOk(body);
}

static String macWithColons(const String& noColons) {
  if (noColons.length() != 12) return noColons;
  String out = "";
  for (int i = 0; i < 12; i += 2) {
    out += noColons.substring(i, i + 2);
    if (i < 10) out += ":";
  }
  return out;
}

static void handleCoinDone() {
  StaticJsonDocument<256> doc;
  deserializeJson(doc, server.arg("plain"));

  if (coinslotGetState() == CS_IDLE) {
    sendJsonError(400, "no_session", "No active session");
    return;
  }

  coinslotSetState(CS_FINALIZING);
  uint32_t pulseCount = coinslotGetPulseCount();
  uint32_t pesos = pulseCount;  // 1 pulse = ₱1 (Allan coin slot programming)

if (pesos > 0) {
    // They paid! Clear their record completely.
    AbuseRecord* record = getLedgerEntry(s_currentMac);
    if (record) {
      record->mac = ""; // Free the slot
      record->strikes = 0;
    }
  }
  
  if (pesos == 0) {
    coinslotDisarm();
    sendJsonOk("{\"pesos\":0,\"minutes\":0,\"status\":\"cancelled\"}");
    s_currentSessionId = "";
    return;
  }

  uint32_t minutes = calculateMinutes(pesos);

  // Create/login MikroTik user
  bool ok = false;
  for (int i = 0; i < 3; i++) {
    if (mikrotikCreateAndLoginUser(s_currentMac,
                                    macWithColons(s_currentMac),
                                    s_currentIp,
                                    minutes)) {
      ok = true;
      break;
    }
    delay(500);
  }

  coinslotDisarm();

  if (!ok) {
    // Write to emergency log
    File f = LittleFS.open("/emergency.log", "a");
    if (f) {
      f.print(millis()); f.print("|");
      f.print(s_currentMac); f.print("|");
      f.print(s_currentIp); f.print("|");
      f.print(pesos); f.print("|");
      f.println(minutes);
      f.close();
    }
    sendJsonError(503, "mikrotik_failed", "Could not register user on MikroTik (logged locally)");
    s_currentSessionId = "";
    return;
  }

  String body = String("{\"pesos\":") + pesos +
                ",\"minutes\":" + minutes +
                ",\"status\":\"ok\"}";
  sendJsonOk(body);
  s_currentSessionId = "";
}

static void handleCoinCancel() {
  if (coinslotGetState() != CS_IDLE) {
    coinslotDisarm();
  }
  s_currentSessionId = "";
  sendJsonOk("{\"status\":\"cancelled\"}");
}

static void handleStatus() {
  // URI: /status/{session_id}
  String uri = server.uri();
  int slash = uri.lastIndexOf('/');
  String sid = (slash >= 0) ? uri.substring(slash + 1) : "";
  (void)sid;

  uint32_t pulses = coinslotGetPulseCount();
  uint32_t minutes = calculateMinutes(pulses);
  uint32_t lastPulse = coinslotLastPulseMs();
  uint32_t inactiveMs = lastPulse ? (millis() - lastPulse) : 0;

  String body = String("{\"pesos\":") + pulses +
                ",\"minutes\":" + minutes +
                ",\"state\":\"" + (coinslotGetState() == CS_ARMED ? "armed" :
                                   coinslotGetState() == CS_COUNTING ? "counting" : "idle") + "\"" +
                ",\"inactive_ms\":" + inactiveMs + "}";
  sendJsonOk(body);
}

static void handleRates() {
  String body = "{\"tiers\":[";
  for (uint8_t i = 0; i < g_rates_count; i++) {
    if (i > 0) body += ",";
    body += "{\"label\":\"" + g_rates[i].label + "\"," +
            "\"pesos\":" + g_rates[i].pesos + "," +
            "\"minutes\":" + g_rates[i].minutes + "}";
  }
  body += "]}";
  sendJsonOk(body);
}

// ---------- ADMIN UI ----------
static const char ADMIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RONwifi Admin</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:#f3f4f6;color:#1f2937;max-width:700px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:20px;margin:8px 0 4px;color:#2563eb}
.subtitle{color:#6b7280;font-size:12px;margin-bottom:16px}
h2{font-size:15px;margin:0 0 12px;color:#1f2937;padding-bottom:8px;border-bottom:1px solid #e5e7eb}
.card{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:16px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;padding:4px 0;font-size:13px}
.label{color:#6b7280}
.value{font-family:monospace;font-weight:600}
.status-ok{color:#10b981;font-weight:bold}
.status-err{color:#ef4444;font-weight:bold}
.status-warn{color:#f59e0b;font-weight:bold}

label.fld{display:block;margin:10px 0 0}
label.fld>span{display:block;font-size:12px;color:#6b7280;margin-bottom:4px;font-weight:600}
input[type=text],input[type=number],input[type=password],select,textarea{width:100%;padding:8px 10px;border:1px solid #d1d5db;border-radius:6px;font-size:14px;font-family:inherit;background:#fff;color:#1f2937}
input[type=checkbox]{width:auto;margin-right:6px;transform:scale(1.2)}
input:focus,select:focus,textarea:focus{outline:none;border-color:#2563eb;box-shadow:0 0 0 3px rgba(37,99,235,.15)}
textarea{font-family:'Consolas',monospace;font-size:12px;min-height:140px;resize:vertical}

.pw{position:relative}
.pw input{padding-right:48px}
.pw .pw-toggle{position:absolute;right:8px;top:8px;padding:4px 8px;background:#6b7280;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:11px;margin:0}

.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}

button{background:#2563eb;color:#fff;border:none;padding:10px 16px;border-radius:6px;cursor:pointer;font-size:14px;margin:4px 4px 4px 0;font-weight:600}
button:hover{background:#1d4ed8}
button.danger{background:#ef4444}
button.danger:hover{background:#dc2626}
button.secondary{background:#6b7280}
button.secondary:hover{background:#4b5563}
button.success{background:#10b981}

.save-bar{position:sticky;bottom:0;background:#fff;border-top:2px solid #2563eb;padding:12px;margin:16px -12px -12px;text-align:center;box-shadow:0 -4px 12px rgba(0,0,0,0.08)}
.save-bar button{padding:14px 32px;font-size:15px}

.msg{padding:10px;border-radius:6px;margin:8px 0;font-size:13px}
.msg-ok{background:#d1fae5;color:#065f46}
.msg-err{background:#fee2e2;color:#991b1b}
.msg-info{background:#dbeafe;color:#1e40af}

pre.diag{background:#0f172a;color:#e2e8f0;padding:12px;border-radius:6px;font-size:11px;line-height:1.5;overflow:auto;max-height:200px;white-space:pre-wrap;word-break:break-word}

.collapsible{cursor:pointer;user-select:none}
.collapsible::before{content:"▼ ";font-size:10px}
.collapsed.collapsible::before{content:"▶ "}
.collapsed + .card-body{display:none}
</style></head><body>

<h1>🤖 RONwifi Admin</h1>
<div class="subtitle">Vendo configuration — changes saved will trigger reboot</div>

<div id="msg"></div>

<!-- STATUS -->
<div class="card">
<h2>📊 Status</h2>
<div id="statusContent">Loading...</div>
</div>

<!-- DIAGNOSTICS -->
<div class="card">
<h2>🔧 Diagnostics</h2>
<button onclick="testOpen()">Test Open Coinslot (3s)</button>
<button onclick="pingMikrotik()">Ping MikroTik</button>
<button class="secondary" onclick="restart()">Restart ESP</button>
<button class="danger" onclick="factoryReset()">Factory Reset</button>
<div id="diagOut" style="margin-top:10px"></div>
<div class="card">
  <h2>🚀 Firmware Upgrade</h2>
  <div class="subtitle">Upload a compiled .bin file to update the system.</div>
  <input type="file" id="firmwareFile" accept=".bin" style="margin-bottom: 12px;">
  <div id="otaProgressContainer" style="display:none; width:100%; background:#e5e7eb; border-radius:6px; overflow:hidden; margin-bottom: 12px;">
    <div id="otaProgressBar" style="width:0%; height:8px; background:#2563eb; transition:width 0.2s;"></div>
  </div>
  <button type="button" class="success" onclick="startFirmwareUpdate()">Upload & Flash</button>
</div>
</div>

<!-- ====================================== FORM ====================================== -->
<form id="configForm" onsubmit="return false">

<!-- IDENTITY -->
<div class="card">
<h2>🏷️ Identity</h2>
<label class="fld"><span>Vendo ID</span><input type="number" name="vendo_id" min="1"></label>
<label class="fld"><span>Vendo Name</span><input type="text" name="vendo_name" maxlength="40"></label>
</div>

<!-- NETWORK -->
<div class="card">
<h2>🌐 Network</h2>
<label class="fld"><span>Mode</span>
<select name="network_mode" onchange="toggleStaticFields()">
<option value="static">Static IP</option>
<option value="dhcp">DHCP (auto)</option>
</select></label>
<div id="staticFields">
<div class="grid2">
<label class="fld"><span>Static IP</span><input type="text" name="network_static_ip" placeholder="10.0.11.7"></label>
<label class="fld"><span>Gateway</span><input type="text" name="network_gateway" placeholder="10.0.11.1"></label>
</div>
<div class="grid2">
<label class="fld"><span>Netmask</span><input type="text" name="network_netmask" value="255.255.255.0"></label>
<label class="fld"><span>DNS</span><input type="text" name="network_dns" placeholder="10.0.11.1"></label>
</div>
</div>
</div>

<!-- WIFI -->
<div class="card">
<h2>📶 WiFi</h2>
<label class="fld"><span>SSID</span><input type="text" name="wifi_ssid" maxlength="32"></label>
<label class="fld"><span>Password</span>
<div class="pw"><input type="password" name="wifi_password" maxlength="63">
<button type="button" class="pw-toggle" onclick="togglePw(this)">SHOW</button></div></label>
</div>

<!-- MQTT -->
<div class="card">
<h2 class="collapsible" onclick="toggleCollapse(this)">📡 MQTT (unused in v0)</h2>
<div class="card-body">
<div class="grid2">
<label class="fld"><span>Host</span><input type="text" name="mqtt_host" placeholder="10.99.0.8"></label>
<label class="fld"><span>Port</span><input type="number" name="mqtt_port" value="1883"></label>
</div>
<label class="fld"><span>Username</span><input type="text" name="mqtt_username"></label>
<label class="fld"><span>Password</span>
<div class="pw"><input type="password" name="mqtt_password">
<button type="button" class="pw-toggle" onclick="togglePw(this)">SHOW</button></div></label>
<label class="fld"><span>Keepalive (seconds)</span><input type="number" name="mqtt_keepalive_seconds" value="60"></label>
</div>
</div>

<!-- MIKROTIK -->
<div class="card">
<h2>🔌 MikroTik</h2>
<div class="grid2">
<label class="fld"><span>Host (gateway IP)</span><input type="text" name="mikrotik_host" placeholder="10.0.11.1"></label>
<label class="fld"><span>Port</span><input type="number" name="mikrotik_port" value="80"></label>
</div>
<label class="fld"><span>Username</span><input type="text" name="mikrotik_username" placeholder="vendo-1-api"></label>
<label class="fld"><span>Password</span>
<div class="pw"><input type="password" name="mikrotik_password">
<button type="button" class="pw-toggle" onclick="togglePw(this)">SHOW</button></div></label>
<div class="msg msg-info" style="margin-top:10px;font-size:12px">
Make sure <code>/ip service enable www</code> is set on MikroTik and the API user exists. Use "Ping MikroTik" above to test.
</div>
</div>

<!-- ADMIN -->
<div class="card">
<h2>👤 Admin (this page)</h2>
<label class="fld"><span>Username</span><input type="text" name="admin_username"></label>
<label class="fld"><span>Password</span>
<div class="pw"><input type="password" name="admin_password">
<button type="button" class="pw-toggle" onclick="togglePw(this)">SHOW</button></div></label>
</div>

<!-- COIN SETTINGS -->
<div class="card">
<h2>🪙 Coin Settings</h2>
<div class="grid3">
<label class="fld"><span>Window (seconds)</span><input type="number" name="coin_window_seconds" min="10" max="300"></label>
<label class="fld"><span>Points Rate</span><input type="number" name="coin_points_rate" step="0.01" min="0" max="1"></label>
<label class="fld"><span>Debounce (ms)</span><input type="number" name="coin_debounce_ms" min="5" max="500"></label>
</div>
</div>

<!-- PINS -->
<div class="card">
<h2>📍 Pins (GPIO)</h2>
<div class="grid2">
<label class="fld"><span>Coinslot Signal</span>
<select name="pins_signal">
<option value="none">none</option>
<option value="D0">D0 (GPIO16)</option>
<option value="D1">D1 (GPIO5)</option>
<option value="D2">D2 (GPIO4) - default</option>
<option value="D3">D3 (GPIO0)</option>
<option value="D4">D4 (GPIO2)</option>
<option value="D5">D5 (GPIO14)</option>
<option value="D6">D6 (GPIO12)</option>
<option value="D7">D7 (GPIO13)</option>
<option value="D8">D8 (GPIO15)</option>
</select></label>
<label class="fld"><span>Coinslot Set (Inhibit)</span>
<select name="pins_set">
<option value="none">none</option>
<option value="D0">D0 (GPIO16)</option>
<option value="D1">D1 (GPIO5) - default</option>
<option value="D2">D2 (GPIO4)</option>
<option value="D3">D3 (GPIO0)</option>
<option value="D4">D4 (GPIO2)</option>
<option value="D5">D5 (GPIO14)</option>
<option value="D6">D6 (GPIO12)</option>
<option value="D7">D7 (GPIO13)</option>
<option value="D8">D8 (GPIO15)</option>
</select></label>
</div>
<div class="grid2">
<label class="fld"><span>LED</span>
<select name="pins_led">
<option value="none">none</option>
<option value="D0">D0 (GPIO16)</option>
<option value="D4">D4 (GPIO2) - built-in</option>
<option value="D5">D5 (GPIO14)</option>
<option value="D6">D6 (GPIO12)</option>
<option value="D7">D7 (GPIO13)</option>
</select></label>
<label class="fld"><span>Buzzer</span>
<select name="pins_buzzer">
<option value="none">none</option>
<option value="D5">D5 (GPIO14)</option>
<option value="D6">D6 (GPIO12)</option>
<option value="D7">D7 (GPIO13)</option>
<option value="D8">D8 (GPIO15)</option>
</select></label>
</div>
<div class="grid3">
<label class="fld"><span>Signal Active Low</span>
<select name="pins_signal_active_low"><option value="true">Yes (falling edge)</option><option value="false">No (rising edge)</option></select></label>
<label class="fld"><span>Set Active Low</span>
<select name="pins_set_active_low"><option value="true">Yes (LOW=enable)</option><option value="false">No (HIGH=enable)</option></select></label>
<label class="fld"><span>A0+3V Reset</span>
<select name="pins_a0_reset_enabled"><option value="true">Enabled</option><option value="false">Disabled</option></select></label>
</div>
</div>

<!-- AUTO RESTART -->
<div class="card">
<h2>♻️ Auto Restart</h2>
<label class="fld" style="display:flex;align-items:center;margin:8px 0">
<input type="checkbox" name="auto_restart_enabled">
<span style="margin-left:6px">Enable scheduled auto-restart</span></label>
<label class="fld"><span>Interval (minutes)</span><input type="number" name="auto_restart_interval" min="1" value="1440"></label>
<div style="font-size:12px;color:#6b7280;margin-top:4px">Default 1440 min = 24h. Reboot waits for active coin session to finish.</div>
</div>

<!-- RATES -->
<div class="card">
<h2>💰 Rates (Pricing Tiers)</h2>
<table id="ratesTable" style="width:100%;border-collapse:collapse;font-size:13px">
<thead><tr><th style="text-align:left;padding:6px">Label</th><th style="width:100px;padding:6px">Pesos</th><th style="width:100px;padding:6px">Minutes</th><th style="width:40px"></th></tr></thead>
<tbody id="ratesBody"></tbody></table>
<button type="button" class="secondary" onclick="addRateRow()" style="margin-top:8px">+ Add Tier</button>
</div>

<!-- SAVE BAR -->
<div class="save-bar">
<button type="button" class="success" onclick="saveAll()">💾 Save All &amp; Reboot</button>
</div>

</form>

<script>
/* ============ UTILS ============ */
function $(id){return document.getElementById(id)}
function el(name){return document.getElementsByName(name)[0]}
function setVal(name,v){var e=el(name);if(!e)return;if(e.type==='checkbox')e.checked=!!v;else e.value=v==null?'':v}
function getVal(name){var e=el(name);if(!e)return null;return e.type==='checkbox'?e.checked:e.value}
function msg(text,kind){var m=$('msg');m.className='msg msg-'+kind;m.textContent=text;m.scrollIntoView({behavior:'smooth',block:'center'})}
function togglePw(btn){var inp=btn.previousElementSibling;if(inp.type==='password'){inp.type='text';btn.textContent='HIDE'}else{inp.type='password';btn.textContent='SHOW'}}
function toggleCollapse(h){h.classList.toggle('collapsed')}
function toggleStaticFields(){var mode=getVal('network_mode');$('staticFields').style.display=(mode==='static')?'':'none'}

/* ============ API ============ */
function api(method,path,body,cb){
  var x=new XMLHttpRequest();
  x.open(method,path,true);
  x.setRequestHeader('Content-Type','application/json');
  x.onreadystatechange=function(){if(x.readyState===4){
    try{cb(x.status,JSON.parse(x.responseText))}catch(e){cb(x.status,{raw:x.responseText})}
  }};
  x.send(body?JSON.stringify(body):null);
}

/* ============ STATUS ============ */
function loadStatus(){
  api('GET','/admin/status',null,function(s,r){
    if(s<200||s>=300){$('statusContent').innerHTML='<span class="status-err">Error loading</span>';return}
    var d=r.data||{};
    var stateClass=d.state==='idle'?'status-ok':d.state==='error'?'status-err':'status-warn';
    var h='';
    h+='<div class="row"><span class="label">Vendo</span><span class="value">'+d.vendo_name+' (#'+d.vendo_id+')</span></div>';
    h+='<div class="row"><span class="label">Firmware</span><span class="value">'+d.firmware+'</span></div>';
    h+='<div class="row"><span class="label">Uptime</span><span class="value">'+Math.floor(d.uptime_s/3600)+'h '+Math.floor((d.uptime_s%3600)/60)+'m '+(d.uptime_s%60)+'s</span></div>';
    h+='<div class="row"><span class="label">Free Heap</span><span class="value">'+(d.free_heap/1024).toFixed(1)+' KB</span></div>';
    h+='<div class="row"><span class="label">WiFi RSSI</span><span class="value">'+d.rssi+' dBm</span></div>';
    h+='<div class="row"><span class="label">IP</span><span class="value">'+d.ip+'</span></div>';
    h+='<div class="row"><span class="label">Coinslot</span><span class="value '+stateClass+'">'+d.state+'</span></div>';
    h+='<div class="row"><span class="label">Pulses (current)</span><span class="value">'+d.pulse_count+'</span></div>';
    $('statusContent').innerHTML=h;
  });
}

/* ============ CONFIG LOAD ============ */
function loadConfig(){
  api('GET','/admin/config_raw',null,function(s,r){
    if(s<200||s>=300){msg('Failed to load config','err');return}
    var c=r.data||{};
    setVal('vendo_id',c.vendo_id);
    setVal('vendo_name',c.vendo_name);
    var n=c.network||{};
    setVal('network_mode',n.mode||'static');
    setVal('network_static_ip',n.static_ip);
    setVal('network_gateway',n.gateway);
    setVal('network_netmask',n.netmask);
    setVal('network_dns',n.dns);
    toggleStaticFields();
    var w=c.wifi||{};
    setVal('wifi_ssid',w.ssid);
    setVal('wifi_password',w.password);
    var m=c.mqtt||{};
    setVal('mqtt_host',m.host);
    setVal('mqtt_port',m.port);
    setVal('mqtt_username',m.username);
    setVal('mqtt_password',m.password);
    setVal('mqtt_keepalive_seconds',m.keepalive_seconds);
    var mt=c.mikrotik||{};
    setVal('mikrotik_host',mt.host);
    setVal('mikrotik_port',mt.port);
    setVal('mikrotik_username',mt.username);
    setVal('mikrotik_password',mt.password);
    var a=c.admin||{};
    setVal('admin_username',a.username);
    setVal('admin_password',a.password);
    var cs=c.coin_settings||{};
    setVal('coin_window_seconds',cs.window_seconds);
    setVal('coin_points_rate',cs.points_rate);
    setVal('coin_debounce_ms',cs.debounce_ms);
    var p=c.pins||{};
    setVal('pins_signal',p.coinslot_signal_pin);
    setVal('pins_set',p.coinslot_set_pin);
    setVal('pins_led',p.led_pin);
    setVal('pins_buzzer',p.buzzer_pin);
    setVal('pins_signal_active_low',String(!!p.signal_active_low));
    setVal('pins_set_active_low',String(!!p.set_active_low));
    setVal('pins_a0_reset_enabled',String(!!p.A0_3V_reset_enabled));
    var ar=c.auto_restart||{};
    setVal('auto_restart_enabled',!!ar.enabled);
    setVal('auto_restart_interval',ar.interval_minutes);
  });
}

/* ============ RATES ============ */
function loadRates(){
  api('GET','/rates',null,function(s,r){
    if(s<200||s>=300)return;
    var t=(r.data&&r.data.tiers)||[];
    $('ratesBody').innerHTML='';
    for(var i=0;i<t.length;i++)addRateRow(t[i]);
  });
}
function addRateRow(t){
  t=t||{label:'',pesos:'',minutes:''};
  var tr=document.createElement('tr');
  tr.innerHTML='<td style="padding:4px"><input type="text" class="rate-label" value="'+(t.label||'').replace(/"/g,'&quot;')+'"></td>'+
    '<td style="padding:4px"><input type="number" class="rate-pesos" value="'+(t.pesos||'')+'"></td>'+
    '<td style="padding:4px"><input type="number" class="rate-minutes" value="'+(t.minutes||'')+'"></td>'+
    '<td style="padding:4px;text-align:center"><button type="button" class="danger" onclick="this.parentNode.parentNode.remove()" style="padding:4px 8px;margin:0">X</button></td>';
  $('ratesBody').appendChild(tr);
}
function collectRates(){
  var rows=$('ratesBody').querySelectorAll('tr');
  var tiers=[];
  for(var i=0;i<rows.length;i++){
    var lbl=rows[i].querySelector('.rate-label').value.trim();
    var p=parseInt(rows[i].querySelector('.rate-pesos').value)||0;
    var mn=parseInt(rows[i].querySelector('.rate-minutes').value)||0;
    if(lbl&&p>0&&mn>0)tiers.push({label:lbl,pesos:p,minutes:mn});
  }
  return {tiers:tiers};
}

/* ============ SAVE ALL ============ */
function buildConfig(){
  return {
    version:1,
    vendo_id:parseInt(getVal('vendo_id'))||1,
    vendo_name:getVal('vendo_name'),
    network:{
      mode:getVal('network_mode'),
      static_ip:getVal('network_static_ip'),
      gateway:getVal('network_gateway'),
      netmask:getVal('network_netmask'),
      dns:getVal('network_dns')
    },
    wifi:{
      ssid:getVal('wifi_ssid'),
      password:getVal('wifi_password')
    },
    mqtt:{
      host:getVal('mqtt_host'),
      port:parseInt(getVal('mqtt_port'))||1883,
      username:getVal('mqtt_username'),
      password:getVal('mqtt_password'),
      keepalive_seconds:parseInt(getVal('mqtt_keepalive_seconds'))||60
    },
    mikrotik:{
      host:getVal('mikrotik_host'),
      port:parseInt(getVal('mikrotik_port'))||80,
      username:getVal('mikrotik_username'),
      password:getVal('mikrotik_password')
    },
    time:{
      backend_url:"http://10.99.0.8:8000/api/time",
      mikrotik_ntp_host:getVal('mikrotik_host'),
      public_ntp_fallback:"pool.ntp.org"
    },
    admin:{
      username:getVal('admin_username'),
      password:getVal('admin_password')
    },
    coin_settings:{
      window_seconds:parseInt(getVal('coin_window_seconds'))||60,
      points_rate:parseFloat(getVal('coin_points_rate'))||0.2,
      debounce_ms:parseInt(getVal('coin_debounce_ms'))||30
    },
    auto_restart:{
      enabled:getVal('auto_restart_enabled'),
      interval_minutes:parseInt(getVal('auto_restart_interval'))||1440,
      notify_backend:true
    },
    pins:{
      coinslot_signal_pin:getVal('pins_signal'),
      coinslot_set_pin:getVal('pins_set'),
      led_pin:getVal('pins_led'),
      buzzer_pin:getVal('pins_buzzer'),
      signal_active_low:getVal('pins_signal_active_low')==='true',
      set_active_low:getVal('pins_set_active_low')==='true',
      A0_3V_reset_enabled:getVal('pins_a0_reset_enabled')==='true'
    },
    config_updated_at:new Date().toISOString()
  };
}

function saveAll(){
  if(!confirm('Save all changes and reboot? The ESP will be offline for ~15 seconds.'))return;
  var cfg=buildConfig();
  var rates=collectRates();
  msg('Saving config...','info');

  api('POST','/admin/config',cfg,function(s1,r1){
    if(s1<200||s1>=300){msg('Config save failed: '+(r1.error?r1.error.message:'unknown'),'err');return}
    api('POST','/admin/rates',rates,function(s2,r2){
      if(s2<200||s2>=300){msg('Rates save failed: '+(r2.error?r2.error.message:'unknown'),'err');return}
      msg('✓ Saved! Rebooting...','ok');
      setTimeout(function(){api('POST','/admin/restart',null,function(){});},800);
    });
  });
}

/* ============ DIAGNOSTICS ============ */
function testOpen(){
  $('diagOut').innerHTML='<div class="msg msg-info">Opening coinslot for 3s...</div>';
  api('POST','/admin/test_open',{duration_ms:3000},function(s,r){
    $('diagOut').innerHTML='<div class="msg msg-ok">✓ Coinslot opened 3s (listen for click)</div>';
  });
}

function pingMikrotik(){
  $('diagOut').innerHTML='<div class="msg msg-info">Pinging MikroTik...</div>';
  api('POST','/admin/ping_mikrotik',null,function(s,r){
    var d=r.data||r.error||{};
    var html='';
    if(r.ok){
      html+='<div class="msg msg-ok">✓ MikroTik reachable! (HTTP '+(d.http_code||200)+')</div>';
      if(d.response)html+='<pre class="diag">'+escHtml(d.response)+'</pre>';
    }else{
      html+='<div class="msg msg-err">✗ MikroTik unreachable</div>';
      html+='<pre class="diag">';
      html+='URL: '+escHtml(d.url||'?')+'\n';
      html+='HTTP Code: '+(d.http_code||'n/a')+'\n';
      html+='Error: '+escHtml(d.error||d.message||'unknown')+'\n';
      if(d.response)html+='Response:\n'+escHtml(d.response);
      html+='</pre>';
      html+='<div class="msg msg-info" style="font-size:12px">Check: /ip service enable www on MikroTik. Verify REST API user exists with full or rest-api group. See serial monitor for details.</div>';
    }
    $('diagOut').innerHTML=html;
  });
}

function escHtml(s){return String(s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}

function factoryReset(){
  if(!confirm('ERASE ALL config files?\n\nESP will reboot into AP mode.\nYou will need to reconfigure from scratch.'))return;
  if(!confirm('Really? This cannot be undone.'))return;
  api('POST','/admin/factory_reset',null,function(){msg('Erased. Rebooting to AP mode...','info')});
}

function restart(){
  if(!confirm('Restart ESP now?'))return;
  api('POST','/admin/restart',null,function(){msg('Rebooting...','info')});
}

/* ============ OTA UPDATE ============ */
function startFirmwareUpdate() {
  var fileInput = $('firmwareFile');
  if (!fileInput.files.length) {
    msg('Please select a .bin file first.', 'err');
    return;
  }
  
  if(!confirm('Flash new firmware? Do NOT disconnect power during this process.')) return;

  var file = fileInput.files[0];
  var formData = new FormData();
  formData.append("update", file, file.name);

  $('otaProgressContainer').style.display = 'block';
  msg('Uploading firmware. Please wait...', 'info');

  var x = new XMLHttpRequest();
  x.open('POST', '/admin/update', true);
  
  x.upload.addEventListener("progress", function(evt) {
    if (evt.lengthComputable) {
      var percentComplete = (evt.loaded / evt.total) * 100;
      $('otaProgressBar').style.width = percentComplete + '%';
    }
  }, false);

  x.onreadystatechange = function() {
    if (x.readyState === 4) {
      if (x.status >= 200 && x.status < 300) {
        msg('✓ Firmware flashed! Vendo is rebooting...', 'ok');
        setTimeout(function(){ window.location.reload(); }, 15000);
      } else {
        msg('Firmware update failed. Check serial logs.', 'err');
        $('otaProgressContainer').style.display = 'none';
      }
    }
  };
  
  x.send(formData);
}

/* ============ BOOT ============ */
loadStatus();loadConfig();loadRates();
setInterval(loadStatus,3000);
</script></body></html>
)HTML";

static void handleAdmin() {
  if (!requireAdminAuth()) return;
  server.send_P(200, "text/html", ADMIN_HTML);
}

static void handleAdminStatus() {
  if (!requireAdminAuth()) return;
  String body = String("{") +
    "\"vendo_id\":" + g_config.vendo_id + "," +
    "\"vendo_name\":\"" + g_config.vendo_name + "\"," +
    "\"firmware\":\"" FIRMWARE_VERSION "\"," +
    "\"uptime_s\":" + (millis() / 1000) + "," +
    "\"free_heap\":" + ESP.getFreeHeap() + "," +
    "\"rssi\":" + WiFi.RSSI() + "," +
    "\"ip\":\"" + WiFi.localIP().toString() + "\"," +
    "\"state\":\"" + (coinslotGetState() == CS_ARMED ? "armed" :
                     coinslotGetState() == CS_COUNTING ? "counting" :
                     coinslotGetState() == CS_FINALIZING ? "finalizing" : "idle") + "\"," +
    "\"pulse_count\":" + coinslotGetPulseCount() +
  "}";
  sendJsonOk(body);
}

static void handleAdminConfig() {
  if (!requireAdminAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String masked = configToJson();
  server.send(200, "application/json",
              String("{\"ok\":true,\"data\":") + masked + "}");
}

static void handleAdminConfigRaw() {
  if (!requireAdminAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String full = configToJsonFull();
  server.send(200, "application/json",
              String("{\"ok\":true,\"data\":") + full + "}");
}

static void handleAdminConfigSave() {
  if (!requireAdminAuth()) return;
  String body = server.arg("plain");
  if (body.length() < 50) {
    sendJsonError(400, "empty", "Config body empty or too small");
    return;
  }
  if (!configSave(body)) {
    sendJsonError(400, "invalid", "Config JSON invalid or missing required fields");
    return;
  }
  sendJsonOk("{\"status\":\"saved\"}");
  // Caller is expected to hit /admin/restart next; we do not auto-reboot here
  // so the success response can reach the browser first.
}

static void handleAdminRatesSave() {
  if (!requireAdminAuth()) return;
  String body = server.arg("plain");
  if (ratesSave(body)) sendJsonOk("{\"status\":\"saved\"}");
  else sendJsonError(400, "invalid", "Could not save rates");
}

static void handleAdminTestOpen() {
  if (!requireAdminAuth()) return;
  StaticJsonDocument<128> doc;
  deserializeJson(doc, server.arg("plain"));
  uint32_t dur = doc["duration_ms"] | 2000;
  coinslotTestOpen(dur);
  sendJsonOk("{\"status\":\"done\"}");
}

static void handleAdminPingMikrotik() {
  if (!requireAdminAuth()) return;
  MikrotikPingResult r = mikrotikPingDetailed();

  // Build JSON with diagnostic info (escape special chars in response/error)
  StaticJsonDocument<1024> doc;
  doc["ok"] = r.ok;
  JsonObject data = doc.createNestedObject(r.ok ? "data" : "error");
  data["http_code"] = r.http_code;
  data["url"] = r.url;
  data["error"] = r.error;
  data["response"] = r.response;
  if (!r.ok) {
    data["code"] = "mikrotik_failed";
    data["message"] = r.error;
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(r.ok ? 200 : 503, "application/json", out);
}

static void handleAdminFactoryReset() {
  if (!requireAdminAuth()) return;
  LittleFS.remove("/config.json");
  LittleFS.remove("/config.backup.json");
  LittleFS.remove("/rates.json");
  LittleFS.remove("/emergency.log");
  sendJsonOk("{\"status\":\"reset\"}");
  delay(500);
  ESP.restart();
}

static void handleAdminRestart() {
  if (!requireAdminAuth()) return;
  sendJsonOk("{\"status\":\"restarting\"}");
  delay(500);
  ESP.restart();
}

static void handleNotFound() {
  sendJsonError(404, "not_found", "Endpoint not found");
}

void httpAutoFinalizeSession() {
  if (coinslotGetState() == CS_IDLE) return;
  
  Serial.println(F("[auto] Timeout reached, auto-finalizing session..."));
  
  coinslotSetState(CS_FINALIZING);
  uint32_t pesos = coinslotGetPulseCount();
  
  if (pesos == 0) {
    // Spammer. The strike is already on their record from handleCoinStart.
    Serial.println(F("[auto] 0 pesos inserted. Disarming."));
    coinslotDisarm();
    s_currentSessionId = "";
    return;
  }

  // They walked away but left money!
  uint32_t minutes = calculateMinutes(pesos);
  
  // They bought time, so clear their strike!
  AbuseRecord* record = getLedgerEntry(s_currentMac);
  if (record) record->mac = ""; 

  mikrotikCreateAndLoginUser(s_currentMac, macWithColons(s_currentMac), s_currentIp, minutes);
  
  coinslotDisarm();
  s_currentSessionId = "";
  Serial.println(F("[auto] Granted time to abandoned session."));
}

// ---------- Init ----------
void httpServerInit() {
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/ping", HTTP_OPTIONS, handleOptions);

  server.on("/coin/start", HTTP_POST, handleCoinStart);
  server.on("/coin/start", HTTP_OPTIONS, handleOptions);

  server.on("/coin/done", HTTP_POST, handleCoinDone);
  server.on("/coin/done", HTTP_OPTIONS, handleOptions);

  server.on("/coin/cancel", HTTP_POST, handleCoinCancel);
  server.on("/coin/cancel", HTTP_OPTIONS, handleOptions);

  server.on("/rates", HTTP_GET, handleRates);
  server.on("/rates", HTTP_OPTIONS, handleOptions);

  // /status/{session_id} - match any path under /status/
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/status/")) {
      handleStatus();
      return;
    }
    handleNotFound();
  });

  // Admin
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/status", HTTP_GET, handleAdminStatus);
  server.on("/admin/config", HTTP_GET, handleAdminConfig);
  server.on("/admin/config", HTTP_POST, handleAdminConfigSave);
  server.on("/admin/config_raw", HTTP_GET, handleAdminConfigRaw);
  server.on("/admin/rates", HTTP_POST, handleAdminRatesSave);
  server.on("/admin/test_open", HTTP_POST, handleAdminTestOpen);
  server.on("/admin/ping_mikrotik", HTTP_POST, handleAdminPingMikrotik);
  server.on("/admin/factory_reset", HTTP_POST, handleAdminFactoryReset);
  server.on("/admin/restart", HTTP_POST, handleAdminRestart);

// ---------- Firmware OTA Update Endpoint ----------
  server.on("/admin/update", HTTP_POST, 
    []() {
      // 1. This runs when the upload is completely finished
      server.sendHeader("Connection", "close");
      server.send(200, "application/json", Update.hasError() ? "{\"ok\":false}" : "{\"ok\":true}");
      delay(1000);
      ESP.restart();
    }, 
    []() {
      // 2. This runs continuously as the file chunks stream in
      HTTPUpload& upload = server.upload();
      
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Update Start: %s\n", upload.filename.c_str());
        // Calculate safe space to avoid overwriting LittleFS
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace, U_FLASH)) { // U_FLASH targets firmware
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { // true = empty the buffers
          Serial.printf("[OTA] Update Success: %u bytes\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();
  Serial.println(F("[http] server started on :80"));
}

void httpServerLoop() {
  server.handleClient();
}

bool shouldArmCoinslot() { return false; }
void clearArmRequest() {}

