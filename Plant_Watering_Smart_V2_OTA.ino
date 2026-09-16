#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <esp_system.h>

/*
  Plant Watering Smart V2 - OTA / Hardened build

  This is the next V2 candidate. The original multi sensor code.ino and the
  first Plant_Watering_Smart_V2.ino are intentionally preserved as fallback.

  Safety additions in this build:
  - ArduinoOTA + authenticated local web OTA
  - Pump/valves forced OFF before startup and before OTA
  - Stale remote command rejection
  - One fixed water-response deadline per automatic session
  - Manual requests have one explicit target (ml when flow is enabled, time otherwise)
  - Hour/day runtime limits persisted in NVS with epoch windows when NTP is available
  - NVS writes are limited to configuration, commands and completed sessions
  - Remote settings are range checked

  IMPORTANT:
  - Change Wi-Fi credentials and OTA_PASSWORD before production use.
  - FIREBASE_AUTH must be secured with proper Firebase Authentication/Rules.
  - setInsecure() is retained only for initial testing; certificate validation is
    required before treating remote pump control as production-secure.
*/

// ========================= NETWORK =========================
static const char *WIFI_SSID = "";
static const char *WIFI_PASSWORD = "";
static const char *FIREBASE_BASE_URL = "https://vernal-catfish-196407.firebaseio.com";
static const char *DEVICE_ROOT = "/plantMonitor/v2";
static const char *FIREBASE_AUTH = "";

// Local OTA security. Change this before flashing.
static const char *OTA_PASSWORD = "";
static const char *FIRMWARE_VERSION = "Plant_Watering_Smart_V2_OTA_1.0";
static const char *OTA_USER = "admin";

// ========================= HARDWARE =========================
static const int NUM_PLANTS = 5;
static const int SENSOR_PINS[NUM_PLANTS] = {34, 35, 32, 33, 39};
static const int PUMP_PIN = 23;
static const int VALVE_PINS[NUM_PLANTS] = {14, 25, 26, 27, 13};
static const int LCD_ADDRESS = 0x27;
static const int LCD_COLS = 16;
static const int LCD_ROWS = 2;
static const int SDA_PIN = 21;
static const int SCL_PIN = 22;

static const bool TANK_SENSOR_ENABLED = false;
static const int TANK_FLOAT_PIN = 18;
static const int TANK_EMPTY_LEVEL = LOW;
static const bool FLOW_SENSOR_ENABLED = false;
static const int FLOW_SENSOR_PIN = 19;
static const float FLOW_PULSES_PER_LITER = 450.0f;

static const int RELAY_ON = LOW;
static const int RELAY_OFF = HIGH;

// ========================= TIMING / SAFETY =========================
static const unsigned long POWER_STABILIZE_MS = 2500;
static const unsigned long BOOT_WATERING_GRACE_MS = 30000;
static const int REQUIRED_VALID_BOOT_SCANS = 5;
static const unsigned long SENSOR_SAMPLE_MS = 1000;
static const int SENSOR_SAMPLES = 10;
static const unsigned long LCD_UPDATE_MS = 1500;
static const unsigned long LCD_ROTATE_MS = 5000;
static const unsigned long REMOTE_STATUS_MS = 5000;
static const unsigned long REMOTE_COMMAND_MS = 3000;
static const unsigned long REMOTE_CONFIG_MS = 15000;
static const unsigned long TELEMETRY_HISTORY_MS = 5UL * 60UL * 1000UL;
static const unsigned long WIFI_RETRY_MS = 30000;
static const unsigned long OTA_IDLE_TIMEOUT_MS = 15UL * 60UL * 1000UL;

static const unsigned long VALVE_PREOPEN_MS = 300;
static const unsigned long VALVE_POSTPUMP_MS = 500;
static const unsigned long DEFAULT_SOAK_MS = 120000;
static const unsigned long DEFAULT_MIN_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const unsigned long DEFAULT_MAX_BURST_MS = 15000;
static const unsigned long DEFAULT_MAX_SESSION_MS = 45000;
static const unsigned long DEFAULT_MAX_HOURLY_MS = 90000;
static const unsigned long DEFAULT_MAX_DAILY_MS = 180000;
static const unsigned long NO_FLOW_TIMEOUT_MS = 5000;
static const unsigned long WATER_RESPONSE_TIMEOUT_MS = 180000;
static const unsigned long REMOTE_COMMAND_MAX_AGE_MS = 10UL * 60UL * 1000UL;
static const unsigned long REMOTE_COMMAND_FUTURE_SKEW_MS = 2UL * 60UL * 1000UL;
static const int MIN_MOISTURE_GAIN_PERCENT = 2;
static const int SENSOR_MIN_VALID = 80;
static const int SENSOR_MAX_VALID = 4095;
static const int SENSOR_CHANGE_DELTA = 4;
static const unsigned long SENSOR_STALE_MS = 6UL * 60UL * 60UL * 1000UL;

enum PlantMode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_DISABLED = 2 };
enum StopReason : uint8_t {
  STOP_NONE = 0, STOP_TARGET_REACHED, STOP_BURST_COMPLETE, STOP_SESSION_LIMIT,
  STOP_HOURLY_LIMIT, STOP_DAILY_LIMIT, STOP_TANK_EMPTY, STOP_NO_FLOW,
  STOP_SENSOR_FAULT, STOP_EMERGENCY, STOP_MANUAL_COMPLETE, STOP_COMMAND_EXPIRED
};

struct PlantConfig {
  String name; int airRaw; int wetRaw; int targetLow; int targetHigh;
  unsigned long burstMs; uint32_t burstMl; unsigned long soakMs;
  unsigned long minIntervalMs; unsigned long maxBurstMs; unsigned long maxSessionMs;
  unsigned long maxHourlyMs; unsigned long maxDailyMs; PlantMode mode;
};

struct PlantState {
  int raw; int moisture; int previousRaw; bool sensorFault; int validBootScans;
  unsigned long lastSensorChangeMs;
  bool watering; bool soaking; bool manualRequest;
  uint32_t manualRequestedMl; unsigned long manualRequestedMs;
  unsigned long burstStartMs; unsigned long sessionStartMs; unsigned long soakUntilMs;
  unsigned long lastWateredMs; unsigned long sessionPumpMs;
  unsigned long hourlyPumpMs; unsigned long dailyPumpMs;
  unsigned long hourWindowStartMs; unsigned long dayWindowStartMs;
  int moistureAtSessionStart; int moistureAfterLastBurst;
  unsigned long responseDeadlineMs; bool waterResponseFault; bool noFlowFault;
  uint32_t burstStartPulses; uint32_t sessionStartPulses;
  uint32_t lastDeliveredMl; uint32_t sessionDeliveredMl; StopReason lastStopReason;
};

