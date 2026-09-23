# V3 Secure One-Time Enrollment

This milestone replaces the development QR flow that exposed only the Device ID.

## New flow

1. Flash `Plant_Watering_Smart_V3.ino` to the physical ESP32.
2. The controller reports a stable Device ID such as `ESPBOARD-A1B2C3`.
3. An authorized factory/operator opens `v3/qr.html`.
4. The operator enters:
   - Device ID
   - Factory enrollment key
5. The Firebase Cloud Function generates a cryptographically random enrollment token.
6. Only the SHA-256 hash of the token is stored in Realtime Database.
7. The QR contains the device ID plus the raw one-time token.
8. The token expires after 15 minutes or is consumed on the first successful claim.
9. The customer scans the QR and registers/signs in.
10. The customer app sends the token to the trusted Cloud Function.
11. The Cloud Function atomically marks the token as claimed and records the customer/device ownership.
12. The dashboard opens using the customer's authenticated Firebase session.

Firebase callable functions automatically validate Firebase Authentication ID tokens for authenticated calls, and Firebase Admin SDK code runs in a privileged server environment rather than in the browser. See the Firebase callable-functions and Admin SDK documentation.

## Database records

Enrollment records:

`/plantMonitor/v3/enrollmentTokens/{deviceId}`

Example:

```json
{
  "tokenHash": "<sha256>",
  "issuedAt": 1760000000000,
  "expiresAt": 1760000900000,
  "status": "issued",
  "usedAt": null,
  "usedBy": null
}
```

After successful claim:

- `status` becomes `claimed`
- `usedAt` is set
- `usedBy` is the customer Firebase UID

Customer ownership remains:

- `/plantMonitor/v3/deviceOwners/{deviceId}`
- `/plantMonitor/v3/users/{uid}/devices/{deviceId}`

## Cloud Functions

The repository now contains:

- `functions/index.js`
  - `createEnrollmentToken`
  - `claimEnrollmentToken`
- `functions/package.json`
- `functions/.gitignore`

The functions use Node.js 20 and Firebase Admin SDK.

## Factory enrollment key

The factory enrollment key is deliberately not stored in the web application.

Set it in Firebase Secret Manager:

```bash
firebase functions:secrets:set PLANT_V3_ENROLLMENT_ADMIN_KEY
```

Then deploy:

```bash
firebase deploy --only functions
```

Do not put the key in `v3/config.js`, GitHub, QR data, or customer documentation.

Firebase documents secret parameters for Cloud Functions and recommends binding secrets only to the functions that need them.

## Function region

The functions are configured for `asia-southeast1`.

The current web configuration points to:

`https://asia-southeast1-vernal-catfish-196407.cloudfunctions.net`

If the Firebase project/region changes, update `v3/config.js`.

## Important production security requirement

The one-time QR token protects the customer enrollment flow, but the existing ESP32-to-Realtime-Database path still uses the legacy V3 device write model. The device currently has a generated `deviceSecret`, but it is not yet used to authenticate every Firebase write.

Before commercial production, Firebase Security Rules and device authentication must be hardened so that:

- customers can only access devices they own;
- devices can only write their own telemetry/status;
- customers cannot create or modify enrollment records;
- only trusted backend functions can create/consume enrollment records;
- permanent device secrets are never exposed to browser code.

Firebase Security Rules are enforced server-side and should be used together with Firebase Authentication. The Admin SDK is intended for trusted server environments and can bypass client Security Rules with administrative privileges.

## Test procedure

### 1. Deploy functions

From the repository root:

```bash
firebase functions:secrets:set PLANT_V3_ENROLLMENT_ADMIN_KEY
firebase deploy --only functions
```

### 2. Deploy/open V3 web

Make sure `v3/config.js` contains the correct Firebase Web API key and functions URL.

### 3. Flash ESP32

Open Arduino Serial Monitor at 115200 baud.

Expected:

```
Device ID: ESPBOARD-A1B2C3
```

### 4. Generate QR

Open:

`v3/qr.html`

Enter:

```
Device ID: ESPBOARD-A1B2C3
Factory enrollment key: <your secret>
```

Click **Generate Secure QR** and print the QR.

### 5. Customer claim

Scan the QR from a phone.

The customer:

- registers/logs in;
- gets the device ID from the QR;
- submits the one-time enrollment token automatically;
- receives ownership;
- enters the V3 dashboard.

### 6. Replay test

Scan the same QR again from a different customer account.

The second claim must fail because the enrollment token has already been consumed.

### 7. Expiry test

Generate a QR, wait until its 15-minute lifetime expires, and verify that claiming it fails.

## Development vs production

The old Device-ID-only QR is no longer the active V3 QR flow on this branch.

The new QR contains:

`index.html?device=ESPBOARD-XXXXXX&enroll=<one-time-token>`

The raw token is intentionally short-lived and single-use. It is not the permanent device secret.

