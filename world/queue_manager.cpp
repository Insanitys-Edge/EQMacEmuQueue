#include "queue_manager.h"
#include "login_server.h"      // No need for world_server.h include  
#include "worlddb.h"           // For WorldDatabase
#include "clientlist.h"        // For ClientList
#include "world_config.h"      // For WorldConfig
#include "../common/eqemu_logsys.h"
#include "../common/servertalk.h"
#include "../common/rulesys.h"  // For RuleB and RuleI macros
#include "../common/ip_util.h"  // For IpUtil::IsIpInPrivateRfc1918
#include <fmt/format.h>
#include <arpa/inet.h>

extern LoginServer* loginserver;  // World server uses pointer, not object
extern WorldDatabase database;  // World server global database
extern ClientList client_list;  // World server global client list
extern QueueManager queue_manager;  // Global queue manager for world server
extern uint32 numplayers;  // Global player count for queue system fallback

// Static member definition
const std::string QueueManager::refresh_queue_query = 
	"SELECT value FROM tblloginserversettings WHERE type = 'RefreshQueue' ORDER BY value DESC LIMIT 1";

const std::string QueueManager::reset_queue_flag_query = 
	"UPDATE tblloginserversettings SET value = '0' WHERE type = 'RefreshQueue'";

// QueueManagerEntry definition (renamed to avoid conflicts)
struct QueueManagerEntry {
	uint32 account_id;
	uint32 queue_position;
	uint32 estimated_wait;
	uint32 ip_address;
	uint32 queued_timestamp;
	uint32 last_updated;
	uint32 last_seen; // Timestamp for grace period tracking
	
	// Extended connection details for auto-connect
	uint32 ls_account_id;
	uint32 world_id;
	uint32 from_id;
	std::string ip_str;
	std::string forum_name;
	uint32 world_account_id;
	
	QueueManagerEntry() : account_id(0), queue_position(0), estimated_wait(0), ip_address(0),
		queued_timestamp(0), last_updated(0), last_seen(0), ls_account_id(0), world_id(0), 
		from_id(0), world_account_id(0) {}
	
	QueueManagerEntry(uint32 acct_id, uint32 pos, uint32 wait, uint32 ip = 0, 
			   uint32 ls_acct_id = 0, uint32 w_id = 0, uint32 f_id = 0, 
			   const std::string& ip_string = "", const std::string& forum = "")
		: account_id(acct_id), queue_position(pos), estimated_wait(wait), ip_address(ip),
		  queued_timestamp(time(nullptr)), last_updated(time(nullptr)), last_seen(time(nullptr)),
		  ls_account_id(ls_acct_id), world_id(w_id), from_id(f_id), 
		  ip_str(ip_string), forum_name(forum), world_account_id(0) {}
};

// Helper function to get world server name
std::string GetWorldServerName() {
	const WorldConfig* config = WorldConfig::get();
	return config ? config->LongName : "World Server";
}

QueueManager::QueueManager()
	: m_queue_paused(false)
{
	LogInfo("QueueManager created - database initialization deferred");
}

void QueueManager::Initialize()
{
	// Initialize AccountRezMgr after database is ready
	m_account_rez_mgr = std::make_unique<AccountRezMgr>();
	LogInfo("QueueManager initialized for world server - using event-driven DB mirroring");
}

QueueManager::~QueueManager()
{
	// Event-driven approach: Database is always synchronized
	// No need for final sync since all operations are immediately mirrored
	LogInfo("QueueManager destroyed - database remains synchronized through event-driven mirroring");
}

void QueueManager::AddToQueue(uint32 world_account_id, uint32 position, uint32 estimated_wait, uint32 ip_address, 
							uint32 ls_account_id, uint32 from_id, 
							const char* ip_str, const char* forum_name)
{
	in_addr addr;
	addr.s_addr = ip_address;
	std::string ip_str_log = inet_ntoa(addr);
	
	// Calculate queue position if not provided
	if (position == 0) {
		position = CalculateQueuePosition(world_account_id);
	}
	
	// Calculate estimated wait time based on queue position
	if (estimated_wait == 0 || estimated_wait == 60) { // Recalculate if default or not provided
		// Get wait time per player from rules or use default
		uint32 wait_per_player = 60; // Default wait per player in seconds
		estimated_wait = position * wait_per_player;
	}
	
	// Create queue entry using world account ID for identification
	QueueManagerEntry new_entry(world_account_id, position, estimated_wait, ip_address, 
								 ls_account_id, 0, from_id,  // world_id removed, set to 0
								 ip_str ? ip_str : ip_str_log, 
								 forum_name ? forum_name : "");
	new_entry.world_account_id = world_account_id;
	
	// 1. Store in memory using world_account_id as key
	m_queued_players[world_account_id] = new_entry;
	
	// 2. Mirror to database immediately (event-driven) - use fixed world server ID
	SaveQueueEntry(world_account_id, 1, position, estimated_wait, ip_address); // Fixed server ID for world server
	LogQueueAction("ADD_TO_QUEUE", world_account_id, 
		fmt::format("pos={} wait={}s ip={} ls_id={} (memory + DB)", position, estimated_wait, ip_str_log, ls_account_id));
	
	SendQueuedClientsUpdate();
	
}

