/**
 * LoDB Example Usage - Cursor-based Cooperative API
 * 
 * This file demonstrates how to use the LoDB cursor-based API for truly cooperative
 * iteration with OSThreads. Each cursor advance processes one record and returns,
 * allowing other OSThreads to run.
 */

#include "LoDB.h"
#include "modules/LoBBSModule/lobbs.pb.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include <cstring>

// Example: Simple operations (insert, get, update, delete)
void lodb_example_simple()
{
    LOG_INFO("=== LoDB Example: Simple Operations ===");

    // Initialize table
    LoDbTable usersTable;
    LoDbError err = lodb_init_table(&usersTable, "example_users", &meshtastic_LoBBSUser_msg,
                                    sizeof(meshtastic_LoBBSUser));
    if (err != LODB_OK) {
        LOG_ERROR("Failed to initialize table");
        return;
    }

    // Insert a user
    meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
    strncpy(user.username, "alice", sizeof(user.username) - 1);
    user.uid = 1001;

    char uuid[LODB_UUID_LEN];
    err = lodb_insert(&usersTable, &user, uuid);
    if (err == LODB_OK) {
        LOG_INFO("✓ Inserted user 'alice' with UUID: %s", uuid);
    }

    // Get the user
    meshtastic_LoBBSUser retrieved;
    err = lodb_get(&usersTable, uuid, &retrieved);
    if (err == LODB_OK) {
        LOG_INFO("✓ Retrieved user: %s (UID: %u)", retrieved.username, retrieved.uid);
    }

    // Update the user
    retrieved.uid = 2001;
    err = lodb_update(&usersTable, uuid, &retrieved);
    if (err == LODB_OK) {
        LOG_INFO("✓ Updated user UID to 2001");
        
        // Verify update
        lodb_get(&usersTable, uuid, &retrieved);
        LOG_INFO("  Verified UID: %u", retrieved.uid);
    }

    // Delete the user
    err = lodb_delete(&usersTable, uuid);
    if (err == LODB_OK) {
        LOG_INFO("✓ Deleted user");
    }
}

// Example: Cursor-based iteration (cooperative)
void lodb_example_cursor()
{
    LOG_INFO("=== LoDB Example: Cursor-based Select ===");

    // Initialize table
    LoDbTable usersTable;
    lodb_init_table(&usersTable, "example_users_cursor", &meshtastic_LoBBSUser_msg,
                    sizeof(meshtastic_LoBBSUser));

    // Insert some test users
    for (int i = 0; i < 5; i++) {
        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        snprintf(user.username, sizeof(user.username), "user%d", i);
        user.uid = 1000 + i;

        char uuid[LODB_UUID_LEN];
        lodb_insert(&usersTable, &user, uuid);
        LOG_INFO("Inserted: %s (UID: %u, UUID: %s)", user.username, user.uid, uuid);
    }

    // Select all users with cursor
    LOG_INFO("--- Selecting all users ---");
    auto cursor = lodb_select_cursor(&usersTable, nullptr, nullptr);
    int count = 0;
    
    // Truly cooperative: one file per call!
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)lodb_cursor_get(cursor);
            const char *uuid = lodb_cursor_get_uuid(cursor);
            LOG_INFO("  [%d] %s (UID: %u, UUID: %s)", ++count, u->username, u->uid, uuid);
        }
        // Note: In OSThread runOnce(), you'd return here for cooperation
    }
    
    lodb_cursor_close(cursor);
    LOG_INFO("✓ Iterated through %d users", count);

    // Select with filter
    LOG_INFO("--- Selecting users with UID >= 1002 ---");
    auto filter = [](const void *rec, void *ctx) -> bool {
        const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)rec;
        return u->uid >= 1002;
    };

    cursor = lodb_select_cursor(&usersTable, filter, nullptr);
    count = 0;
    
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)lodb_cursor_get(cursor);
            LOG_INFO("  [%d] %s (UID: %u)", ++count, u->username, u->uid);
        }
    }
    
    lodb_cursor_close(cursor);
    LOG_INFO("✓ Found %d matching users", count);

    // Cleanup: delete all records
    cursor = lodb_select_cursor(&usersTable, nullptr, nullptr);
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            const char *uuid = lodb_cursor_get_uuid(cursor);
            lodb_delete(&usersTable, uuid);
        }
    }
    lodb_cursor_close(cursor);
}