PlantConfig plants[NUM_PLANTS] = {
  {"Aglaonema", 3300, 1150, 35, 60, 3000, 100, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Jade Plant", 3400, 1200, 15, 30, 2500, 85, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"ZZ Plant", 3400, 1250, 20, 35, 2500, 85, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Monstera", 2600, 1200, 35, 45, 3000, 100, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Bamboo", 3400, 1250, 10, 40, 3000, 100, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO}
};

static const uint8_t SENSOR_CALIBRATION_VERSION = 2;
PlantState states[NUM_PLANTS];
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
WiFiClientSecure secureClient;
Preferences prefs;
WebServer server(80);
volatile uint32_t flowPulseCount = 0;

bool systemReady = false, wifiConnected = false, emergencyStop = false;
bool tankEmpty = false, hardwareSafe = false, otaInProgress = false, timeSynced = false;
int activePlant = -1, displayPlant = 0;
unsigned long bootMs = 0, lastSensorMs = 0, lastLcdMs = 0, lastRotateMs = 0;
unsigned long lastStatusMs = 0, lastCommandMs = 0, lastConfigMs = 0;
unsigned long lastTelemetryHistoryMs = 0, lastWifiRetryMs = 0, otaStartedMs = 0;
String deviceIp = "offline", lastCommandId = "", lastConfigVersion = "";

void IRAM_ATTR onFlowPulse() { flowPulseCount++; }
String boolJson(bool v) { return v ? "true" : "false"; }
String modeText(PlantMode m) { if (m == MODE_MANUAL) return "MANUAL"; if (m == MODE_DISABLED) return "DISABLED"; return "AUTO"; }
PlantMode parseMode(String v) { v.toUpperCase(); if (v == "MANUAL") return MODE_MANUAL; if (v == "DISABLED") return MODE_DISABLED; return MODE_AUTO; }
String stopReasonText(StopReason r) {
  switch (r) {
    case STOP_TARGET_REACHED: return "Target reached"; case STOP_BURST_COMPLETE: return "Burst complete";
    case STOP_SESSION_LIMIT: return "Session limit"; case STOP_HOURLY_LIMIT: return "Hourly limit";
    case STOP_DAILY_LIMIT: return "Daily limit"; case STOP_TANK_EMPTY: return "Tank empty";
    case STOP_NO_FLOW: return "No flow"; case STOP_SENSOR_FAULT: return "Sensor fault";
    case STOP_EMERGENCY: return "Emergency stop"; case STOP_MANUAL_COMPLETE: return "Manual complete";
    case STOP_COMMAND_EXPIRED: return "Command expired"; default: return "None";
  }
}
String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on"; case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset"; case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog"; case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog"; case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout"; case ESP_RST_SDIO: return "SDIO"; default: return "Unknown";
  }
}

String firebaseUrl(const String &path) {
  String u = String(FIREBASE_BASE_URL) + String(DEVICE_ROOT) + path + ".json";
  if (strlen(FIREBASE_AUTH)) { u += "?auth="; u += FIREBASE_AUTH; }
  return u;
}

bool httpRequest(const String &method, const String &path, const String &json, String *response = nullptr) {
  if (!wifiConnected || otaInProgress) return false;
  secureClient.setInsecure();
  HTTPClient http;
  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  http.setTimeout(6000);
  if (method == "PUT") http.addHeader("Content-Type", "application/json");
  int code = -1;
  if (method == "GET") code = http.GET();
  else if (method == "PUT") code = http.PUT(json);
  else if (method == "POST") code = http.POST(json);
  if (response && code >= 200 && code < 300) *response = http.getString();
  http.end();
  return code >= 200 && code < 300;
}
bool httpGet(const String &path, String &body) { return httpRequest("GET", path, "", &body); }
bool httpPutJson(const String &path, const String &json) { return httpRequest("PUT", path, json); }
bool httpPostJson(const String &path, const String &json) { return httpRequest("POST", path, json); }

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
    maxProbability=max(maxProbability,(int)p.toInt()); 
    float m=mm.toFloat(); if(m>0)rainMm+=m; parsed=true;
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


String jsonStringValue(const String &json, const String &key, const String &fallback = "") {
  String token = "\"" + key + "\""; int p = json.indexOf(token); if (p < 0) return fallback;
  p = json.indexOf(':', p + token.length()); if (p < 0) return fallback; p++;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) p++;
  if (p >= (int)json.length()) return fallback;
  if (json[p] == '"') { int e = p + 1; while (e < (int)json.length()) { if (json[e] == '"' && json[e-1] != '\\') break; e++; } if (e >= (int)json.length()) return fallback; return json.substring(p + 1, e); }
  int e = p; while (e < (int)json.length() && json[e] != ',' && json[e] != '}' && json[e] != '\n') e++;
  String v = json.substring(p, e); v.trim(); return v;
}
long jsonLongValue(const String &json, const String &key, long fallback) { String v = jsonStringValue(json, key, ""); if (!v.length() || v == "null") return fallback; return v.toInt(); }
uint64_t jsonUint64Value(const String &json, const String &key, uint64_t fallback) { String v = jsonStringValue(json, key, ""); if (!v.length() || v == "null") return fallback; return strtoull(v.c_str(), nullptr, 10); }

void forceAllOutputsOff() {
  pinMode(PUMP_PIN, OUTPUT); digitalWrite(PUMP_PIN, RELAY_OFF);
  for (int i = 0; i < NUM_PLANTS; i++) { pinMode(VALVE_PINS[i], OUTPUT); digitalWrite(VALVE_PINS[i], RELAY_OFF); }
  activePlant = -1; hardwareSafe = true;
}
void closeAllValves() { for (int i = 0; i < NUM_PLANTS; i++) { digitalWrite(VALVE_PINS[i], RELAY_OFF); states[i].watering = false; } }
void emergencyHardwareStop(StopReason reason) {
  digitalWrite(PUMP_PIN, RELAY_OFF); delay(50); closeAllValves();
  if (activePlant >= 0) states[activePlant].lastStopReason = reason;
  activePlant = -1;
}
void updateTankState() {
  if (!TANK_SENSOR_ENABLED) { tankEmpty = false; return; }
  tankEmpty = digitalRead(TANK_FLOAT_PIN) == TANK_EMPTY_LEVEL;
  if (tankEmpty && activePlant >= 0) emergencyHardwareStop(STOP_TANK_EMPTY);
}