uint32 QueueManager::CalculateQueuePosition(uint32 account_id) 
{
	// Calculate next queue position based on current queue size
	uint32 current_queue_size = GetTotalQueueSize();
	uint32 next_position = current_queue_size + 1;
	
	LogInfo("QueueManager [{}] - CALCULATE_POSITION: Account [{}] assigned position [{}] (current queue size: {})", 
		GetWorldServerName(), 
		account_id, next_position, current_queue_size);
	
	return next_position;
}
void QueueManager::RemoveFromQueue(uint32 account_id)
{
	auto it = m_queued_players.find(account_id);
	if (it == m_queued_players.end()) {
		// Not found by direct lookup, try LS account ID mapping using encapsulated method
		uint32 world_account_id = GetWorldAccountFromLS(account_id);
		if (world_account_id != account_id) {  // Only search if we got a different mapping
			it = m_queued_players.find(world_account_id);
		}
	}
	
	if (it != m_queued_players.end()) {
		in_addr addr;
		addr.s_addr = it->second.ip_address;
		std::string ip_str = inet_ntoa(addr);
		
		// Store account ID for removal operations
		uint32 account_id_to_remove = it->first;
		uint32 ls_account_id_to_notify = it->second.ls_account_id; // Store for notification
		uint32 ip_address_to_notify = it->second.ip_address;
		
		// 1. Remove from memory
		m_queued_players.erase(it);
		
		// 2. Mirror to database immediately (event-driven)
		RemoveQueueEntry(account_id_to_remove, 1); // Fixed server ID for world server
		LogQueueAction("REMOVE", account_id_to_remove, 
			fmt::format("IP: {} (memory + DB)", ip_str));
		
		// 3. Send "removal notification" to the player who just left the queue
		if (ls_account_id_to_notify > 0 && loginserver && loginserver->Connected()) {
			auto removal_pack = new ServerPacket(ServerOP_QueueDirectUpdate, sizeof(ServerQueueDirectUpdate_Struct));
			ServerQueueDirectUpdate_Struct* removal_update = (ServerQueueDirectUpdate_Struct*)removal_pack->pBuffer;
			
			removal_update->ls_account_id = ls_account_id_to_notify;
			removal_update->ip_address = ip_address_to_notify;
			removal_update->queue_position = 0; // Position 0 = removed from queue
			removal_update->estimated_wait = 0;
			
			loginserver->SendPacket(removal_pack);
			delete removal_pack;
			
			LogInfo("Sent queue removal notification to account [{}] - they should now see normal population", 
				account_id_to_remove);
		}
		
		// 4. TARGETED BROADCAST: Send position updates to remaining queued clients (their positions may have changed)
		if (!m_queued_players.empty()) {
			SendQueuedClientsUpdate();
			LogInfo("Sent queue position updates to [{}] remaining clients after player removal", m_queued_players.size());
		}
	}
}
void QueueManager::UpdateQueuePositions()
{
	if (m_queued_players.empty()) {
		return;
	}
	
	// Don't update queue positions if server is down/locked
	if (m_queue_paused) {
		LogInfo("Queue updates paused due to server status - [{}] players remain queued", m_queued_players.size());
		return;
	}
	
	// Check if queue is manually frozen via rule
	if (RuleB(World, FreezeQueue)) {
		LogInfo("Queue updates frozen by rule - [{}] players remain queued with frozen positions", m_queued_players.size());
		return;
	}
	
	// Get server capacity for capacity decisions
	uint32 max_capacity = RuleI(Quarm, PlayerPopulationCap);
	uint32 current_population = GetEffectivePopulation();
	
	// Check pending connections for completion or grace period expiration
	std::vector<uint32> completed_connections;
	std::vector<uint32> expired_connections;
	uint32 current_time = time(nullptr);
	
	for (auto it = m_pending_connections.begin(); it != m_pending_connections.end(); ) {
		uint32 account_id = it->first;
		uint32 pending_time = it->second;
		uint32 time_pending = current_time - pending_time;
		
		LogDebug("Checking pending connection: account [{}] pending for [{}]s", account_id, time_pending);
		
		bool is_active = client_list.ActiveConnection(account_id);
		LogDebug("ActiveConnection check for account [{}]: {}", account_id, is_active ? "CONNECTED" : "NOT CONNECTED");
		
		if (is_active) {
			// Successfully connected to world server
			completed_connections.push_back(account_id);
			LogInfo("Account [{}] confirmed connected to world server after [{}]s - removing from queue", 
				account_id, time_pending);
			it = m_pending_connections.erase(it);
		} else if (time_pending > pending_connection_grace_period) {
			// Grace period expired, assume connection failed
			expired_connections.push_back(account_id);
			LogInfo("Account [{}] connection grace period expired after [{}]s - allowing re-queue", 
				account_id, time_pending);
			it = m_pending_connections.erase(it);
		} else {
			// Still within grace period, keep pending
			LogDebug("Account [{}] still pending - keeping in pending list ({}s remaining)", 
				account_id, pending_connection_grace_period - time_pending);
			++it;
		}
	}
	
	// Remove completed connections from queue
	for (uint32 account_id : completed_connections) {
		RemoveFromQueue(account_id);  // Use consistent memory + DB removal
	}
	
	// Recalculate effective population after pending connection cleanup
	current_population = GetEffectivePopulation();
	
	// Track auto-connects to prevent over-capacity situations
	uint32 auto_connects_initiated = 0;
	uint32 available_slots = (current_population < max_capacity) ? (max_capacity - current_population) : 0;
	
	// Process queue for new auto-connects
	bool positions_changed = false;
	std::vector<uint32> players_to_remove;
	bool someone_waiting_for_capacity = false;
	
	// Use safer iteration pattern to avoid iterator invalidation
	// when RemoveFromQueue is called from other threads (like SendUserToWorldRequest)
	auto it = m_queued_players.begin();
	while (it != m_queued_players.end()) {
		QueueManagerEntry& entry = it->second;
		uint32 account_id = it->first;
		
		// Advance iterator BEFORE any operations that might modify the map
		++it;
		
		// Check if player reached front of queue
		if (entry.queue_position <= 1) {
			in_addr addr;
			addr.s_addr = entry.ip_address;
			std::string ip_str = inet_ntoa(addr);
			
			// Skip if already pending connection
			if (IsConnectionPending(account_id)) {
				LogQueueAction("ALREADY_PENDING", entry.account_id, 
					fmt::format("IP: {} at position [{}] already pending connection - skipping", 
						ip_str, entry.queue_position));
				continue;  // Skip this player, check next one
			}
			
			if (auto_connects_initiated < available_slots) {
				// Server has capacity - proceed with auto-connect
				LogQueueAction("FRONT_OF_QUEUE", entry.account_id, 
					fmt::format("IP: {} reached position [{}] - auto-connecting (world: {}, pending: {}, total: {}/{}, slot {}/{})", 
						ip_str, entry.queue_position, GetWorldPopulation(), GetPendingConnectionCount(), 
						current_population, max_capacity, auto_connects_initiated + 1, available_slots));
				
				// Send authorization and auto-connect
				// No need for authorization in world server context - handled internally
				if (entry.ls_account_id > 0) {
					// Send a dialog before auto-connecting
					SendDialogToQueuedPlayer(entry.account_id, 
						fmt::format("You are next in line! Connecting to server automatically...\nPosition: {} of {} players", 
							entry.queue_position, GetTotalQueueSize()));
					
					// Auto-connect since server has capacity
					AutoConnectQueuedPlayer(entry);
				}
				
				// Mark as pending connection (don't remove from queue yet)
				auto_connects_initiated++;
				MarkConnectionPending(account_id);
				positions_changed = true;
			} else {
				// Server at capacity OR we've used all available slots this cycle
				someone_waiting_for_capacity = true;
				
				LogQueueAction("WAITING_CAPACITY", entry.account_id, 
					fmt::format("IP: {} at position [{}] waiting for capacity (world: {}, pending: {}, total: {}/{}, used slots: {}/{})", 
						ip_str, entry.queue_position, GetWorldPopulation(), GetPendingConnectionCount(), 
						current_population, max_capacity, auto_connects_initiated, available_slots));
				
				// Don't auto-connect, but allow this player to stay at position 1
				// Other players can still advance behind them
			}
		}
	}
	
	// Remove players who reached the front
	for (uint32 account_id : players_to_remove) {
		RemoveFromQueue(account_id);
	}

	if (positions_changed) {
		LogInfo("Updated queue positions for [{}] players, initiated [{}] auto-connects (pending: {})", 
			m_queued_players.size(), auto_connects_initiated, GetPendingConnectionCount());
			// Event-driven approach: Database changes are handled immediately in RemoveFromQueue calls
			// No need for dirty flag tracking or periodic sync
			
			// TARGETED BROADCAST: Send position updates to all queued clients
			SendQueuedClientsUpdate();
	} else if (someone_waiting_for_capacity && !m_queued_players.empty()) {
		LogInfo("Auto-connect paused for position 1 players - server at capacity (world: {}, pending: {}, total: {}/{}) with [{}] players queued", 
			GetWorldPopulation(), GetPendingConnectionCount(), current_population, max_capacity, m_queued_players.size());
	}
	
	// Log pending connection cleanup if any occurred
	if (!completed_connections.empty() || !expired_connections.empty()) {
		LogInfo("Pending connection cleanup: [{}] confirmed connected, [{}] grace period expired", 
			completed_connections.size(), expired_connections.size());
	}
}

