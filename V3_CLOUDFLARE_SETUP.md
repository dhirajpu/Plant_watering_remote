<!-- Cloudflare deployment verification marker: keeps Workers Builds aligned with the V3 migration branch. -->
# Plant Life Care V3 — Cloudflare Deployment

V3 is deployed as one Cloudflare Worker application with static assets, Worker API routes, and one Cloudflare D1 database. This matches the current Cloudflare Workers Builds application connected to GitHub.

## Production architecture

- Web app: `/v3/` is served as the Worker static asset bundle.
- API runtime: `/api/*` is handled by `worker.js`, which adapts the existing API handler in `/functions/api/[[path]].js`.
- Database: one D1 database named `plantcaremaindb`.
- Database binding: `DB`.
- Static asset binding: `ASSETS`.
- Database schema: `migrations/0001_initial_schema.sql`.
- Customers, devices, ownership, enrollment tokens, telemetry, watering history, commands, security state and audit logs are stored in the same D1 database.
- Each ESP32 is a device row identified by `ESPBOARD-XXXXXX`; devices are never separate databases.

## Cloudflare setup

1. Create exactly one D1 database named `plantcaremaindb`.
2. The repository `wrangler.jsonc` defines the production D1 binding as `DB` using the production database ID.
3. Add a production secret named `FACTORY_ENROLLMENT_KEY`. Do not put the real value in GitHub.
4. Apply the migration once to the remote database:
   `npx wrangler d1 migrations apply plantcaremaindb --remote`
5. In the connected Workers Builds application use:
   - **Root directory:** `/`
   - **Build command:** leave blank (or `exit 0` if the UI requires a command).
   - **Deploy command:** `npx wrangler deploy`
   - **Version command:** leave blank unless your Cloudflare setup specifically requires version uploads.
   - **Production branch:** the branch you use for production (normally `main`).
6. The Worker serves the V3 frontend from `/v3` and routes `/api/*` to the API handler.
7. The public application is available at the Worker hostname configured in Cloudflare.

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

The V3 firmware in this branch uses the Cloudflare API. Before flashing a device, replace `REPLACE_WITH_CLOUDFLARE_PAGES_DOMAIN` in `Plant_Watering_Smart_V3.ino` with the final production Worker domain. The V3 firmware and physical ESP32 flow still require end-to-end testing before treating the system as production-ready.
