#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static const char *WIFI_SSID = "Airtel_Dhiraj";
static const char *WIFI_PASSWORD = "Airtel@9798";

static const char *FIREBASE_BASE_URL = "https://vernal-catfish-196407.firebaseio.com/";
static const char *FIREBASE_DEVICE_PATH = "/plantMonitor/main";
static const char *FIREBASE_AUTH = "";

static const int LCD_ADDRESS = 0x27;
static const int LCD_COLS = 16;
static const int LCD_ROWS = 2;
static const int SDA_PIN = 21;
static const int SCL_PIN = 22;

// ============ MULTI-SENSOR CONFIGURATION ============
static const int NUM_PLANTS = 5;

// Plant names
static const char *PLANT_NAMES[NUM_PLANTS] = {
  "ZZ Plant",
  "Monstera",
  "Pothos",
  "Snake Plant",
  "Fern"
};

// Sensor GPIO pins (Analog inputs on your ESP32 board labels: P34, P35, P32, P33, SVN)
static const int SENSOR_PINS[NUM_PLANTS] = {34, 35, 32, 33, 39};

// Relay GPIO pins (Digital outputs on your board labels: P23, P25, P26, P27, P13)
static const int RELAY_PINS[NUM_PLANTS] = {23, 25, 26, 27, 13};

// ============ PLANT-SPECIFIC CALIBRATION ============
// Each plant has different moisture requirements
struct PlantConfig {
  const char *name;
  int airRaw;           // Dry sensor reading
  int wetRaw;           // Wet sensor reading
  int targetLow;        // Start watering at this %
  int targetHigh;       // Stop watering at this %
  unsigned long burstMs;// Fixed watering duration
};

// Configure each plant's watering preferences
static const PlantConfig PLANT_CONFIG[NUM_PLANTS] = {
  // ZZ Plant - tolerates dry soil well
  {"ZZ Plant",      4000, 750,  40,  60,  3000},
  
  // Monstera - needs more frequent watering
  {"Monstera",      4000, 750,  50,  70,  4000},
  
  // Pothos - moderate watering
  {"Pothos",        4000, 750,  45,  65,  3500},
  
  // Snake Plant - very drought tolerant
  {"Snake Plant",   4000, 750,  30,  50,  2000},
  
  // Fern - needs consistently moist soil
  {"Fern",          4000, 750,  60,  80,  5000}
};

// ============ PLANT STATE TRACKING ============
struct PlantState {
  int filteredRaw;
  int moisturePct;
  int previousRaw;
  bool sensorFault;
  bool waterSupplyIssue;
  bool pumpRunning;
  unsigned long lastMeaningfulChangeMs;
  unsigned long waterCheckPumpMs;
  unsigned long waterCheckStartMoisture;
  unsigned long lastWaterIssueMs;
  unsigned long burstStartMs;
  unsigned long currentBurstMs;
};

static PlantState plantStates[NUM_PLANTS];

// ============ DISPLAY STATE ============
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
WiFiClientSecure secureClient;

char lastLine1[LCD_COLS + 1] = "";
char lastLine2[LCD_COLS + 1] = "";

bool lcdNeedsRefresh = true;
bool wifiConnected = false;
bool showStartupIp = false;

unsigned long lastSampleMs = 0;
unsigned long lastRemotePushMs = 0;
unsigned long lastLcdUpdateMs = 0;
unsigned long startupIpUntilMs = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastLcdNavUpdateMs = 0;

String deviceIp = "offline";
int currentDisplayPlantIndex = 0;  // Which plant to display on LCD

static const unsigned long LCD_UPDATE_MS = 1500;
static const unsigned long SENSOR_SAMPLE_MS = 500;
static const unsigned long REMOTE_PUSH_MS = 5000;
static const unsigned long STALE_MS = 300000;
static const unsigned long LOOP_DELAY_MS = 50;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
static const unsigned long WIFI_RETRY_MS = 30000;
static const unsigned long STARTUP_IP_SHOW_MS = 8000;
static const unsigned long WATER_CHECK_MIN_PUMP_MS = 12000;
static const unsigned long WATER_RETRY_MS = 60000;
static const unsigned long LCD_NAV_UPDATE_MS = 5000;  // Auto-switch plant display every 5s
static const int CHANGE_DELTA = 5;
static const int SENSOR_SAMPLES = 12;
static const int WATER_GAIN_CLEAR_PERCENT = 2;