bool QueueManager::EvaluateConnectionRequest(const ConnectionRequest& request, uint32 max_capacity,
                                            UsertoWorldResponse* response, Client* client)
{
	QueueDecisionOutcome decision = QueueDecisionOutcome::QueuePlayer; // Default to queueing
	
	// 1. Auto-connects always bypass (shouldn't get -6, but just in case)
	if (request.is_auto_connect) {
		LogInfo("QueueManager [{}] - AUTO_CONNECT: Account [{}] bypassing queue evaluation", 
			GetWorldServerName(), request.account_id);
		decision = QueueDecisionOutcome::AutoConnect;
	}
	// 2. Check if player is already queued (queue toggle behavior)
	else if (IsAccountQueued(request.account_id)) {
		uint32 queue_position = GetQueuePosition(request.account_id);
		LogInfo("QueueManager [{}] - QUEUE_TOGGLE: Account [{}] already queued at position [{}] - toggling off", 
			GetWorldServerName(), request.account_id, queue_position);
		decision = QueueDecisionOutcome::QueueToggle;
	}
	// 3. Check GM bypass rules - this is where we override world server's decision
	else if (RuleB(World, QueueBypassGMLevel) && request.status >= 80) {
		LogInfo("QueueManager [{}] - GM_BYPASS: Account [{}] (status: {}) overriding world server capacity decision", 
			GetWorldServerName(), request.account_id, request.status);
		decision = QueueDecisionOutcome::GMBypass;
	}
	// 4. Check grace period whitelist (disconnected players still within grace period)
	else if (IsAccountInGraceWhitelist(request.account_id)) {
		LogInfo("QueueManager [{}] - GRACE_BYPASS: Account [{}] in grace period whitelist - overriding capacity check", 
			GetWorldServerName(), request.account_id);
		decision = QueueDecisionOutcome::GraceBypass;
	}
	// 5. Default case - no bypass conditions met, queue the player
	else {
		LogInfo("QueueManager [{}] - QUEUE_PLAYER: Account [{}] at capacity with no bypass conditions - adding to queue", 
			GetWorldServerName(), request.account_id);
		decision = QueueDecisionOutcome::QueuePlayer;
	}
	
	// Handle the decision outcome
	switch (decision) {
		case QueueDecisionOutcome::AutoConnect:
		case QueueDecisionOutcome::GMBypass:
		case QueueDecisionOutcome::GraceBypass:
			// These cases override the -6 and allow connection
			return true;
			
		case QueueDecisionOutcome::QueueToggle:
			RemoveFromQueue(request.account_id);
			LogInfo("QUEUE TOGGLE: Player clicked PLAY while queued - removed account [{}] from server [{}]", request.account_id, GetWorldServerName());
			return false;
			
		case QueueDecisionOutcome::QueuePlayer:
			// Add to queue for this server
			{
				uint32 queue_position = CalculateQueuePosition(request.world_account_id);
				uint32 estimated_wait = queue_position * 60; // 60 seconds per position default
				
				AddToQueue(
					request.world_account_id,        // world_account_id (primary key)
					queue_position,                  // position
					estimated_wait,                  // estimated_wait  
					request.ip_address,              // ip_address
					request.ls_account_id,          // ls_account_id
					response ? response->FromID : 0, // from_id
					request.ip_str,                  // ip_str
					request.forum_name               // forum_name
				);
				
				LogInfo("Added account [{}] to queue at position [{}] with estimated wait [{}] seconds", 
					request.world_account_id, queue_position, estimated_wait);
				
				return false; // Don't override -6, player should remain queued
			}
	}
	
	// Should never reach here, but default to not overriding
	return false;
}





