#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>

/*
  Plant Watering Smart V2
  -----------------------
  Safe replacement candidate for the currently deployed multi sensor code.ino.

  IMPORTANT:
  - The old firmware is intentionally left untouched.
  - V2 keeps the same 5 sensor / 5 valve / 1 shared pump pin mapping.
  - Tank and flow sensors are OPTIONAL and disabled by default so V2 can be tested
    with the current hardware first.
  - Change WIFI_SSID / WIFI_PASSWORD before flashing. Do not commit real secrets.

  Features implemented in this firmware:
  1. Remote manual watering command
  2. AUTO / MANUAL / DISABLED mode per plant
  3. Emergency stop / resume
  4. Optional tank-low float switch protection
  5. Optional flow sensor + no-flow protection
  6. Volume-based watering when flow sensor is enabled; time fallback otherwise
  7. Soil soak delay between bursts
  8. Hard burst/session/hour/day watering limits
  9. Minimum interval between watering sessions
  10. Remote per-plant settings via Firebase
  11. Persistent settings using ESP32 Preferences/NVS
  12. Remote dry/wet calibration commands
  13. Watering event history written to Firebase
  14. Periodic telemetry history for dashboard graphs

  Power-return hardening:
  - Pump and all valves are forced OFF before LCD/WiFi/sensor startup.
  - Boot stabilization delay after power return.
  - Multiple valid sensor scans are required before watering is enabled.
  - Automatic watering is inhibited during a boot grace period.
  - Last remote command IDs are remembered to prevent replay after reboot.
*/

// ========================= NETWORK =========================
static const char *WIFI_SSID = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static const char *FIREBASE_BASE_URL = "https://vernal-catfish-196407.firebaseio.com";
static const char *DEVICE_ROOT = "/plantMonitor/v2";
static const char *FIREBASE_AUTH = "";

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

// Optional hardware. Keep disabled until physically connected and tested.
static const bool TANK_SENSOR_ENABLED = false;
static const int TANK_FLOAT_PIN = 18;
// Change this if your float switch logic is opposite.
static const int TANK_EMPTY_LEVEL = LOW;

static const bool FLOW_SENSOR_ENABLED = false;
static const int FLOW_SENSOR_PIN = 19;
// Calibrate for your actual sensor. Many hall flow sensors differ significantly.
static const float FLOW_PULSES_PER_LITER = 450.0f;

// Relay board is active LOW in the currently deployed design.
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
static const int MIN_MOISTURE_GAIN_PERCENT = 2;

static const int SENSOR_MIN_VALID = 80;
static const int SENSOR_MAX_VALID = 4095;
static const int SENSOR_CHANGE_DELTA = 4;
static const unsigned long SENSOR_STALE_MS = 6UL * 60UL * 60UL * 1000UL;

// ========================= TYPES =========================
enum PlantMode : uint8_t {
  MODE_AUTO = 0,
  MODE_MANUAL = 1,
  MODE_DISABLED = 2
};

enum StopReason : uint8_t {
  STOP_NONE = 0,
  STOP_TARGET_REACHED,
  STOP_BURST_COMPLETE,
  STOP_SESSION_LIMIT,
  STOP_HOURLY_LIMIT,
  STOP_DAILY_LIMIT,
  STOP_TANK_EMPTY,
  STOP_NO_FLOW,
  STOP_SENSOR_FAULT,
  STOP_EMERGENCY,
  STOP_MANUAL_COMPLETE
};

struct PlantConfig {
  String name;
  int airRaw;
  int wetRaw;
  int targetLow;
  int targetHigh;
  unsigned long burstMs;
  uint32_t burstMl;
  unsigned long soakMs;
  unsigned long minIntervalMs;
  unsigned long maxBurstMs;
  unsigned long maxSessionMs;
  unsigned long maxHourlyMs;
  unsigned long maxDailyMs;
  PlantMode mode;
};

struct PlantState {
  int raw;
  int moisture;
  int previousRaw;
  bool sensorFault;
  int validBootScans;
  unsigned long lastSensorChangeMs;

  bool watering;
  bool soaking;
  bool manualRequest;
  uint32_t manualRequestedMl;
  unsigned long manualRequestedMs;

  unsigned long burstStartMs;
  unsigned long sessionStartMs;
  unsigned long soakUntilMs;
  unsigned long lastWateredMs;
  unsigned long sessionPumpMs;
  unsigned long hourlyPumpMs;
  unsigned long dailyPumpMs;
  unsigned long hourWindowStartMs;
  unsigned long dayWindowStartMs;

  int moistureAtSessionStart;
  int moistureAfterLastBurst;
  unsigned long responseCheckAtMs;
  bool waterResponseFault;
  bool noFlowFault;

  uint32_t burstStartPulses;
  uint32_t sessionStartPulses;
  uint32_t lastDeliveredMl;
  uint32_t sessionDeliveredMl;
  StopReason lastStopReason;
};