String keyFor(int i, const char *suffix) { return "p" + String(i) + suffix; }
void savePlantConfig(int i) {
  prefs.putString(keyFor(i,"name").c_str(), plants[i].name); prefs.putInt(keyFor(i,"air").c_str(), plants[i].airRaw);
  prefs.putInt(keyFor(i,"wet").c_str(), plants[i].wetRaw); prefs.putInt(keyFor(i,"low").c_str(), plants[i].targetLow);
  prefs.putInt(keyFor(i,"high").c_str(), plants[i].targetHigh); prefs.putULong(keyFor(i,"burst").c_str(), plants[i].burstMs);
  prefs.putUInt(keyFor(i,"bml").c_str(), plants[i].burstMl); prefs.putULong(keyFor(i,"soak").c_str(), plants[i].soakMs);
  prefs.putULong(keyFor(i,"minint").c_str(), plants[i].minIntervalMs); prefs.putUChar(keyFor(i,"mode").c_str(), (uint8_t)plants[i].mode);
}
void loadPlantConfig(int i) {
  plants[i].name = prefs.getString(keyFor(i,"name").c_str(), plants[i].name); plants[i].airRaw = prefs.getInt(keyFor(i,"air").c_str(), plants[i].airRaw);
  plants[i].wetRaw = prefs.getInt(keyFor(i,"wet").c_str(), plants[i].wetRaw); plants[i].targetLow = prefs.getInt(keyFor(i,"low").c_str(), plants[i].targetLow);
  plants[i].targetHigh = prefs.getInt(keyFor(i,"high").c_str(), plants[i].targetHigh); plants[i].burstMs = prefs.getULong(keyFor(i,"burst").c_str(), plants[i].burstMs);
  plants[i].burstMl = prefs.getUInt(keyFor(i,"bml").c_str(), plants[i].burstMl); plants[i].soakMs = prefs.getULong(keyFor(i,"soak").c_str(), plants[i].soakMs);
  plants[i].minIntervalMs = prefs.getULong(keyFor(i,"minint").c_str(), plants[i].minIntervalMs); plants[i].mode = (PlantMode)prefs.getUChar(keyFor(i,"mode").c_str(), (uint8_t)plants[i].mode);
  plants[i].targetLow = constrain(plants[i].targetLow, 0, 95); plants[i].targetHigh = constrain(plants[i].targetHigh, plants[i].targetLow + 1, 100);
  plants[i].burstMs = constrain(plants[i].burstMs, 500UL, plants[i].maxBurstMs);
  plants[i].burstMl = constrain(plants[i].burstMl, (uint32_t)10, (uint32_t)2000);
  plants[i].soakMs = constrain(plants[i].soakMs, 10000UL, 30UL * 60UL * 1000UL);
  plants[i].minIntervalMs = constrain(plants[i].minIntervalMs, 60000UL, 7UL * 24UL * 60UL * 60UL * 1000UL);
  if (plants[i].mode > MODE_DISABLED) plants[i].mode = MODE_AUTO;
}

void applySensorCalibrationVersion() {
  uint8_t savedVersion = prefs.getUChar("sensorCalVer", 0);
  if (savedVersion == SENSOR_CALIBRATION_VERSION) return;
  const int defaultAirRaw[NUM_PLANTS] = {3300, 3400, 3400, 2600, 3400};
  const int defaultWetRaw[NUM_PLANTS] = {1150, 1200, 1250, 1200, 1250};
  Serial.println("Applying updated plant sensor calibration...");
  for (int i = 0; i < NUM_PLANTS; i++) {
    plants[i].airRaw = defaultAirRaw[i]; plants[i].wetRaw = defaultWetRaw[i]; savePlantConfig(i);
    Serial.print(plants[i].name); Serial.print(" calibration: dry="); Serial.print(plants[i].airRaw); Serial.print(" wet="); Serial.println(plants[i].wetRaw);
  }
  prefs.putUChar("sensorCalVer", SENSOR_CALIBRATION_VERSION);
}

uint64_t epochMs() { time_t now = time(nullptr); if (now < 1700000000) return 0; return (uint64_t)now * 1000ULL; }
void loadRuntimeLimits(int i) {
  uint64_t now = epochMs(); uint64_t hourStart = prefs.getULong64(keyFor(i,"hrStart").c_str(), 0); uint64_t dayStart = prefs.getULong64(keyFor(i,"dayStart").c_str(), 0);
  states[i].hourlyPumpMs = prefs.getULong(keyFor(i,"hrMs").c_str(), 0); states[i].dailyPumpMs = prefs.getULong(keyFor(i,"dayMs").c_str(), 0);
  if (now && hourStart && now - hourStart >= 3600000ULL) { states[i].hourlyPumpMs = 0; prefs.putULong64(keyFor(i,"hrStart").c_str(), now); prefs.putULong(keyFor(i,"hrMs").c_str(), 0); }
  if (now && dayStart && now - dayStart >= 86400000ULL) { states[i].dailyPumpMs = 0; prefs.putULong64(keyFor(i,"dayStart").c_str(), now); prefs.putULong(keyFor(i,"dayMs").c_str(), 0); }
  states[i].hourWindowStartMs = millis(); states[i].dayWindowStartMs = millis();
}
void persistRuntimeLimits(int i) {
  uint64_t now = epochMs();
  if (now) { uint64_t hs = prefs.getULong64(keyFor(i,"hrStart").c_str(), 0); uint64_t ds = prefs.getULong64(keyFor(i,"dayStart").c_str(), 0); if (!hs) prefs.putULong64(keyFor(i,"hrStart").c_str(), now); if (!ds) prefs.putULong64(keyFor(i,"dayStart").c_str(), now); }
  prefs.putULong(keyFor(i,"hrMs").c_str(), states[i].hourlyPumpMs); prefs.putULong(keyFor(i,"dayMs").c_str(), states[i].dailyPumpMs);
  uint64_t last = epochMs(); if (last) prefs.putULong64(keyFor(i,"lastWater").c_str(), last);
}
void refreshTimeWindows(int i) {
  uint64_t now = epochMs();
  if (now) {
    uint64_t hs = prefs.getULong64(keyFor(i,"hrStart").c_str(), 0); uint64_t ds = prefs.getULong64(keyFor(i,"dayStart").c_str(), 0);
    if (!hs) { prefs.putULong64(keyFor(i,"hrStart").c_str(), now); hs = now; } if (!ds) { prefs.putULong64(keyFor(i,"dayStart").c_str(), now); ds = now; }
    if (now - hs >= 3600000ULL) { states[i].hourlyPumpMs = 0; prefs.putULong64(keyFor(i,"hrStart").c_str(), now); prefs.putULong(keyFor(i,"hrMs").c_str(), 0); }
    if (now - ds >= 86400000ULL) { states[i].dailyPumpMs = 0; prefs.putULong64(keyFor(i,"dayStart").c_str(), now); prefs.putULong(keyFor(i,"dayMs").c_str(), 0); }
  } else {
    if (millis() - states[i].hourWindowStartMs >= 3600000UL) { states[i].hourWindowStartMs = millis(); states[i].hourlyPumpMs = 0; }
    if (millis() - states[i].dayWindowStartMs >= 86400000UL) { states[i].dayWindowStartMs = millis(); states[i].dailyPumpMs = 0; }
  }
}

