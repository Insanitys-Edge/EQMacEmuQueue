-- Login Queue System Database Update
-- Add queue persistence table for maintaining queue positions across restarts
-- This enables queue persistence during emergency maintenance with 100+ queued players

CREATE TABLE IF NOT EXISTS tblLoginQueue (
    account_id INT UNSIGNED NOT NULL,
    world_server_id INT UNSIGNED NOT NULL,
    queue_position INT UNSIGNED NOT NULL,
    estimated_wait INT UNSIGNED NOT NULL,
    ip_address INT UNSIGNED NOT NULL,
    queued_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (account_id, world_server_id),
    INDEX idx_world_position (world_server_id, queue_position),
    INDEX idx_timestamp (queued_timestamp)
) ENGINE=InnoDB;


-- Server Population Tracking Table
-- Real-time population data updated by world server InterserverTimer
-- Contains accurate client_list.GetClientCount() values for queue decisions
CREATE TABLE IF NOT EXISTS server_population (
    server_id INT NOT NULL DEFAULT 1,
    current_population INT NOT NULL DEFAULT 0,
    last_updated TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (server_id)
) ENGINE=InnoDB;

-- Active IP Connections Table
-- Mirrors IPTracker::m_active_connections for test script access
-- Real-time IP reservation tracking for queue system testing
CREATE TABLE IF NOT EXISTS active_ip_connections (
    ip_address INT UNSIGNED NOT NULL,
    account_id INT UNSIGNED NOT NULL,
    last_seen TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    grace_period INT UNSIGNED NOT NULL DEFAULT 60,
    is_in_raid TINYINT(1) NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (ip_address),
    INDEX idx_account_id (account_id),
    INDEX idx_last_seen (last_seen),
    INDEX idx_grace_period (grace_period)
) ENGINE=InnoDB;

-- Initialize server_population table with default row
INSERT IGNORE INTO server_population (server_id, current_population) VALUES (1, 0);

-- Clean up any potential old data (entries older than 24 hours)
DELETE FROM tblLoginQueue WHERE queued_timestamp < DATE_SUB(NOW(), INTERVAL 24 HOUR);

-- Queue refresh setting
-- Add unique constraint to prevent duplicate type entries
ALTER TABLE tblloginserversettings ADD UNIQUE KEY unique_type (type);

-- Initialize RefreshQueue setting with default value
INSERT INTO tblloginserversettings (type, value, category, description, defaults)
VALUES ('RefreshQueue', '0', 'options', 'Trigger queue refresh - auto-reset by system', '0')
ON DUPLICATE KEY UPDATE value = '0', description = 'Trigger queue refresh - auto-reset by system'; 

-- Account Grace Period Whitelist Table
-- This table stores accounts that are within their grace period after disconnection
-- Updated every 5 seconds by the world server AccountRezMgr
-- Queried by the login server for bypass decisions

CREATE TABLE IF NOT EXISTS account_grace_whitelist (
    account_id INT UNSIGNED NOT NULL PRIMARY KEY,
    expires_at INT UNSIGNED NOT NULL,
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Clean up any existing expired entries
DELETE FROM account_grace_whitelist WHERE expires_at < UNIX_TIMESTAMP(); 