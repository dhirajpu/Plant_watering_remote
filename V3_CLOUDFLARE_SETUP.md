# Plant Life Care V3 — Cloudflare Deployment

V3 is migrated from Firebase to Cloudflare Pages Functions + one Cloudflare D1 database.

## Production architecture

- Web app: `/v3/` on Cloudflare Pages.
- API/Workers runtime: Cloudflare Pages Functions under `/functions/api/[[path]].js`.
- Database: one D1 database named `plantCareMainDB`.
- Database schema: `migrations/0001_initial_schema.sql`.
- Customers, devices, ownership, enrollment tokens, telemetry, watering history, commands, security state and audit logs are stored in the same D1 database.
- Each ESP32 is a device row identified by `ESPBOARD-XXXXXX`; devices are never separate databases.

## Cloudflare setup

1. Create exactly one D1 database named `plantCareMainDB`.
2. In the Pages project, go to **Settings → Bindings → Add → D1 database**, set variable name to `DB`, select `plantCareMainDB`, and redeploy. Cloudflare documents D1 bindings for Pages Functions here: https://developers.cloudflare.com/pages/functions/bindings/
3. Add a production secret named `FACTORY_ENROLLMENT_KEY`. Do not put the real value in GitHub.
4. Apply the migration once to the remote database:
   `npx wrangler d1 migrations apply plantCareMainDB --remote`
5. In Pages build settings use **Build command:** `exit 0` and **Build output directory:** `v3`. The Functions directory remains at repository root (`/functions`).
6. Set the Pages production branch to the branch you use for production (normally `main`).
7. Deploy the Pages project. The public V3 application will be at the Pages site root, for example `https://<project>.pages.dev/`, and the factory QR page will be `/qr.html`.

## Secure QR flow

Factory operator:
1. Flash V3 firmware.
2. Connect the ESP32 to Wi-Fi once so it registers its `deviceId` and random NVS `deviceSecret` with the API.
3. Factory QR portal sends `deviceId + factory key` to `/api/enrollment/create`.
4. API stores only a SHA-256 hash of the random one-time token and returns the raw token.
5. QR contains the device ID and 15-minute token.
6. Customer scans QR, registers/logs in, then calls `/api/enrollment/claim`.
7. API atomically consumes the token and assigns the device to that customer.

## Security

- Customer passwords are PBKDF2-SHA-256 hashed with a per-account random salt; plaintext passwords are never stored.
- Customer sessions are opaque random tokens; only their SHA-256 hashes are stored.
- Device API requests use the ESP32's per-device random secret; only its hash is stored in D1.
- Enrollment tokens are random, short-lived and one-time; only their SHA-256 hashes are stored.
- Customer access is checked against `devices.owner_customer_id`.
- Customer code cannot directly access D1.
- V2 firmware is untouched.

## Important

The V3 firmware in this branch uses the Cloudflare API. Before flashing a device, replace `REPLACE_WITH_CLOUDFLARE_PAGES_DOMAIN` in `Plant_Watering_Smart_V3.ino` with the final Pages domain. Until the firmware migration is flashed and tested, do not treat the Cloudflare backend as production-ready.
