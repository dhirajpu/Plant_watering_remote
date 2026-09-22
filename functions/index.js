const crypto = require("crypto");
const { initializeApp } = require("firebase-admin/app");
const { getDatabase } = require("firebase-admin/database");
const { onCall, HttpsError } = require("firebase-functions/v2/https");
const { defineSecret } = require("firebase-functions/params");

initializeApp();

const db = getDatabase();
const enrollmentAdminKey = defineSecret("PLANT_V3_ENROLLMENT_ADMIN_KEY");
const DEVICE_RE = /^ESPBOARD-[A-F0-9]{6}$/;
const TOKEN_BYTES = 32;
const TOKEN_TTL_MS = 15 * 60 * 1000;

function normalizeDeviceId(value) {
  return String(value || "").trim().toUpperCase();
}

function hashToken(token) {
  return crypto.createHash("sha256").update(token, "utf8").digest("hex");
}

function randomToken() {
  return crypto.randomBytes(TOKEN_BYTES).toString("base64url");
}

function requireCustomer(request) {
  if (!request.auth || !request.auth.uid) {
    throw new HttpsError("unauthenticated", "Sign in before claiming a device.");
  }
  return request.auth.uid;
}

exports.createEnrollmentToken = onCall(
  { region: "asia-southeast1", secrets: [enrollmentAdminKey], enforceAppCheck: false },
  async (request) => {
    const deviceId = normalizeDeviceId(request.data?.deviceId);
    const adminKey = String(request.data?.adminKey || "");

    if (!DEVICE_RE.test(deviceId)) {
      throw new HttpsError("invalid-argument", "Invalid Device ID. Expected ESPBOARD-XXXXXX.");
    }
    if (!adminKey || adminKey !== enrollmentAdminKey.value()) {
      throw new HttpsError("permission-denied", "Invalid factory enrollment key.");
    }

    const ownerRef = db.ref(`plantMonitor/v3/deviceOwners/${deviceId}`);
    const ownerSnap = await ownerRef.once("value");
    if (ownerSnap.exists() && ownerSnap.val()) {
      throw new HttpsError("already-exists", "This device is already claimed.");
    }

    const token = randomToken();
    const now = Date.now();
    const record = {
      tokenHash: hashToken(token),
      issuedAt: now,
      expiresAt: now + TOKEN_TTL_MS,
      usedAt: null,
      usedBy: null,
      status: "issued"
    };

    await db.ref(`plantMonitor/v3/enrollmentTokens/${deviceId}`).set(record);

    return {
      deviceId,
      token,
      expiresAt: record.expiresAt,
      expiresInSeconds: TOKEN_TTL_MS / 1000
    };
  }
);

exports.claimEnrollmentToken = onCall(
  { region: "asia-southeast1", enforceAppCheck: false },
  async (request) => {
    const uid = requireCustomer(request);
    const deviceId = normalizeDeviceId(request.data?.deviceId);
    const token = String(request.data?.token || "").trim();

    if (!DEVICE_RE.test(deviceId) || token.length < 32 || token.length > 128) {
      throw new HttpsError("invalid-argument", "Invalid enrollment data.");
    }

    const enrollmentRef = db.ref(`plantMonitor/v3/enrollmentTokens/${deviceId}`);
    let claimState = { ok: false, alreadyClaimedByCaller: false };

    const tx = await enrollmentRef.transaction((current) => {
      if (!current) return;
      if (current.usedBy === uid) {
        claimState = { ok: true, alreadyClaimedByCaller: true };
        return current;
      }
      if (current.usedAt || current.status === "claimed") return;
      if (!current.expiresAt || Number(current.expiresAt) < Date.now()) return;
      if (current.tokenHash !== hashToken(token)) return;

      claimState = { ok: true, alreadyClaimedByCaller: false };
      return {
        ...current,
        status: "claimed",
        usedAt: Date.now(),
        usedBy: uid
      };
    });

    if (!tx.committed || !claimState.ok) {
      throw new HttpsError("permission-denied", "Invalid, expired, or already-used enrollment token.");
    }

    const ownerRef = db.ref(`plantMonitor/v3/deviceOwners/${deviceId}`);
    const ownerSnap = await ownerRef.once("value");
    const existingOwner = ownerSnap.val();
    if (existingOwner && existingOwner !== uid) {
      throw new HttpsError("already-exists", "This device is already claimed by another customer.");
    }

    const nowIso = new Date().toISOString();
    const updates = {};
    updates[`plantMonitor/v3/deviceOwners/${deviceId}`] = uid;
    updates[`plantMonitor/v3/users/${uid}/devices/${deviceId}`] = {
      deviceId,
      claimedAt: nowIso,
      source: "secure-enrollment-qr"
    };
    updates[`plantMonitor/v3/devices/${deviceId}/registration`] = {
      ownerUid: uid,
      claimedAt: nowIso,
      enrollment: "one-time-token"
    };

    await db.ref().update(updates);

    return {
      deviceId,
      claimed: true,
      alreadyClaimedByCaller: claimState.alreadyClaimedByCaller
    };
  }
);
