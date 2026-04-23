//this code is belongs to arduino it's working code with live status on phone
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static const char *WIFI_SSID = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

static const char *FIREBASE_BASE_URL = "https://vernal-catfish-196407.firebaseio.com/";
static const char *FIREBASE_DEVICE_PATH = "/plantMonitor/main";
static const char *FIREBASE_AUTH = "";

static const int LCD_ADDRESS = 0x27;
static const int LCD_COLS = 16;
static const int LCD_ROWS = 2;
static const int SDA_PIN = 21;
static const int SCL_PIN = 22;

static const int SENSOR_PIN = 34;
static const int RELAY_PIN = 23;

static const int AIR_RAW = 4000;
static const int WET_RAW = 750;
static const int TARGET_LOW = 50;
static const int TARGET_HIGH = 70;

static const unsigned long LCD_UPDATE_MS = 1500;
static const unsigned long SENSOR_SAMPLE_MS = 500;
static const unsigned long REMOTE_PUSH_MS = 5000;
static const unsigned long STALE_MS = 300000;
static const unsigned long LOOP_DELAY_MS = 50;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
static const unsigned long STARTUP_IP_SHOW_MS = 8000;
static const int CHANGE_DELTA = 5;
static const int SENSOR_SAMPLES = 12;

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
WiFiClientSecure secureClient;

char lastLine1[LCD_COLS + 1] = "";
char lastLine2[LCD_COLS + 1] = "";

int filteredRaw = AIR_RAW;
int moisturePct = 0;
int previousRaw = -1;
bool sensorFault = false;
bool pumpRunning = false;
bool lcdNeedsRefresh = true;
bool wifiConnected = false;
bool showStartupIp = false;
unsigned long lastMeaningfulChangeMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastRemotePushMs = 0;
unsigned long lastLcdUpdateMs = 0;
unsigned long burstStartMs = 0;
unsigned long currentBurstMs = 0;
unsigned long startupIpUntilMs = 0;
String deviceIp = "offline";

void relayOn()
{
  digitalWrite(RELAY_PIN, LOW);
}

void relayOff()
{
  digitalWrite(RELAY_PIN, HIGH);
}

void relayInit()
{
  pinMode(RELAY_PIN, OUTPUT_OPEN_DRAIN);
  relayOff();
}

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

int readFilteredRaw()
{
  long total = 0;
  for (int index = 0; index < SENSOR_SAMPLES; ++index)
  {
    total += analogRead(SENSOR_PIN);
    delay(4);
  }
  return total / SENSOR_SAMPLES;
}

int rawToPercent(int raw)
{
  raw = constrain(raw, WET_RAW, AIR_RAW);
  return map(raw, AIR_RAW, WET_RAW, 0, 100);
}

unsigned long getBurstDurationMs(int pct)
{
  if (pct >= TARGET_LOW)
  {
    return 0;
  }

  if (pct <= 40)
  {
    return map(pct, 0, 40, 15000, 5000);
  }

  return map(pct, 41, 49, 4500, 3000);
}

void updateSensorState()
{
  filteredRaw = readFilteredRaw();
  moisturePct = rawToPercent(filteredRaw);

  if (previousRaw < 0 || abs(filteredRaw - previousRaw) > CHANGE_DELTA)
  {
    lastMeaningfulChangeMs = millis();
    sensorFault = false;
  }

  if (millis() - lastMeaningfulChangeMs > STALE_MS)
  {
    sensorFault = true;
  }

  previousRaw = filteredRaw;
}

void stopPump()
{
  bool wasRunning = pumpRunning;
  pumpRunning = false;
  currentBurstMs = 0;
  relayOff();

  if (wasRunning)
  {
    delay(80);
    restoreLcdAfterPump();
  }

  lcdNeedsRefresh = true;
  lastLcdUpdateMs = 0;
}

void startPumpBurst(unsigned long burstMs)
{
  if (burstMs == 0)
  {
    stopPump();
    return;
  }

  pumpRunning = true;
  currentBurstMs = burstMs;
  burstStartMs = millis();
  relayOn();
  lcdNeedsRefresh = true;
}