// Example: Cooperative OSThread that processes records one at a time
class UserProcessorThread : public concurrency::OSThread
{
  private:
    LoDbTable *table;
    LoDbCursor *cursor;
    int processed_count;

  public:
    UserProcessorThread(LoDbTable *t) : OSThread("UserProcessor"), table(t), cursor(nullptr), processed_count(0) {}

    int32_t runOnce() override
    {
        // Start cursor if not already started
        if (!cursor) {
            LOG_INFO("Starting user processing...");
            cursor = lodb_select_cursor(table, nullptr, nullptr);
            if (!cursor) {
                LOG_ERROR("Failed to create cursor");
                return 5000; // Try again in 5 seconds
            }
        }

        // Check if exhausted (done processing)
        if (lodb_cursor_is_exhausted(cursor)) {
            LOG_INFO("✓ Finished processing %d users", processed_count);
            lodb_cursor_close(cursor);
            cursor = nullptr;
            processed_count = 0;
            return 10000; // Wait 10 seconds before next scan
        }

        // Process ONE file per runOnce() call (truly cooperative!)
        if (lodb_cursor_next(cursor)) {
            // Found a matching record - process it
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)lodb_cursor_get(cursor);
            const char *uuid = lodb_cursor_get_uuid(cursor);
            
            LOG_INFO("Processing user %s (UID: %u, UUID: %s)", u->username, u->uid, uuid);
            processed_count++;
        }
        
        // Return ASAP for next file (match or no match)
        // Other OSThreads get scheduled between each file!
        return 0;
    }
};

// Example: Batch update using cooperative cursor
void lodb_example_batch_update()
{
    LOG_INFO("=== LoDB Example: Batch Update (Cooperative) ===");

    // Setup table with data
    LoDbTable usersTable;
    lodb_init_table(&usersTable, "example_batch", &meshtastic_LoBBSUser_msg,
                    sizeof(meshtastic_LoBBSUser));

    // Insert users
    for (int i = 0; i < 3; i++) {
        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        snprintf(user.username, sizeof(user.username), "user%d", i);
        user.uid = 100 + i;
        char uuid[LODB_UUID_LEN];
        lodb_insert(&usersTable, &user, uuid);
    }

    // Update all users: increment UID by 1000
    LOG_INFO("--- Updating all users (increment UID by 1000) ---");
    auto cursor = lodb_select_cursor(&usersTable, nullptr, nullptr);
    int updated = 0;
    
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            // Get current record
            meshtastic_LoBBSUser user;
            memcpy(&user, lodb_cursor_get(cursor), sizeof(user));
            const char *uuid = lodb_cursor_get_uuid(cursor);
            
            // Modify
            uint32_t old_uid = user.uid;
            user.uid += 1000;
            
            // Update
            lodb_update(&usersTable, uuid, &user);
            LOG_INFO("  Updated %s: UID %u → %u", user.username, old_uid, user.uid);
            updated++;
        }
        // In a real OSThread, return here for cooperation
        // For this example, we'll continue (but you get the idea!)
    }
    
    lodb_cursor_close(cursor);
    LOG_INFO("✓ Updated %d users", updated);

    // Verify updates
    LOG_INFO("--- Verifying updates ---");
    cursor = lodb_select_cursor(&usersTable, nullptr, nullptr);
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)lodb_cursor_get(cursor);
            LOG_INFO("  %s: UID = %u", u->username, u->uid);
        }
    }
    lodb_cursor_close(cursor);

    // Cleanup
    cursor = lodb_select_cursor(&usersTable, nullptr, nullptr);
    while (!lodb_cursor_is_exhausted(cursor)) {
        if (lodb_cursor_next(cursor)) {
            lodb_delete(&usersTable, lodb_cursor_get_uuid(cursor));
        }
    }
    lodb_cursor_close(cursor);
}

// Uncomment to run examples (e.g., from setup() or a module)
/*
void setup_lodb_examples() {
    lodb_example_simple();
    lodb_example_cursor();
    lodb_example_batch_update();
    
    // For OSThread example, you'd create it like:
    // LoDbTable usersTable;
    // lodb_init_table(&usersTable, "users", &meshtastic_LoBBSUser_msg, sizeof(meshtastic_LoBBSUser));
    // new UserProcessorThread(&usersTable);  // Runs cooperatively!
}
*/
