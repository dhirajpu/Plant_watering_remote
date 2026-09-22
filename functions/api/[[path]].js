const DEVICE_RE = /^ESPBOARD-[A-F0-9]{6}$/;
const TOKEN_TTL_MS = 15 * 60 * 1000;
const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000;
const PBKDF2_ITERATIONS = 100000;

function json(data, status = 200, extra = {}) {
  return Response.json(data, { status, headers: { "Cache-Control": "no-store", ...extra } });
}
function fail(message, status = 400) { return json({ error: message }, status); }
function now() { return Date.now(); }
function normalizeDeviceId(v) { return String(v || "").trim().toUpperCase(); }
function normalizeEmail(v) { return String(v || "").trim().toLowerCase(); }
function bytesToHex(bytes) { return Array.from(bytes).map(b => b.toString(16).padStart(2, "0")).join(""); }
function bytesToBase64Url(bytes) {
  let s = ""; for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}
function randomToken(bytes = 32) { const b = new Uint8Array(bytes); crypto.getRandomValues(b); return bytesToBase64Url(b); }
async function sha256Hex(value) {
  const data = new TextEncoder().encode(String(value));
  const digest = await crypto.subtle.digest("SHA-256", data);
  return bytesToHex(new Uint8Array(digest));
}
async function pbkdf2(password, saltHex) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", salt: new TextEncoder().encode(saltHex), iterations: PBKDF2_ITERATIONS, hash: "SHA-256" },
    key, 256
  );
  return bytesToHex(new Uint8Array(bits));
}
function bearer(request) {
  const h = request.headers.get("Authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7).trim() : "";
}
async function customerFromSession(request, env) {
  const token = bearer(request);
  if (!token) return null;
  const hash = await sha256Hex(token);
  const row = await env.DB.prepare(
    "SELECT c.id,c.email,c.name,c.disabled FROM sessions s JOIN customers c ON c.id=s.customer_id WHERE s.token_hash=? AND s.expires_at>? LIMIT 1"
  ).bind(hash, now()).first();
  if (!row || row.disabled) return null;
  await env.DB.prepare("UPDATE sessions SET last_seen_at=? WHERE token_hash=?").bind(now(), hash).run();
  return row;
}
async function deviceFromSecret(deviceId, request, env) {
  const secret = bearer(request);
  if (!secret || !DEVICE_RE.test(deviceId)) return null;
  const hash = await sha256Hex(secret);
  return env.DB.prepare("SELECT * FROM devices WHERE device_id=? AND device_secret_hash=? AND disabled=0 LIMIT 1")
    .bind(deviceId, hash).first();
}
async function requireOwner(deviceId, request, env) {
  const customer = await customerFromSession(request, env);
  if (!customer) return { error: fail("Authentication required.", 401) };
  const device = await env.DB.prepare("SELECT * FROM devices WHERE device_id=? AND owner_customer_id=? AND disabled=0 LIMIT 1")
    .bind(deviceId, customer.id).first();
  if (!device) return { error: fail("You do not have access to this device.", 403) };
  return { customer, device };
}
async function audit(env, customerId, deviceId, action, details = {}) {
  await env.DB.prepare("INSERT INTO audit_logs(customer_id,device_id,action,details_json,created_at) VALUES(?,?,?,?,?)")
    .bind(customerId || null, deviceId || null, action, JSON.stringify(details), now()).run();
}
function parseJsonText(text, fallback = {}) { try { return text ? JSON.parse(text) : fallback; } catch { return fallback; } }
async function writeAdminAudit(env, adminId, deviceId, action, details = {}) {
  return audit(env, null, deviceId, action, { ...details, adminId });
}

const SUPER_ADMIN_SESSION_TTL_MS = 8 * 60 * 60 * 1000;
const SUPPORT_SESSION_TTL_MS = 30 * 60 * 1000;

async function superAdminFromSession(request, env) {
  const token = bearer(request);
  if (!token) return null;
  const hash = await sha256Hex(token);
  const row = await env.DB.prepare(
    "SELECT a.id,a.email,a.name,a.disabled FROM super_admin_sessions s JOIN super_admins a ON a.id=s.admin_id WHERE s.token_hash=? AND s.expires_at>? LIMIT 1"
  ).bind(hash, now()).first();
  if (!row || row.disabled) return null;
  await env.DB.prepare("UPDATE super_admin_sessions SET last_seen_at=? WHERE token_hash=?").bind(now(), hash).run();
  return row;
}

async function superAdminRequired(request, env) {
  const admin = await superAdminFromSession(request, env);
  return admin ? { admin } : { error: fail("Super Admin authentication required.", 401) };
}

async function adminLogin(body, env) {
  const email = normalizeEmail(body.email), password = String(body.password || "");
  if (!email || !password) return fail("Email and password are required.", 400);

  let row = await env.DB.prepare("SELECT * FROM super_admins WHERE email=? LIMIT 1").bind(email).first();

  // Secure bootstrap: the first admin can be created only from a server-side
  // Wrangler secret pair. Remove SUPERADMIN_PASSWORD after the first bootstrap.
  if (!row && env.SUPERADMIN_EMAIL && env.SUPERADMIN_PASSWORD && email === normalizeEmail(env.SUPERADMIN_EMAIL) && password === env.SUPERADMIN_PASSWORD) {
    const id = crypto.randomUUID(), salt = bytesToHex(crypto.getRandomValues(new Uint8Array(16)));
    const hash = await pbkdf2(password, salt), t = now();
    await env.DB.prepare("INSERT INTO super_admins(id,email,name,password_salt,password_hash,created_at,updated_at) VALUES(?,?,?,?,?,?,?)")
      .bind(id,email,"Super Admin",salt,hash,t,t).run();
    row = await env.DB.prepare("SELECT * FROM super_admins WHERE id=?").bind(id).first();
    await audit(env, id, null, "super_admin_bootstrapped", { email });
  }

  if (!row || row.disabled) return fail("Incorrect email or password.", 401);
  const hash = await pbkdf2(password, row.password_salt);
  if (hash !== row.password_hash) return fail("Incorrect email or password.", 401);

  const token = randomToken(), tokenHash = await sha256Hex(token), t = now();
  await env.DB.prepare("INSERT INTO super_admin_sessions(token_hash,admin_id,expires_at,created_at,last_seen_at) VALUES(?,?,?,?,?)")
    .bind(tokenHash,row.id,t+SUPER_ADMIN_SESSION_TTL_MS,t,t).run();
  await audit(env, row.id, null, "super_admin_login", { email: row.email });
  return json({ token, expiresAt: t + SUPER_ADMIN_SESSION_TTL_MS, admin: { id: row.id, email: row.email, name: row.name } });
}

async function adminMe(request, env) {
  const admin = await superAdminFromSession(request, env);
  if (!admin) return fail("Session expired.", 401);
  return json({ admin });
}

async function adminStats(request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const [d,c,aud,s]=await Promise.all([
    env.DB.prepare("SELECT COUNT(*) AS n, SUM(CASE WHEN owner_customer_id IS NOT NULL THEN 1 ELSE 0 END) AS claimed, SUM(CASE WHEN disabled=1 THEN 1 ELSE 0 END) AS disabled FROM devices").first(),
    env.DB.prepare("SELECT COUNT(*) AS n, SUM(CASE WHEN disabled=1 THEN 1 ELSE 0 END) AS disabled FROM customers").first(),
    env.DB.prepare("SELECT COUNT(*) AS n FROM audit_logs").first(),
    env.DB.prepare("SELECT COUNT(*) AS n FROM support_sessions WHERE status='active' AND expires_at>?").bind(now()).first()
  ]);
  return json({devices:Number(d?.n||0),claimed:Number(d?.claimed||0),disabledDevices:Number(d?.disabled||0),customers:Number(c?.n||0),disabledCustomers:Number(c?.disabled||0),auditLogs:Number(aud?.n||0),activeSupport:Number(s?.n||0)});
}

async function adminDevices(request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const rows=await env.DB.prepare("SELECT d.device_id AS deviceId,d.firmware_version AS firmware,d.last_seen_at AS lastSeen,d.created_at AS createdAt,d.updated_at AS updatedAt,d.maintenance_expires_at AS maintenanceExpires,d.disabled,d.owner_customer_id AS ownerId,c.email AS ownerEmail,c.name AS ownerName,d.status_json AS status FROM devices d LEFT JOIN customers c ON c.id=d.owner_customer_id ORDER BY d.created_at DESC").all();
  return json(rows.results.map(x=>({...x,disabled:!!x.disabled,status:parseJsonText(x.status)})));
}

async function adminCustomers(request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const rows=await env.DB.prepare("SELECT c.id,c.email,c.name,c.created_at AS createdAt,c.updated_at AS updatedAt,c.disabled,COUNT(d.device_id) AS deviceCount FROM customers c LEFT JOIN devices d ON d.owner_customer_id=c.id GROUP BY c.id ORDER BY c.created_at DESC").all();
  return json(rows.results.map(x=>({...x,disabled:!!x.disabled,deviceCount:Number(x.deviceCount||0)})));
}

async function adminAudit(request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const rows=await env.DB.prepare("SELECT a.id,a.action,a.created_at AS createdAt,a.details_json AS details,a.customer_id AS customerId,a.device_id AS deviceId,c.email AS customerEmail FROM audit_logs a LEFT JOIN customers c ON c.id=a.customer_id ORDER BY a.created_at DESC LIMIT 250").all();
  return json(rows.results.map(x=>({...x,details:parseJsonText(x.details)})));
}

async function adminRegisterDevice(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId), secret=String(body.deviceSecret||"");
  if(!DEVICE_RE.test(deviceId)||secret.length<32)return fail("Valid Device ID and a 32+ character device secret are required.");
  const hash=await sha256Hex(secret), t=now();
  const existing=await env.DB.prepare("SELECT device_id,device_secret_hash FROM devices WHERE device_id=?").bind(deviceId).first();
  if(existing && existing.device_secret_hash!==hash)return fail("Device already exists with a different secret.",409);
  if(!existing)await env.DB.prepare("INSERT INTO devices(device_id,device_secret_hash,created_at,updated_at,last_seen_at) VALUES(?,?,?,?,?)").bind(deviceId,hash,t,t,t).run();
  else await env.DB.prepare("UPDATE devices SET updated_at=? WHERE device_id=?").bind(t,deviceId).run();
  await writeAdminAudit(env,a.admin.id,deviceId,"admin_device_registered",{source:"superadmin"});
  return json({ok:true,deviceId,created:!existing},existing?200:201);
}