void controlPump()
{
  if (sensorFault)
  {
    stopPump();
    return;
  }

  if (moisturePct >= TARGET_LOW)
  {
    stopPump();
    return;
  }

  if (!pumpRunning)
  {
    startPumpBurst(getBurstDurationMs(moisturePct));
    return;
  }

  if (millis() - burstStartMs >= currentBurstMs)
  {
    stopPump();
  }
}

String getStatusText()
{
  if (sensorFault)
  {
    return "Sensor Fault";
  }

  if (pumpRunning)
  {
    return "Motor ON";
  }

  if (moisturePct > TARGET_HIGH)
  {
    return "Wet";
  }

  if (moisturePct >= TARGET_LOW)
  {
    return "Monitoring";
  }

  return "Need Water";
}

String getSensorHealthText()
{
  if (sensorFault)
  {
    return "Sensor Fault";
  }

  return "Ideal";
}

unsigned long getLastChangeSeconds()
{
  return (millis() - lastMeaningfulChangeMs) / 1000;
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

  if (pumpRunning)
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

  snprintf(line1, sizeof(line1), "Moisture:%3d%%", moisturePct);

  if (sensorFault)
  {
    snprintf(line2, sizeof(line2), "Sensor Fault");
  }
  else if (pumpRunning)
  {
    snprintf(line2, sizeof(line2), "Motor ON");
  }
  else if (moisturePct > TARGET_HIGH)
  {
    snprintf(line2, sizeof(line2), "Status: WET");
  }
  else if (moisturePct >= TARGET_LOW)
  {
    snprintf(line2, sizeof(line2), "Status: IDEAL");
  }
  else
  {
    snprintf(line2, sizeof(line2), "Need Water");
  }

  writeLine(0, line1);
  writeLine(1, line2);
  lcdNeedsRefresh = false;
}

void connectWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

String buildRemoteJson()
{
  String json = "{";
  json += "\"deviceName\":\"plantMonitor\",";
  json += "\"moisture\":" + String(moisturePct) + ",";
  json += "\"raw\":" + String(filteredRaw) + ",";
  json += "\"pump\":" + String(pumpRunning ? "true" : "false") + ",";
  json += "\"fault\":" + String(sensorFault ? "true" : "false") + ",";
  json += "\"sensorHealth\":\"" + getSensorHealthText() + "\",";
  json += "\"status\":\"" + getStatusText() + "\",";
  json += "\"targetLow\":" + String(TARGET_LOW) + ",";
  json += "\"targetHigh\":" + String(TARGET_HIGH) + ",";
  json += "\"targetRange\":\"" + String(TARGET_LOW) + "-" + String(TARGET_HIGH) + "%\",";
  json += "\"lastChangeSec\":" + String(getLastChangeSeconds()) + ",";
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

void setup()
{
  Serial.begin(115200);
  delay(200);

  analogReadResolution(12);
  pinMode(SENSOR_PIN, INPUT);
  relayInit();
  safeLcdInit();

  lastMeaningfulChangeMs = millis();
  updateSensorState();
  connectWifi();
  refreshDisplay(true);
}

void loop()
{
  if (millis() - lastSampleMs >= SENSOR_SAMPLE_MS)
  {
    lastSampleMs = millis();
    updateSensorState();
    controlPump();

    Serial.print("RAW=");
    Serial.print(filteredRaw);
    Serial.print(" Moisture=");
    Serial.print(moisturePct);
    Serial.print("% Pump=");
    Serial.print(pumpRunning ? "ON" : "OFF");
    Serial.print(" Fault=");
    Serial.print(sensorFault ? "YES" : "NO");
    Serial.print(" IP=");
    Serial.println(deviceIp);
  }

  if (wifiConnected && millis() - lastRemotePushMs >= REMOTE_PUSH_MS)
  {
    lastRemotePushMs = millis();
    sendRemoteStatus();
  }

  refreshDisplay(false);
  delay(LOOP_DELAY_MS);
}
