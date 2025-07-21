## ⏲️ EQMacEmu World Server Timer Flow - Queue Management

### **Timer Definitions & Initialization:**
📁🌍 `world/main.cpp`
```cpp
Timer QueueAdvancementTimer(3000);  // 3 seconds - queue progression timer
```
└─ ⏲️ **QueueAdvancementTimer.Start()** 
    └─ Begins 3-second cycle in main loop

### **Main Timer Check Loop (Every 3 seconds):**
📁🌍 `world/main.cpp` [loop_fn]
```cpp
if (QueueAdvancementTimer.Check()) {
    queue_manager.UpdateQueuePositions();
    queue_manager.PeriodicMaintenance(); 
}
```

## 🔄 **Primary Timer Flow (Every 3 seconds):**
```
📁🌍 `world/main.cpp`
└─ ⏲️ **QueueAdvancementTimer.Check()** → ✅ *Returns true every 3000ms*
    ↓
    └─📁🌍 `queue_manager.cpp`
        └─ **[UpdateQueuePositions]** 🚀 *Queue progression & auto-connect logic*
            └─ ❔ Check if queue paused → `RuleB(World, FreezeQueue)`
            └─ ❔ Check if manually frozen → `m_queue_paused`
            └─ 🔍 **Check pending connections** 
                └─ ✅ `client_list.ActiveConnection()` → Remove from queue
                └─ ⏰ Grace period expired → Allow re-queue
            └─ 🎯 **Process position 1 players**
                └─ ✅ Server has capacity → Auto-connect
                └─ ❌ Server at capacity → Wait for capacity
            └─ 📈 **Advance queue positions**
                └─ Movement: `1 + (rand() % 3)` positions forward
                └─ Update estimated wait times: `position * 60` seconds
            └─ 📡 **[SendQueuedClientsUpdate]** → Notify login server
        ↓
        └─ **[PeriodicMaintenance]** 🧹 *Cleanup & sync operations*
            ↓
            └─📁🌍 `account_reservation_manager.cpp`
                └─ **[m_account_rez_mgr.PeriodicMaintenance]** 🏠 *Account tracking*
                    └─ 🧹 **[CleanupStaleConnections]**
                        └─ ✅ `client_list.ActiveConnection()` → Update last_seen
                        └─ ❌ No active char → Check grace period
                        └─ ⏰ Grace expired → Remove reservation
                        └─ 🛡️ **Update grace whitelist** → Queue bypass
                    └─ 💾 **[SyncAllConnectionsToDatabase]** *(Every 5 minutes)*
                        └─ Clear `active_account_connections` table
                        └─ Re-insert all current reservations
                        └─ 🛟 Crash recovery data preservation
            ↓
            └─ 🔍 **[CheckForExternalChanges]**
                └─ Query `tblloginserversettings` → RefreshQueue flag
                └─ Flag = "1" → **[RestoreQueueFromDatabase]**
                └─ Reset flag to "0" → Signal processed
            └─ 🧹 **Queue player cleanup**
                └─ ❌ Player disconnected from login server
                └─ ⏰ Exceeded queue grace period (30s)
                └─ **[RemoveFromQueue]** → Memory + DB removal
```
## 📊 **Population Tracking Integration:**
```
📁🌍 `queue_manager.cpp` [GetEffectivePopulation]
└─ 🏠 **Account Reservations**: `m_account_rez_mgr.Total()`
    └─ Players with active reservations (60s normal, 600s raid grace)
└─ ⏳ **Pending Connections**: `m_pending_connections.size()`
    └─ Auto-connects in progress (30s grace period)
└─ 🧪 **Test Offset**: `World:TestPopulationOffset` rule
    └─ Simulation offset for testing queue behavior
└─ ➡️ **Sync to DB**: Update `server_population` table
    └─ 📡 Login server reads this for server list population display
```
## ⚡ **Real-time Updates Flow:**
```
📁🌍 `queue_manager.cpp` [SendQueuedClientsUpdate]
└─ 📡 **For each queued player:**
    └─ Create `ServerOP_QueueDirectUpdate` packet
    └─ Send to login server with: `ls_account_id`, `queue_position`, `estimated_wait`
    ↓
    └─📁📨 `loginserver/world_server.cpp`
        └─ **[ProcessQueueDirectUpdate]**
            └─ Find client by `ls_account_id`
            └─ **[SendServerListPacket]** → Push update to specific client
            └─ 🎮 Client sees updated queue position immediately
```
## 🔄 **Timer Dependencies & Intervals:**

| Timer | Interval | Purpose | Location |
|-------|----------|---------|----------|
| **QueueAdvancementTimer** | **3000ms** | Queue progression & maintenance | `world/main.cpp` |
| EQTimeTimer | 600000ms (10min) | EQ time updates | `world/main.cpp` |
| InterserverTimer | 10000ms | DB ping/reconnect | `world/main.cpp` |
| process_timer | 32ms | Main event loop | `world/main.cpp` |
| m_statusupdate_timer | 15000ms | Send status to login server | `world/login_server.cpp` |

## 🚨 **Error Conditions & Fallbacks:**

📁🌍 `queue_manager.cpp`
└─ **Queue Paused** → `m_queue_paused = true`
    └─ No position advancement, players remain queued
└─ **Queue Frozen** → `RuleB(World, FreezeQueue) = true`
    └─ Manual freeze via rule, positions locked
└─ **Database Unavailable** → Event-driven fallback
    └─ Memory operations continue, DB sync resumes when available
└─ **Login Server Disconnected** → Queue operations continue
    └─ Position updates resume when connection restored

## 🎯 **Key Performance Optimizations:**

- **Event-driven DB mirroring**: Immediate sync on queue changes, not bulk periodic
- **Targeted client updates**: Only send updates to queued clients, not broadcasts  
- **Connection state caching**: Pending connections tracked in memory for 30s grace
- **Rule caching**: Queue enabled status cached to avoid repeated DB queries
- **Grace period optimization**: Expired entries cleaned up during normal checks

## 🔄 **Auto-Connect Sequence:**

```
⏲️ Timer triggers (3s) → UpdateQueuePositions() → Player at position 1
    ↓
✅ Server has capacity → SendQueueAuthorization() → AutoConnectQueuedPlayer()
    ↓  
📡 ServerOP_QueueAutoConnect → Login Server → SendUserToWorldRequest()
    ↓
🎮 Client automatically connects → Remove from queue when confirmed
``` 