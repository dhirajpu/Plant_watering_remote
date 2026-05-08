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

// Sensor GPIO pins (Analog inputs: P34, P35, P32, P33, SVN/P39)
static const int SENSOR_PINS[NUM_PLANTS] = {34, 35, 32, 33, 39};

// Shared pump GPIO pin (P23) - runs whenever ANY plant is watered
static const int PUMP_PIN = 23;

// Solenoid valve GPIO pins per plant (8-channel relay plan)
// Plant mapping:
//   Plant 0 (ZZ Plant)   -> GPIO14 (Valve CH1)
//   Plant 1 (Monstera)   -> GPIO25 (Valve CH2)
//   Plant 2 (Pothos)     -> GPIO26 (Valve CH3)
//   Plant 3 (Snake Plant)-> GPIO27 (Valve CH4)
//   Plant 4 (Fern)       -> GPIO13 (Valve CH5)
// Relay CH6-CH8 are intentionally free for future use.
static const int VALVE_PINS[NUM_PLANTS] = {14, 25, 26, 27, 13};

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
  {"ZZ Plant",      4000, 1750,  40,  60,  3000},
  
  // Monstera - new sensor calibrated (dry=3500, wet=1150)
  {"Monstera",      3500, 1150, 50,  70,  4000},
  
  // Pothos - new sensor calibrated (dry=3500, wet=1150)
  {"Pothos",        3500, 1150, 45,  65,  3500},
  
  // Snake Plant - new sensor calibrated (dry=3500, wet=1150)
  {"Snake Plant",   3500, 1150, 30,  50,  2000},
  
  // Fern - new sensor calibrated (dry=3500, wet=1150)
  {"Fern",          3500, 1150, 60,  80,  5000}
};

// ============ PLANT STATE TRACKING ============
struct PlantState {
  int filteredRaw;
  int moisturePct;
  int previousRaw;
  int rawSpread;
  int invalidReadStreak;
  int validReadStreak;
  bool adcBoundsFault;
  bool rangeFault;
  bool floatingFault;
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
int activePlantIndex = -1;         // Single plant currently using shared pump
bool systemReady = false;           // Flag to track boot completion

// Boot stage tracking
enum BootStage {
  BOOT_START,
  BOOT_LCD_INIT,
  BOOT_SENSORS,
  BOOT_RELAYS,
  BOOT_WIFI,
  BOOT_COMPLETE
};
BootStage currentBootStage = BOOT_START;
unsigned long bootStageStartMs = 0;

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
static const int ADC_RAW_MIN_VALID = 80;
static const int ADC_RAW_MAX_VALID = 4080;
static const int CALIBRATION_RANGE_MARGIN = 200;
static const int FLOATING_SENSOR_SPREAD_THRESHOLD = 300;
static const int SENSOR_FAULT_STREAK_LIMIT = 2;
static const int SENSOR_FAULT_CLEAR_STREAK = 4;

// ============ INITIALIZATION FUNCTIONS ============

void safeLcdInit()
{
  currentBootStage = BOOT_LCD_INIT;
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
  currentBootStage = BOOT_RELAYS;
  // Shared pump pin (GPIO23)
  pinMode(PUMP_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PUMP_PIN, HIGH);  // Pump OFF

  // Solenoid valve pins on 8-channel relay
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    pinMode(VALVE_PINS[i], OUTPUT_OPEN_DRAIN);
    digitalWrite(VALVE_PINS[i], HIGH);  // Valve CLOSED
  }
  Serial.println("Pump and all valves initialized (OFF/CLOSED state)");
}

