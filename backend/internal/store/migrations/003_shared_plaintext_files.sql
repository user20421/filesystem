DROP TABLE IF EXISTS file_chunks;
DROP TABLE IF EXISTS files;
CREATE TABLE IF NOT EXISTS file_contents (
  id CHAR(36) NOT NULL PRIMARY KEY,
  size BIGINT UNSIGNED NOT NULL,
  sha256 CHAR(64) NOT NULL,
  chunk_size INT UNSIGNED NOT NULL,
  chunk_count INT UNSIGNED NOT NULL,
  status ENUM('uploading','ready','deleting') NOT NULL DEFAULT 'uploading',
  ref_count INT UNSIGNED NOT NULL DEFAULT 0,
  revision BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  completed_at DATETIME(6) NULL,
  UNIQUE KEY uq_file_content (size, sha256),
  INDEX idx_content_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE TABLE IF NOT EXISTS files (
  id CHAR(36) NOT NULL PRIMARY KEY,
  content_id CHAR(36) NOT NULL,
  owner_id CHAR(36) NOT NULL,
  recipient_id CHAR(36) NOT NULL,
  original_name VARCHAR(255) NOT NULL,
  status ENUM('active','deleting') NOT NULL DEFAULT 'active',
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  CONSTRAINT fk_file_content FOREIGN KEY (content_id) REFERENCES file_contents(id),
  CONSTRAINT fk_file_owner FOREIGN KEY (owner_id) REFERENCES users(id),
  CONSTRAINT fk_file_recipient FOREIGN KEY (recipient_id) REFERENCES users(id),
  INDEX idx_file_content (content_id, status),
  INDEX idx_file_owner_status (owner_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE TABLE IF NOT EXISTS file_chunks (
  content_id CHAR(36) NOT NULL,
  chunk_index INT UNSIGNED NOT NULL,
  object_key VARCHAR(512) NOT NULL UNIQUE,
  plaintext_size INT UNSIGNED NOT NULL,
  sha256 CHAR(64) NOT NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (content_id, chunk_index),
  CONSTRAINT fk_chunk_content FOREIGN KEY (content_id) REFERENCES file_contents(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