// ============ INITIALIZATION FUNCTIONS ============

void safeLcdInit()
{
  delay(250);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  delay(50);
  lcd.init();
  delay(20);
  lcd.backlight();
  delay(20);
  lcd.clear();
  delay(20);
}

void restoreLcdAfterPump()
{
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);
  delay(20);
  lcd.init();
  delay(20);
  lcd.backlight();
  delay(20);
  lastLine1[0] = '\0';
  lastLine2[0] = '\0';
}

void initializeRelays()
{
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    pinMode(RELAY_PINS[i], OUTPUT_OPEN_DRAIN);
    digitalWrite(RELAY_PINS[i], HIGH);  // Relay OFF
  }
  Serial.println("All relays initialized (OFF state)");
}

void initializeSensors()
{
  analogReadResolution(12);
  
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    pinMode(SENSOR_PINS[i], INPUT);
    
    // Initialize state
    plantStates[i].filteredRaw = PLANT_CONFIG[i].airRaw;
    plantStates[i].moisturePct = 0;
    plantStates[i].previousRaw = -1;
    plantStates[i].sensorFault = false;
    plantStates[i].waterSupplyIssue = false;
    plantStates[i].pumpRunning = false;
    plantStates[i].lastMeaningfulChangeMs = millis();
    plantStates[i].waterCheckPumpMs = 0;
    plantStates[i].waterCheckStartMoisture = 0;
    plantStates[i].lastWaterIssueMs = 0;
    plantStates[i].burstStartMs = 0;
    plantStates[i].currentBurstMs = 0;
  }
  
  Serial.print("Initialized ");
  Serial.print(NUM_PLANTS);
  Serial.println(" sensors");
}

// ============ SENSOR & WATERING LOGIC ============

int readFilteredRaw(int plantIndex)
{
  long total = 0;
  for (int index = 0; index < SENSOR_SAMPLES; ++index)
  {
    total += analogRead(SENSOR_PINS[plantIndex]);
    delay(4);
  }
  return total / SENSOR_SAMPLES;
}

int rawToPercent(int raw, int plantIndex)
{
  int airRaw = PLANT_CONFIG[plantIndex].airRaw;
  int wetRaw = PLANT_CONFIG[plantIndex].wetRaw;
  
  raw = constrain(raw, wetRaw, airRaw);
  return map(raw, airRaw, wetRaw, 0, 100);
}

void relayOn(int plantIndex)
{
  digitalWrite(RELAY_PINS[plantIndex], LOW);
}

void relayOff(int plantIndex)
{
  digitalWrite(RELAY_PINS[plantIndex], HIGH);
}

void resetWaterSupplyMonitor(int plantIndex)
{
  plantStates[plantIndex].waterCheckPumpMs = 0;
  plantStates[plantIndex].waterCheckStartMoisture = plantStates[plantIndex].moisturePct;
}

void evaluateWaterSupplyIssue(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  int targetLow = PLANT_CONFIG[plantIndex].targetLow;
  
  if (state->moisturePct >= targetLow)
  {
    state->waterSupplyIssue = false;
    resetWaterSupplyMonitor(plantIndex);
    return;
  }

  int moistureGain = state->moisturePct - state->waterCheckStartMoisture;
  if (moistureGain >= WATER_GAIN_CLEAR_PERCENT)
  {
    state->waterSupplyIssue = false;
    resetWaterSupplyMonitor(plantIndex);
    return;
  }

  if (!state->waterSupplyIssue && state->waterCheckPumpMs >= WATER_CHECK_MIN_PUMP_MS)
  {
    state->waterSupplyIssue = true;
    state->lastWaterIssueMs = millis();
    lcdNeedsRefresh = true;
  }
}