void initializeSensors()
{
  currentBootStage = BOOT_SENSORS;
  analogReadResolution(12);
  
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    pinMode(SENSOR_PINS[i], INPUT);
    
    // Initialize state
    plantStates[i].filteredRaw = PLANT_CONFIG[i].airRaw;
    plantStates[i].moisturePct = 0;
    plantStates[i].previousRaw = -1;
    plantStates[i].rawSpread = 0;
    plantStates[i].invalidReadStreak = 0;
    plantStates[i].validReadStreak = 0;
    plantStates[i].adcBoundsFault = false;
    plantStates[i].rangeFault = false;
    plantStates[i].floatingFault = false;
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

int readFilteredRaw(int plantIndex, int *spreadOut)
{
  long total = 0;
  int minimumRaw = 4095;
  int maximumRaw = 0;

  for (int index = 0; index < SENSOR_SAMPLES; ++index)
  {
    int sample = analogRead(SENSOR_PINS[plantIndex]);
    total += sample;
    minimumRaw = min(minimumRaw, sample);
    maximumRaw = max(maximumRaw, sample);
    delay(4);
  }

  if (spreadOut != nullptr)
  {
    *spreadOut = maximumRaw - minimumRaw;
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
  // Open target plant valve first, then start shared pump
  digitalWrite(VALVE_PINS[plantIndex], LOW);  // Open valve
  delay(50);                                   // Let valve open before water flows
  digitalWrite(PUMP_PIN, LOW);  // Start shared pump
}

void relayOff(int plantIndex)
{
  digitalWrite(PUMP_PIN, HIGH);  // Stop shared pump first
  delay(50);                                   // Let pump fully stop
  digitalWrite(VALVE_PINS[plantIndex], HIGH);  // Close valve
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
  int rawSpread = 0;
  int wetRaw = PLANT_CONFIG[plantIndex].wetRaw;
  int airRaw = PLANT_CONFIG[plantIndex].airRaw;
  
  state->filteredRaw = readFilteredRaw(plantIndex, &rawSpread);
  state->rawSpread = rawSpread;
  state->moisturePct = rawToPercent(state->filteredRaw, plantIndex);

  if (state->previousRaw < 0 || abs(state->filteredRaw - state->previousRaw) > CHANGE_DELTA)
  {
    state->lastMeaningfulChangeMs = millis();
  }

  int validMinRaw = wetRaw - CALIBRATION_RANGE_MARGIN;
  int validMaxRaw = airRaw + CALIBRATION_RANGE_MARGIN;

  bool skipDisconnectCheck = state->pumpRunning;
  if (skipDisconnectCheck)
  {
    state->adcBoundsFault = false;
    state->rangeFault = false;
    state->floatingFault = false;
  }
  else
  {
    state->adcBoundsFault = (state->filteredRaw <= ADC_RAW_MIN_VALID || state->filteredRaw >= ADC_RAW_MAX_VALID);
    state->rangeFault = (state->filteredRaw < validMinRaw || state->filteredRaw > validMaxRaw);
    state->floatingFault = (rawSpread >= FLOATING_SENSOR_SPREAD_THRESHOLD);
  }

  bool disconnectedNow = (!skipDisconnectCheck && (state->adcBoundsFault || state->rangeFault || state->floatingFault));

  if (disconnectedNow)
  {
    state->invalidReadStreak++;
    state->validReadStreak = 0;
  }
  else
  {
    state->validReadStreak++;
    state->invalidReadStreak = max(0, state->invalidReadStreak - 1);
  }

  bool disconnectedFault = (state->invalidReadStreak >= SENSOR_FAULT_STREAK_LIMIT);
  if (state->validReadStreak >= SENSOR_FAULT_CLEAR_STREAK && !disconnectedFault)
  {
    disconnectedFault = false;
  }

  bool staleFault = (millis() - state->lastMeaningfulChangeMs > STALE_MS);
  state->sensorFault = (staleFault || disconnectedFault);

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

int findNextPlantNeedingWater()
{
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    PlantState *state = &plantStates[i];

    if (state->sensorFault)
    {
      continue;
    }

    if (state->waterSupplyIssue)
    {
      if (millis() - state->lastWaterIssueMs < WATER_RETRY_MS)
      {
        continue;
      }

      state->waterSupplyIssue = false;
      resetWaterSupplyMonitor(i);
    }

    if (state->moisturePct < PLANT_CONFIG[i].targetLow)
    {
      return i;
    }
  }

  return -1;
}

void controlSharedPump()
{
  if (activePlantIndex >= 0)
  {
    PlantState *active = &plantStates[activePlantIndex];
    int targetHigh = PLANT_CONFIG[activePlantIndex].targetHigh;

    if (active->sensorFault)
    {
      stopPump(activePlantIndex);
      activePlantIndex = -1;
      return;
    }

    if (active->waterSupplyIssue)
    {
      stopPump(activePlantIndex);
      activePlantIndex = -1;
      return;
    }

    // Hysteresis: start below targetLow, stop only at/above targetHigh.
    // Also never stop mid-burst due to noisy readings.
    if (active->pumpRunning)
    {
      if (millis() - active->burstStartMs >= active->currentBurstMs)
      {
        stopPump(activePlantIndex);

        if (active->moisturePct >= targetHigh)
        {
          resetWaterSupplyMonitor(activePlantIndex);
          activePlantIndex = -1;
        }
      }
      return;
    }

    if (active->moisturePct >= targetHigh)
    {
      resetWaterSupplyMonitor(activePlantIndex);
      activePlantIndex = -1;
      return;
    }

    // Continue burst watering for this owner until targetHigh is reached.
    startPumpBurst(activePlantIndex);
    return;
  }

  int nextPlant = findNextPlantNeedingWater();
  if (nextPlant >= 0)
  {
    activePlantIndex = nextPlant;
    startPumpBurst(activePlantIndex);
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

String getSensorFaultReasonText(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];

  if (!state->sensorFault)
  {
    return "None";
  }

  if (state->adcBoundsFault)
  {
    return "Sensor Disconnected";
  }

  if (state->rangeFault)
  {
    return "Outside Calibration";
  }

  if (state->floatingFault)
  {
    return "Floating/Unwired";
  }

  if (millis() - state->lastMeaningfulChangeMs > STALE_MS)
  {
    return "No Change/Disconnected";
  }

  return "Sensor fault";
}

String getSensorHealthText(int plantIndex)
{
  PlantState *state = &plantStates[plantIndex];
  
  if (state->sensorFault)
  {
    return "Fault: " + getSensorFaultReasonText(plantIndex);
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
  if (!systemReady)
  {
    // Display progressive boot messages
    const char *line1 = "Starting...";
    const char *line2 = "Connecting...";
    
    switch (currentBootStage)
    {
      case BOOT_START:
        line1 = "Plant Care";
        line2 = "Starting...";
        break;
      case BOOT_LCD_INIT:
        line1 = "Display Init";
        line2 = "Ready";
        break;
      case BOOT_SENSORS:
        line1 = "Sensors Init";
        line2 = "Calibrating...";
        break;
      case BOOT_RELAYS:
        line1 = "Relays Init";
        line2 = "Pump & Valves";
        break;
      case BOOT_WIFI:
        line1 = "WiFi Connect";
        line2 = "Please wait...";
        break;
      case BOOT_COMPLETE:
        line1 = "System Ready!";
        line2 = "Loading...";
        break;
      default:
        break;
    }
    
    writeLine(0, line1);
    writeLine(1, line2);
    return;
  }

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
  currentBootStage = BOOT_WIFI;
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
    json += "\"sensorFaultReason\":\"" + getSensorFaultReasonText(i) + "\",";
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
    Serial.print(" FaultReason=");
    Serial.print(getSensorFaultReasonText(i));
    Serial.print(" Spread=");
    Serial.print(state->rawSpread);
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

  currentBootStage = BOOT_START;
  safeLcdInit();
  refreshDisplay(true);  // Show initial boot message
  delay(500);
  
  initializeSensors();
  refreshDisplay(true);
  delay(500);
  
  initializeRelays();
  refreshDisplay(true);
  delay(500);
  
  for (int i = 0; i < NUM_PLANTS; i++)
  {
    updateSensorState(i);
  }
  
  connectWifi();
  refreshDisplay(true);
  
  currentBootStage = BOOT_COMPLETE;
  refreshDisplay(true);
  delay(1000);
  
  systemReady = true;  // Mark system as ready for normal operation
}

void loop()
{
  maintainWifiConnection();

  if (millis() - lastSampleMs >= SENSOR_SAMPLE_MS)
  {
    lastSampleMs = millis();
    
    // Update all sensor states
    for (int i = 0; i < NUM_PLANTS; i++)
    {
      updateSensorState(i);
    }

    // Shared pump controller (single active plant at a time)
    controlSharedPump();

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
