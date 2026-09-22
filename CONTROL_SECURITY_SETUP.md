# V2 Control Security Setup

The V2 dashboard is view-only by default. Actions that can change the ESP32 state require a control-password session.

## Protected actions

- Settings open/save
- AUTO / MANUAL / DISABLED mode changes
- Water Now
- Emergency Stop / Resume
- Clear Fault
- Dry/Wet calibration
- Firmware OTA page
- Change Control Password
- Diagnostics view

History, moisture/status, connectivity and other read-only monitoring remain available without authentication.

## Password storage

The ESP32 stores only a salted SHA-256 password hash in NVS Preferences. The plaintext password is not stored in Firebase or in the dashboard JavaScript. ESP32 Preferences survives restart and power loss.

On a controller whose security NVS has never been initialized, the firmware uses the compile-time `CONTROL_DEFAULT_PASSWORD` value once to create the stored hash. **Change this constant before the first production flash.** After the hash exists, changing the constant in a later firmware build does not overwrite the existing password.

## Authentication flow

1. Dashboard asks for the control password.
2. Dashboard requests a one-time challenge through Firebase.
3. ESP32 generates a random server nonce.
4. Dashboard computes the salted password hash and a challenge proof with SHA-256.
5. ESP32 verifies the proof.
6. ESP32 creates a five-minute in-memory control session token.
7. Protected commands include that token.
8. ESP32 rejects protected commands without a valid session token.

The dashboard keeps the session token in browser `sessionStorage`, so closing the tab ends the browser-side session. The ESP32 token itself is held only in RAM.

## Important Firebase requirement

This feature is an application/firmware control layer. It does **not** replace Firebase Realtime Database Security Rules. Before exposing the Firebase database publicly, configure rules so unauthorized clients cannot freely read/write the command, security, configuration or status paths. The ESP32 should also use Firebase Authentication or another authenticated database credential in production.

## First-time use

1. Edit `CONTROL_DEFAULT_PASSWORD` in `Plant_Watering_Smart_V2_OTA.ino`.
2. Flash the firmware.
3. Open the V2 dashboard.
4. Click a protected action and enter that password.
5. Open Diagnostics and use **Change Control Password** to set the permanent password.
6. Use **Lock Controls** whenever you want to lock the dashboard immediately.

## Existing V2 behavior preserved

The security layer does not change the automatic watering algorithm, sensor fault handling, heartbeat/offline detection, NTP timestamps, watering history, Firebase telemetry/history, or OTA update mechanism. OTA itself remains protected by its existing OTA username/password in addition to the dashboard control-password gate.
