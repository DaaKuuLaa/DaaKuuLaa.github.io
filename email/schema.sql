CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS emails (
  id TEXT PRIMARY KEY,
  message_id TEXT UNIQUE,
  recipient TEXT NOT NULL,
  sender_name TEXT DEFAULT '',
  sender_address TEXT NOT NULL,
  subject TEXT DEFAULT '',
  text_content TEXT DEFAULT '',
  html_content TEXT DEFAULT '',
  received_at TEXT DEFAULT (datetime('now')),
  is_read INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_emails_received_at ON emails(received_at DESC);
CREATE INDEX IF NOT EXISTS idx_emails_recipient ON emails(recipient);
CREATE INDEX IF NOT EXISTS idx_emails_is_read ON emails(is_read);

CREATE TABLE IF NOT EXISTS attachments (
  id TEXT PRIMARY KEY,
  email_id TEXT NOT NULL,
  filename TEXT NOT NULL,
  content_type TEXT DEFAULT '',
  size INTEGER DEFAULT 0,
  stored_in_repo INTEGER DEFAULT 0,
  too_large INTEGER DEFAULT 0,
  repo_path TEXT DEFAULT '',
  FOREIGN KEY (email_id) REFERENCES emails(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_attachments_email_id ON attachments(email_id);