// ========================= DEFAULT PLANT PROFILE =========================
// Kept aligned with current multi sensor code.ino names/thresholds.
PlantConfig plants[NUM_PLANTS] = {
  {"ZZ Plant",     4000, 1750, 40, 60, 3000, 100, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Monstera",     3500, 1150, 50, 70, 4000, 135, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Pothos",       3500, 1150, 45, 65, 3500, 115, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Snake Plant",  3500, 1150, 30, 50, 2000,  70, DEFAULT_SOAK_MS, 6UL * 60UL * 60UL * 1000UL, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO},
  {"Fern",         3500, 1150, 60, 80, 5000, 165, DEFAULT_SOAK_MS, DEFAULT_MIN_INTERVAL_MS, DEFAULT_MAX_BURST_MS, DEFAULT_MAX_SESSION_MS, DEFAULT_MAX_HOURLY_MS, DEFAULT_MAX_DAILY_MS, MODE_AUTO}
};

PlantState states[NUM_PLANTS];

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
WiFiClientSecure secureClient;
Preferences prefs;

volatile uint32_t flowPulseCount = 0;

bool systemReady = false;
bool wifiConnected = false;
bool emergencyStop = false;
bool tankEmpty = false;
bool hardwareSafe = false;
int activePlant = -1;
int displayPlant = 0;
unsigned long bootMs = 0;
unsigned long lastSensorMs = 0;
unsigned long lastLcdMs = 0;
unsigned long lastRotateMs = 0;
unsigned long lastStatusMs = 0;
unsigned long lastCommandMs = 0;
unsigned long lastConfigMs = 0;
unsigned long lastTelemetryHistoryMs = 0;
unsigned long lastWifiRetryMs = 0;
String deviceIp = "offline";
String lastCommandId = "";
String lastConfigVersion = "";

// ========================= HELPERS =========================
void IRAM_ATTR onFlowPulse() {
  flowPulseCount++;
}

String boolJson(bool value) {
  return value ? "true" : "false";
}

String modeText(PlantMode mode) {
  if (mode == MODE_MANUAL) return "MANUAL";
  if (mode == MODE_DISABLED) return "DISABLED";
  return "AUTO";
}

PlantMode parseMode(const String &value) {
  String v = value;
  v.toUpperCase();
  if (v == "MANUAL") return MODE_MANUAL;
  if (v == "DISABLED") return MODE_DISABLED;
  return MODE_AUTO;
}

String stopReasonText(StopReason reason) {
  switch (reason) {
    case STOP_TARGET_REACHED: return "Target reached";
    case STOP_BURST_COMPLETE: return "Burst complete";
    case STOP_SESSION_LIMIT: return "Session limit";
    case STOP_HOURLY_LIMIT: return "Hourly limit";
    case STOP_DAILY_LIMIT: return "Daily limit";
    case STOP_TANK_EMPTY: return "Tank empty";
    case STOP_NO_FLOW: return "No flow";
    case STOP_SENSOR_FAULT: return "Sensor fault";
    case STOP_EMERGENCY: return "Emergency stop";
    case STOP_MANUAL_COMPLETE: return "Manual complete";
    default: return "None";
  }
}

String resetReasonText() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "Unknown";
  }
}

String firebaseUrl(const String &path) {
  String url = String(FIREBASE_BASE_URL) + String(DEVICE_ROOT) + path + ".json";
  if (strlen(FIREBASE_AUTH) > 0) {
    url += "?auth=";
    url += FIREBASE_AUTH;
  }
  return url;
}

bool httpPutJson(const String &path, const String &json) {
  if (!wifiConnected) return false;
  secureClient.setInsecure();
  HTTPClient http;
  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(json);
  http.end();
  return code >= 200 && code < 300;
}

bool httpPostJson(const String &path, const String &json) {
  if (!wifiConnected) return false;
  secureClient.setInsecure();
  HTTPClient http;
  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  return code >= 200 && code < 300;
}

bool httpGet(const String &path, String &body) {
  if (!wifiConnected) return false;
  secureClient.setInsecure();
  HTTPClient http;
  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  int code = http.GET();
  if (code >= 200 && code < 300) body = http.getString();
  http.end();
  return code >= 200 && code < 300;
}

String jsonStringValue(const String &json, const String &key, const String &fallback = "") {
  String token = "\"" + key + "\"";
  int p = json.indexOf(token);
  if (p < 0) return fallback;
  p = json.indexOf(':', p + token.length());
  if (p < 0) return fallback;
  p++;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r')) p++;
  if (p >= (int)json.length()) return fallback;
  if (json[p] == '"') {
    int end = json.indexOf('"', p + 1);
    if (end < 0) return fallback;
    return json.substring(p + 1, end);
  }
  int end = p;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\n') end++;
  String value = json.substring(p, end);
  value.trim();
  return value;
}

long jsonLongValue(const String &json, const String &key, long fallback) {
  String value = jsonStringValue(json, key, "");
  if (!value.length() || value == "null") return fallback;
  return value.toInt();
}

bool jsonBoolValue(const String &json, const String &key, bool fallback) {
  String value = jsonStringValue(json, key, "");
  value.toLowerCase();
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return fallback;
}

// ========================= RELAY / POWER SAFETY =========================
void forceAllOutputsOff() {
  // Critical: configure and switch OFF before doing anything else at boot.
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, RELAY_OFF);
  for (int i = 0; i < NUM_PLANTS; i++) {
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], RELAY_OFF);
  }
  activePlant = -1;
  hardwareSafe = true;
}

void closeAllValves() {
  for (int i = 0; i < NUM_PLANTS; i++) {
    digitalWrite(VALVE_PINS[i], RELAY_OFF);
    states[i].watering = false;
  }
}