async function adminCreateEnrollment(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId);
  if(!DEVICE_RE.test(deviceId))return fail("Invalid Device ID. Expected ESPBOARD-XXXXXX.");
  const device=await env.DB.prepare("SELECT device_id,owner_customer_id,disabled FROM devices WHERE device_id=? LIMIT 1").bind(deviceId).first();
  if(!device)return fail("Device is not registered yet.",404);
  if(device.disabled)return fail("This device is disabled.",403);
  if(device.owner_customer_id)return fail("This device is already claimed.",409);
  const token=randomToken(32),t=now(),hash=await sha256Hex(token);
  await env.DB.prepare("INSERT INTO enrollment_tokens(device_id,token_hash,issued_at,expires_at,status) VALUES(?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET token_hash=excluded.token_hash,issued_at=excluded.issued_at,expires_at=excluded.expires_at,used_at=NULL,used_by_customer_id=NULL,status='issued'")
    .bind(deviceId,hash,t,t+TOKEN_TTL_MS,"issued").run();
  await writeAdminAudit(env,a.admin.id,deviceId,"enrollment_qr_generated",{expiresAt:t+TOKEN_TTL_MS});
  return json({deviceId,token,expiresAt:t+TOKEN_TTL_MS,expiresInSeconds:TOKEN_TTL_MS/1000});
}

