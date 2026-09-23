PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS customers (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  name TEXT NOT NULL,
  password_salt TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  disabled INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  customer_id TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  last_seen_at INTEGER NOT NULL,
  FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_sessions_customer ON sessions(customer_id);
CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);

CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  device_secret_hash TEXT NOT NULL,
  owner_customer_id TEXT,
  firmware_version TEXT,
  status_json TEXT NOT NULL DEFAULT '{}',
  config_version TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  last_seen_at INTEGER,
  maintenance_expires_at INTEGER,
  disabled INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (owner_customer_id) REFERENCES customers(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS idx_devices_owner ON devices(owner_customer_id);
CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen_at);

CREATE TABLE IF NOT EXISTS enrollment_tokens (
  device_id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL,
  issued_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  used_at INTEGER,
  used_by_customer_id TEXT,
  status TEXT NOT NULL DEFAULT 'issued',
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
  FOREIGN KEY (used_by_customer_id) REFERENCES customers(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS idx_enrollment_expiry ON enrollment_tokens(expires_at);

CREATE TABLE IF NOT EXISTS device_configs (
  device_id TEXT NOT NULL,
  plant_index INTEGER NOT NULL,
  config_json TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY (device_id, plant_index),
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS device_security (
  device_id TEXT PRIMARY KEY,
  meta_json TEXT NOT NULL DEFAULT '{}',
  request_json TEXT NOT NULL DEFAULT '{}',
  challenge_json TEXT NOT NULL DEFAULT '{}',
  response_json TEXT NOT NULL DEFAULT '{}',
  updated_at INTEGER NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS device_commands (
  device_id TEXT PRIMARY KEY,
  command_json TEXT NOT NULL DEFAULT '{}',
  ack_json TEXT NOT NULL DEFAULT '{}',
  updated_at INTEGER NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS telemetry (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  recorded_at INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_telemetry_device_time ON telemetry(device_id, recorded_at DESC);

CREATE TABLE IF NOT EXISTS watering_history (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  recorded_at INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_watering_device_time ON watering_history(device_id, recorded_at DESC);

CREATE TABLE IF NOT EXISTS audit_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  customer_id TEXT,
  device_id TEXT,
  action TEXT NOT NULL,
  details_json TEXT NOT NULL DEFAULT '{}',
  created_at INTEGER NOT NULL,
  FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE SET NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS idx_audit_device_time ON audit_logs(device_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_customer_time ON audit_logs(customer_id, created_at DESC);