void updateSensorState(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  
  state->filteredRaw = readFilteredRaw(plantIndex);
  state->moisturePct = rawToPercent(state->filteredRaw, plantIndex);

  if (state->previousRaw < 0 || abs(state->filteredRaw - state->previousRaw) > CHANGE_DELTA)
  {
    state->lastMeaningfulChangeMs = millis();
    state->sensorFault = false;
  }

  if (millis() - state->lastMeaningfulChangeMs > STALE_MS)
  {
    state->sensorFault = true;
  }

  evaluateWaterSupplyIssue(plantIndex);

  state->previousRaw = state->filteredRaw;
}

void stopPump(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  bool wasRunning = state->pumpRunning;

  if (wasRunning)
  {
    state->waterCheckPumpMs += millis() - state->burstStartMs;
  }

  state->pumpRunning = false;
  state->currentBurstMs = 0;
  relayOff(plantIndex);

  if (wasRunning)
  {
    delay(80);
    restoreLcdAfterPump();
  }

  lcdNeedsRefresh = true;
  lastLcdUpdateMs = 0;
}

void startPumpBurst(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  unsigned long burstMs = PLANT_CONFIG[plantIndex].burstMs;

  if (burstMs == 0)
  {
    stopPump(plantIndex);
    return;
  }

  state->pumpRunning = true;
  state->currentBurstMs = burstMs;
  state->burstStartMs = millis();

  if (state->waterCheckPumpMs == 0)
  {
    state->waterCheckStartMoisture = state->moisturePct;
  }

  relayOn(plantIndex);
  lcdNeedsRefresh = true;
  
  Serial.print("Watering ");
  Serial.print(PLANT_CONFIG[plantIndex].name);
  Serial.print(" for ");
  Serial.print(burstMs);
  Serial.println("ms");
}

void controlPump(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  int targetLow = PLANT_CONFIG[plantIndex].targetLow;
  int targetHigh = PLANT_CONFIG[plantIndex].targetHigh;
  
  if (state->sensorFault)
  {
    stopPump(plantIndex);
    return;
  }

  if (state->waterSupplyIssue)
  {
    stopPump(plantIndex);

    if (millis() - state->lastWaterIssueMs < WATER_RETRY_MS)
    {
      return;
    }

    state->waterSupplyIssue = false;
    resetWaterSupplyMonitor(plantIndex);
  }

  if (state->moisturePct >= targetLow)
  {
    resetWaterSupplyMonitor(plantIndex);
    stopPump(plantIndex);
    return;
  }

  if (!state->pumpRunning)
  {
    startPumpBurst(plantIndex);
    return;
  }

  if (millis() - state->burstStartMs >= state->currentBurstMs)
  {
    stopPump(plantIndex);
  }
}

String getStatusText(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  
  if (state->sensorFault)
  {
    return "Sensor Fault";
  }

  if (state->waterSupplyIssue)
  {
    return "Check Tank";
  }

  if (state->pumpRunning)
  {
    return "Watering...";
  }

  if (state->moisturePct > PLANT_CONFIG[plantIndex].targetHigh)
  {
    return "Wet";
  }

  if (state->moisturePct >= PLANT_CONFIG[plantIndex].targetLow)
  {
    return "OK";
  }

  return "Dry";
}

String getSensorHealthText(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  
  if (state->sensorFault)
  {
    return "Fault";
  }

  if (state->waterSupplyIssue)
  {
    return "Water Issue";
  }

  return "Good";
}

unsigned long getLastChangeSeconds(int plantIndex)
{
  return (millis() - plantStates[plantIndex].lastMeaningfulChangeMs) / 1000;
}

// ============ DISPLAY FUNCTIONS ============

void padLine(char *dest, const char *src)
{
  snprintf(dest, LCD_COLS + 1, "%-16.16s", src);
}

void writeLine(uint8_t row, const char *text)
{
  char padded[LCD_COLS + 1];
  padLine(padded, text);

  char *cache = row == 0 ? lastLine1 : lastLine2;
  if (strncmp(cache, padded, LCD_COLS) == 0)
  {
    return;
  }

  lcd.setCursor(0, row);
  lcd.print(padded);
  strncpy(cache, padded, LCD_COLS + 1);
}

