from pathlib import Path
import re

fw=Path('Plant_Watering_Smart_V2_OTA.ino')
s=fw.read_text()

# Reuse the already-reviewed weather implementation from the first patch script.
if 'WEATHER_API_URL' not in s:
    source=Path('.github/weather_dashboard_patch.py').read_text()
    add=source.split("add=r'''",1)[1].split("'''",1)[0]
    marker='static const char *OTA_USER = "admin";'
    if marker not in s: raise SystemExit('weather marker missing')
    s=s.replace(marker,marker+add,1)

if 'if(!manual&&!weatherAllowsAutomaticWatering())return false;' not in s:
    marker='if(!bootAllowsWatering()||emergencyStop||tankEmpty||s.sensorFault||s.noFlowFault||s.waterResponseFault)return false;'
    if marker not in s: raise SystemExit('canStartPlant marker missing')
    s=s.replace(marker,marker+'if(!manual&&!weatherAllowsAutomaticWatering())return false;',1)

old='void maintainWifi(){bool c=WiFi.status()==WL_CONNECTED;if(c){if(!wifiConnected){wifiConnected=true;deviceIp=WiFi.localIP().toString();Serial.print("WiFi connected: ");Serial.println(deviceIp);configTime(0,0,"pool.ntp.org","time.nist.gov");}return;}wifiConnected=false;deviceIp="offline";if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){lastWifiRetryMs=millis();WiFi.disconnect();beginWifi();}}'
if old in s:
    new='''void maintainWifi(){bool c=WiFi.status()==WL_CONNECTED;if(c){if(!wifiConnected){wifiConnected=true;deviceIp=WiFi.localIP().toString();prefs.putBool("wifiWasConnected",true);Serial.print("WiFi connected: ");Serial.println(deviceIp);configTime(0,0,"pool.ntp.org","time.nist.gov");recordConnectivityEvent("WIFI_CONNECTED",deviceIp);flushConnectivityHistory();}return;}if(wifiConnected){saveConnectivityEventLocal("WIFI_DISCONNECTED","WiFi connection lost");wifiConnected=false;deviceIp="offline";}else deviceIp="offline";if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){lastWifiRetryMs=millis();WiFi.disconnect();beginWifi();}}'''
    s=s.replace(old,new,1)
else:
    raise SystemExit('maintainWifi marker missing')

if 'refreshWeather();flushConnectivityHistory();' not in s:
    m=re.search(r'void\s+loop\s*\(\s*\)\s*\{',s)
    if not m: raise SystemExit('loop function missing')
    s=s[:m.end()]+'\n  refreshWeather();\n  flushConnectivityHistory();'+s[m.end():]

if 'wifiWasConnected' not in s:
    marker='prefs.begin("plantV2",false);'
    if marker not in s: raise SystemExit('prefs marker missing')
    s=s.replace(marker,marker+'if(prefs.getBool("wifiWasConnected",false))saveConnectivityEventLocal("DEVICE_RESTART","Previous session ended without a clean Wi-Fi disconnect");prefs.putBool("wifiWasConnected",false);',1)

fw.write_text(s)

# Dashboard HTML loads the new visual/connection assets.
p=Path('v2/index.html'); h=p.read_text()
if 'plant-icon.svg' not in h:
    h=h.replace('<link rel="stylesheet" href="./styles.css">','<link rel="icon" type="image/svg+xml" href="./plant-icon.svg">\n  <link rel="stylesheet" href="./styles.css">\n  <link rel="stylesheet" href="./enhancements.css">',1)
if 'id="weatherStatus"' not in h:
    h=h.replace('<article class="summary-card"><span>Tank</span>','<article class="summary-card weather-summary"><span>🌦️ Bengaluru Weather</span><strong id="weatherStatus">--</strong><small id="weatherHint">--</small></article>\n          <article class="summary-card"><span>🪣 Tank</span>',1)
if 'connectivityHistory' not in h:
    h=h.replace('<div id="history" class="history"><div class="empty">No watering history yet.</div></div>','<div id="history" class="history"><div class="empty">No watering history yet.</div></div>\n            <div class="section-head connectivity-head"><div><h2>Connectivity History</h2><p>Wi-Fi events are buffered locally when the network is unavailable and uploaded after reconnection.</p></div></div>\n            <div id="connectivityHistory" class="history"><div class="empty">No connectivity events yet.</div></div>',1)
if 'enhancements.js' not in h:
    h=h.replace('<script src="./app.js"></script>','<script src="./app.js"></script>\n  <script src="./enhancements.js"></script>',1)
p.write_text(h)
print('v2 patch applied')
