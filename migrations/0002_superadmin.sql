PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS super_admins (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  name TEXT NOT NULL,
  password_salt TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  disabled INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS super_admin_sessions (
  token_hash TEXT PRIMARY KEY,
  admin_id TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  last_seen_at INTEGER NOT NULL,
  FOREIGN KEY (admin_id) REFERENCES super_admins(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_super_admin_sessions_admin ON super_admin_sessions(admin_id);
CREATE INDEX IF NOT EXISTS idx_super_admin_sessions_expiry ON super_admin_sessions(expires_at);

CREATE TABLE IF NOT EXISTS support_sessions (
  id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL,
  admin_id TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  status TEXT NOT NULL DEFAULT 'active',
  created_at INTEGER NOT NULL,
  ended_at INTEGER,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
  FOREIGN KEY (admin_id) REFERENCES super_admins(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_support_device_time ON support_sessions(device_id, created_at DESC);
