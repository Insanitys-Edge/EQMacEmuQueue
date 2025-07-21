#include "account_reservation_manager.h"
#include "worlddb.h"
#include "clientlist.h"
#include "../common/eqemu_logsys.h"
#include "../common/rulesys.h"
#include <fmt/format.h>
#include <set>

extern ClientList client_list;
extern class WorldDatabase database;
extern uint32        numplayers;
// Static member definition
const std::string AccountRezMgr::population_sync_query_format = 
	"INSERT INTO server_population (server_id, current_population) VALUES (1, {}) "
	"ON DUPLICATE KEY UPDATE current_population = {}, last_updated = NOW()";

const std::string AccountRezMgr::test_population_offset_query = 
	"SELECT rule_value FROM rule_values WHERE rule_name = 'World:TestPopulationOffset' LIMIT 1";

const std::string AccountRezMgr::queue_enablement_query = 
	"SELECT rule_value FROM rule_values WHERE rule_name = 'World:EnableQueue' LIMIT 1";

// ===== AccountRezMgr Class Implementation =====

AccountRezMgr::AccountRezMgr() : m_last_cleanup(0), m_last_database_sync(0) {
	// Cache queue enablement rule to avoid repeated database queries
	m_queue_enabled = true; // Default to enabled
	auto results = database.QueryDatabase(queue_enablement_query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		if (row[0]) {
			std::string rule_value = row[0];
			m_queue_enabled = (rule_value == "true" || rule_value == "1");
		}
	}
	LogInfo("AccountRezMgr initialized - Queue system: {}", m_queue_enabled ? "ENABLED" : "DISABLED");
}

void AccountRezMgr::AddRez(uint32 account_id, uint32 ip_address, uint32 grace_period_seconds) {
	// Use rule for default grace period if 0 is passed
	if (grace_period_seconds == 0) {
		grace_period_seconds = RuleI(World, DefaultGracePeriod);
	}
	
	m_account_reservations[account_id] = PlayerInfo(account_id, 0, grace_period_seconds, false, ip_address);
	LogConnectionChange(account_id, "registered");
	
	// Update grace whitelist status immediately
	UpdateGraceWhitelistStatus(account_id);
}

void AccountRezMgr::RemoveRez(uint32 account_id) {
	auto it = m_account_reservations.find(account_id);
	if (it != m_account_reservations.end()) {
		LogConnectionChange(account_id, "removed");
		m_account_reservations.erase(it);
		// Update database immediately
		RemoveConnectionFromDatabase(account_id);
		
		// Remove from grace whitelist since they're no longer reserved
		RemoveFromGraceWhitelist(account_id);
	}
}

void AccountRezMgr::UpdateLastSeen(uint32 account_id)
{
	auto it = m_account_reservations.find(account_id);
	if (it != m_account_reservations.end()) {
		it->second.last_seen = time(nullptr);
		
		// Update grace whitelist status since last_seen affects expiration
		UpdateGraceWhitelistStatus(account_id);
	}
}

bool AccountRezMgr::CheckGracePeriod(uint32 account_id, uint32 current_time) {
	if (current_time == 0) {
		current_time = time(nullptr);
	}
	
	auto it = m_account_reservations.find(account_id);
	if (it == m_account_reservations.end()) {
		return true; // Connection doesn't exist, should be removed
	}
	
	uint32 time_since_last_seen = current_time - it->second.last_seen;
	bool exceeded_grace_period = time_since_last_seen > it->second.grace_period;
	
	return exceeded_grace_period;
}

uint32 AccountRezMgr::GetRemainingGracePeriod(uint32 account_id, uint32 current_time) const {
	if (current_time == 0) {
		current_time = time(nullptr);
	}
	
	auto it = m_account_reservations.find(account_id);
	if (it == m_account_reservations.end()) {
		return 0; // Connection doesn't exist
	}
	
	uint32 time_since_last_seen = current_time - it->second.last_seen;
	if (time_since_last_seen >= it->second.grace_period) {
		return 0; // Grace period expired
	}
	
	return it->second.grace_period - time_since_last_seen;
}

