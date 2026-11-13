ALTER TABLE messages
  ADD COLUMN read_at DATETIME(6) NULL AFTER created_at;