int readAverageRaw(int pin) { long total = 0; for (int s=0;s<SENSOR_SAMPLES;s++){ total += analogRead(pin); delay(2); } return total/SENSOR_SAMPLES; }
int rawToPercent(int i, int raw) { if (plants[i].airRaw <= plants[i].wetRaw + 10) return 0; int c=constrain(raw,plants[i].wetRaw,plants[i].airRaw); return map(c,plants[i].airRaw,plants[i].wetRaw,0,100); }
void updatePlantSensor(int i) {
  PlantState &s=states[i]; int raw=readAverageRaw(SENSOR_PINS[i]); bool bounds=raw<=SENSOR_MIN_VALID||raw>=SENSOR_MAX_VALID;
  if (s.previousRaw<0 || abs(raw-s.previousRaw)>=SENSOR_CHANGE_DELTA) s.lastSensorChangeMs=millis();
  bool stale=s.lastSensorChangeMs>0 && millis()-s.lastSensorChangeMs>SENSOR_STALE_MS;
  s.sensorFault=bounds||stale; s.raw=raw; s.moisture=rawToPercent(i,raw); s.previousRaw=raw;
  if (!s.sensorFault) { if(s.validBootScans<REQUIRED_VALID_BOOT_SCANS)s.validBootScans++; } else s.validBootScans=0;
}
bool allPlantsBootValidated(){for(int i=0;i<NUM_PLANTS;i++)if(states[i].validBootScans<REQUIRED_VALID_BOOT_SCANS)return false;return true;}