uint32 AccountRezMgr::EffectivePopulation() {
	
	// Fast path: If queue system is disabled, just return base population
	if (!m_queue_enabled) {
		LogInfo("Queue system disabled - returning numplayers: {}", numplayers);
		return numplayers;
	}
	uint32 base_population = Total();
	
	// Queue enabled: Add test offset for simulation purposes
	uint32 test_offset = 0;
	auto results = database.QueryDatabase(test_population_offset_query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		if (row[0]) {
			// Database results are strings, need to convert to number then cast to uint32
			test_offset = static_cast<uint32>(std::stoul(row[0]));
		}
	}
	
	uint32 effective_population = base_population + test_offset;
	
	// Debug: Show population calculation breakdown
	LogInfo("DEBUG: Population calculation - Base reservations: {}, test offset: {}, effective total: {}", 
		base_population, test_offset, effective_population);
	
	// Sync population to database for login server
	auto sync_query = fmt::format(fmt::runtime(population_sync_query_format), effective_population, effective_population);
	auto result = database.QueryDatabase(sync_query);
	if (result.Success()) {
		LogInfo("Account reservations synced with login server report: {} base accounts, {} effective population", base_population, effective_population);
	} else {
		LogError("Failed to sync population database: {}", result.ErrorMessage());
	}
	
	return effective_population;
}

void AccountRezMgr::RefreshQueueEnabledStatus() {
	bool old_status = m_queue_enabled;
	m_queue_enabled = true; // Default to enabled
	auto results = database.QueryDatabase(queue_enablement_query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		if (row[0]) {
			std::string rule_value = row[0];
			m_queue_enabled = (rule_value == "true" || rule_value == "1");
		}
	}
	
	if (old_status != m_queue_enabled) {
		LogInfo("Queue system status changed: {} -> {}", 
			old_status ? "ENABLED" : "DISABLED", 
			m_queue_enabled ? "ENABLED" : "DISABLED");
	}
}

// Database synchronization methods
void AccountRezMgr::SyncConnectionToDatabase(uint32 account_id, const PlayerInfo& info) {
	std::string query = fmt::format(
		"INSERT INTO active_account_connections (account_id, ip_address, last_seen, grace_period, is_in_raid) "
		"VALUES ({}, {}, FROM_UNIXTIME({}), {}, {}) "
		"ON DUPLICATE KEY UPDATE "
		"ip_address = VALUES(ip_address), "
		"last_seen = VALUES(last_seen), "
		"grace_period = VALUES(grace_period), "
		"is_in_raid = VALUES(is_in_raid)",
		account_id, info.ip_address, info.last_seen, info.grace_period, info.is_in_raid ? 1 : 0
	);
	
	auto result = database.QueryDatabase(query);
	if (!result.Success()) {
		LogError("Failed to sync account connection to database: {}", result.ErrorMessage());
	}
}

void AccountRezMgr::RemoveConnectionFromDatabase(uint32 account_id) {
	std::string query = fmt::format(
		"DELETE FROM active_account_connections WHERE account_id = {}",
		account_id
	);
	
	auto result = database.QueryDatabase(query);
	if (!result.Success()) {
		LogError("Failed to remove account connection from database: {}", result.ErrorMessage());
	}
}

void AccountRezMgr::SyncAllConnectionsToDatabase() {
	// Clear existing entries
	auto clear_result = database.QueryDatabase("DELETE FROM active_account_connections");
	if (!clear_result.Success()) {
		LogError("Failed to clear active_account_connections table: {}", clear_result.ErrorMessage());
		return;
	}
	
	// Sync all current connections
	for (const auto& pair : m_account_reservations) {
		SyncConnectionToDatabase(pair.first, pair.second);
	}
	
	LogInfo("AccountRezMgr: Synced {} connections to database", m_account_reservations.size());
}

void AccountRezMgr::PeriodicMaintenance() {
	uint32 current_time = time(nullptr);
	
	// Always cleanup stale connections (important for accuracy)
	CleanupStaleConnections();
	
	// Grace whitelist is now event-based - no periodic sync needed
	
	// Only sync full account reservation list periodically (preserves reservations if server crashes)
	if (ShouldPerformDatabaseSync()) {
		SyncAllConnectionsToDatabase();
		m_last_database_sync = current_time;
		LogInfo("AccountRezMgr: Full reservation list synced to database for crash recovery");
	}
}