bool QueueManager::IsAccountQueued(uint32 account_id) const
{
	// First check if it's a direct world account ID lookup
	if (m_queued_players.find(account_id) != m_queued_players.end()) {
		return true;
	}
	
	// If not found, check if it's an LS account ID that needs mapping using encapsulated method
	uint32 world_account_id = GetWorldAccountFromLS(account_id);
	return (world_account_id != account_id && m_queued_players.find(world_account_id) != m_queued_players.end());
}


uint32 QueueManager::GetQueuePosition(uint32 account_id) const
{
	// First check if it's a direct world account ID lookup
	auto it = m_queued_players.find(account_id);
	if (it != m_queued_players.end()) {
		return it->second.queue_position;
	}
	
	// If not found, check if it's an LS account ID that needs mapping using encapsulated method
	uint32 world_account_id = GetWorldAccountFromLS(account_id);
	if (world_account_id != account_id) {  // Only search if we got a different mapping
		auto world_it = m_queued_players.find(world_account_id);
		if (world_it != m_queued_players.end()) {
			return world_it->second.queue_position;
		}
	}
	
	return 0;
}

uint32 QueueManager::GetTotalQueueSize() const
{
	return static_cast<uint32>(m_queued_players.size());
}

void QueueManager::SyncQueueToDatabase()
{
	uint32 server_id = 1; // Fixed server ID for world server
	Database* db = GetDatabase();
	if (!db) {
		LogError("Cannot sync queue to database - database not available");
		return;
	}
	
	LogInfo("Syncing queue to database for world server [{}] with [{}] entries", 
		server_id, m_queued_players.size());
	
	// Clear existing entries for this world server
	auto clear_query = fmt::format("DELETE FROM tblLoginQueue WHERE world_server_id = {}", server_id);
	database.QueryDatabase(clear_query);
	
	// Save all current queue entries
	for (const auto& pair : m_queued_players) {
		const QueueManagerEntry& entry = pair.second;
		SaveQueueEntry(
			entry.account_id,
			server_id,
			entry.queue_position,
			entry.estimated_wait,
			entry.ip_address
		);
	}
	
	LogInfo("Queue sync completed for world server [{}]", server_id);
}

void QueueManager::RestoreQueueFromDatabase()
{
	// Check if queue persistence is enabled
	if (!RuleB(World, EnableQueuePersistence)) {
		LogInfo("Queue persistence disabled - clearing old queue entries for world server [{}]", 1);
		auto clear_query = fmt::format("DELETE FROM tblLoginQueue WHERE world_server_id = {}", 1);
		database.QueryDatabase(clear_query);  // Use global database
		return;
	}
	
	LogInfo("Restoring queue from database for world server [{}]", 1);
	
	// Load queue entries from database
	std::vector<std::tuple<uint32, uint32, uint32, uint32>> queue_entries;
	if (!LoadQueueEntries(1, queue_entries)) { // Fixed server ID for world server
		LogError("Failed to load queue entries from database");
		return;
	}
	
	// Restore queue entries to memory
	m_queued_players.clear();
	uint32 restored_count = 0;
	
	for (const auto& entry_tuple : queue_entries) {
		uint32 world_account_id = std::get<0>(entry_tuple);  // This should be world account ID
		uint32 queue_position = std::get<1>(entry_tuple);
		uint32 estimated_wait = std::get<2>(entry_tuple);
		uint32 ip_address = std::get<3>(entry_tuple);
		
		// Skip entries with invalid world account IDs
		if (world_account_id == 0) {
			LogInfo("QueueManager [{}] - SKIP: Invalid world account ID [0] during restoration", 
				GetWorldServerName());
			continue;
		}
		
		QueueManagerEntry entry;
		entry.account_id = world_account_id;
		entry.queue_position = queue_position;
		entry.estimated_wait = estimated_wait;
		entry.ip_address = ip_address;
		entry.queued_timestamp = time(nullptr); // Current time for restored entries
		entry.last_updated = time(nullptr);
		
		// For restored entries, we don't have LS account ID or extended connection details
		entry.ls_account_id = 0; // Unknown for restored entries
		entry.world_id = 0;
		entry.from_id = 0;
		entry.ip_str = "";
		entry.forum_name = "";
		entry.world_account_id = world_account_id;
		
		// Use world account ID as map key (consistent with runtime behavior)
		m_queued_players[world_account_id] = entry;
		restored_count++;
		
		LogInfo("QueueManager [{}] - RESTORE: World account [{}] at position [{}] with wait [{}] - persistent queue entry restored", 
			GetWorldServerName(), 
			world_account_id, queue_position, estimated_wait);
	}
	
	if (restored_count > 0) {
		LogInfo("Restored [{}] persistent queue entries from database for world server [{}]", 
			restored_count, 1);
		LogInfo("NOTE: Restored entries use world account IDs only - LS account mapping will be established when players reconnect");
	} else {
		LogInfo("No queue entries to restore for world server [{}]", 1);
	}
	// SendServerListUpdate(); // REMOVED: Now using targeted push updates
}

void QueueManager::CheckForExternalChanges()
{
	// Check for queue refresh flag in database using cached query
	auto results = database.QueryDatabase(refresh_queue_query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		std::string flag_value = row[0] ? row[0] : "0";
		
		// If flag is anything other than "0", process it and reset to "0"
		if (flag_value != "0") {
			bool should_refresh = (flag_value == "1" || flag_value == "true");
			
			if (should_refresh) {
				LogInfo("Queue refresh flag detected - refreshing queue from database");
				RestoreQueueFromDatabase();
				LogInfo("Queue refreshed from database - clients updated");
			} else {
				LogInfo("RefreshQueue flag value = '{}' - resetting to 0", flag_value);
			}
			
			// Always reset the flag to 0, regardless of the value
			auto reset_result = database.QueryDatabase(reset_queue_flag_query);
			
			if (reset_result.Success()) {
				LogInfo("RefreshQueue flag reset to 0");
			} else {
				LogError("Failed to reset RefreshQueue flag: {}", reset_result.ErrorMessage());
			}
		}
	} else {
		if (!results.Success()) {
			LogError("CheckForExternalChanges: Query failed - {}", results.ErrorMessage());
		} else {
			LogDebug("CheckForExternalChanges: No RefreshQueue flag found in database");
		}
	}
}