async function adminSetDevice(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId);
  if(!DEVICE_RE.test(deviceId))return fail("Invalid Device ID.");
  const device=await env.DB.prepare("SELECT device_id FROM devices WHERE device_id=?").bind(deviceId).first();
  if(!device)return fail("Device not found.",404);
  const t=now();
  if(body.disabled!==undefined){
    await env.DB.prepare("UPDATE devices SET disabled=?,updated_at=? WHERE device_id=?").bind(body.disabled?1:0,t,deviceId).run();
    await writeAdminAudit(env,a.admin.id,deviceId,body.disabled?"device_disabled":"device_enabled",{});
  }
  if(body.maintenanceExpiresAt!==undefined){
    const v=body.maintenanceExpiresAt===null?null:Number(body.maintenanceExpiresAt);
    await env.DB.prepare("UPDATE devices SET maintenance_expires_at=?,updated_at=? WHERE device_id=?").bind(v,t,deviceId).run();
    await writeAdminAudit(env,a.admin.id,deviceId,"maintenance_updated",{maintenanceExpiresAt:v});
  }
  return json({ok:true});
}

async function adminAssignOwner(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId), email=normalizeEmail(body.email);
  if(!DEVICE_RE.test(deviceId))return fail("Invalid Device ID.");
  const device=await env.DB.prepare("SELECT device_id,owner_customer_id FROM devices WHERE device_id=?").bind(deviceId).first();
  if(!device)return fail("Device not found.",404);
  let customer=null;
  if(email)customer=await env.DB.prepare("SELECT id,email,name FROM customers WHERE email=? AND disabled=0 LIMIT 1").bind(email).first();
  if(email&&!customer)return fail("Customer not found or disabled.",404);
  await env.DB.prepare("UPDATE devices SET owner_customer_id=?,updated_at=? WHERE device_id=?").bind(customer?.id||null,now(),deviceId).run();
  await writeAdminAudit(env,a.admin.id,deviceId,customer?"device_owner_assigned":"device_owner_removed",{customerId:customer?.id||null,email:customer?.email||null});
  return json({ok:true,owner:customer||null});
}

