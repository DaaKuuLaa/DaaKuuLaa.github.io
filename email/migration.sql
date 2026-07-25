-- Migration 1: add too_large column to attachments
ALTER TABLE attachments ADD COLUMN too_large INTEGER DEFAULT 0;

-- Migration 2: migrate existing bare usernames to full email format
UPDATE users SET username = username || '@dkl.cc.cd' WHERE username NOT LIKE '%@%';

-- Migration 3: reset all data (clear emails, attachments, users)
DELETE FROM attachments;
DELETE FROM emails;
DELETE FROM users;