void refreshDisplay(bool force)
{
  if (showStartupIp)
  {
    if (!force && millis() >= startupIpUntilMs)
    {
      showStartupIp = false;
      lcdNeedsRefresh = true;
      lastLcdUpdateMs = 0;
    }
    else
    {
      if (!force && !lcdNeedsRefresh)
      {
        return;
      }

      char line1[LCD_COLS + 1];
      char line2[LCD_COLS + 1];
      snprintf(line1, sizeof(line1), "IP Address");
      snprintf(line2, sizeof(line2), "%s", deviceIp.c_str());
      writeLine(0, line1);
      writeLine(1, line2);
      lcdNeedsRefresh = false;
      lastLcdUpdateMs = millis();
      return;
    }
  }

  // Auto-navigate through plants
  if (millis() - lastLcdNavUpdateMs >= LCD_NAV_UPDATE_MS)
  {
    lastLcdNavUpdateMs = millis();
    currentDisplayPlantIndex = (currentDisplayPlantIndex + 1) % NUM_PLANTS;
    lcdNeedsRefresh = true;
  }

  PlantState *state = &plantStates[currentDisplayPlantIndex];
  
  if (state->pumpRunning)
  {
    if (!force && !lcdNeedsRefresh)
    {
      return;
    }
  }
  else if (!force && !lcdNeedsRefresh && millis() - lastLcdUpdateMs < LCD_UPDATE_MS)
  {
    return;
  }

  lastLcdUpdateMs = millis();

  char line1[LCD_COLS + 1];
  char line2[LCD_COLS + 1];

  snprintf(line1, sizeof(line1), "%s:%3d%%", PLANT_CONFIG[currentDisplayPlantIndex].name, state->moisturePct);
  snprintf(line2, sizeof(line2), "%s", getStatusText(currentDisplayPlantIndex).c_str());

  writeLine(0, line1);
  writeLine(1, line2);
  lcdNeedsRefresh = false;
}

// ============ WIFI & REMOTE FUNCTIONS ============

void connectWifi()
{
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lastWifiRetryMs = millis();

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(500);
    Serial.print('.');
  }

  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected)
  {
    deviceIp = WiFi.localIP().toString();
    showStartupIp = true;
    startupIpUntilMs = millis() + STARTUP_IP_SHOW_MS;
    lcdNeedsRefresh = true;
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(deviceIp);
  }
  else
  {
    deviceIp = "offline";
    Serial.println();
    Serial.println("WiFi not connected. Remote sync disabled.");
  }
}

void updateWifiState(bool connected)
{
  if (connected)
  {
    deviceIp = WiFi.localIP().toString();

    if (!wifiConnected)
    {
      wifiConnected = true;
      showStartupIp = true;
      startupIpUntilMs = millis() + STARTUP_IP_SHOW_MS;
      lcdNeedsRefresh = true;
      lastLcdUpdateMs = 0;
      Serial.println();
      Serial.print("WiFi connected. IP: ");
      Serial.println(deviceIp);
    }

    return;
  }

  if (wifiConnected)
  {
    Serial.println();
    Serial.println("WiFi disconnected. Auto-retry enabled.");
  }

  wifiConnected = false;
  deviceIp = "offline";
  showStartupIp = false;
  lcdNeedsRefresh = true;
}

