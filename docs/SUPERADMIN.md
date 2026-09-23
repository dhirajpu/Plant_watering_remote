# Super Admin V3

## URL

Use:

`/superadmin`

The Worker internally serves `v3/superadmin.html`; the HTML filename is not part of the advertised URL.

## First-time setup

The Super Admin login is backed by D1 and uses PBKDF2-SHA-256. The first administrator is bootstrapped only when these server-side Wrangler variables are configured:

- `SUPERADMIN_EMAIL`
- `SUPERADMIN_PASSWORD`

Set them as Worker secrets/variables; never put them in `v3/config.js` or any browser code.

After the first successful login, the admin account is stored in `super_admins`. Remove `SUPERADMIN_PASSWORD` from the Worker environment after bootstrap so the bootstrap credential is no longer available to the application.

## Database

Migration `migrations/0002_superadmin.sql` adds:

- `super_admins`
- `super_admin_sessions`
- `support_sessions`

This repository already had the original V3 schema applied manually, so do not blindly rerun migration 0001 against the existing production database. Apply the 0002 SQL once to the existing D1 database (or register/apply it through your normal migration workflow after reconciling the existing migration history).

## Dashboard

The authenticated dashboard provides:

1. Dashboard statistics
2. Register Device
3. Generate one-time Enrollment QR
4. Devices / device health
5. Customer / Ownership
6. Remote Support
7. Maintenance
8. Audit Logs

### Remote Support security

Remote support is application-level controller support. It does **not** provide access to Windows, macOS, the customer's browser, printer, or other applications.

A support session expires after 30 minutes. Controller commands are rejected unless the requesting Super Admin has an active support session for that exact device. Support commands and session start/end events are written to the audit log.

### Device enrollment

Enrollment QR tokens remain one-time and expire after 15 minutes. Super Admin QR generation does not expose the factory enrollment key to the browser.

### Session security

Customer sessions remain separate from Super Admin sessions. Super Admin session tokens are opaque random tokens; only SHA-256 token hashes are stored in D1.

## Production checklist

- Apply `0002_superadmin.sql` to `plantcaremaindb`.
- Configure `SUPERADMIN_EMAIL` and `SUPERADMIN_PASSWORD` on the Worker.
- Log in once at `/superadmin`.
- Remove `SUPERADMIN_PASSWORD` after bootstrap.
- Confirm the dashboard loads Devices, Customers and Audit Logs.
- Confirm an active Remote Support session is required before sending a controller command.
- Test device registration/enrollment with a real V3 ESP32 before treating the flow as production-ready.