async function adminCommand(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId), action=String(body.action||"").trim();
  const allowed=["emergency_stop","resume","water_now","set_mode","clear_fault","calibrate_dry","calibrate_wet","set_config"];
  if(!DEVICE_RE.test(deviceId)||!allowed.includes(action))return fail("Unsupported device command.");
  const supportId=String(body.supportSessionId||"");
  if(!supportId)return fail("Start a Remote Support session before issuing commands.",403);
  const support=await env.DB.prepare("SELECT id FROM support_sessions WHERE id=? AND device_id=? AND admin_id=? AND status='active' AND expires_at>? LIMIT 1").bind(supportId,deviceId,a.admin.id,now()).first();
  if(!support)return fail("Remote Support session is missing or expired.",403);
  const device=await env.DB.prepare("SELECT device_id,disabled,owner_customer_id FROM devices WHERE device_id=?").bind(deviceId).first();
  if(!device)return fail("Device not found.",404);
  if(device.disabled)return fail("Device is disabled.",403);
  const id=randomToken(16), t=now();
  const command={id,action,...body.extra,issuedAtEpochSec:Math.floor(t/1000),source:"superadmin",adminId:a.admin.id};
  await env.DB.prepare("INSERT INTO device_commands(device_id,command_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET command_json=excluded.command_json,updated_at=excluded.updated_at")
    .bind(deviceId,JSON.stringify(command),t).run();
  await writeAdminAudit(env,a.admin.id,deviceId,"support_command_issued",{action,commandId:id});
  return json({ok:true,id});
}

async function adminStartSupport(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const deviceId=normalizeDeviceId(body.deviceId);
  const device=await env.DB.prepare("SELECT device_id,disabled FROM devices WHERE device_id=?").bind(deviceId).first();
  if(!device)return fail("Device not found.",404);
  if(device.disabled)return fail("Device is disabled.",403);
  const id=crypto.randomUUID(),t=now(),expires=t+SUPPORT_SESSION_TTL_MS;
  await env.DB.prepare("INSERT INTO support_sessions(id,device_id,admin_id,expires_at,status,created_at) VALUES(?,?,?,?,?,?)").bind(id,deviceId,a.admin.id,expires,"active",t).run();
  await writeAdminAudit(env,a.admin.id,deviceId,"support_session_started",{supportSessionId:id,expiresAt:expires});
  return json({id,deviceId,expiresAt:expires});
}

async function adminEndSupport(body, request, env) {
  const a=await superAdminRequired(request,env); if(a.error)return a.error;
  const id=String(body.id||""); if(!id)return fail("Support session id required.");
  await env.DB.prepare("UPDATE support_sessions SET status='ended',ended_at=? WHERE id=? AND admin_id=? AND status='active'").bind(now(),id,a.admin.id).run();
  await writeAdminAudit(env,a.admin.id,null,"support_session_ended",{supportSessionId:id});
  return json({ok:true});
}