uint32_t pulsesToMl(uint32_t p){if(!FLOW_SENSOR_ENABLED||FLOW_PULSES_PER_LITER<=0)return 0;return(uint32_t)(((float)p/FLOW_PULSES_PER_LITER)*1000.0f);}
uint32_t currentBurstMl(int i){return FLOW_SENSOR_ENABLED?pulsesToMl(flowPulseCount-states[i].burstStartPulses):0;}
uint32_t currentSessionMl(int i){return FLOW_SENSOR_ENABLED?pulsesToMl(flowPulseCount-states[i].sessionStartPulses):0;}
bool bootAllowsWatering(){return systemReady&&hardwareSafe&&!otaInProgress&&millis()-bootMs>=BOOT_WATERING_GRACE_MS&&allPlantsBootValidated();}
bool canStartPlant(int i,bool manual){
  PlantState&s=states[i];PlantConfig&c=plants[i];refreshTimeWindows(i);
  if(!bootAllowsWatering()||emergencyStop||tankEmpty||s.sensorFault||s.noFlowFault||s.waterResponseFault)return false;if(!manual&&!weatherAllowsAutomaticWatering())return false;
  if(c.mode==MODE_DISABLED||(!manual&&c.mode!=MODE_AUTO))return false;
  if(!manual&&s.lastWateredMs>0&&millis()-s.lastWateredMs<c.minIntervalMs)return false;
  if(s.hourlyPumpMs>=c.maxHourlyMs||s.dailyPumpMs>=c.maxDailyMs)return false; return true;
}
void openValveThenPump(int i){for(int p=0;p<NUM_PLANTS;p++)if(p!=i)digitalWrite(VALVE_PINS[p],RELAY_OFF);digitalWrite(VALVE_PINS[i],RELAY_ON);delay(VALVE_PREOPEN_MS);digitalWrite(PUMP_PIN,RELAY_ON);}
void publishWateringHistory(int i,StopReason reason,unsigned long duration,uint32_t ml){
  String j="{";j+="\"plantIndex\":"+String(i)+",";j+="\"plantName\":\""+plants[i].name+"\",";uint64_t now=epochMs();j+="\"eventEpochMs\":"+String((unsigned long long)now)+",";j+="\"eventUptimeMs\":"+String(millis())+",";j+="\"durationMs\":"+String(duration)+",";j+="\"deliveredMl\":"+String(ml)+",";j+="\"moistureStart\":"+String(states[i].moistureAtSessionStart)+",";j+="\"moistureEnd\":"+String(states[i].moisture)+",";j+="\"reason\":\""+stopReasonText(reason)+"\",";j+="\"manual\":"+boolJson(states[i].manualRequest)+"}";httpPostJson("/history/watering",j);
}
void finishPlantSession(int i,StopReason reason){
  PlantState&s=states[i];uint32_t ml=FLOW_SENSOR_ENABLED?currentSessionMl(i):0;s.lastDeliveredMl=ml;s.sessionDeliveredMl=ml;s.lastWateredMs=millis();s.lastStopReason=reason;s.responseDeadlineMs=0;publishWateringHistory(i,reason,s.sessionPumpMs,ml);persistRuntimeLimits(i);s.manualRequest=false;s.manualRequestedMl=0;s.manualRequestedMs=0;s.sessionPumpMs=0;s.sessionStartMs=0;s.soaking=false;activePlant=-1;
}
void stopBurst(int i,StopReason reason,bool finishSession){
  PlantState&s=states[i];if(!s.watering){if(finishSession)finishPlantSession(i,reason);return;}unsigned long d=millis()-s.burstStartMs;s.sessionPumpMs+=d;s.hourlyPumpMs+=d;s.dailyPumpMs+=d;digitalWrite(PUMP_PIN,RELAY_OFF);delay(VALVE_POSTPUMP_MS);digitalWrite(VALVE_PINS[i],RELAY_OFF);s.watering=false;s.lastStopReason=reason;s.moistureAfterLastBurst=s.moisture;if(finishSession)finishPlantSession(i,reason);else{s.soaking=true;s.soakUntilMs=millis()+plants[i].soakMs;}
}
void startPlantSession(int i,bool manual){PlantState&s=states[i];if(!canStartPlant(i,manual))return;activePlant=i;s.sessionStartMs=millis();s.sessionPumpMs=0;s.moistureAtSessionStart=s.moisture;s.sessionStartPulses=flowPulseCount;s.soaking=false;s.responseDeadlineMs=manual?0:millis()+WATER_RESPONSE_TIMEOUT_MS;s.lastStopReason=STOP_NONE;}
void startBurst(int i){
  PlantState&s=states[i];PlantConfig&c=plants[i];if(activePlant!=i||s.watering)return;if(emergencyStop||tankEmpty||s.sensorFault||otaInProgress)return;refreshTimeWindows(i);
  if(s.sessionPumpMs>=c.maxSessionMs){finishPlantSession(i,STOP_SESSION_LIMIT);return;}if(s.hourlyPumpMs>=c.maxHourlyMs){finishPlantSession(i,STOP_HOURLY_LIMIT);return;}if(s.dailyPumpMs>=c.maxDailyMs){finishPlantSession(i,STOP_DAILY_LIMIT);return;}
  s.burstStartMs=millis();s.burstStartPulses=flowPulseCount;s.watering=true;openValveThenPump(i);
}
bool manualTargetReached(int i){PlantState&s=states[i];if(!s.manualRequest)return false;if(FLOW_SENSOR_ENABLED&&s.manualRequestedMl>0)return currentSessionMl(i)>=s.manualRequestedMl;if(s.manualRequestedMs>0)return s.sessionPumpMs+(s.watering?millis()-s.burstStartMs:0)>=s.manualRequestedMs;return false;}
bool normalBurstReached(int i){PlantState&s=states[i];PlantConfig&c=plants[i];if(FLOW_SENSOR_ENABLED&&c.burstMl>0)return currentBurstMl(i)>=c.burstMl;return millis()-s.burstStartMs>=min(c.burstMs,c.maxBurstMs);}
void evaluateActivePlant(){
  if(activePlant<0)return;int i=activePlant;PlantState&s=states[i];PlantConfig&c=plants[i];updateTankState();
  if(emergencyStop){stopBurst(i,STOP_EMERGENCY,true);return;}if(tankEmpty){stopBurst(i,STOP_TANK_EMPTY,true);return;}if(s.sensorFault){stopBurst(i,STOP_SENSOR_FAULT,true);return;}
  if(s.watering){
    unsigned long run=millis()-s.burstStartMs;if(FLOW_SENSOR_ENABLED&&run>=NO_FLOW_TIMEOUT_MS&&flowPulseCount==s.burstStartPulses){s.noFlowFault=true;stopBurst(i,STOP_NO_FLOW,true);return;}
    if(s.sessionPumpMs+run>=c.maxSessionMs){stopBurst(i,STOP_SESSION_LIMIT,true);return;}if(s.hourlyPumpMs+run>=c.maxHourlyMs){stopBurst(i,STOP_HOURLY_LIMIT,true);return;}if(s.dailyPumpMs+run>=c.maxDailyMs){stopBurst(i,STOP_DAILY_LIMIT,true);return;}
    if(manualTargetReached(i)){stopBurst(i,STOP_MANUAL_COMPLETE,true);return;}if(normalBurstReached(i)){stopBurst(i,STOP_BURST_COMPLETE,false);return;}return;
  }
  if(s.soaking){if(millis()<s.soakUntilMs)return;s.soaking=false;if(s.manualRequest&&manualTargetReached(i)){finishPlantSession(i,STOP_MANUAL_COMPLETE);return;}if(!s.manualRequest&&s.moisture>=c.targetHigh){finishPlantSession(i,STOP_TARGET_REACHED);return;}if(!s.manualRequest&&s.responseDeadlineMs>0&&millis()>=s.responseDeadlineMs&&s.moisture-s.moistureAtSessionStart<MIN_MOISTURE_GAIN_PERCENT){s.waterResponseFault=true;finishPlantSession(i,STOP_NO_FLOW);return;}if(s.manualRequest&&s.manualRequestedMs>0&&s.sessionPumpMs>=s.manualRequestedMs){finishPlantSession(i,STOP_MANUAL_COMPLETE);return;}startBurst(i);
  }
}
int nextAutomaticPlant(){static int last=-1;for(int step=1;step<=NUM_PLANTS;step++){int i=(last+step)%NUM_PLANTS;if(plants[i].mode!=MODE_AUTO)continue;if(states[i].moisture>=plants[i].targetLow)continue;if(!canStartPlant(i,false))continue;last=i;return i;}return-1;}
void wateringController(){updateTankState();if(otaInProgress){if(activePlant>=0)emergencyHardwareStop(STOP_EMERGENCY);return;}if(emergencyStop){if(activePlant>=0)emergencyHardwareStop(STOP_EMERGENCY);return;}if(activePlant>=0){evaluateActivePlant();return;}for(int i=0;i<NUM_PLANTS;i++)if(states[i].manualRequest&&canStartPlant(i,true)){startPlantSession(i,true);startBurst(i);return;}int n=nextAutomaticPlant();if(n>=0){startPlantSession(n,false);startBurst(n);}}