void QueueManager::PeriodicMaintenance()
{
	// PERFORMANCE OPTIMIZATION: Queue changes are batched and synced periodically
	// instead of immediate sync after every add/remove operation.
	// This reduces database load from dozens of syncs per minute to one every 30 seconds.
	
	// Maintain account reservations
	if (m_account_rez_mgr) {
		m_account_rez_mgr->PeriodicMaintenance();
	}
	
	CheckForExternalChanges();
	
	// Clean up players who have exceeded grace period after disconnecting from login server
	std::vector<uint32> expired_players;
	uint32 current_time = time(nullptr);
	
	for (const auto& pair : m_queued_players) {
		const QueueManagerEntry& entry = pair.second;
		uint32 account_id = pair.first;
		
		// Check if player is still connected to login server (not world server)
		bool still_connected = false;
		
		if (loginserver) {
			// Use stored LS account ID from queue entry (no database lookup needed)
			uint32 ls_account_id = entry.ls_account_id;
			
			// Add safety validation before calling IsAccountInGame
			if (ls_account_id > 0) {
				LogDebug("Checking if LS account [{}] is connected to world server", ls_account_id);
				
				try {
					// Re-enable the check with safety guards
					still_connected = client_list.IsAccountInGame(ls_account_id);
					LogDebug("IsAccountInGame result for LS account [{}]: {}", ls_account_id, still_connected);
				} catch (const std::exception& e) {
					LogError("Exception in IsAccountInGame for LS account [{}]: {}", ls_account_id, e.what());
					still_connected = false;
				} catch (...) {
					LogError("Unknown exception in IsAccountInGame for LS account [{}]", ls_account_id);
					still_connected = false;
				}
			} else {
				LogInfo("Skipping IsAccountInGame check - invalid LS account ID [{}] for world account [{}]", 
					ls_account_id, account_id);
				still_connected = false;
			}
			
			if (still_connected) {
				// Player is connected to world server but still in queue - this is wrong
				LogInfo("Player [{}] (LS: {}) connected to world server but still queued - removing from queue", 
					account_id, ls_account_id);
				// They should be removed from queue since they're already connected
				still_connected = false; // Force removal from queue
			}
		}
		
		if (still_connected) {
			// Player is connected - update their last_seen timestamp in queue entry
			// Note: We need to modify the entry in the map
			auto queue_it = m_queued_players.find(account_id);
			if (queue_it != m_queued_players.end()) {
				queue_it->second.last_seen = current_time;
			}
		} else {
			// Player is disconnected - check if they've exceeded grace period
			uint32 disconnected_time = current_time - entry.last_seen;
			if (disconnected_time > queue_grace_period) {
				expired_players.push_back(account_id);
			}
		}
	}
	
	// Remove players who have exceeded grace period
	for (uint32 account_id : expired_players) {
		auto it = m_queued_players.find(account_id);
		if (it != m_queued_players.end()) {
			in_addr addr;
			addr.s_addr = it->second.ip_address;
			std::string ip_str = inet_ntoa(addr);
			
			uint32 disconnected_time = current_time - it->second.last_seen;
			
			LogQueueAction("CLEANUP_GRACE_EXPIRED", account_id, 
				fmt::format("IP: {} position [{}] - disconnected for {}s (grace: {}s)", 
					ip_str, it->second.queue_position, disconnected_time, queue_grace_period));
			
			// Use RemoveFromQueue for consistent memory + DB removal
			RemoveFromQueue(account_id);
		}
	}

	// Event-driven approach: no periodic bulk database sync needed
	// Database is kept synchronized through immediate operations in AddToQueue/RemoveFromQueue
}

void QueueManager::ClearAllQueues()
{
	if (!m_queued_players.empty()) {
		uint32 count = GetTotalQueueSize();
		LogInfo("Clearing all queue entries for world server [{}] - removing [{}] players", 
			GetWorldServerName(), count);
		
		// 1. Clear memory
		m_queued_players.clear();
		
		// 2. Clear database immediately (event-driven)
		uint32 server_id = 1; // Fixed server ID for world server
		auto clear_query = fmt::format("DELETE FROM tblLoginQueue WHERE world_server_id = {}", server_id);
		auto result = database.QueryDatabase(clear_query);
		
		if (result.Success()) {
			LogInfo("Queue cleared - [{}] players removed (memory + DB)", count);
		} else {
			LogError("Queue cleared from memory but failed to clear database: {}", result.ErrorMessage());
		}
	}
}


// Private helper functions

// Encapsulated database operations
uint32 QueueManager::GetWorldAccountFromLS(uint32 ls_account_id) const
{
	uint32 world_account_id = database.GetAccountIDFromLSID(ls_account_id);
	if (world_account_id == 0) {
		// For new accounts that don't have world accounts yet, use LS account ID as fallback
		LogDebug("No world account mapping for LS account [{}] - using LS account ID as fallback", ls_account_id);
		return ls_account_id;
	}
	return world_account_id;
}

uint32 QueueManager::GetLSAccountFromWorld(uint32 world_account_id) const
{
	auto query = fmt::format("SELECT lsaccount_id FROM account WHERE id = {} LIMIT 1", world_account_id);
	uint32 ls_account_id = QuerySingleUint32(query, 0);
	
	if (ls_account_id > 0) {
		return ls_account_id;
	}
	
	// Fallback: assume world_account_id is actually the LS account ID for new accounts
	LogDebug("No LS account mapping for world account [{}] - using world account ID as fallback", world_account_id);
	return world_account_id;
}

// Database query helpers
bool QueueManager::ExecuteQuery(const std::string& query, const std::string& operation_desc) const
{
	auto results = database.QueryDatabase(query);
	if (!results.Success()) {
		LogError("Failed to {}: {}", operation_desc, results.ErrorMessage());
		return false;
	}
	return true;
}

uint32 QueueManager::QuerySingleUint32(const std::string& query, uint32 default_value) const
{
	auto results = database.QueryDatabase(query);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		if (row[0]) {
			try {
				return static_cast<uint32>(std::stoul(row[0]));
			} catch (const std::exception&) {
				LogError("Failed to parse uint32 from query result: {}", row[0]);
			}
		}
	}
	return default_value;
}

