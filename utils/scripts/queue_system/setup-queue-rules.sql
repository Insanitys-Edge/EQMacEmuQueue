-- EQMacEmu Queue System Configuration Rules
-- 
-- This script sets up all the necessary rules for the queue system to function properly.
-- Run this after installing the base database schema.
--
-- Usage:
--   mysql -u username -p database_name < setup-queue-rules.sql

USE quarm;

-- Queue System Core Rules
-- These control the basic queue functionality

-- Enable the queue system (true/false)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:EnableQueue', 'true', 'Enable the queue system when server is at capacity')
ON DUPLICATE KEY UPDATE rule_value = 'true', notes = 'Enable the queue system when server is at capacity';

-- Freeze queue advancement (true/false) - stops players from moving up in queue
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:FreezeQueue', 'false', 'Manually freeze queue advancement - players stay at their current positions')
ON DUPLICATE KEY UPDATE rule_value = 'false', notes = 'Manually freeze queue advancement - players stay at their current positions';

-- Player population cap (number of players before queue activates)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'Quarm:PlayerPopulationCap', '1200', 'Maximum players before queue system activates')
ON DUPLICATE KEY UPDATE rule_value = '1200', notes = 'Maximum players before queue system activates';

-- Estimated wait time per player in queue (seconds)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:QueueEstimatedWaitPerPlayer', '60', 'Estimated wait time in seconds per position in queue')
ON DUPLICATE KEY UPDATE rule_value = '60', notes = 'Estimated wait time in seconds per position in queue';

-- Enable queue debug logging (true/false)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:EnableQueueLogging', 'true', 'Enable detailed logging for queue system debugging')
ON DUPLICATE KEY UPDATE rule_value = 'true', notes = 'Enable detailed logging for queue system debugging';

-- GM bypass queue (true/false) - if true, GMs skip the queue
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:QueueBypassGMLevel', 'true', 'Allow GMs to bypass the queue system')
ON DUPLICATE KEY UPDATE rule_value = 'true', notes = 'Allow GMs to bypass the queue system';

-- Queue Persistence Rules
-- These control whether queue positions are saved across server restarts

-- Enable queue persistence (true/false)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:EnableQueuePersistence', 'true', 'Save queue positions to database for server restart recovery')
ON DUPLICATE KEY UPDATE rule_value = 'true', notes = 'Save queue positions to database for server restart recovery';

-- Population Testing Rules
-- These are useful for testing the queue system

-- Test population offset (adds fake population for testing)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:TestPopulationOffset', '0', 'Add fake population for queue testing (0 = disabled)')
ON DUPLICATE KEY UPDATE rule_value = '0', notes = 'Add fake population for queue testing (0 = disabled)';

-- Account Tracking Rules
-- These control the account-based population tracking system

-- Default grace period for normal players (seconds)
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:DefaultGracePeriod', '60', 'Grace period in seconds for normal players after disconnect')
ON DUPLICATE KEY UPDATE rule_value = '60', notes = 'Grace period in seconds for normal players after disconnect';

-- Grace period for raid members (seconds) - longer because raids are important
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:RaidGracePeriod', '600', 'Grace period in seconds for raid members after disconnect')
ON DUPLICATE KEY UPDATE rule_value = '600', notes = 'Grace period in seconds for raid members after disconnect';

-- Account cleanup interval (seconds) - how often to clean up stale connections
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:IPCleanupInterval', '30', 'How often to cleanup stale account connections (seconds)')
ON DUPLICATE KEY UPDATE rule_value = '30', notes = 'How often to cleanup stale account connections (seconds)';

-- Account database sync interval (seconds) - how often to sync detailed data
INSERT INTO rule_values (ruleset_id, rule_name, rule_value, notes) 
VALUES (1, 'World:IPDatabaseSyncInterval', '300', 'How often to sync account details to database (seconds)')
ON DUPLICATE KEY UPDATE rule_value = '300', notes = 'How often to sync account details to database (seconds)';

-- Display current configuration
SELECT 'Queue System Configuration:' as '';
SELECT 
    CASE 
        WHEN rule_name LIKE '%Queue%' OR rule_name LIKE '%Population%' THEN '🔧 Queue Settings'
        WHEN rule_name LIKE '%Grace%' OR rule_name LIKE '%Cleanup%' OR rule_name LIKE '%Sync%' THEN '⏱️  Timing Settings'
        WHEN rule_name LIKE '%Test%' THEN '🧪 Testing Settings'
        ELSE '⚙️  Other Settings'
    END as Category,
    REPLACE(REPLACE(rule_name, 'World:', ''), 'Quarm:', '') as Setting,
    rule_value as Value,
    notes as Description
FROM rule_values 
WHERE rule_name IN (
    'World:EnableQueue',
    'World:FreezeQueue',
    'Quarm:PlayerPopulationCap',
    'World:QueueEstimatedWaitPerPlayer',
    'World:EnableQueueLogging',
    'World:QueueBypassGMLevel',
    'World:EnableQueuePersistence',
    'World:TestPopulationOffset',
    'World:DefaultGracePeriod',
    'World:RaidGracePeriod',
    'World:IPCleanupInterval',
    'World:IPDatabaseSyncInterval'
)
ORDER BY 
    CASE 
        WHEN rule_name LIKE '%Queue%' OR rule_name LIKE '%Population%' THEN 1
        WHEN rule_name LIKE '%Grace%' OR rule_name LIKE '%Cleanup%' OR rule_name LIKE '%Sync%' THEN 2
        WHEN rule_name LIKE '%Test%' THEN 3
        ELSE 4
    END,
    FIELD(rule_name, 
        'World:EnableQueue',
        'World:FreezeQueue',
        'Quarm:PlayerPopulationCap', 
        'World:QueueEstimatedWaitPerPlayer',
        'World:QueueBypassGMLevel',
        'World:EnableQueuePersistence',
        'World:TestPopulationOffset',
        'World:DefaultGracePeriod',
        'World:RaidGracePeriod',
        'World:IPCleanupInterval',
        'World:IPDatabaseSyncInterval'
    );

-- Quick Start Instructions
SELECT '' as '';
SELECT 'Queue System Setup Complete!' as '';
SELECT '' as '';
SELECT 'Quick Start:' as '';
SELECT '1. Set population cap: UPDATE rule_values SET rule_value=''500'' WHERE rule_name=''Quarm:PlayerPopulationCap'';' as '';
SELECT '2. Test with fake population: UPDATE rule_values SET rule_value=''450'' WHERE rule_name=''World:TestPopulationOffset'';' as '';
SELECT '3. Monitor with: source/EQMacEmu/utils/scripts/queue_system/queue-system-test-interactive.sh' as '';
SELECT '4. Disable persistence for testing: UPDATE rule_values SET rule_value=''false'' WHERE rule_name=''World:EnableQueuePersistence'';' as '';
SELECT '' as '';
SELECT 'See queue-system-test-interactive.sh for testing and monitoring tools.' as ''; 

-- Trigger queue refresh to apply new rules immediately
INSERT INTO tblloginserversettings (type, value, category, description) 
VALUES ('RefreshQueue', '1', 'options', 'Trigger queue refresh - auto-reset by system') 
ON DUPLICATE KEY UPDATE value = '1'; 