bool commandTimestampValid(const String &json){uint64_t issued=jsonUint64Value(json,"issuedAtEpochMs",0);if(!issued)return false;uint64_t now=epochMs();if(!now)return false;if(issued>now+REMOTE_COMMAND_FUTURE_SKEW_MS)return false;return now-issued<=REMOTE_COMMAND_MAX_AGE_MS;}
void applyCalibrationCommand(int i,const String&kind){if(i<0||i>=NUM_PLANTS||states[i].sensorFault)return;if(kind=="dry")plants[i].airRaw=states[i].raw;if(kind=="wet")plants[i].wetRaw=states[i].raw;if(plants[i].airRaw>plants[i].wetRaw+100)savePlantConfig(i);}
void applyRemotePlantConfig(int i,const String&json){if(i<0||i>=NUM_PLANTS||json=="null"||json.length()<2)return;String name=jsonStringValue(json,"name",plants[i].name);long low=jsonLongValue(json,"targetLow",plants[i].targetLow),high=jsonLongValue(json,"targetHigh",plants[i].targetHigh);long burst=jsonLongValue(json,"burstMs",plants[i].burstMs),bml=jsonLongValue(json,"burstMl",plants[i].burstMl),soak=jsonLongValue(json,"soakSec",plants[i].soakMs/1000UL),interval=jsonLongValue(json,"minIntervalMin",plants[i].minIntervalMs/60000UL);String mode=jsonStringValue(json,"mode",modeText(plants[i].mode));if(name.length()&&name.length()<=24)plants[i].name=name;plants[i].targetLow=constrain(low,0,95);plants[i].targetHigh=constrain(high,plants[i].targetLow+1,100);plants[i].burstMs=constrain((unsigned long)max(500L,burst),500UL,plants[i].maxBurstMs);plants[i].burstMl=constrain((uint32_t)max(10L,bml),(uint32_t)10,(uint32_t)2000);plants[i].soakMs=constrain((unsigned long)max(10L,soak)*1000UL,10000UL,30UL*60UL*1000UL);plants[i].minIntervalMs=constrain((unsigned long)max(1L,interval)*60000UL,60000UL,7UL*24UL*60UL*60UL*1000UL);plants[i].mode=parseMode(mode);savePlantConfig(i);}
void pollRemoteConfig(){String v;if(!httpGet("/config/version",v))return;v.replace("\"","");v.trim();if(!v.length()||v=="null"||v==lastConfigVersion)return;for(int i=0;i<NUM_PLANTS;i++){String b;if(httpGet("/config/plants/"+String(i),b))applyRemotePlantConfig(i,b);}lastConfigVersion=v;prefs.putString("cfgVersion",v);}
void acknowledgeCommand(const String&id,const String&result){String j="{\"id\":\""+id+"\",\"result\":\""+result+"\",\"handledAtUptimeMs\":"+String(millis())+"}";httpPutJson("/commandAck",j);}
void handleRemoteCommand(const String&json){if(json=="null"||json.length()<2)return;String id=jsonStringValue(json,"id","");String action=jsonStringValue(json,"action","");if(!id.length()||!action.length()||id==lastCommandId)return;int plant=jsonLongValue(json,"plantIndex",-1);long sec=jsonLongValue(json,"durationSec",0),ml=jsonLongValue(json,"amountMl",0);action.toLowerCase();String result="ignored";if(action!="emergency_stop"&&!commandTimestampValid(json)){lastCommandId=id;prefs.putString("lastCmd",lastCommandId);acknowledgeCommand(id,"expired_or_clock_unavailable");return;}if(action=="emergency_stop"){emergencyStop=true;prefs.putBool("eStop",true);for(int i=0;i<NUM_PLANTS;i++){states[i].manualRequest=false;states[i].manualRequestedMl=0;states[i].manualRequestedMs=0;states[i].soaking=false;}emergencyHardwareStop(STOP_EMERGENCY);forceAllOutputsOff();result="stopped";}else if(action=="resume"){emergencyStop=false;prefs.putBool("eStop",false);forceAllOutputsOff();result="resumed";}else if(action=="water_now"&&plant>=0&&plant<NUM_PLANTS){if(!emergencyStop&&!tankEmpty&&plants[plant].mode!=MODE_DISABLED){states[plant].manualRequest=true;states[plant].manualRequestedMl=FLOW_SENSOR_ENABLED?(uint32_t)constrain((long)ml,0L,2000L):0;states[plant].manualRequestedMs=FLOW_SENSOR_ENABLED?0:(unsigned long)constrain(sec,1L,(long)(plants[plant].maxSessionMs/1000UL))*1000UL;if(FLOW_SENSOR_ENABLED&&states[plant].manualRequestedMl==0)states[plant].manualRequestedMl=plants[plant].burstMl;if(!FLOW_SENSOR_ENABLED&&states[plant].manualRequestedMs==0)states[plant].manualRequestedMs=5000;result="queued";}else result="blocked_by_safety";}else if(action=="set_mode"&&plant>=0&&plant<NUM_PLANTS){plants[plant].mode=parseMode(jsonStringValue(json,"mode","AUTO"));savePlantConfig(plant);if(plants[plant].mode==MODE_DISABLED&&activePlant==plant)stopBurst(plant,STOP_MANUAL_COMPLETE,true);result="mode_saved";}else if(action=="calibrate_dry"&&plant>=0&&plant<NUM_PLANTS){applyCalibrationCommand(plant,"dry");result="dry_saved";}else if(action=="calibrate_wet"&&plant>=0&&plant<NUM_PLANTS){applyCalibrationCommand(plant,"wet");result="wet_saved";}else if(action=="clear_fault"&&plant>=0&&plant<NUM_PLANTS){states[plant].noFlowFault=false;states[plant].waterResponseFault=false;result="fault_cleared";}lastCommandId=id;prefs.putString("lastCmd",lastCommandId);acknowledgeCommand(id,result);}
void pollRemoteCommand(){String b;if(httpGet("/command",b))handleRemoteCommand(b);}

String buildPlantJson(int i){PlantState&s=states[i];PlantConfig&c=plants[i];String j="{";j+="\"name\":\""+c.name+"\",\"moisture\":"+String(s.moisture)+",\"raw\":"+String(s.raw)+",";j+="\"targetLow\":"+String(c.targetLow)+",\"targetHigh\":"+String(c.targetHigh)+",\"targetRange\":\""+String(c.targetLow)+"-"+String(c.targetHigh)+"%\",";j+="\"mode\":\""+modeText(c.mode)+"\",\"watering\":"+boolJson(s.watering)+",\"soaking\":"+boolJson(s.soaking)+",\"manualQueued\":"+boolJson(s.manualRequest)+",";j+="\"sensorFault\":"+boolJson(s.sensorFault)+",\"noFlowFault\":"+boolJson(s.noFlowFault)+",\"waterResponseFault\":"+boolJson(s.waterResponseFault)+",\"fault\":"+boolJson(s.sensorFault||s.noFlowFault||s.waterResponseFault)+",";j+="\"lastStopReason\":\""+stopReasonText(s.lastStopReason)+"\",\"lastWateredUptimeMs\":"+String(s.lastWateredMs)+",\"lastDeliveredMl\":"+String(s.lastDeliveredMl)+",\"hourlyPumpMs\":"+String(s.hourlyPumpMs)+",\"dailyPumpMs\":"+String(s.dailyPumpMs)+",\"burstMs\":"+String(c.burstMs)+",\"burstMl\":"+String(c.burstMl)+",\"soakSec\":"+String(c.soakMs/1000UL)+",\"minIntervalMin\":"+String(c.minIntervalMs/60000UL)+"}";return j;}
String buildStatusJson(){String j="{";j+="\"firmware\":\""+String(FIRMWARE_VERSION)+"\",\"ota\":true,\"otaPort\":80,\"otaUser\":\""+String(OTA_USER)+"\",\"totalPlants\":"+String(NUM_PLANTS)+",\"systemReady\":"+boolJson(systemReady)+",\"wateringAllowed\":"+boolJson(bootAllowsWatering()&&!emergencyStop&&!tankEmpty)+",\"emergencyStop\":"+boolJson(emergencyStop)+",\"tankSensorEnabled\":"+boolJson(TANK_SENSOR_ENABLED)+",\"tankEmpty\":"+boolJson(tankEmpty)+",\"flowSensorEnabled\":"+boolJson(FLOW_SENSOR_ENABLED)+",\"flowPulses\":"+String(flowPulseCount)+",\"activePlantIndex\":"+String(activePlant)+",\"ip\":\""+deviceIp+"\",\"wifiRssi\":"+String(wifiConnected?WiFi.RSSI():0)+",\"uptimeMin\":"+String(millis()/60000UL)+",\"updatedAtMs\":"+String(millis())+",\"resetReason\":\""+resetReasonText()+"\",\"timeSynced\":"+boolJson(timeSynced)+",\"plants\":[";for(int i=0;i<NUM_PLANTS;i++){if(i)j+=",";j+=buildPlantJson(i);}j+="]}";return j;}
void publishStatus(){httpPutJson("/status",buildStatusJson());}
void publishTelemetryHistory(){String j="{\"uptimeMs\":"+String(millis())+",\"epochMs\":"+String((unsigned long long)epochMs())+",\"tankEmpty\":"+boolJson(tankEmpty)+",\"emergencyStop\":"+boolJson(emergencyStop)+",\"plants\":[";for(int i=0;i<NUM_PLANTS;i++){if(i)j+=",";j+="{\"name\":\""+plants[i].name+"\",\"moisture\":"+String(states[i].moisture)+",\"raw\":"+String(states[i].raw)+"}";}j+="]}";httpPostJson("/history/telemetry",j);}