void emergencyHardwareStop(StopReason reason) {
  digitalWrite(PUMP_PIN, RELAY_OFF);
  delay(50);
  closeAllValves();
  if (activePlant >= 0) states[activePlant].lastStopReason = reason;
  activePlant = -1;
}

void updateTankState() {
  if (!TANK_SENSOR_ENABLED) {
    tankEmpty = false;
    return;
  }
  tankEmpty = digitalRead(TANK_FLOAT_PIN) == TANK_EMPTY_LEVEL;
  if (tankEmpty && activePlant >= 0) emergencyHardwareStop(STOP_TANK_EMPTY);
}

// ========================= PREFERENCES =========================
String keyFor(int index, const char *suffix) {
  return "p" + String(index) + suffix;
}

void savePlantConfig(int i) {
  prefs.putString(keyFor(i, "name").c_str(), plants[i].name);
  prefs.putInt(keyFor(i, "air").c_str(), plants[i].airRaw);
  prefs.putInt(keyFor(i, "wet").c_str(), plants[i].wetRaw);
  prefs.putInt(keyFor(i, "low").c_str(), plants[i].targetLow);
  prefs.putInt(keyFor(i, "high").c_str(), plants[i].targetHigh);
  prefs.putULong(keyFor(i, "burst").c_str(), plants[i].burstMs);
  prefs.putUInt(keyFor(i, "bml").c_str(), plants[i].burstMl);
  prefs.putULong(keyFor(i, "soak").c_str(), plants[i].soakMs);
  prefs.putULong(keyFor(i, "minint").c_str(), plants[i].minIntervalMs);
  prefs.putUChar(keyFor(i, "mode").c_str(), (uint8_t)plants[i].mode);
}

void loadPlantConfig(int i) {
  plants[i].name = prefs.getString(keyFor(i, "name").c_str(), plants[i].name);
  plants[i].airRaw = prefs.getInt(keyFor(i, "air").c_str(), plants[i].airRaw);
  plants[i].wetRaw = prefs.getInt(keyFor(i, "wet").c_str(), plants[i].wetRaw);
  plants[i].targetLow = prefs.getInt(keyFor(i, "low").c_str(), plants[i].targetLow);
  plants[i].targetHigh = prefs.getInt(keyFor(i, "high").c_str(), plants[i].targetHigh);
  plants[i].burstMs = prefs.getULong(keyFor(i, "burst").c_str(), plants[i].burstMs);
  plants[i].burstMl = prefs.getUInt(keyFor(i, "bml").c_str(), plants[i].burstMl);
  plants[i].soakMs = prefs.getULong(keyFor(i, "soak").c_str(), plants[i].soakMs);
  plants[i].minIntervalMs = prefs.getULong(keyFor(i, "minint").c_str(), plants[i].minIntervalMs);
  plants[i].mode = (PlantMode)prefs.getUChar(keyFor(i, "mode").c_str(), (uint8_t)plants[i].mode);

  // Sanity limits protect against corrupt remote/NVS values.
  plants[i].targetLow = constrain(plants[i].targetLow, 0, 95);
  plants[i].targetHigh = constrain(plants[i].targetHigh, plants[i].targetLow + 1, 100);
  plants[i].burstMs = constrain(plants[i].burstMs, 500UL, plants[i].maxBurstMs);
  plants[i].soakMs = constrain(plants[i].soakMs, 10000UL, 30UL * 60UL * 1000UL);
}

