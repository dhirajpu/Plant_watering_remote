from pathlib import Path
import re

fw=Path('Plant_Watering_Smart_V2_OTA.ino')
s=fw.read_text()

if 'WEATHER_API_URL' not in s:
    marker='static const char *OTA_USER = "admin";'
    add=r'''

// ========================= WEATHER / CONNECTIVITY =========================
// Bengaluru weather is an additional AUTO-watering signal. Soil moisture and
// all existing safety limits remain authoritative.
static const char *WEATHER_API_URL = "https://api.open-meteo.com/v1/forecast?latitude=12.9716&longitude=77.5946&hourly=precipitation_probability,precipitation&forecast_days=2&timezone=Asia%2FKolkata";
static const unsigned long WEATHER_REFRESH_MS = 30UL*60UL*1000UL;
static const int WEATHER_RAIN_PROBABILITY_THRESHOLD = 60;
static const float WEATHER_RAIN_MM_THRESHOLD = 1.0f;
static const int WEATHER_LOOKAHEAD_HOURS = 6;
static const int CONNECTIVITY_HISTORY_MAX = 20;
bool weatherValid=false, weatherDelayAuto=false;
int weatherRainProbability=0;
float weatherRainMm6h=0.0f;
unsigned long lastWeatherMs=0, weatherUpdatedMs=0;

String weatherArrayNumber(const String &json,const String &key,int index){
  String token="\""+key+"\""; int p=json.indexOf(token); if(p<0)return "-1";
  p=json.indexOf('[',p+token.length()); if(p<0)return "-1";
  int n=0,start=-1; bool inNumber=false;
  for(int i=p+1;i<(int)json.length();i++){
    char c=json[i]; if(c==']')break;
    bool numeric=(c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E';
    if(numeric&&!inNumber){inNumber=true;start=i;}
    if(!numeric&&inNumber){if(n==index)return json.substring(start,i);n++;inNumber=false;start=-1;}
  }
  if(inNumber&&n==index)return json.substring(start); return "-1";
}
void refreshWeather(){
  if(!wifiConnected||!timeSynced||otaInProgress)return;
  if(lastWeatherMs&&millis()-lastWeatherMs<WEATHER_REFRESH_MS)return;
  lastWeatherMs=millis(); secureClient.setInsecure(); HTTPClient http;
  if(!http.begin(secureClient,WEATHER_API_URL)){weatherValid=false;return;}
  http.setTimeout(5000); int code=http.GET();
  if(code<200||code>=300){http.end();weatherValid=false;return;}
  String body=http.getString(); http.end();
  time_t now=time(nullptr); if(now<1700000000){weatherValid=false;return;}
  struct tm utcNow; gmtime_r(&now,&utcNow); int hour=(utcNow.tm_hour+5)%24;
  if(utcNow.tm_min>=30)hour=(hour+1)%24;
  int maxProbability=0; float rainMm=0; bool parsed=false;
  for(int off=0;off<WEATHER_LOOKAHEAD_HOURS;off++){
    String p=weatherArrayNumber(body,"precipitation_probability",hour+off);
    String mm=weatherArrayNumber(body,"precipitation",hour+off);
    if(p=="-1"||mm=="-1")continue;
    maxProbability=max(maxProbability,p.toInt()); float m=mm.toFloat(); if(m>0)rainMm+=m; parsed=true;
  }
  if(!parsed){weatherValid=false;return;}
  weatherRainProbability=constrain(maxProbability,0,100); weatherRainMm6h=rainMm;
  weatherDelayAuto=weatherRainProbability>=WEATHER_RAIN_PROBABILITY_THRESHOLD&&weatherRainMm6h>=WEATHER_RAIN_MM_THRESHOLD;
  weatherValid=true; weatherUpdatedMs=millis();
  Serial.printf("Bengaluru weather: rain=%d%%, next %dh=%.1fmm, delay=%s\n",weatherRainProbability,WEATHER_LOOKAHEAD_HOURS,weatherRainMm6h,weatherDelayAuto?"YES":"NO");
}
bool weatherAllowsAutomaticWatering(){return !weatherValid||!weatherDelayAuto;}
String connectivityKey(int slot){return "conn"+String(slot);}
void saveConnectivityEventLocal(const String &type,const String &reason){
  int head=prefs.getInt("connHead",0),count=prefs.getInt("connCount",0); uint64_t now=epochMs();
  String j="{\"type\":\""+type+"\",\"reason\":\""+reason+"\",\"epochMs\":"+String((unsigned long long)now)+",\"uptimeMs\":"+String(millis())+"}";
  prefs.putString(connectivityKey(head).c_str(),j); head=(head+1)%CONNECTIVITY_HISTORY_MAX; if(count<CONNECTIVITY_HISTORY_MAX)count++;
  prefs.putInt("connHead",head);prefs.putInt("connCount",count);
}
void publishConnectivityEvent(const String &j){httpPostJson("/history/connectivity",j);}
void flushConnectivityHistory(){
  if(!wifiConnected||otaInProgress)return; int head=prefs.getInt("connHead",0),count=prefs.getInt("connCount",0); if(count<=0)return;
  int oldest=(head-count+CONNECTIVITY_HISTORY_MAX)%CONNECTIVITY_HISTORY_MAX;
  for(int n=0;n<count;n++){int slot=(oldest+n)%CONNECTIVITY_HISTORY_MAX;String e=prefs.getString(connectivityKey(slot).c_str(),"");if(e.length()){publishConnectivityEvent(e);prefs.remove(connectivityKey(slot).c_str());}}
  prefs.putInt("connHead",0);prefs.putInt("connCount",0);
}
void recordConnectivityEvent(const String &type,const String &reason){
  String j="{\"type\":\""+type+"\",\"reason\":\""+reason+"\",\"epochMs\":"+String((unsigned long long)epochMs())+",\"uptimeMs\":"+String(millis())+"}";
  if(wifiConnected)publishConnectivityEvent(j);else saveConnectivityEventLocal(type,reason);
}
'''
    if marker not in s: raise SystemExit('weather marker missing')
    s=s.replace(marker,marker+add,1)