void QueueManager::UpdateLastSeen(uint32 account_id)
{
	// Update last seen timestamp if this account is in our queue
	auto it = m_queued_players.find(account_id);
	if (it != m_queued_players.end()) {
		it->second.last_seen = time(nullptr);
	}
}

void QueueManager::LogQueueAction(const std::string& action, uint32 account_id, const std::string& details) const
{
	std::string server_name = GetWorldServerName(); // Use helper function
	LogInfo("QueueManager [{}] - {}: Account [{}] {}", server_name, action, account_id, details);
}

// REMOVED: SendServerListUpdate method - now using targeted push updates instead of broadcasts
// void QueueManager::SendServerListUpdate() const
// {
// 	if (m_world_server) {
// 		server.client_manager->UpdateServerList();
// 	}
// }

void QueueManager::SendQueuedClientsUpdate() const {
	LogInfo("DEBUG: SendQueuedClientsUpdate called - checking queue state...");
	
	if (m_queued_players.empty()) {
		LogInfo("DEBUG: No queued players to update");
		return;
	}
	
	if (!loginserver) {
		LogError("DEBUG: Login server not available - cannot send queue updates");
		return;
	}
	
	if (!loginserver->Connected()) {
		LogError("DEBUG: Login server not connected - cannot send queue updates");
		return;
	}
	
	LogInfo("DEBUG: Found [{}] queued players, login server available - sending updates", m_queued_players.size());
	
	// FIRST: Send normal server list update to ALL clients (so non-queued players see current population)
	if (loginserver && loginserver->Connected()) {
		auto broadcast_pack = new ServerPacket(ServerOP_WorldListUpdate, 0);
		loginserver->SendPacket(broadcast_pack);
		delete broadcast_pack;
		LogDebug("Sent general server list update to all clients before queue-specific updates");
	}
	
	uint32 packets_sent = 0;
	uint32 skipped_invalid = 0;
	
	// Send individual direct update packets for each queued client
	for (const auto& pair : m_queued_players) {
		const QueueManagerEntry& entry = pair.second;
		
		// Only include clients with valid connection info
		if (entry.ip_address == 0 || entry.ls_account_id == 0) {
			skipped_invalid++;
			in_addr addr;
			addr.s_addr = entry.ip_address;
			LogInfo("DEBUG: Skipping queue update for account [{}] - invalid connection info (IP: {}, LS ID: {})", 
				entry.account_id, inet_ntoa(addr), entry.ls_account_id);
			continue;
		}
		
		// Send queue position update notification (not pre-built packet)
		auto direct_pack = new ServerPacket(ServerOP_QueueDirectUpdate, sizeof(ServerQueueDirectUpdate_Struct));
		ServerQueueDirectUpdate_Struct* direct_update = (ServerQueueDirectUpdate_Struct*)direct_pack->pBuffer;
		
		// Send queue position data - login server will rebuild server list
		direct_update->ls_account_id = entry.ls_account_id;
		direct_update->ip_address = entry.ip_address;
		direct_update->queue_position = entry.queue_position;
		direct_update->estimated_wait = entry.estimated_wait;
		
		LogInfo("DEBUG: Created packet - opcode: 0x{:X}, size: {}, LS ID: {}, position: {}", 
			ServerOP_QueueDirectUpdate, sizeof(ServerQueueDirectUpdate_Struct), 
			direct_update->ls_account_id, direct_update->queue_position);
		
		// Send to login server for client notification
		loginserver->SendPacket(direct_pack);
		LogInfo("DEBUG: SendPacket called successfully");
		
		delete direct_pack;
		
		packets_sent++;
		
		in_addr addr;
		addr.s_addr = entry.ip_address;
		LogInfo("DEBUG: Sent queue position update for account [{}] (LS: {}) IP [{}] position [{}] wait [{}]s", 
			entry.account_id, entry.ls_account_id, inet_ntoa(addr), entry.queue_position, entry.estimated_wait);
	}
	
	if (packets_sent > 0) {
		LogInfo("DEBUG: Successfully sent [{}] queue position update notifications to login server", packets_sent);
	}
	
	if (skipped_invalid > 0) {
		LogInfo("DEBUG: Skipped [{}] queue entries with invalid connection info", skipped_invalid);
	}
	
	LogInfo("DEBUG: SendQueuedClientsUpdate completed - sent: {}, skipped: {}, total queued: {}", 
		packets_sent, skipped_invalid, m_queued_players.size());
}

Database* QueueManager::GetDatabase() const
{
	return &database;  // Return address of global database object
}

bool QueueManager::IsAccountInGraceWhitelist(uint32 account_id) const
{
	uint32 current_time = time(nullptr);
	
	// Event-based cleanup: Remove expired entries while checking
	auto cleanup_query = fmt::format(
		"DELETE FROM account_grace_whitelist WHERE expires_at <= {}", 
		current_time
	);
	ExecuteQuery(cleanup_query, "cleanup expired grace whitelist entries");
	
	// Check if account is still in whitelist after cleanup
	auto query = fmt::format(
		"SELECT expires_at FROM account_grace_whitelist WHERE account_id = {} LIMIT 1", 
		account_id
	);
	
	uint32 expires_at = QuerySingleUint32(query, 0);
	bool is_whitelisted = (expires_at > 0);
	
	if (is_whitelisted) {
		LogInfo("QueueManager [{}] - GRACE_WHITELIST: Account [{}] found in grace period whitelist (expires in {}s)", 
			GetWorldServerName(), 
			account_id, expires_at - current_time);
	}
	
	return is_whitelisted;
}

