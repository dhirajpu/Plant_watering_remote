# V3 Customer Registration, Login and QR Claiming

## What is implemented

V3 now has a customer-facing account layer on top of the existing V3 dashboard:

1. Email/password customer registration using Firebase Authentication REST API.
2. Customer login and persistent browser session.
3. Automatic Firebase ID-token refresh using the refresh token.
4. Customer profile stored at:
   `/plantMonitor/v3/users/{uid}`
5. Device ownership stored at:
   `/plantMonitor/v3/deviceOwners/{deviceId}`
6. Customer device list stored at:
   `/plantMonitor/v3/users/{uid}/devices/{deviceId}`
7. QR generator at `v3/qr.html`.
8. QR opens `v3/index.html?device=PLANT-XXXXXX`.
9. First authenticated customer can claim an unclaimed V3 Device ID.
10. Once claimed, the V3 dashboard automatically points at that device's Firebase path.
11. Existing V3 control-password protection remains in the controller firmware.
12. V2 files remain unchanged.

Firebase Authentication REST sign-up/sign-in is supported by Firebase and returns an ID token that can be used for authenticated Realtime Database REST requests. See the Firebase documentation linked in the project notes.

## Firebase setup for this test

In Firebase Console:

### 1. Enable Email/Password authentication

Authentication -> Sign-in method -> Email/Password -> Enable.

### 2. Add the Web API key

Firebase Console -> Project settings -> Your apps -> Web API Key.

Put it in:

`v3/config.js`

Replace:

`REPLACE_WITH_FIREBASE_WEB_API_KEY`

Do not put a Firebase service-account JSON, database secret, or private key into the web application.

### 3. Keep V3 device data working

The current V3 ESP32 firmware writes directly to Realtime Database. Do not deploy restrictive device-write rules yet unless the firmware is first upgraded to use a proper device authentication mechanism.

For this development milestone, customer account/claim data is implemented in the same V3 database. The existing device path remains compatible with the current firmware.

## Test flow

1. Flash `Plant_Watering_Smart_V3.ino`.
2. Confirm the ESP32 reports a Device ID such as `PLANT-A1B2C3`.
3. Confirm V3 data is appearing under:
   `/plantMonitor/v3/devices/PLANT-A1B2C3`
4. Deploy/open the V3 web application.
5. Open `v3/qr.html`.
6. Enter the Device ID and generate the QR.
7. Scan the QR with a phone.
8. Register a new customer.
9. The scanned device is claimed automatically if it is unclaimed.
10. The dashboard loads the device telemetry.
11. Sign out and sign in again; the claimed device should remain associated with the account.
12. Open the same customer account from another browser/device; the claimed device should appear there.

## Important security limitation of this milestone

The QR currently contains the Device ID itself. That is suitable for development/testing but is **not the final commercial enrollment design**.

For production, the QR should contain a one-time enrollment token that is:
- generated per physical device,
- stored securely,
- single-use or short-lived,
- validated by a trusted backend,
- never exposed as the permanent device secret.

The production design should also authenticate the ESP32 itself before allowing it to write device telemetry/commands. Firebase Security Rules should then enforce customer ownership rather than relying on browser code. Firebase documents that Realtime Database Rules can use authenticated `auth.uid` to restrict user data access.

Do not use the current Device-ID-only claiming flow for real customer sales until that enrollment layer is replaced.