// ========================= LOCAL OTA =========================
// The upload callback only receives/writes multipart chunks. The POST handler
// sends the final HTTP response after the upload callback has completed.
bool otaUploadSuccess = false;
bool otaUploadFailed = false;
size_t otaUploadBytes = 0;
size_t otaUploadTotal = 0;

void otaSafeStart(){
  otaInProgress=true;systemReady=false;emergencyHardwareStop(STOP_EMERGENCY);
  digitalWrite(PUMP_PIN,RELAY_OFF);closeAllValves();otaStartedMs=millis();
}

void handleUpdatePage(){
  if(!server.authenticate(OTA_USER,OTA_PASSWORD)){server.requestAuthentication();return;}
  String h="<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Plant V2 OTA</title><style>body{font-family:Arial,sans-serif;max-width:600px;margin:40px auto;padding:0 16px}"
           "progress{width:100%;height:28px}button{padding:10px 18px;margin-top:12px}#status{margin-top:14px;font-weight:bold}.ok{color:green}.err{color:#b00020}</style></head><body>"
           "<h2>Plant Watering V2 OTA</h2><p>Current firmware: "+String(FIRMWARE_VERSION)+"</p>"
           "<form id='otaForm' method='POST' action='/update' enctype='multipart/form-data'>"
           "<input id='firmware' type='file' name='firmware' accept='.bin' required><br>"
           "<button id='uploadBtn' type='submit'>Upload firmware</button></form>"
           "<progress id='progress' value='0' max='100' style='display:none'></progress>"
           "<div id='status'>All pump/valves are forced OFF during update.</div>"
           "<script>const f=document.getElementById('otaForm'),i=document.getElementById('firmware'),p=document.getElementById('progress'),s=document.getElementById('status'),b=document.getElementById('uploadBtn');"
           "f.addEventListener('submit',function(e){e.preventDefault();if(!i.files.length)return;"
           "b.disabled=true;p.style.display='block';p.value=0;s.className='';s.textContent='Uploading: 0%';"
           "const x=new XMLHttpRequest();x.open('POST','/update',true);"
           "x.upload.onprogress=function(e){if(e.lengthComputable){const n=Math.round(e.loaded*100/e.total);p.value=n;s.textContent='Uploading: '+n+'%';}};"
           "x.onload=function(){if(x.status>=200&&x.status<300){p.value=100;s.className='ok';s.innerHTML=x.responseText;}else{s.className='err';s.textContent='Update failed: HTTP '+x.status+' '+x.responseText;b.disabled=false;}};"
           "x.onerror=function(){s.className='err';s.textContent='Upload failed: connection lost before the ESP32 confirmed the update.';b.disabled=false;};"
           "x.ontimeout=function(){s.className='err';s.textContent='Upload timed out. Check the ESP32 Serial Monitor before retrying.';b.disabled=false;};"
           "x.timeout=600000;x.send(new FormData(f));});</script></body></html>";
  server.send(200,"text/html",h);
}

void handleUpdateUpload(){
  if(!server.authenticate(OTA_USER,OTA_PASSWORD))return;
  HTTPUpload&u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    otaUploadSuccess=false;otaUploadFailed=false;otaUploadBytes=0;otaUploadTotal=0;
    otaSafeStart();otaUploadTotal=u.totalSize;
    Serial.print("Web OTA upload started: ");Serial.println(u.filename);
    if(!Update.begin(UPDATE_SIZE_UNKNOWN)){Update.printError(Serial);otaUploadFailed=true;}
  }else if(u.status==UPLOAD_FILE_WRITE){
    otaUploadBytes+=u.currentSize;
    if(!otaUploadFailed&&!Update.hasError()){
      size_t written=Update.write(u.buf,u.currentSize);
      if(written!=u.currentSize){Update.printError(Serial);otaUploadFailed=true;}
    }
    if(otaUploadTotal>0)Serial.printf("Web OTA: %u%%\n",(unsigned)((otaUploadBytes*100UL)/otaUploadTotal));
    else Serial.printf("Web OTA: %u bytes\n",(unsigned)otaUploadBytes);
  }else if(u.status==UPLOAD_FILE_END){
    if(!otaUploadFailed&&Update.end(true)){
      otaUploadSuccess=true;
      Serial.printf("Web OTA upload complete: %u bytes\n",(unsigned)otaUploadBytes);
    }else{
      Update.printError(Serial);otaUploadFailed=true;otaUploadSuccess=false;
      otaInProgress=false;systemReady=true;bootMs=millis();forceAllOutputsOff();
      Serial.println("Web OTA update failed; running firmware was not replaced.");
    }
  }
}

void handleUpdateComplete(){
  if(!server.authenticate(OTA_USER,OTA_PASSWORD)){server.requestAuthentication();return;}
  if(otaUploadSuccess){
    server.send(200,"text/html","<!doctype html><html><body><h2>OTA update successful</h2><p>The firmware was written successfully.</p><p>The ESP32 will reboot now.</p></body></html>");
    delay(1000);ESP.restart();return;
  }
  if(otaUploadFailed){
    server.send(500,"text/html","<!doctype html><html><body><h2>OTA update failed</h2><p>The firmware update was not completed. The existing firmware remains installed.</p><p>Check the ESP32 Serial Monitor for the Update error and retry with a valid .bin file.</p></body></html>");
    return;
  }
  server.send(400,"text/plain","No firmware upload was received.");
}

