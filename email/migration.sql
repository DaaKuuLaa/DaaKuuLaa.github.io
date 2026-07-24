-- Migration: add too_large column to attachments
ALTER TABLE attachments ADD COLUMN too_large INTEGER DEFAULT 0;