if 'weatherAllowsAutomaticWatering()' not in s or 'if(!manual&&!weatherAllowsAutomaticWatering())' not in s:
    marker='if(!bootAllowsWatering()||emergencyStop||tankEmpty||s.sensorFault||s.noFlowFault||s.waterResponseFault)return false;'
    if marker not in s: raise SystemExit('canStartPlant marker missing')
    s=s.replace(marker,marker+'if(!manual&&!weatherAllowsAutomaticWatering())return false;',1)

old='void maintainWifi(){bool c=WiFi.status()==WL_CONNECTED;if(c){if(!wifiConnected){wifiConnected=true;deviceIp=WiFi.localIP().toString();Serial.print("WiFi connected: ");Serial.println(deviceIp);configTime(0,0,"pool.ntp.org","time.nist.gov");}return;}wifiConnected=false;deviceIp="offline";if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){lastWifiRetryMs=millis();WiFi.disconnect();beginWifi();}}'
if old in s:
    new='''void maintainWifi(){bool c=WiFi.status()==WL_CONNECTED;if(c){if(!wifiConnected){wifiConnected=true;deviceIp=WiFi.localIP().toString();prefs.putBool("wifiWasConnected",true);Serial.print("WiFi connected: ");Serial.println(deviceIp);configTime(0,0,"pool.ntp.org","time.nist.gov");recordConnectivityEvent("WIFI_CONNECTED",deviceIp);flushConnectivityHistory();}return;}if(wifiConnected){saveConnectivityEventLocal("WIFI_DISCONNECTED","WiFi connection lost");wifiConnected=false;deviceIp="offline";}else deviceIp="offline";if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){lastWifiRetryMs=millis();WiFi.disconnect();beginWifi();}}'''
    s=s.replace(old,new,1)
else:
    raise SystemExit('maintainWifi exact marker not found')

if 'refreshWeather();flushConnectivityHistory();' not in s:
    marker='if(millis()-lastStatusMs>=REMOTE_STATUS_MS){lastStatusMs=millis();publishStatus();}'
    if marker not in s: raise SystemExit('loop status marker missing')
    s=s.replace(marker,marker+'if(wifiConnected){refreshWeather();flushConnectivityHistory();}',1)

if 'wifiWasConnected' not in s:
    marker='prefs.begin("plantV2",false);'
    if marker not in s: raise SystemExit('prefs marker missing')
    s=s.replace(marker,marker+'if(prefs.getBool("wifiWasConnected",false))saveConnectivityEventLocal("DEVICE_RESTART","Previous session ended without a clean Wi-Fi disconnect");prefs.putBool("wifiWasConnected",false);',1)

if 'weatherValid' not in s[s.find('void publishStatus'):s.find('void publishStatus')+4000]:
    marker='j+="\\"wifiRssi\\":"+String(wifiConnected?WiFi.RSSI():0)+'
    if marker not in s: raise SystemExit('status marker missing')
    s=s.replace(marker,marker+'"\\"weatherValid\\":"+boolJson(weatherValid)+","+"\\"weatherRainProbability\\":"+String(weatherRainProbability)+","+"\\"weatherRainMm6h\\":"+String(weatherRainMm6h,1)+","+"\\"weatherDelayAuto\\":"+boolJson(weatherDelayAuto)+',1)

fw.write_text(s)

# Dashboard index enhancements. The existing enhancement assets are separate so the patch is small.
p=Path('v2/index.html'); h=p.read_text()
h=h.replace('<link rel="stylesheet" href="./styles.css">','<link rel="icon" type="image/svg+xml" href="./plant-icon.svg">\n  <link rel="stylesheet" href="./styles.css">\n  <link rel="stylesheet" href="./enhancements.css">',1)
h=h.replace('<article class="summary-card"><span>Tank</span>','<article class="summary-card weather-summary"><span>🌦️ Bengaluru Weather</span><strong id="weatherStatus">--</strong><small id="weatherHint">--</small></article>\n          <article class="summary-card"><span>🪣 Tank</span>',1)
h=h.replace('<article class="summary-card"><span>Flow</span>','<article class="summary-card"><span>💧 Flow</span>',1)
h=h.replace('<div id="history" class="history"><div class="empty">No watering history yet.</div></div>','<div id="history" class="history"><div class="empty">No watering history yet.</div></div>\n          <div class="section-head connectivity-head"><div><h2>Connectivity History</h2><p>Wi-Fi events are buffered locally when the network is unavailable and uploaded after reconnection.</p></div></div>\n          <div id="connectivityHistory" class="history"><div class="empty">No connectivity events yet.</div></div>',1)
h=h.replace('<script src="./app.js"></script>','<script src="./app.js"></script>\n  <script src="./enhancements.js"></script>',1)
p.write_text(h)
print('weather firmware/dashboard patch applied')