void setupLocalOta(){
  ArduinoOTA.setHostname("plant-watering-v2");ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([](){otaSafeStart();Serial.println("OTA start: outputs OFF");});
  ArduinoOTA.onEnd([](){Serial.println("OTA complete; rebooting");});
  ArduinoOTA.onProgress([](unsigned int p,unsigned int t){Serial.printf("OTA %u%%\r",(p*100)/t);});
  ArduinoOTA.onError([](ota_error_t e){Serial.printf("OTA error %u; outputs remain OFF\n",e);otaInProgress=false;systemReady=true;forceAllOutputsOff();});ArduinoOTA.begin();
  server.on("/update",HTTP_GET,handleUpdatePage);
  server.on("/update",HTTP_POST,handleUpdateComplete,handleUpdateUpload);
  server.on("/",HTTP_GET,[](){if(!server.authenticate(OTA_USER,OTA_PASSWORD)){server.requestAuthentication();return;}server.send(200,"text/plain","Plant Watering V2 local OTA. Open /update to upload a .bin firmware.");});
  server.begin();
}

void lcdLine(uint8_t row,String t){while(t.length()<LCD_COLS)t+=" ";if(t.length()>LCD_COLS)t=t.substring(0,LCD_COLS);lcd.setCursor(0,row);lcd.print(t);}
void refreshLcd(){if(millis()-lastLcdMs<LCD_UPDATE_MS)return;lastLcdMs=millis();if(!systemReady){lcdLine(0,"Plant Care V2");lcdLine(1,"Starting safely");return;}if(emergencyStop){lcdLine(0,"EMERGENCY STOP");lcdLine(1,"Pump disabled");return;}if(tankEmpty){lcdLine(0,"TANK EMPTY");lcdLine(1,"Pump disabled");return;}if(!bootAllowsWatering()){lcdLine(0,"Safety startup");lcdLine(1,"Checking sensors");return;}if(millis()-lastRotateMs>=LCD_ROTATE_MS){lastRotateMs=millis();displayPlant=(displayPlant+1)%NUM_PLANTS;}int i=activePlant>=0?activePlant:displayPlant;lcdLine(0,plants[i].name+":"+String(states[i].moisture)+"%");String l2=states[i].watering?"Watering "+modeText(plants[i].mode):states[i].soaking?"Soaking...":states[i].sensorFault?"Sensor fault":modeText(plants[i].mode)+" / "+String(plants[i].targetLow)+"-"+String(plants[i].targetHigh);lcdLine(1,l2);}
void beginWifi(){WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);lastWifiRetryMs=millis();}
void maintainWifi(){
  bool c=WiFi.status()==WL_CONNECTED;
  if(c){
    if(!wifiConnected){
      wifiConnected=true; deviceIp=WiFi.localIP().toString();
      prefs.putBool("wifiWasConnected",true);
      Serial.print("WiFi connected: "); Serial.println(deviceIp);
      configTime(0,0,"pool.ntp.org","time.nist.gov");
      recordConnectivityEvent("WIFI_CONNECTED",deviceIp);
      flushConnectivityHistory();
    }
    return;
  }
  if(wifiConnected){
    saveConnectivityEventLocal("WIFI_DISCONNECTED","WiFi connection lost");
    wifiConnected=false; deviceIp="offline";
  }else deviceIp="offline";
  if(millis()-lastWifiRetryMs>=WIFI_RETRY_MS){
    lastWifiRetryMs=millis(); WiFi.disconnect(); beginWifi();
  }
}
void updateClock(){if(timeSynced)return;time_t now=time(nullptr);if(now>=1700000000){timeSynced=true;Serial.println("NTP time synchronized");for(int i=0;i<NUM_PLANTS;i++)loadRuntimeLimits(i);}}

void setup(){
  forceAllOutputsOff();Serial.begin(115200);delay(100);Serial.println();Serial.println("========================================");Serial.println(FIRMWARE_VERSION);Serial.print("Reset reason: ");Serial.println(resetReasonText());Serial.println("Pump and valves forced OFF.");Serial.println("========================================");
  delay(POWER_STABILIZE_MS);bootMs=millis();prefs.begin("plantV2",false);emergencyStop=prefs.getBool("eStop",false);lastCommandId=prefs.getString("lastCmd","");lastConfigVersion=prefs.getString("cfgVersion","");
  for(int i=0;i<NUM_PLANTS;i++)loadPlantConfig(i);
  applySensorCalibrationVersion();analogReadResolution(12);
  for(int i=0;i<NUM_PLANTS;i++){states[i]={};states[i].previousRaw=-1;states[i].lastSensorChangeMs=millis();states[i].hourWindowStartMs=millis();states[i].dayWindowStartMs=millis();loadRuntimeLimits(i);}
  if(TANK_SENSOR_ENABLED)pinMode(TANK_FLOAT_PIN,INPUT_PULLUP);if(FLOW_SENSOR_ENABLED){pinMode(FLOW_SENSOR_PIN,INPUT_PULLUP);attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN),onFlowPulse,FALLING);}
  Wire.begin(SDA_PIN,SCL_PIN);Wire.setClock(50000);lcd.init();lcd.backlight();lcd.clear();lcdLine(0,"Plant Care V2");lcdLine(1,"Safe boot...");
  for(int scan=0;scan<REQUIRED_VALID_BOOT_SCANS;scan++){for(int i=0;i<NUM_PLANTS;i++)updatePlantSensor(i);delay(250);}updateTankState();beginWifi();systemReady=true;setupLocalOta();
  Serial.println("System initialized. Automatic watering blocked during boot grace period.");
}
void loop(){
  refreshWeather();
  flushConnectivityHistory();
  if(otaInProgress){server.handleClient();ArduinoOTA.handle();digitalWrite(PUMP_PIN,RELAY_OFF);closeAllValves();delay(10);return;}
  maintainWifi();updateClock();server.handleClient();ArduinoOTA.handle();updateTankState();
  if(millis()-lastSensorMs>=SENSOR_SAMPLE_MS){lastSensorMs=millis();for(int i=0;i<NUM_PLANTS;i++)updatePlantSensor(i);}
  wateringController();
  if(wifiConnected&&millis()-lastCommandMs>=REMOTE_COMMAND_MS){lastCommandMs=millis();pollRemoteCommand();}
  if(wifiConnected&&millis()-lastConfigMs>=REMOTE_CONFIG_MS){lastConfigMs=millis();pollRemoteConfig();}
  if(wifiConnected&&millis()-lastStatusMs>=REMOTE_STATUS_MS){lastStatusMs=millis();publishStatus();}
  if(wifiConnected&&millis()-lastTelemetryHistoryMs>=TELEMETRY_HISTORY_MS){lastTelemetryHistoryMs=millis();publishTelemetryHistory();}
  refreshLcd();delay(20);
}
