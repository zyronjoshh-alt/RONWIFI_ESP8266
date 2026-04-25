#include "setup_ap.h"
#include "config.h"
#include "wifi_manager.h"
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char SETUP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RONwifi Setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#f3f4f6;color:#1f2937;max-width:500px;margin:0 auto;padding:16px}
h1{color:#2563eb;font-size:22px;margin:8px 0 8px}
.subtitle{color:#6b7280;font-size:13px;margin-bottom:16px}
.card{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:16px;margin-bottom:12px}
h2{font-size:15px;margin:0 0 12px;color:#1f2937}
label{display:block;font-size:12px;color:#6b7280;margin:8px 0 4px;font-weight:600}
input,textarea,select{width:100%;padding:10px;border:1px solid #e5e7eb;border-radius:6px;font-size:14px;box-sizing:border-box;font-family:inherit}
textarea{font-family:monospace;font-size:12px;min-height:260px}
button{background:#2563eb;color:#fff;border:none;padding:12px 16px;border-radius:6px;cursor:pointer;font-size:14px;width:100%;font-weight:600;margin-top:8px}
button.secondary{background:#6b7280}
.tabs{display:flex;gap:4px;margin-bottom:12px}
.tab{flex:1;padding:10px;text-align:center;background:#fff;border:1px solid #e5e7eb;border-radius:6px;cursor:pointer;font-size:13px}
.tab.active{background:#2563eb;color:#fff;border-color:#2563eb}
.pane{display:none}
.pane.active{display:block}
.msg{padding:10px;border-radius:6px;margin-top:10px;font-size:13px}
.msg-ok{background:#d1fae5;color:#065f46}
.msg-err{background:#fee2e2;color:#991b1b}
.hint{font-size:11px;color:#6b7280;margin-top:4px}
</style></head><body>
<h1>RONwifi Setup</h1>
<p class="subtitle">Initial configuration for this vendo</p>

<div class="tabs">
<div class="tab active" onclick="showTab('manual')">Manual</div>
<div class="tab" onclick="showTab('upload')">Upload Backup</div>
<div class="tab" onclick="showTab('json')">Raw JSON</div>
</div>

<div id="pane-manual" class="pane active">
<div class="card">
<h2>Vendo Identity</h2>
<label>Vendo ID</label>
<input id="vendo_id" type="number" value="1">
<label>Vendo Name</label>
<input id="vendo_name" value="VENDO1-BALE">
</div>

<div class="card">
<h2>WiFi</h2>
<label>SSID</label>
<input id="wifi_ssid" placeholder="RONwifi-Mgmt">
<label>Password</label>
<input id="wifi_password" type="password">
</div>

<div class="card">
<h2>Network</h2>
<label>Mode</label>
<select id="net_mode"><option value="static">Static</option><option value="dhcp">DHCP</option></select>
<label>Static IP</label>
<input id="net_ip" value="10.0.11.7">
<label>Gateway</label>
<input id="net_gw" value="10.0.11.1">
<label>Netmask</label>
<input id="net_mask" value="255.255.255.0">
<label>DNS</label>
<input id="net_dns" value="10.0.11.1">
</div>

<div class="card">
<h2>MQTT</h2>
<label>Host</label>
<input id="mqtt_host" value="10.99.0.8">
<label>Port</label>
<input id="mqtt_port" type="number" value="1883">
<label>Username</label>
<input id="mqtt_user" value="vendo-1">
<label>Password</label>
<input id="mqtt_pass" type="password">
</div>

<div class="card">
<h2>MikroTik</h2>
<label>Host</label>
<input id="mt_host" value="10.0.11.1">
<label>Port</label>
<input id="mt_port" type="number" value="80">
<label>Username</label>
<input id="mt_user" value="vendo-1-api">
<label>Password</label>
<input id="mt_pass" type="password">
</div>

<div class="card">
<h2>Admin (for /admin page)</h2>
<label>Username</label>
<input id="adm_user" value="admin">
<label>Password</label>
<input id="adm_pass" type="password" value="admin">
</div>

<button onclick="saveManual()">Save & Reboot</button>
</div>

<div id="pane-upload" class="pane">
<div class="card">
<h2>Upload Backup File</h2>
<p class="subtitle">Select a .ronwifi.json backup file previously exported from backend.</p>
<input type="file" id="backupFile" accept=".json,application/json">
<button onclick="uploadBackup()">Load & Reboot</button>
</div>
</div>

<div id="pane-json" class="pane">
<div class="card">
<h2>Paste Config JSON</h2>
<textarea id="jsonArea" placeholder='{"version":1,"vendo_id":1,...}'></textarea>
<button onclick="saveJson()">Save & Reboot</button>
</div>
</div>

<div id="msg"></div>

<script>
function showTab(n){
  var tabs=document.querySelectorAll('.tab');
  var panes=document.querySelectorAll('.pane');
  for(var i=0;i<tabs.length;i++){tabs[i].classList.remove('active');panes[i].classList.remove('active')}
  var idx={manual:0,upload:1,json:2}[n];
  tabs[idx].classList.add('active');
  document.getElementById('pane-'+n).classList.add('active');
}
function msg(text,ok){
  var m=document.getElementById('msg');
  m.className='msg '+(ok?'msg-ok':'msg-err');
  m.textContent=text;
}
function val(id){return document.getElementById(id).value}
function buildConfig(){
  return {
    version:1,
    vendo_id:parseInt(val('vendo_id'))||1,
    vendo_name:val('vendo_name'),
    network:{mode:val('net_mode'),static_ip:val('net_ip'),gateway:val('net_gw'),netmask:val('net_mask'),dns:val('net_dns')},
    wifi:{ssid:val('wifi_ssid'),password:val('wifi_password')},
    mqtt:{host:val('mqtt_host'),port:parseInt(val('mqtt_port'))||1883,username:val('mqtt_user'),password:val('mqtt_pass'),keepalive_seconds:60},
    mikrotik:{host:val('mt_host'),port:parseInt(val('mt_port'))||80,username:val('mt_user'),password:val('mt_pass')},
    time:{backend_url:"http://10.99.0.8:8000/api/time",mikrotik_ntp_host:val('mt_host'),public_ntp_fallback:"pool.ntp.org"},
    admin:{username:val('adm_user'),password:val('adm_pass')},
    coin_settings:{window_seconds:60,points_rate:0.20,debounce_ms:30},
    auto_restart:{enabled:false,interval_minutes:1440,notify_backend:true},
    pins:{coinslot_signal_pin:"D2",coinslot_set_pin:"D1",led_pin:"none",buzzer_pin:"none",signal_active_low:true,set_active_low:true,A0_3V_reset_enabled:true}
  };
}
function post(body){
  var x=new XMLHttpRequest();
  x.open('POST','/save',true);
  x.setRequestHeader('Content-Type','application/json');
  x.onreadystatechange=function(){
    if(x.readyState===4){
      if(x.status>=200&&x.status<300){msg('Saved! Rebooting in 3s...',true);setTimeout(function(){},3000)}
      else{msg('Error: '+x.responseText,false)}
    }
  };
  x.send(body);
}
function saveManual(){
  if(!val('wifi_ssid')){msg('WiFi SSID required',false);return}
  post(JSON.stringify(buildConfig()));
}
function saveJson(){
  try{JSON.parse(val('jsonArea'))}catch(e){msg('Invalid JSON: '+e,false);return}
  post(val('jsonArea'));
}
function uploadBackup(){
  var f=document.getElementById('backupFile').files[0];
  if(!f){msg('Select a file first',false);return}
  var r=new FileReader();
  r.onload=function(){post(r.result)};
  r.readAsText(f);
}
</script></body></html>
)HTML";

void runSetupMode() {
  wifiStartSetupAP();

  ESP8266WebServer server(80);

  server.on("/", HTTP_GET, [&server]() {
    server.send_P(200, "text/html", SETUP_HTML);
  });

  server.on("/save", HTTP_POST, [&server]() {
    String body = server.arg("plain");
    if (body.length() < 10) {
      server.send(400, "text/plain", "empty body");
      return;
    }
    if (!configSave(body)) {
      server.send(400, "text/plain", "invalid config JSON");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(2000);
    ESP.restart();
  });

  // Captive-portal-ish redirect for any other path
  server.onNotFound([&server]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println(F("[setup] AP setup server running on 192.168.4.1"));

  // Block here forever — the only way out is ESP.restart() after config save
  while (true) {
    server.handleClient();
    delay(10);
    yield();
  }
}