void AccountRezMgr::CleanupStaleConnections() {
	uint32 current_time = time(nullptr);
	std::vector<uint32> accounts_to_remove;
	
	// First pass: identify what needs to be updated or removed
	for (auto& pair : m_account_reservations) {
		uint32 account_id = pair.first;
		
		// Check if account has active character
		bool has_active_character = client_list.ActiveConnection(account_id);
		
		if (has_active_character) {
			// Has active character - keep connection and update last_seen
			UpdateLastSeen(account_id);
			LogConnectionChange(account_id, "kept_active_char");
		} else {
			// No active character - detected disconnect
			LogInfo("AccountRezMgr: Account [{}] disconnected - adding to grace whitelist", account_id);
			
			// Add to grace whitelist for login server queue bypass
			UpdateGraceWhitelistStatus(account_id);
			
			if (CheckGracePeriod(account_id, current_time)) {
				// Grace period expired - mark for removal
				LogConnectionChange(account_id, "cleanup_grace_expired");
				accounts_to_remove.push_back(account_id);
			} else {
				// Still within grace period - keep reservation, now in grace whitelist
				uint32 time_since_last_seen = current_time - pair.second.last_seen;
				uint32 time_remaining = pair.second.grace_period - time_since_last_seen;
				LogInfo("AccountRezMgr: Account [{}] in grace period, keeping reservation. Time remaining: [{}] seconds", account_id, time_remaining);
			}
		}
	}
	
	// Second pass: remove all identified connections using proper method
	for (uint32 account_id : accounts_to_remove) {
		RemoveRez(account_id);
	}
}

void AccountRezMgr::LogConnectionChange(uint32 account_id, const std::string& action) const {
	LogInfo("AccountRezMgr: {} connection for account [{}] - total active accounts: {}", 
		action, account_id, m_account_reservations.size());
}

std::string AccountRezMgr::AccountToString(uint32 account_id) const {
	return fmt::format("Account[{}]", account_id);
}

bool AccountRezMgr::ShouldPerformCleanup() const {
	return (time(nullptr) - m_last_cleanup) >= RuleI(World, IPCleanupInterval);
}

bool AccountRezMgr::ShouldPerformDatabaseSync() const {
	return (time(nullptr) - m_last_database_sync) >= RuleI(World, IPDatabaseSyncInterval);
}

// Event-based grace whitelist management methods
bool AccountRezMgr::IsAccountInGraceWhitelist(uint32 account_id) {
	uint32 current_time = time(nullptr);
	
	// Query whitelist and check if expired
	auto query = fmt::format(
		"SELECT expires_at FROM account_grace_whitelist WHERE account_id = {} LIMIT 1", 
		account_id
	);
	
	auto results = database.QueryDatabase(query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		uint32 expires_at = static_cast<uint32>(std::stoul(row[0]));
		
		if (current_time >= expires_at) {
			// Expired - remove from whitelist and return false
			RemoveFromGraceWhitelist(account_id);
			LogInfo("AccountRezMgr: Account [{}] grace whitelist expired, removed", account_id);
			return false;
		}
		
		// Still valid
		LogInfo("AccountRezMgr: Account [{}] found in grace whitelist (expires in {}s)", 
			account_id, expires_at - current_time);
		return true;
	}
	
	return false; // Not in whitelist
}

void AccountRezMgr::UpdateGraceWhitelistStatus(uint32 account_id) {
	auto it = m_account_reservations.find(account_id);
	if (it == m_account_reservations.end()) {
		// No reservation - nothing to do
		return;
	}
	
	const PlayerInfo& info = it->second;
	uint32 current_time = time(nullptr);
	uint32 expires_at = info.last_seen + info.grace_period;
	
	// Add to grace whitelist - maintenance will clean up expired entries
	auto query = fmt::format(
		"INSERT INTO account_grace_whitelist (account_id, expires_at) "
		"VALUES ({}, {}) ON DUPLICATE KEY UPDATE expires_at = VALUES(expires_at)",
		account_id, expires_at
	);
	
	auto result = database.QueryDatabase(query);
	if (result.Success()) {
		LogInfo("AccountRezMgr: Account [{}] added to grace whitelist (expires: {})", 
			account_id, expires_at);
	}
}

void AccountRezMgr::RemoveFromGraceWhitelist(uint32 account_id) {
	auto query = fmt::format(
		"DELETE FROM account_grace_whitelist WHERE account_id = {}", 
		account_id
	);
	
	auto result = database.QueryDatabase(query);
	if (result.Success() && result.RowsAffected() > 0) {
		LogInfo("AccountRezMgr: Account [{}] removed from grace whitelist", account_id);
	}
} 