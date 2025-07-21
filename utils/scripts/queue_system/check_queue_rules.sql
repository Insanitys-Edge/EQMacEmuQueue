-- Quick Quarm Queue Rules Check
-- Run with: mysql -u username -p quarm < check_queue_rules.sql

SELECT '=== QUEUE SYSTEM RULES ===' as '';

SELECT 
    rule_name as 'Rule Name',
    rule_value as 'Current Value',
    notes as 'Description'
FROM rule_values 
WHERE rule_name IN (
    'World:EnableQueue',
    'Quarm:PlayerPopulationCap', 
    'World:TestPopulationOffset',
    'World:QueueBypassGMLevel',
    'World:QueueEstimatedWaitPerPlayer',
    'World:EnableQueueLogging',
    'World:FreezeQueue',
    'World:MaxPlayersOnline',
    'World:DefaultGracePeriod',
    'World:IPCleanupInterval',
    'World:IPDatabaseSyncInterval'
)
ORDER BY rule_name;

SELECT '' as '';
SELECT '=== CURRENT SERVER STATUS ===' as '';

SELECT 
    server_id as 'Server ID',
    current_population as 'Current Population',
    last_updated as 'Last Updated'
FROM server_population;

SELECT '' as '';
SELECT '=== ACTIVE ACCOUNT RESERVATIONS ===' as '';

SELECT COUNT(*) as 'Active Reservations' FROM active_account_connections;

SELECT '' as '';
SELECT '=== QUEUE ENTRIES ===' as '';

SELECT COUNT(*) as 'Players in Queue' FROM tblLoginQueue; 