void QueueManager::AutoConnectQueuedPlayer(const QueueManagerEntry& entry)
{
	if (!loginserver || !loginserver->Connected()) {
		LogError("Cannot auto-connect queued player - login server not available or not connected");
		return;
	}
	
	// Check if player is still connected to login server by sending auto-connect request
	// The login server will handle the actual connection logic and client verification
	auto autoconnect_pack = new ServerPacket(ServerOP_QueueAutoConnect, sizeof(ServerQueueAutoConnect_Struct));
	ServerQueueAutoConnect_Struct* sqac = (ServerQueueAutoConnect_Struct*)autoconnect_pack->pBuffer;
	
	sqac->loginserver_account_id = entry.ls_account_id;
	sqac->world_id = 0;  // Not used in this context
	sqac->from_id = entry.from_id;
	sqac->to_id = 0;  // Not used in this context
	sqac->ip_address = entry.ip_address;
	
	// Convert IP to string format
	in_addr addr;
	addr.s_addr = entry.ip_address;
	strncpy(sqac->ip_addr_str, inet_ntoa(addr), sizeof(sqac->ip_addr_str) - 1);
	sqac->ip_addr_str[sizeof(sqac->ip_addr_str) - 1] = '\0';
	
	strncpy(sqac->forum_name, entry.forum_name.c_str(), sizeof(sqac->forum_name) - 1);
	sqac->forum_name[sizeof(sqac->forum_name) - 1] = '\0';
	
	// Send auto-connect request to login server
	loginserver->SendPacket(autoconnect_pack);
	delete autoconnect_pack;
	
	LogInfo("Sent auto-connect request for account [{}] (LS Account: {}) (IP: {}) to login server", 
		entry.account_id, entry.ls_account_id, sqac->ip_addr_str);
}

void QueueManager::SendDialogToQueuedPlayer(uint32 account_id, const std::string& message)
{
	// Use encapsulated method to get LS account ID from world account ID
	uint32 ls_account_id = GetLSAccountFromWorld(account_id);
	
	if (!loginserver || !loginserver->Connected()) {
		LogInfo("Cannot send dialog to player [{}] - login server not available", account_id);
		return;
	}
	
	// Send system-wide message targeting specific player
	size_t message_len = message.length() + 1;
	auto dialog_pack = new ServerPacket(ServerOP_SystemwideMessage, sizeof(ServerSystemwideMessage) + message_len);
	ServerSystemwideMessage* swm = (ServerSystemwideMessage*)dialog_pack->pBuffer;
	
	swm->lsaccount_id = ls_account_id;
	memset(swm->key, 0, sizeof(swm->key));  // Key not needed for system messages
	swm->type = 13;  // System message type for dialog
	strcpy(swm->message, message.c_str());
	
	loginserver->SendPacket(dialog_pack);
	delete dialog_pack;
	
	LogInfo("Sent dialog to queued player [{}] (LS: {}): {}", account_id, ls_account_id, message);
}

void QueueManager::SendDialogToAllQueuedPlayers(const std::string& message)
{
	uint32 sent_count = 0;
	
	for (const auto& pair : m_queued_players) {
		uint32 account_id = pair.first;
		SendDialogToQueuedPlayer(account_id, message);
		sent_count++;
	}
	
	LogInfo("Sent dialog to [{}] queued players: {}", sent_count, message);
}

void QueueManager::AnnounceServerMaintenance(const std::string& maintenance_message)
{
	std::string full_message = fmt::format("SERVER ANNOUNCEMENT\n\n{}\n\nYou will remain in queue during maintenance.", 
		maintenance_message);
	SendDialogToAllQueuedPlayers(full_message);
}

void QueueManager::AnnounceQueueStatusUpdate()
{
	uint32 total_queued = GetTotalQueueSize();
	if (total_queued == 0) {
		return;
	}
	
	uint32 max_capacity = RuleI(Quarm, PlayerPopulationCap);
	uint32 current_population = GetEffectivePopulation();
	
	std::string status_message = fmt::format("QUEUE STATUS UPDATE\n\nTotal in queue: {}\nServer population: {}/{}\nEstimated wait: {} minutes", 
		total_queued, current_population, max_capacity, (total_queued * 2)); // Rough estimate: 2 min per position
	
	SendDialogToAllQueuedPlayers(status_message);
}

uint32 QueueManager::GetEffectivePopulation()
{
	// Integrated account reservation manager logic for single source of truth
	
	// Safety check - if account reservation manager not initialized yet, use fallback
	if (!m_account_rez_mgr) {
		LogInfo("AccountRezMgr not initialized yet - returning numplayers: {}", numplayers);
		return numplayers;
	}
	
	// Fast path: If queue system is disabled, just return base numplayers
	if (!m_account_rez_mgr->IsQueueEnabled()) {
		LogInfo("Queue system disabled - returning numplayers: {}", numplayers);
		return numplayers;
	}
	
	// Base population from account reservations
	uint32 base_population = m_account_rez_mgr->Total();
	
	// Add pending queue connections (auto-connects in progress)
	uint32 pending_count = static_cast<uint32>(m_pending_connections.size());
	
	// Add test offset for simulation purposes (integrated from AccountRezMgr)
	uint32 test_offset = QuerySingleUint32(AccountRezMgr::test_population_offset_query, 0);
	
	// Calculate final effective population
	uint32 effective_population = base_population + pending_count + test_offset;
	
	// Debug: Show population calculation breakdown
	LogInfo("Account reservations: {}, pending connections: {}, test offset: {}, effective total: {}", 
		base_population, pending_count, test_offset, effective_population);
	
	// Sync population to database for login server reporting (integrated from AccountRezMgr)
	auto sync_query = fmt::format(fmt::runtime(AccountRezMgr::population_sync_query_format), effective_population, effective_population);
	if (ExecuteQuery(sync_query, "sync population to database")) {
		LogInfo("Population synced with login server. Effective population: {}", effective_population);
	}
	
	return effective_population;
}

uint32 QueueManager::GetWorldPopulation() const
{
	extern uint32 numplayers;
	return numplayers; // Use global world server player count
}

void QueueManager::MarkConnectionPending(uint32 account_id)
{
	m_pending_connections[account_id] = time(nullptr);
	LogInfo("Marked account [{}] as pending connection - total pending: [{}]", 
		account_id, m_pending_connections.size());
}

bool QueueManager::IsConnectionPending(uint32 account_id) const
{
	return m_pending_connections.find(account_id) != m_pending_connections.end();
}

uint32 QueueManager::GetPendingConnectionCount() const
{
	return static_cast<uint32>(m_pending_connections.size());
}