void maintainWifiConnection()
{
  wl_status_t status = WiFi.status();
  bool connected = status == WL_CONNECTED;

  updateWifiState(connected);
  if (connected)
  {
    return;
  }

  if (millis() - lastWifiRetryMs < WIFI_RETRY_MS)
  {
    return;
  }

  lastWifiRetryMs = millis();
  Serial.println("Retrying WiFi connection...");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

String buildRemoteJson()
{
  String json = "{";
  json += "\"deviceName\":\"multiPlantMonitor\",";
  json += "\"totalPlants\":" + String(NUM_PLANTS) + ",";
  json += "\"plants\":[";

  for (int i = 0; i < NUM_PLANTS; i++)
  {
    PlantState *state = &plantStates[i];
    
    if (i > 0) json += ",";
    
    json += "{";
    json += "\"name\":\"" + String(PLANT_CONFIG[i].name) + "\",";
    json += "\"moisture\":" + String(state->moisturePct) + ",";
    json += "\"raw\":" + String(state->filteredRaw) + ",";
    json += "\"watering\":" + String(state->pumpRunning ? "true" : "false") + ",";
    json += "\"fault\":" + String((state->sensorFault || state->waterSupplyIssue) ? "true" : "false") + ",";
    json += "\"waterIssue\":" + String(state->waterSupplyIssue ? "true" : "false") + ",";
    json += "\"sensorHealth\":\"" + getSensorHealthText(i) + "\",";
    json += "\"status\":\"" + getStatusText(i) + "\",";
    json += "\"targetLow\":" + String(PLANT_CONFIG[i].targetLow) + ",";
    json += "\"targetHigh\":" + String(PLANT_CONFIG[i].targetHigh) + ",";
    json += "\"targetRange\":\"" + String(PLANT_CONFIG[i].targetLow) + "-" + String(PLANT_CONFIG[i].targetHigh) + "%\",";
    json += "\"lastChangeSec\":" + String(getLastChangeSeconds(i)) + "";
    json += "}";
  }

  json += "],";
  json += "\"uptimeMin\":" + String(millis() / 60000) + ",";
  json += "\"ip\":\"" + deviceIp + "\",";
  json += "\"updatedAtMs\":" + String(millis()) + "}";
  
  return json;
}

String buildFirebaseUrl()
{
  String url = String(FIREBASE_BASE_URL) + String(FIREBASE_DEVICE_PATH) + ".json";
  if (strlen(FIREBASE_AUTH) > 0)
  {
    url += "?auth=";
    url += FIREBASE_AUTH;
  }
  return url;
}

void sendRemoteStatus()
{
  if (!wifiConnected)
  {
    return;
  }

  secureClient.setInsecure();
  HTTPClient http;
  String url = buildFirebaseUrl();

  if (!http.begin(secureClient, url))
  {
    Serial.println("Remote sync begin failed.");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(buildRemoteJson());

  Serial.print("Remote sync HTTP=");
  Serial.println(httpCode);
  http.end();
}

// ============ SERIAL LOGGING ============

void printSerialStatus()
{
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    PlantState *state = &plantStates[i];
    
    Serial.print(PLANT_CONFIG[i].name);
    Serial.print(" | RAW=");
    Serial.print(state->filteredRaw);
    Serial.print(" Moisture=");
    Serial.print(state->moisturePct);
    Serial.print("% (");
    Serial.print(PLANT_CONFIG[i].targetLow);
    Serial.print("-");
    Serial.print(PLANT_CONFIG[i].targetHigh);
    Serial.print("%) Watering=");
    Serial.print(state->pumpRunning ? "YES" : "NO");
    Serial.print(" Fault=");
    Serial.print((state->sensorFault || state->waterSupplyIssue) ? "YES" : "NO");
    Serial.println();
  }
  Serial.println("---");
}

// ============ SETUP & LOOP ============

void setup()
{
  Serial.begin(115200);
  delay(200);

  Serial.println("\n\n========================================");
  Serial.println("Multi-Plant Monitor Started");
  Serial.println("========================================");
  Serial.print("Number of plants: ");
  Serial.println(NUM_PLANTS);
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(PLANT_CONFIG[i].name);
    Serial.print(" (Target: ");
    Serial.print(PLANT_CONFIG[i].targetLow);
    Serial.print("-");
    Serial.print(PLANT_CONFIG[i].targetHigh);
    Serial.print("%)");
    Serial.println();
  }
  Serial.println("========================================\n");

  safeLcdInit();
  initializeSensors();
  initializeRelays();
  
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    updateSensorState(i);
  }
  
  connectWifi();
  refreshDisplay(true);
}

void loop()
{
  maintainWifiConnection();

  if (millis() - lastSampleMs >= SENSOR_SAMPLE_MS)
  {
    lastSampleMs = millis();
    
    // Update all sensor states and control pumps
    for (int i = 0; i < NUM_PLANTS; i++)
    {
      updateSensorState(i);
      controlPump(i);
    }

    printSerialStatus();
  }

  if (wifiConnected && millis() - lastRemotePushMs >= REMOTE_PUSH_MS)
  {
    lastRemotePushMs = millis();
    sendRemoteStatus();
  }

  refreshDisplay(false);
  delay(LOOP_DELAY_MS);
}