// ========================= SENSOR =========================
int readAverageRaw(int pin) {
  long total = 0;
  for (int s = 0; s < SENSOR_SAMPLES; s++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / SENSOR_SAMPLES;
}

int rawToPercent(int i, int raw) {
  if (plants[i].airRaw <= plants[i].wetRaw + 10) return 0;
  int clipped = constrain(raw, plants[i].wetRaw, plants[i].airRaw);
  return map(clipped, plants[i].airRaw, plants[i].wetRaw, 0, 100);
}

void updatePlantSensor(int i) {
  PlantState &s = states[i];
  int raw = readAverageRaw(SENSOR_PINS[i]);
  bool boundsFault = raw <= SENSOR_MIN_VALID || raw >= SENSOR_MAX_VALID;

  if (s.previousRaw < 0 || abs(raw - s.previousRaw) >= SENSOR_CHANGE_DELTA) {
    s.lastSensorChangeMs = millis();
  }

  bool stale = s.lastSensorChangeMs > 0 && millis() - s.lastSensorChangeMs > SENSOR_STALE_MS;
  s.sensorFault = boundsFault || stale;
  s.raw = raw;
  s.moisture = rawToPercent(i, raw);
  s.previousRaw = raw;

  if (!s.sensorFault) {
    if (s.validBootScans < REQUIRED_VALID_BOOT_SCANS) s.validBootScans++;
  } else {
    s.validBootScans = 0;
  }
}

bool allPlantsBootValidated() {
  for (int i = 0; i < NUM_PLANTS; i++) {
    if (states[i].validBootScans < REQUIRED_VALID_BOOT_SCANS) return false;
  }
  return true;
}

// ========================= FLOW / WATER VOLUME =========================
uint32_t pulsesToMl(uint32_t pulses) {
  if (!FLOW_SENSOR_ENABLED || FLOW_PULSES_PER_LITER <= 0.0f) return 0;
  float liters = ((float)pulses) / FLOW_PULSES_PER_LITER;
  return (uint32_t)(liters * 1000.0f);
}

uint32_t currentBurstMl(int i) {
  if (!FLOW_SENSOR_ENABLED) return 0;
  return pulsesToMl(flowPulseCount - states[i].burstStartPulses);
}

uint32_t currentSessionMl(int i) {
  if (!FLOW_SENSOR_ENABLED) return 0;
  return pulsesToMl(flowPulseCount - states[i].sessionStartPulses);
}

// ========================= WATERING SAFETY =========================
void refreshTimeWindows(int i) {
  PlantState &s = states[i];
  unsigned long now = millis();
  if (s.hourWindowStartMs == 0 || now - s.hourWindowStartMs >= 60UL * 60UL * 1000UL) {
    s.hourWindowStartMs = now;
    s.hourlyPumpMs = 0;
  }
  if (s.dayWindowStartMs == 0 || now - s.dayWindowStartMs >= 24UL * 60UL * 60UL * 1000UL) {
    s.dayWindowStartMs = now;
    s.dailyPumpMs = 0;
  }
}

bool bootAllowsWatering() {
  if (!systemReady || !hardwareSafe) return false;
  if (millis() - bootMs < BOOT_WATERING_GRACE_MS) return false;
  if (!allPlantsBootValidated()) return false;
  return true;
}

bool canStartPlant(int i, bool isManual) {
  PlantState &s = states[i];
  PlantConfig &c = plants[i];
  refreshTimeWindows(i);

  if (!bootAllowsWatering()) return false;
  if (emergencyStop || tankEmpty) return false;
  if (s.sensorFault) return false;
  if (s.noFlowFault || s.waterResponseFault) return false;
  if (c.mode == MODE_DISABLED) return false;
  if (!isManual && c.mode != MODE_AUTO) return false;
  if (!isManual && s.lastWateredMs > 0 && millis() - s.lastWateredMs < c.minIntervalMs) return false;
  if (s.hourlyPumpMs >= c.maxHourlyMs) return false;
  if (s.dailyPumpMs >= c.maxDailyMs) return false;
  return true;
}

void openValveThenPump(int i) {
  // Double safety: close every other valve first.
  for (int p = 0; p < NUM_PLANTS; p++) {
    if (p != i) digitalWrite(VALVE_PINS[p], RELAY_OFF);
  }
  digitalWrite(VALVE_PINS[i], RELAY_ON);
  delay(VALVE_PREOPEN_MS);
  digitalWrite(PUMP_PIN, RELAY_ON);
}

void publishWateringHistory(int i, StopReason reason, unsigned long durationMs, uint32_t deliveredMl) {
  String json = "{";
  json += "\"plantIndex\":" + String(i) + ",";
  json += "\"plantName\":\"" + plants[i].name + "\",";
  json += "\"eventUptimeMs\":" + String(millis()) + ",";
  json += "\"durationMs\":" + String(durationMs) + ",";
  json += "\"deliveredMl\":" + String(deliveredMl) + ",";
  json += "\"moistureStart\":" + String(states[i].moistureAtSessionStart) + ",";
  json += "\"moistureEnd\":" + String(states[i].moisture) + ",";
  json += "\"reason\":\"" + stopReasonText(reason) + "\",";
  json += "\"manual\":" + boolJson(states[i].manualRequest);
  json += "}";
  httpPostJson("/history/watering", json);
}

void finishPlantSession(int i, StopReason reason) {
  PlantState &s = states[i];
  unsigned long durationMs = s.sessionPumpMs;
  uint32_t deliveredMl = FLOW_SENSOR_ENABLED ? currentSessionMl(i) : 0;
  s.lastDeliveredMl = deliveredMl;
  s.sessionDeliveredMl = deliveredMl;
  s.lastWateredMs = millis();
  s.lastStopReason = reason;
  publishWateringHistory(i, reason, durationMs, deliveredMl);
  s.manualRequest = false;
  s.manualRequestedMl = 0;
  s.manualRequestedMs = 0;
  s.sessionPumpMs = 0;
  s.sessionStartMs = 0;
  s.soaking = false;
  activePlant = -1;
}

void stopBurst(int i, StopReason reason, bool finishSession) {
  PlantState &s = states[i];
  if (!s.watering) {
    if (finishSession) finishPlantSession(i, reason);
    return;
  }

  unsigned long burstDuration = millis() - s.burstStartMs;
  s.sessionPumpMs += burstDuration;
  s.hourlyPumpMs += burstDuration;
  s.dailyPumpMs += burstDuration;

  digitalWrite(PUMP_PIN, RELAY_OFF);
  delay(VALVE_POSTPUMP_MS);
  digitalWrite(VALVE_PINS[i], RELAY_OFF);
  s.watering = false;
  s.lastStopReason = reason;
  s.moistureAfterLastBurst = s.moisture;

  if (finishSession) {
    finishPlantSession(i, reason);
  } else {
    s.soaking = true;
    s.soakUntilMs = millis() + plants[i].soakMs;
    s.responseCheckAtMs = millis() + WATER_RESPONSE_TIMEOUT_MS;
  }
}

void startPlantSession(int i, bool manual) {
  PlantState &s = states[i];
  if (!canStartPlant(i, manual)) return;

  activePlant = i;
  s.sessionStartMs = millis();
  s.sessionPumpMs = 0;
  s.moistureAtSessionStart = s.moisture;
  s.sessionStartPulses = flowPulseCount;
  s.soaking = false;
  s.lastStopReason = STOP_NONE;
}

void startBurst(int i) {
  PlantState &s = states[i];
  PlantConfig &c = plants[i];
  if (activePlant != i || s.watering) return;
  if (emergencyStop || tankEmpty || s.sensorFault) return;

  refreshTimeWindows(i);
  if (s.sessionPumpMs >= c.maxSessionMs) {
    finishPlantSession(i, STOP_SESSION_LIMIT);
    return;
  }
  if (s.hourlyPumpMs >= c.maxHourlyMs) {
    finishPlantSession(i, STOP_HOURLY_LIMIT);
    return;
  }
  if (s.dailyPumpMs >= c.maxDailyMs) {
    finishPlantSession(i, STOP_DAILY_LIMIT);
    return;
  }

  s.burstStartMs = millis();
  s.burstStartPulses = flowPulseCount;
  s.watering = true;
  s.soaking = false;
  openValveThenPump(i);
}

bool manualTargetReached(int i) {
  PlantState &s = states[i];
  if (!s.manualRequest) return false;
  if (FLOW_SENSOR_ENABLED && s.manualRequestedMl > 0) return currentSessionMl(i) >= s.manualRequestedMl;
  if (s.manualRequestedMs > 0) return s.sessionPumpMs + (s.watering ? millis() - s.burstStartMs : 0) >= s.manualRequestedMs;
  return false;
}

bool normalBurstReached(int i) {
  PlantState &s = states[i];
  PlantConfig &c = plants[i];
  if (FLOW_SENSOR_ENABLED && c.burstMl > 0) return currentBurstMl(i) >= c.burstMl;
  return millis() - s.burstStartMs >= min(c.burstMs, c.maxBurstMs);
}

void evaluateActivePlant() {
  if (activePlant < 0) return;
  int i = activePlant;
  PlantState &s = states[i];
  PlantConfig &c = plants[i];

  updateTankState();

  if (emergencyStop) {
    stopBurst(i, STOP_EMERGENCY, true);
    return;
  }
  if (tankEmpty) {
    stopBurst(i, STOP_TANK_EMPTY, true);
    return;
  }
  if (s.sensorFault) {
    stopBurst(i, STOP_SENSOR_FAULT, true);
    return;
  }

  if (s.watering) {
    unsigned long runningMs = millis() - s.burstStartMs;

    if (FLOW_SENSOR_ENABLED && runningMs >= NO_FLOW_TIMEOUT_MS && flowPulseCount == s.burstStartPulses) {
      s.noFlowFault = true;
      stopBurst(i, STOP_NO_FLOW, true);
      return;
    }

    unsigned long totalSessionMs = s.sessionPumpMs + runningMs;
    if (totalSessionMs >= c.maxSessionMs) {
      stopBurst(i, STOP_SESSION_LIMIT, true);
      return;
    }
    if (s.hourlyPumpMs + runningMs >= c.maxHourlyMs) {
      stopBurst(i, STOP_HOURLY_LIMIT, true);
      return;
    }
    if (s.dailyPumpMs + runningMs >= c.maxDailyMs) {
      stopBurst(i, STOP_DAILY_LIMIT, true);
      return;
    }

    if (manualTargetReached(i)) {
      stopBurst(i, STOP_MANUAL_COMPLETE, true);
      return;
    }

    if (normalBurstReached(i)) {
      stopBurst(i, STOP_BURST_COMPLETE, false);
      return;
    }
    return;
  }

  if (s.soaking) {
    if (millis() < s.soakUntilMs) return;
    s.soaking = false;

    // Manual watering that already completed during previous burst.
    if (manualTargetReached(i)) {
      finishPlantSession(i, STOP_MANUAL_COMPLETE);
      return;
    }

    // Automatic target reached.
    if (!s.manualRequest && s.moisture >= c.targetHigh) {
      finishPlantSession(i, STOP_TARGET_REACHED);
      return;
    }

    // If soil never responds after enough time, stop repeated watering.
    if (!s.manualRequest && s.responseCheckAtMs > 0 && millis() >= s.responseCheckAtMs) {
      if (s.moisture - s.moistureAtSessionStart < MIN_MOISTURE_GAIN_PERCENT) {
        s.waterResponseFault = true;
        finishPlantSession(i, STOP_NO_FLOW);
        return;
      }
    }

    startBurst(i);
  }
}

int nextAutomaticPlant() {
  static int lastChosen = -1;
  for (int step = 1; step <= NUM_PLANTS; step++) {
    int i = (lastChosen + step) % NUM_PLANTS;
    PlantState &s = states[i];
    if (plants[i].mode != MODE_AUTO) continue;
    if (s.moisture >= plants[i].targetLow) continue;
    if (!canStartPlant(i, false)) continue;
    lastChosen = i;
    return i;
  }
  return -1;
}

void wateringController() {
  updateTankState();

  if (emergencyStop) {
    if (activePlant >= 0) emergencyHardwareStop(STOP_EMERGENCY);
    return;
  }

  if (activePlant >= 0) {
    evaluateActivePlant();
    return;
  }

  // Manual requests get priority.
  for (int i = 0; i < NUM_PLANTS; i++) {
    if (states[i].manualRequest && canStartPlant(i, true)) {
      startPlantSession(i, true);
      startBurst(i);
      return;
    }
  }

  int next = nextAutomaticPlant();
  if (next >= 0) {
    startPlantSession(next, false);
    startBurst(next);
  }
}

// ========================= CALIBRATION / REMOTE CONFIG =========================
void applyCalibrationCommand(int i, const String &kind) {
  if (i < 0 || i >= NUM_PLANTS || states[i].sensorFault) return;
  if (kind == "dry") plants[i].airRaw = states[i].raw;
  if (kind == "wet") plants[i].wetRaw = states[i].raw;
  if (plants[i].airRaw > plants[i].wetRaw + 100) savePlantConfig(i);
}

void applyRemotePlantConfig(int i, const String &json) {
  if (i < 0 || i >= NUM_PLANTS || json == "null" || json.length() < 2) return;

  String name = jsonStringValue(json, "name", plants[i].name);
  int low = jsonLongValue(json, "targetLow", plants[i].targetLow);
  int high = jsonLongValue(json, "targetHigh", plants[i].targetHigh);
  long burstMs = jsonLongValue(json, "burstMs", plants[i].burstMs);
  long burstMl = jsonLongValue(json, "burstMl", plants[i].burstMl);
  long soakSec = jsonLongValue(json, "soakSec", plants[i].soakMs / 1000UL);
  long minIntervalMin = jsonLongValue(json, "minIntervalMin", plants[i].minIntervalMs / 60000UL);
  String mode = jsonStringValue(json, "mode", modeText(plants[i].mode));

  if (name.length() > 0 && name.length() <= 24) plants[i].name = name;
  plants[i].targetLow = constrain(low, 0, 95);
  plants[i].targetHigh = constrain(high, plants[i].targetLow + 1, 100);
  plants[i].burstMs = constrain((unsigned long)max(500L, burstMs), 500UL, plants[i].maxBurstMs);
  plants[i].burstMl = constrain((uint32_t)max(10L, burstMl), (uint32_t)10, (uint32_t)2000);
  plants[i].soakMs = constrain((unsigned long)max(10L, soakSec) * 1000UL, 10000UL, 30UL * 60UL * 1000UL);
  plants[i].minIntervalMs = constrain((unsigned long)max(1L, minIntervalMin) * 60000UL, 60000UL, 7UL * 24UL * 60UL * 60UL * 1000UL);
  plants[i].mode = parseMode(mode);
  savePlantConfig(i);
}

void pollRemoteConfig() {
  String versionBody;
  if (!httpGet("/config/version", versionBody)) return;
  versionBody.replace("\"", "");
  versionBody.trim();
  if (!versionBody.length() || versionBody == "null" || versionBody == lastConfigVersion) return;

  for (int i = 0; i < NUM_PLANTS; i++) {
    String body;
    if (httpGet("/config/plants/" + String(i), body)) applyRemotePlantConfig(i, body);
  }
  lastConfigVersion = versionBody;
  prefs.putString("cfgVersion", lastConfigVersion);
}

void acknowledgeCommand(const String &id, const String &result) {
  String json = "{";
  json += "\"id\":\"" + id + "\",";
  json += "\"result\":\"" + result + "\",";
  json += "\"handledAtUptimeMs\":" + String(millis());
  json += "}";
  httpPutJson("/commandAck", json);
}

void handleRemoteCommand(const String &json) {
  if (json == "null" || json.length() < 2) return;
  String id = jsonStringValue(json, "id", "");
  String action = jsonStringValue(json, "action", "");
  if (!id.length() || !action.length() || id == lastCommandId) return;

  int plant = jsonLongValue(json, "plantIndex", -1);
  long durationSec = jsonLongValue(json, "durationSec", 0);
  long amountMl = jsonLongValue(json, "amountMl", 0);
  action.toLowerCase();
  String result = "ignored";

  if (action == "emergency_stop") {
    emergencyStop = true;
    prefs.putBool("eStop", true);
    emergencyHardwareStop(STOP_EMERGENCY);
    result = "stopped";
  } else if (action == "resume") {
    emergencyStop = false;
    prefs.putBool("eStop", false);
    result = "resumed";
  } else if (action == "water_now" && plant >= 0 && plant < NUM_PLANTS) {
    if (!emergencyStop && !tankEmpty && plants[plant].mode != MODE_DISABLED) {
      states[plant].manualRequest = true;
      states[plant].manualRequestedMl = constrain((uint32_t)max(0L, amountMl), (uint32_t)0, (uint32_t)2000);
      states[plant].manualRequestedMs = constrain((unsigned long)max(0L, durationSec) * 1000UL, 0UL, plants[plant].maxSessionMs);
      if (states[plant].manualRequestedMl == 0 && states[plant].manualRequestedMs == 0) states[plant].manualRequestedMs = 5000;
      result = "queued";
    } else {
      result = "blocked_by_safety";
    }
  } else if (action == "set_mode" && plant >= 0 && plant < NUM_PLANTS) {
    plants[plant].mode = parseMode(jsonStringValue(json, "mode", "AUTO"));
    savePlantConfig(plant);
    if (plants[plant].mode == MODE_DISABLED && activePlant == plant) stopBurst(plant, STOP_MANUAL_COMPLETE, true);
    result = "mode_saved";
  } else if (action == "calibrate_dry" && plant >= 0 && plant < NUM_PLANTS) {
    applyCalibrationCommand(plant, "dry");
    result = "dry_saved";
  } else if (action == "calibrate_wet" && plant >= 0 && plant < NUM_PLANTS) {
    applyCalibrationCommand(plant, "wet");
    result = "wet_saved";
  } else if (action == "clear_fault" && plant >= 0 && plant < NUM_PLANTS) {
    states[plant].noFlowFault = false;
    states[plant].waterResponseFault = false;
    result = "fault_cleared";
  }

  lastCommandId = id;
  prefs.putString("lastCmd", lastCommandId);
  acknowledgeCommand(id, result);
}

void pollRemoteCommand() {
  String body;
  if (httpGet("/command", body)) handleRemoteCommand(body);
}

// ========================= STATUS / HISTORY =========================
String buildPlantJson(int i) {
  PlantState &s = states[i];
  PlantConfig &c = plants[i];
  String json = "{";
  json += "\"name\":\"" + c.name + "\",";
  json += "\"moisture\":" + String(s.moisture) + ",";
  json += "\"raw\":" + String(s.raw) + ",";
  json += "\"targetLow\":" + String(c.targetLow) + ",";
  json += "\"targetHigh\":" + String(c.targetHigh) + ",";
  json += "\"targetRange\":\"" + String(c.targetLow) + "-" + String(c.targetHigh) + "%\",";
  json += "\"mode\":\"" + modeText(c.mode) + "\",";
  json += "\"watering\":" + boolJson(s.watering) + ",";
  json += "\"soaking\":" + boolJson(s.soaking) + ",";
  json += "\"manualQueued\":" + boolJson(s.manualRequest) + ",";
  json += "\"sensorFault\":" + boolJson(s.sensorFault) + ",";
  json += "\"noFlowFault\":" + boolJson(s.noFlowFault) + ",";
  json += "\"waterResponseFault\":" + boolJson(s.waterResponseFault) + ",";
  json += "\"fault\":" + boolJson(s.sensorFault || s.noFlowFault || s.waterResponseFault) + ",";
  json += "\"lastStopReason\":\"" + stopReasonText(s.lastStopReason) + "\",";
  json += "\"lastWateredUptimeMs\":" + String(s.lastWateredMs) + ",";
  json += "\"lastDeliveredMl\":" + String(s.lastDeliveredMl) + ",";
  json += "\"hourlyPumpMs\":" + String(s.hourlyPumpMs) + ",";
  json += "\"dailyPumpMs\":" + String(s.dailyPumpMs) + ",";
  json += "\"burstMs\":" + String(c.burstMs) + ",";
  json += "\"burstMl\":" + String(c.burstMl) + ",";
  json += "\"soakSec\":" + String(c.soakMs / 1000UL) + ",";
  json += "\"minIntervalMin\":" + String(c.minIntervalMs / 60000UL);
  json += "}";
  return json;
}

String buildStatusJson() {
  String json = "{";
  json += "\"firmware\":\"Plant_Watering_Smart_V2\",";
  json += "\"totalPlants\":" + String(NUM_PLANTS) + ",";
  json += "\"systemReady\":" + boolJson(systemReady) + ",";
  json += "\"wateringAllowed\":" + boolJson(bootAllowsWatering() && !emergencyStop && !tankEmpty) + ",";
  json += "\"emergencyStop\":" + boolJson(emergencyStop) + ",";
  json += "\"tankSensorEnabled\":" + boolJson(TANK_SENSOR_ENABLED) + ",";
  json += "\"tankEmpty\":" + boolJson(tankEmpty) + ",";
  json += "\"flowSensorEnabled\":" + boolJson(FLOW_SENSOR_ENABLED) + ",";
  json += "\"flowPulses\":" + String(flowPulseCount) + ",";
  json += "\"activePlantIndex\":" + String(activePlant) + ",";
  json += "\"ip\":\"" + deviceIp + "\",";
  json += "\"wifiRssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",";
  json += "\"uptimeMin\":" + String(millis() / 60000UL) + ",";
  json += "\"updatedAtMs\":" + String(millis()) + ",";
  json += "\"resetReason\":\"" + resetReasonText() + "\",";
  json += "\"plants\":[";
  for (int i = 0; i < NUM_PLANTS; i++) {
    if (i) json += ",";
    json += buildPlantJson(i);
  }
  json += "]}";
  return json;
}

void publishStatus() {
  httpPutJson("/status", buildStatusJson());
}

void publishTelemetryHistory() {
  String json = "{";
  json += "\"uptimeMs\":" + String(millis()) + ",";
  json += "\"tankEmpty\":" + boolJson(tankEmpty) + ",";
  json += "\"emergencyStop\":" + boolJson(emergencyStop) + ",";
  json += "\"plants\":[";
  for (int i = 0; i < NUM_PLANTS; i++) {
    if (i) json += ",";
    json += "{\"name\":\"" + plants[i].name + "\",\"moisture\":" + String(states[i].moisture) + ",\"raw\":" + String(states[i].raw) + "}";
  }
  json += "]}";
  httpPostJson("/history/telemetry", json);
}

// ========================= LCD =========================
void lcdLine(uint8_t row, String text) {
  while (text.length() < LCD_COLS) text += " ";
  if (text.length() > LCD_COLS) text = text.substring(0, LCD_COLS);
  lcd.setCursor(0, row);
  lcd.print(text);
}

void refreshLcd() {
  if (millis() - lastLcdMs < LCD_UPDATE_MS) return;
  lastLcdMs = millis();

  if (!systemReady) {
    lcdLine(0, "Plant Care V2");
    lcdLine(1, "Starting safely");
    return;
  }
  if (emergencyStop) {
    lcdLine(0, "EMERGENCY STOP");
    lcdLine(1, "Pump disabled");
    return;
  }
  if (tankEmpty) {
    lcdLine(0, "TANK EMPTY");
    lcdLine(1, "Pump disabled");
    return;
  }
  if (!bootAllowsWatering()) {
    lcdLine(0, "Safety startup");
    lcdLine(1, "Checking sensors");
    return;
  }

  if (millis() - lastRotateMs >= LCD_ROTATE_MS) {
    lastRotateMs = millis();
    displayPlant = (displayPlant + 1) % NUM_PLANTS;
  }

  int i = activePlant >= 0 ? activePlant : displayPlant;
  String line1 = plants[i].name + ":" + String(states[i].moisture) + "%";
  String line2;
  if (states[i].watering) line2 = "Watering " + modeText(plants[i].mode);
  else if (states[i].soaking) line2 = "Soaking...";
  else if (states[i].sensorFault) line2 = "Sensor fault";
  else line2 = modeText(plants[i].mode) + " / " + String(plants[i].targetLow) + "-" + String(plants[i].targetHigh);
  lcdLine(0, line1);
  lcdLine(1, line2);
}

// ========================= WIFI =========================
void beginWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiRetryMs = millis();
}