void QueueManager::SendQueueAuthorization(uint32 account_id)
{
	if (!loginserver || !loginserver->Connected()) {
		LogError("Cannot send queue authorization for account [{}] - login server not available", account_id);
		return;
	}
	
	// Send authorization packet to login server
	auto auth_pack = new ServerPacket(ServerOP_QueueAuthorization, sizeof(ServerQueueAuthorization_Struct));
	ServerQueueAuthorization_Struct* sqas = (ServerQueueAuthorization_Struct*)auth_pack->pBuffer;
	
	sqas->account_id = account_id;
	sqas->authorization_timestamp = static_cast<uint32>(time(nullptr));
	sqas->timeout_seconds = 300; // 5 minutes to connect
	
	loginserver->SendPacket(auth_pack);
	delete auth_pack;
	
	LogInfo("Sent queue authorization for account [{}] - valid for [{}] seconds", account_id, sqas->timeout_seconds);
}


void QueueManager::ProcessQueueAutoConnect(uint16_t opcode, const EQ::Net::Packet& p)
{
	// In world server context, this method is not used the same way
	// The world server SENDS auto-connect requests, it doesn't process them
	// This method exists for interface compatibility but logs that it's not used
	
	LogInfo("ProcessQueueAutoConnect called on world server - this is for interface compatibility only");
	LogInfo("World server sends auto-connect requests via AutoConnectQueuedPlayer method instead");
} 

void QueueManager::SendClientAuth(std::string ip, std::string account, std::string key, unsigned int account_id, uint8 version)
{
	if (!loginserver || !loginserver->Connected()) {
		LogError("Cannot send client auth for account [{}] - login server not available", account_id);
		return;
	}
	
	// Send client authentication packet to login server
	auto auth_pack = new ServerPacket(ServerOP_LSClientAuth, sizeof(ClientAuth));
	ClientAuth* client_auth = (ClientAuth*)auth_pack->pBuffer;
	
	client_auth->loginserver_account_id = account_id;
	strncpy(client_auth->account_name, account.c_str(), sizeof(client_auth->account_name) - 1);
	client_auth->account_name[sizeof(client_auth->account_name) - 1] = '\0';
	
	strncpy(client_auth->key, key.c_str(), sizeof(client_auth->key) - 1);
	client_auth->key[sizeof(client_auth->key) - 1] = '\0';
	
	client_auth->lsadmin = 0;
	client_auth->is_world_admin = 0;
	client_auth->ip_address = inet_addr(ip.c_str());
	client_auth->version = version;
	
	// Check if client is from local network
	std::string world_address = "127.0.0.1";  // Default fallback
	if (loginserver && loginserver->Connected()) {
		// Get world address from configuration if available
		const WorldConfig* config = WorldConfig::get();
		if (!config->WorldAddress.empty()) {
			world_address = config->WorldAddress;
		}
	}
	
	if (ip.compare(world_address) == 0) {
		client_auth->is_client_from_local_network = 1;
	} else if (IpUtil::IsIpInPrivateRfc1918(ip)) {
		LogInfo("Client is authenticating from a local address [{}]", ip);
		client_auth->is_client_from_local_network = 1;
	} else {
		client_auth->is_client_from_local_network = 0;
	}
	
	// Forum name - empty for world server context
	memset(client_auth->forum_name, 0, sizeof(client_auth->forum_name));
	
	loginserver->SendPacket(auth_pack);
	delete auth_pack;
	
	LogInfo("Sent client authentication to login server for account [{}] ({})", account_id, ip);
} 

// DATABASE QUEUE OPERATIONS - Moved from loginserver Database class

void QueueManager::SaveQueueEntry(uint32 account_id, uint32 world_server_id, uint32 queue_position, uint32 estimated_wait, uint32 ip_address)
{
	auto query = fmt::format(
		"REPLACE INTO tblLoginQueue (account_id, world_server_id, queue_position, estimated_wait, ip_address) "
		"VALUES ({}, {}, {}, {}, {})",
		account_id, world_server_id, queue_position, estimated_wait, ip_address
	);

	ExecuteQuery(query, fmt::format("save queue entry for account {} on world server {}", account_id, world_server_id));
}

void QueueManager::RemoveQueueEntry(uint32 account_id, uint32 world_server_id)
{
	auto query = fmt::format(
		"DELETE FROM tblLoginQueue WHERE account_id = {} AND world_server_id = {}",
		account_id, world_server_id
	);

	ExecuteQuery(query, fmt::format("remove queue entry for account {} on world server {}", account_id, world_server_id));
}

bool QueueManager::LoadQueueEntries(uint32 world_server_id, std::vector<std::tuple<uint32, uint32, uint32, uint32>>& queue_entries)
{
	auto query = fmt::format(
		"SELECT account_id, queue_position, estimated_wait, ip_address "
		"FROM tblLoginQueue WHERE world_server_id = {} "
		"ORDER BY queue_position ASC",
		world_server_id
	);

	auto results = database.QueryDatabase(query);  // Use world server global database
	if (!results.Success()) {
		LogError("Failed to load queue entries for world server {}", world_server_id);
		return false;
	}

	queue_entries.clear();
	for (auto row = results.begin(); row != results.end(); ++row) {
		uint32 account_id = atoi(row[0]);
		uint32 queue_position = atoi(row[1]);
		uint32 estimated_wait = atoi(row[2]);
		uint32 ip_address = atoi(row[3]);
		
		queue_entries.emplace_back(account_id, queue_position, estimated_wait, ip_address);
	}

	LogInfo("Loaded {} queue entries for world server {}", queue_entries.size(), world_server_id);
	return true;
}

void QueueManager::RemovePlayerOnDisconnect(uint32 account_id)
{
	// Remove player from this world server's queue when they disconnect from login server
	if (IsAccountQueued(account_id)) {
		RemoveFromQueue(account_id);
		LogInfo("Removed account [{}] from queue due to disconnection from login server", account_id);
	}
}

void QueueManager::FullPeriodicMaintenance()
{
	// Enhanced periodic maintenance that includes queue advancement
	// This replaces the functionality that was previously done through server iteration
	
	LogDebug("Starting full periodic maintenance for queue manager");
	
	// 1. Standard maintenance (cleanup, database sync, etc.)
	PeriodicMaintenance();
	
	// 2. Queue advancement (move players forward in queue)
	UpdateQueuePositions();
	
	LogDebug("Completed full periodic maintenance for queue manager");
} 