async function authRegister(body, env) {
  const email = normalizeEmail(body.email), password = String(body.password || ""), name = String(body.name || "").trim();
  if (!email || !email.includes("@") || password.length < 8 || name.length < 2) return fail("Name, valid email and an 8+ character password are required.");
  const existing = await env.DB.prepare("SELECT id FROM customers WHERE email=? LIMIT 1").bind(email).first();
  if (existing) return fail("Email is already registered.", 409);
  const id = crypto.randomUUID(), salt = bytesToHex(crypto.getRandomValues(new Uint8Array(16)));
  const hash = await pbkdf2(password, salt), t = now(), token = randomToken(), tokenHash = await sha256Hex(token);
  await env.DB.batch([
    env.DB.prepare("INSERT INTO customers(id,email,name,password_salt,password_hash,created_at,updated_at) VALUES(?,?,?,?,?,?,?)").bind(id,email,name,salt,hash,t,t),
    env.DB.prepare("INSERT INTO sessions(token_hash,customer_id,expires_at,created_at,last_seen_at) VALUES(?,?,?,?,?)").bind(tokenHash,id,t+SESSION_TTL_MS,t,t)
  ]);
  return json({ token, expiresAt: t + SESSION_TTL_MS, customer: { id, email, name } }, 201);
}
async function authLogin(body, env) {
  const email = normalizeEmail(body.email), password = String(body.password || "");
  const row = await env.DB.prepare("SELECT * FROM customers WHERE email=? LIMIT 1").bind(email).first();
  if (!row || row.disabled) return fail("Incorrect email or password.", 401);
  const hash = await pbkdf2(password, row.password_salt);
  if (hash !== row.password_hash) return fail("Incorrect email or password.", 401);
  const token = randomToken(), tokenHash = await sha256Hex(token), t = now();
  await env.DB.prepare("INSERT INTO sessions(token_hash,customer_id,expires_at,created_at,last_seen_at) VALUES(?,?,?,?,?)")
    .bind(tokenHash,row.id,t+SESSION_TTL_MS,t,t).run();
  return json({ token, expiresAt: t + SESSION_TTL_MS, customer: { id: row.id, email: row.email, name: row.name } });
}
async function authMe(request, env) {
  const c = await customerFromSession(request, env);
  if (!c) return fail("Session expired.", 401);
  return json({ customer: { id: c.id, email: c.email, name: c.name } });
}

async function createEnrollment(body, request, env) {
  const deviceId = normalizeDeviceId(body.deviceId);
  const supplied = String(body.factoryKey || request.headers.get("X-Factory-Key") || "");
  if (!DEVICE_RE.test(deviceId)) return fail("Invalid Device ID. Expected ESPBOARD-XXXXXX.");
  if (!env.FACTORY_ENROLLMENT_KEY || supplied !== env.FACTORY_ENROLLMENT_KEY) return fail("Invalid factory enrollment key.", 403);
  const device = await env.DB.prepare("SELECT device_id,owner_customer_id,disabled FROM devices WHERE device_id=? LIMIT 1").bind(deviceId).first();
  if (!device) return fail("Device is not registered yet. Connect the controller to Wi-Fi so it can register before generating its QR.", 404);
  if (device.disabled) return fail("This device is disabled.", 403);
  if (device.owner_customer_id) return fail("This device is already claimed.", 409);
  const token = randomToken(32), t = now(), hash = await sha256Hex(token);
  await env.DB.prepare(
    "INSERT INTO enrollment_tokens(device_id,token_hash,issued_at,expires_at,status) VALUES(?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET token_hash=excluded.token_hash,issued_at=excluded.issued_at,expires_at=excluded.expires_at,used_at=NULL,used_by_customer_id=NULL,status='issued'"
  ).bind(deviceId,hash,t,t+TOKEN_TTL_MS,"issued").run();
  return json({ deviceId, token, expiresAt: t + TOKEN_TTL_MS, expiresInSeconds: TOKEN_TTL_MS/1000 });
}
async function claimEnrollment(body, request, env) {
  const customer = await customerFromSession(request, env);
  if (!customer) return fail("Sign in before claiming a device.", 401);
  const deviceId = normalizeDeviceId(body.deviceId), token = String(body.token || "").trim();
  if (!DEVICE_RE.test(deviceId) || token.length < 32) return fail("Invalid enrollment data.");
  const tokenHash = await sha256Hex(token), t = now();
  const tIso = new Date(t).toISOString();

  // Claim the device and consume the exact enrollment token in one atomic D1 batch.
  // The device UPDATE only succeeds while the device is unowned and the token is
  // still valid/issued. The token UPDATE then succeeds only for the customer who
  // won that device claim. A concurrent claimant therefore cannot steal the device
  // or consume the winner's token.
  const result = await env.DB.batch([
    env.DB.prepare(
      "UPDATE devices SET owner_customer_id=?,updated_at=? WHERE device_id=? AND disabled=0 AND owner_customer_id IS NULL AND EXISTS (SELECT 1 FROM enrollment_tokens WHERE device_id=? AND token_hash=? AND status='issued' AND used_at IS NULL AND expires_at>?)"
    ).bind(customer.id, t, deviceId, deviceId, tokenHash, t),
    env.DB.prepare(
      "UPDATE enrollment_tokens SET status='claimed',used_at=?,used_by_customer_id=? WHERE device_id=? AND token_hash=? AND status='issued' AND used_at IS NULL AND expires_at>? AND EXISTS (SELECT 1 FROM devices WHERE device_id=? AND owner_customer_id=?)"
    ).bind(t, customer.id, deviceId, tokenHash, t, deviceId, customer.id)
  ]);

  const deviceChanges = Number(result?.[0]?.meta?.changes || 0);
  const tokenChanges = Number(result?.[1]?.meta?.changes || 0);
  if (deviceChanges !== 1 || tokenChanges !== 1) {
    return fail("Invalid, expired, already-used, or already-claimed enrollment token.", 409);
  }

  await audit(env, customer.id, deviceId, "device_claimed", {
    source: "secure-enrollment-qr",
    claimedAt: tIso
  });
  return json({ deviceId, claimed: true });
}

