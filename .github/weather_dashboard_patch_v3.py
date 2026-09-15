from pathlib import Path
import re

fw=Path('Plant_Watering_Smart_V2_OTA.ino')
s=fw.read_text()

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

def replace_function(src,name,replacement):
    m=re.search(r'\bvoid\s+'+re.escape(name)+r'\s*\(\s*\)\s*\{',src)
    if not m: raise SystemExit(name+' function missing')
    brace=m.end()-1; depth=0
    for i in range(brace,len(src)):
        if src[i]=='{': depth+=1
        elif src[i]=='}':
            depth-=1
            if depth==0:return src[:m.start()]+replacement+src[i+1:]
    raise SystemExit(name+' closing brace missing')

wifi='''void maintainWifi(){\n  bool c=WiFi.status()==WL_CONNECTED;\n  if(c){\n    if(!wifiConnected){\n      wifiConnected=true; deviceIp=WiFi.localIP().toString();\n      prefs.putBool("wifiWasConnected",true);\n      Serial.print("WiFi connected: "); Serial.println(deviceIp);\n      configTime(0,0,"pool.ntp.org","time.nist.gov");\n      recordConnectivityEvent("WIFI_CONNECTED",deviceIp);\n      flushConnectivityHistory();\n    }\n    return;\n  }\n  if(wifiConnected){\n    saveConnectivityEventLocal("WIFI_DISCONNECTED","WiFi connection lost");\n    wifiConnected=false; deviceIp="offline";\n  }else deviceIp="offline";\n  if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){\n    lastWifiRetryMs=millis(); WiFi.disconnect(); beginWifi();\n  }\n}'''
s=replace_function(s,'maintainWifi',wifi)

if 'refreshWeather();' not in s:
    m=re.search(r'void\s+loop\s*\(\s*\)\s*\{',s)
    if not m: raise SystemExit('loop missing')
    s=s[:m.end()]+'\n  refreshWeather();\n  flushConnectivityHistory();'+s[m.end():]

if 'wifiWasConnected' not in s:
    marker='prefs.begin("plantV2",false);'
    if marker not in s: raise SystemExit('prefs marker missing')
    s=s.replace(marker,marker+'if(prefs.getBool("wifiWasConnected",false))saveConnectivityEventLocal("DEVICE_RESTART","Previous session ended without a clean Wi-Fi disconnect");prefs.putBool("wifiWasConnected",false);',1)

fw.write_text(s)

p=Path('v2/index.html'); h=p.read_text()
if 'plant-icon.svg' not in h:h=h.replace('<link rel="stylesheet" href="./styles.css">','<link rel="icon" type="image/svg+xml" href="./plant-icon.svg">\n  <link rel="stylesheet" href="./styles.css">\n  <link rel="stylesheet" href="./enhancements.css">',1)
if 'id="weatherStatus"' not in h:h=h.replace('<article class="summary-card"><span>Tank</span>','<article class="summary-card weather-summary"><span>🌦️ Bengaluru Weather</span><strong id="weatherStatus">--</strong><small id="weatherHint">--</small></article>\n          <article class="summary-card"><span>🪣 Tank</span>',1)
if 'connectivityHistory' not in h:h=h.replace('<div id="history" class="history"><div class="empty">No watering history yet.</div></div>','<div id="history" class="history"><div class="empty">No watering history yet.</div></div>\n            <div class="section-head connectivity-head"><div><h2>Connectivity History</h2><p>Wi-Fi events are buffered locally when the network is unavailable and uploaded after reconnection.</p></div></div>\n            <div id="connectivityHistory" class="history"><div class="empty">No connectivity events yet.</div></div>',1)
if 'enhancements.js' not in h:h=h.replace('<script src="./app.js"></script>','<script src="./app.js"></script>\n  <script src="./enhancements.js"></script>',1)
p.write_text(h)
print('v3 patch applied')
