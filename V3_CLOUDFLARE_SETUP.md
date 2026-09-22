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
2. Copy the generated D1 database ID into `wrangler.jsonc` (replace the placeholder).
3. In the Pages project, go to **Settings → Bindings → Add → D1 database**, set variable name to `DB`, select `plantCareMainDB`, and redeploy. Cloudflare documents D1 bindings for Pages Functions here: https://developers.cloudflare.com/pages/functions/bindings/
4. Add a production secret/environment variable named `FACTORY_ENROLLMENT_KEY`. Do not put the real value in GitHub. The placeholder in `wrangler.jsonc` is not a production secret.
5. Apply the migration once to the remote database:
   `npx wrangler d1 migrations apply plantCareMainDB --remote`
6. Connect the GitHub repository to Pages. Use the repository root as the Pages output directory because the V3 static site is in `/v3` and the Pages Function is in `/functions`.
7. Deploy the Pages project. The public app will be `/v3/`.

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

The current V3 firmware is being migrated from direct Firebase REST calls to the Cloudflare API in this branch. Until the firmware migration is flashed and tested, do not treat the Cloudflare backend as production-ready.