void maintainWifi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!wifiConnected) {
      wifiConnected = true;
      deviceIp = WiFi.localIP().toString();
      Serial.print("WiFi connected: ");
      Serial.println(deviceIp);
    }
    return;
  }

  if (wifiConnected) Serial.println("WiFi lost; local watering continues.");
  wifiConnected = false;
  deviceIp = "offline";

  if (millis() - lastWifiRetryMs >= WIFI_RETRY_MS) {
    lastWifiRetryMs = millis();
    WiFi.disconnect();
    beginWifi();
  }
}

// ========================= SETUP / LOOP =========================
void setup() {
  // FIRST ACTION AFTER RESET: make water hardware safe.
  forceAllOutputsOff();

  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("========================================");
  Serial.println("Plant Watering Smart V2");
  Serial.print("Reset reason: ");
  Serial.println(resetReasonText());
  Serial.println("Pump and valves forced OFF.");
  Serial.println("========================================");

  // Allow PSU / relay board / ESP32 rail to stabilize after outage or brownout.
  delay(POWER_STABILIZE_MS);
  bootMs = millis();

  prefs.begin("plantV2", false);
  emergencyStop = prefs.getBool("eStop", false);
  lastCommandId = prefs.getString("lastCmd", "");
  lastConfigVersion = prefs.getString("cfgVersion", "");
  for (int i = 0; i < NUM_PLANTS; i++) loadPlantConfig(i);

  analogReadResolution(12);
  for (int i = 0; i < NUM_PLANTS; i++) {
    pinMode(SENSOR_PINS[i], INPUT);
    states[i] = {};
    states[i].previousRaw = -1;
    states[i].lastSensorChangeMs = millis();
    states[i].hourWindowStartMs = millis();
    states[i].dayWindowStartMs = millis();
  }

  if (TANK_SENSOR_ENABLED) pinMode(TANK_FLOAT_PIN, INPUT_PULLUP);
  if (FLOW_SENSOR_ENABLED) {
    pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), onFlowPulse, FALLING);
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdLine(0, "Plant Care V2");
  lcdLine(1, "Safe boot...");

  // Prime sensors before any possibility of watering.
  for (int scan = 0; scan < REQUIRED_VALID_BOOT_SCANS; scan++) {
    for (int i = 0; i < NUM_PLANTS; i++) updatePlantSensor(i);
    delay(250);
  }

  updateTankState();
  beginWifi();

  systemReady = true;
  Serial.println("System initialized. Automatic watering remains blocked during boot grace period.");
}

void loop() {
  maintainWifi();
  updateTankState();

  if (millis() - lastSensorMs >= SENSOR_SAMPLE_MS) {
    lastSensorMs = millis();
    for (int i = 0; i < NUM_PLANTS; i++) updatePlantSensor(i);
  }

  wateringController();

  if (wifiConnected && millis() - lastCommandMs >= REMOTE_COMMAND_MS) {
    lastCommandMs = millis();
    pollRemoteCommand();
  }

  if (wifiConnected && millis() - lastConfigMs >= REMOTE_CONFIG_MS) {
    lastConfigMs = millis();
    pollRemoteConfig();
  }

  if (wifiConnected && millis() - lastStatusMs >= REMOTE_STATUS_MS) {
    lastStatusMs = millis();
    publishStatus();
  }

  if (wifiConnected && millis() - lastTelemetryHistoryMs >= TELEMETRY_HISTORY_MS) {
    lastTelemetryHistoryMs = millis();
    publishTelemetryHistory();
  }

  refreshLcd();
  delay(20);
}