async function deviceRegister(body, env) {
  const deviceId=normalizeDeviceId(body.deviceId), secret=String(body.deviceSecret||"");
  if(!DEVICE_RE.test(deviceId)||secret.length<32) return fail("Invalid device registration.",400);
  const hash=await sha256Hex(secret), t=now();
  const existing=await env.DB.prepare("SELECT * FROM devices WHERE device_id=? LIMIT 1").bind(deviceId).first();
  if (existing && existing.device_secret_hash !== hash) return fail("Device identity conflict.",409);
  if (!existing) {
    await env.DB.prepare("INSERT INTO devices(device_id,device_secret_hash,created_at,updated_at,last_seen_at) VALUES(?,?,?,?,?)").bind(deviceId,hash,t,t,t).run();
  } else {
    await env.DB.prepare("UPDATE devices SET updated_at=?,last_seen_at=? WHERE device_id=?").bind(t,t,deviceId).run();
  }
  return json({ok:true,deviceId});
}

async function deviceApi(deviceId, subPath, request, env) {
  const device = await deviceFromSecret(deviceId, request, env);
  if (!device) return fail("Invalid device credentials.",401);
  const method=request.method, path=subPath.replace(/^\/+|\/+$/g,"");
  const body = method === "GET" ? null : await request.json().catch(()=>({}));
  const t=now();
  if (path==="register") return fail("Use /api/device/register.",400);
  if (path==="status") {
    if(method==="GET") return json(parseJsonText(device.status_json));
    await env.DB.prepare("UPDATE devices SET status_json=?,firmware_version=?,updated_at=?,last_seen_at=? WHERE device_id=?")
      .bind(JSON.stringify(body||{}),String(body?.firmware||""),t,t,deviceId).run();
    return json({ok:true});
  }
  if (path==="security/meta"||path==="security/request"||path==="security/challenge"||path==="security/response") {
    const field=path.split("/")[1]+"_json";
    if(method==="GET"){const r=await env.DB.prepare("SELECT "+field+" AS value FROM device_security WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.value));}
    await env.DB.prepare("INSERT INTO device_security(device_id,"+field+",updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET "+field+"=excluded."+field+",updated_at=excluded.updated_at").bind(deviceId,JSON.stringify(body||{}),t).run();
    return json({ok:true});
  }
  if(path==="command"){
    if(method==="GET"){const r=await env.DB.prepare("SELECT command_json FROM device_commands WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.command_json));}
    await env.DB.prepare("INSERT INTO device_commands(device_id,command_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET command_json=excluded.command_json,updated_at=excluded.updated_at").bind(deviceId,JSON.stringify(body||{}),t).run();
    return json({ok:true});
  }
  if(path==="commandAck"){
    if(method==="GET"){const r=await env.DB.prepare("SELECT ack_json FROM device_commands WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.ack_json));}
    await env.DB.prepare("INSERT INTO device_commands(device_id,ack_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET ack_json=excluded.ack_json,updated_at=excluded.updated_at").bind(deviceId,JSON.stringify(body||{}),t).run();
    return json({ok:true});
  }
  const configMatch=path.match(/^config\/plants\/(\d+)$/);
  if(configMatch){
    const i=Number(configMatch[1]); if(i<0||i>4)return fail("Invalid plant index.");
    if(method==="GET"){const r=await env.DB.prepare("SELECT config_json FROM device_configs WHERE device_id=? AND plant_index=?").bind(deviceId,i).first();return json(parseJsonText(r?.config_json));}
    await env.DB.prepare("INSERT INTO device_configs(device_id,plant_index,config_json,updated_at) VALUES(?,?,?,?) ON CONFLICT(device_id,plant_index) DO UPDATE SET config_json=excluded.config_json,updated_at=excluded.updated_at").bind(deviceId,i,JSON.stringify(body||{}),t).run();
    return json({ok:true});
  }
  if(path==="config/version"){
    if(method==="GET"){const r=await env.DB.prepare("SELECT config_version FROM devices WHERE device_id=?").bind(deviceId).first();return json(r?.config_version||null);}
    await env.DB.prepare("UPDATE devices SET config_version=?,updated_at=? WHERE device_id=?").bind(typeof body==="string"?body:JSON.stringify(body),t,deviceId).run();
    return json({ok:true});
  }
  if(path==="history/telemetry"){
    if(method==="GET"){const rows=await env.DB.prepare("SELECT payload_json FROM telemetry WHERE device_id=? ORDER BY recorded_at DESC LIMIT 100").bind(deviceId).all();return json(Object.fromEntries(rows.results.reverse().map((r,i)=>[String(i),parseJsonText(r.payload_json)])));}
    await env.DB.prepare("INSERT INTO telemetry(device_id,recorded_at,payload_json) VALUES(?,?,?)").bind(deviceId,t,JSON.stringify(body||{})).run();return json({ok:true});
  }
  if(path==="history/watering"){
    if(method==="GET"){const rows=await env.DB.prepare("SELECT payload_json FROM watering_history WHERE device_id=? ORDER BY recorded_at DESC LIMIT 100").bind(deviceId).all();return json(Object.fromEntries(rows.results.reverse().map((r,i)=>[String(i),parseJsonText(r.payload_json)])));}
    await env.DB.prepare("INSERT INTO watering_history(device_id,recorded_at,payload_json) VALUES(?,?,?)").bind(deviceId,t,JSON.stringify(body||{})).run();return json({ok:true});
  }
  return fail("Unknown device endpoint.",404);
}

async function customerDevice(deviceId, subPath, request, env) {
  const access=await requireOwner(deviceId,request,env); if(access.error)return access.error;
  const path=subPath.replace(/^\/+|\/+$/g,""), method=request.method;
  if(path==="status"&&method==="GET"){const r=await env.DB.prepare("SELECT status_json FROM devices WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.status_json));}
  if(path==="history/telemetry"&&method==="GET"){const rows=await env.DB.prepare("SELECT payload_json FROM telemetry WHERE device_id=? ORDER BY recorded_at DESC LIMIT 100").bind(deviceId).all();return json(Object.fromEntries(rows.results.reverse().map((r,i)=>[String(i),parseJsonText(r.payload_json)])));}
  if(path==="history/watering"&&method==="GET"){const rows=await env.DB.prepare("SELECT payload_json FROM watering_history WHERE device_id=? ORDER BY recorded_at DESC LIMIT 100").bind(deviceId).all();return json(Object.fromEntries(rows.results.reverse().map((r,i)=>[String(i),parseJsonText(r.payload_json)])));}
  if(["command","security/meta","security/request","security/challenge","security/response"].includes(path)){
    if(method==="GET"){
      if(path==="command"){const r=await env.DB.prepare("SELECT command_json FROM device_commands WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.command_json));}
      const field=path.split("/")[1]+"_json";const r=await env.DB.prepare("SELECT "+field+" AS value FROM device_security WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.value));
    }
    const body=await request.json().catch(()=>({}));
    if(path==="command"){await env.DB.prepare("INSERT INTO device_commands(device_id,command_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET command_json=excluded.command_json,updated_at=excluded.updated_at").bind(deviceId,JSON.stringify(body),now()).run();return json({ok:true});}
    const field=path.split("/")[1]+"_json";await env.DB.prepare("INSERT INTO device_security(device_id,"+field+",updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET "+field+"=excluded."+field+",updated_at=excluded.updated_at").bind(deviceId,JSON.stringify(body),now()).run();return json({ok:true});
  }
  const configMatch=path.match(/^config\/plants\/(\d+)$/);
  if(configMatch&&method==="GET"){const r=await env.DB.prepare("SELECT config_json FROM device_configs WHERE device_id=? AND plant_index=?").bind(deviceId,Number(configMatch[1])).first();return json(parseJsonText(r?.config_json));}
  if(path==="commandAck"&&method==="GET"){const r=await env.DB.prepare("SELECT ack_json FROM device_commands WHERE device_id=?").bind(deviceId).first();return json(parseJsonText(r?.ack_json));}
  return fail("Not found.",404);
}

export async function onRequest(context) {
  try {
    const { request, env, params } = context;
    if (!env.DB) return fail("Cloudflare D1 binding DB is not configured.",500);
    const parts = Array.isArray(params.path) ? params.path : (params.path ? [params.path] : []);
    if (parts.length === 0) return json({ service:"Plant Life Care API", status:"ok" });
    const route=parts.join("/");
    if(request.method==="OPTIONS") return new Response(null,{status:204,headers:{"Access-Control-Allow-Origin":"*","Access-Control-Allow-Headers":"Content-Type, Authorization, X-Factory-Key","Access-Control-Allow-Methods":"GET,POST,PUT,PATCH,OPTIONS"}});
    if(route==="auth/register"&&request.method==="POST")return authRegister(await request.json(),env);
    if(route==="auth/login"&&request.method==="POST")return authLogin(await request.json(),env);
    if(route==="auth/me"&&request.method==="GET")return authMe(request,env);
    if(route==="superadmin/login"&&request.method==="POST")return adminLogin(await request.json(),env);
    if(route==="superadmin/me"&&request.method==="GET")return adminMe(request,env);
    if(route==="superadmin/stats"&&request.method==="GET")return adminStats(request,env);
    if(route==="superadmin/devices"&&request.method==="GET")return adminDevices(request,env);
    if(route==="superadmin/customers"&&request.method==="GET")return adminCustomers(request,env);
    if(route==="superadmin/audit"&&request.method==="GET")return adminAudit(request,env);
    if(route==="superadmin/device/register"&&request.method==="POST")return adminRegisterDevice(await request.json(),request,env);
    if(route==="superadmin/enrollment/create"&&request.method==="POST")return adminCreateEnrollment(await request.json(),request,env);
    if(route==="superadmin/device/update"&&request.method==="POST")return adminSetDevice(await request.json(),request,env);
    if(route==="superadmin/device/owner"&&request.method==="POST")return adminAssignOwner(await request.json(),request,env);
    if(route==="superadmin/device/command"&&request.method==="POST")return adminCommand(await request.json(),request,env);
    if(route==="superadmin/support/start"&&request.method==="POST")return adminStartSupport(await request.json(),request,env);
    if(route==="superadmin/support/end"&&request.method==="POST")return adminEndSupport(await request.json(),request,env);
    if(route==="enrollment/create"&&request.method==="POST")return createEnrollment(await request.json(),request,env);
    if(route==="enrollment/claim"&&request.method==="POST")return claimEnrollment(await request.json(),request,env);
    if(route==="device/register"&&request.method==="POST")return deviceRegister(await request.json(),env);
    if(parts[0]==="device"&&parts[1]){
      const deviceId=normalizeDeviceId(parts[1]), sub=parts.slice(2).join("/");
      if(!DEVICE_RE.test(deviceId))return fail("Invalid Device ID.",400);
      const hasDeviceBearer=!!bearer(request);
      const device=hasDeviceBearer?await deviceFromSecret(deviceId,request,env):null;
      if(device)return deviceApi(deviceId,sub,request,env);
      return customerDevice(deviceId,sub,request,env);
    }
    if(route==="devices"&&request.method==="GET"){
      const c=await customerFromSession(request,env);if(!c)return fail("Authentication required.",401);
      const rows=await env.DB.prepare("SELECT device_id AS deviceId,firmware_version AS firmware,last_seen_at AS lastSeen,maintenance_expires_at AS maintenanceExpires FROM devices WHERE owner_customer_id=? AND disabled=0 ORDER BY created_at").bind(c.id).all();
      return json(rows.results);
    }
    return fail("Not found.",404);
  } catch (e) {
    return fail(e?.message || "Internal server error.",500);
  }
}
