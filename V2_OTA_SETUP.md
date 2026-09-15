# Smart Watering V2 OTA Setup

## Firmware files

- `multi sensor code.ino` — original deployed fallback. Do not modify.
- `Plant_Watering_Smart_V2.ino` — first V2 candidate kept as fallback.
- `Plant_Watering_Smart_V2_OTA.ino` — hardened V2 candidate with OTA and safety fixes.

## First installation

1. Open `Plant_Watering_Smart_V2_OTA.ino` in Arduino IDE.
2. Select the correct ESP32 board and an OTA-capable partition scheme.
3. Change `WIFI_SSID` and `WIFI_PASSWORD`.
4. Change `OTA_PASSWORD` from `CHANGE_ME_OTA_PASSWORD`.
5. Configure Firebase Authentication/Rules before using remote pump controls in production.
6. Flash the firmware by USB the first time.
7. Open Serial Monitor at 115200 baud and confirm the ESP32 connects to Wi-Fi.

## Wireless update from the dashboard

The V2 dashboard Diagnostics page shows the controller IP and an **Open Firmware Update** button when the OTA firmware is online.

The browser/device running the dashboard must be on the same local Wi-Fi network as the ESP32.

1. Build/export the new ESP32 firmware as a `.bin` file.
2. Open Diagnostics in the dashboard.
3. Select **Open Firmware Update**.
4. Authenticate with the OTA username/password configured in the firmware.
5. Upload the `.bin` file.
6. The controller immediately forces the pump and all valves OFF.
7. The update is written and the ESP32 reboots automatically.
8. After reboot the controller performs the normal safe boot, sensor validation and watering grace period.

## Arduino IDE network upload

`ArduinoOTA` is also enabled. After the first USB installation and Wi-Fi connection, the ESP32 can appear as a network upload target in Arduino IDE. The same OTA password is required.

## Safety behavior during OTA

- Automatic watering is disabled while an OTA update is active.
- Pump is forced OFF.
- Every valve is forced OFF.
- A failed upload leaves the outputs OFF until the controller is restarted.
- After a successful upload the ESP32 reboots and repeats the safe boot sequence.

## Fallback

Keep USB access available. OTA cannot recover a completely unbootable device or a bad partition layout. If OTA is unavailable, flash the firmware by USB.

## Required production hardening before remote use

- Replace placeholder Wi-Fi credentials without committing secrets to a public repository.
- Use Firebase Authentication and restrictive Realtime Database Rules.
- Replace `WiFiClientSecure::setInsecure()` with proper certificate validation.
- Use a strong unique OTA password.
- Calibrate flow sensor pulses/liter if flow hardware is enabled.
- Test emergency stop, power return, tank-low protection, no-flow protection and sensor-fault behavior with the actual relay/pump hardware before unattended operation.
