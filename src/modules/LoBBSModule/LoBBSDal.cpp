#include "LoBBSDal.h"
#include "configuration.h"
#include "gps/RTC.h"

#include <SHA256.h>
#include <cctype>
#include <cstring>

LoBBSDal::LoBBSDal(uint32_t hostNodeId) : hostNodeId(hostNodeId)
{
    // Initialize LoDB database
    db = new LoDb("lobbs");
    db->registerTable("users", &meshtastic_LoBBSUser_msg, sizeof(meshtastic_LoBBSUser));
    db->registerTable("sessions", &meshtastic_LoBBSSession_msg, sizeof(meshtastic_LoBBSSession));
}

LoBBSDal::~LoBBSDal()
{
    delete db;
}

bool LoBBSDal::isValidUsername(const char *username)
{
    // Must start with a letter
    if (!isalpha(username[0])) {
        return false;
    }

    // Check rest of characters: alphanumeric or underscore only
    for (size_t i = 1; username[i] != '\0'; i++) {
        if (!isalnum(username[i]) && username[i] != '_') {
            return false;
        }
    }
    return true;
}

/**
 * Validate a password. Must be between 5 and 50 characters long and contain only letters, numbers, underscores, and common
 * special characters.
 * @param password The password to validate
 * @return True if the password is valid, false otherwise
 */
bool LoBBSDal::isValidPassword(const char *password)
{
    // Check each character: alphanumeric, underscore, or common special chars
    for (size_t i = 0; password[i] != '\0'; i++) {
        char c = password[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '!' && c != '@' && c != '#' && c != '$' && c != '%') {
            return false;
        }
    }
    return true;
}

void LoBBSDal::hashPassword(const char *password, uint8_t *hash)
{
    SHA256 sha256;
    sha256.reset();
    sha256.update(password, strlen(password));
    sha256.finalize(hash, 32);
}

bool LoBBSDal::loadUserByUsername(const char *username, meshtastic_LoBBSUser *user)
{
    // Convert username to UUID with host node ID as salt
    lodb_uuid_t userUuid = lodb_new_uuid(username, hostNodeId);
    LoDbError err = db->get("users", userUuid, user);
    if (err == LODB_OK) {
        LOG_DEBUG("Loaded user by username: %s", username);
        return true;
    }
    LOG_DEBUG("User not found: %s", username);
    return false;
}

bool LoBBSDal::loadUserByNodeId(uint32_t nodeId, meshtastic_LoBBSUser *user)
{
    // First, lookup session by node ID (use node ID directly as UUID)
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;
    LOG_DEBUG("sessionUuid: " LODB_UUID_FMT, LODB_UUID_ARGS(sessionUuid));

    meshtastic_LoBBSSession session = meshtastic_LoBBSSession_init_zero;
    LoDbError err = db->get("sessions", sessionUuid, &session);
    if (err != LODB_OK) {
        LOG_DEBUG("No session found for node 0x%08x", nodeId);
        return false;
    } else {
        LOG_DEBUG("Session found for node 0x%08x", nodeId);
        LOG_DEBUG("Session user UUID: " LODB_UUID_FMT, LODB_UUID_ARGS(session.user_uuid));
        LOG_DEBUG("Session last login time: %d", session.last_login_time);
        LOG_DEBUG("Session node ID: %08x", nodeId);
    }

    // Now load the user by UUID from session
    err = db->get("users", session.user_uuid, user);
    if (err == LODB_OK) {
        LOG_DEBUG("Loaded user by node ID: 0x%08x -> UUID: " LODB_UUID_FMT, nodeId, LODB_UUID_ARGS(session.user_uuid));
        return true;
    }
    LOG_DEBUG("User UUID not found: " LODB_UUID_FMT, LODB_UUID_ARGS(session.user_uuid));
    return false;
}

bool LoBBSDal::createUser(const char *username, const char *password, uint32_t nodeId)
{
    // Calculate UUID first (username with host node ID as salt)
    lodb_uuid_t userUuid = lodb_new_uuid(username, hostNodeId);

    // Create user record
    meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
    strncpy(user.username, username, sizeof(user.username) - 1);
    user.uuid = userUuid;
    user.password_hash.size = 32;
    hashPassword(password, user.password_hash.bytes);
    LoDbError err = db->insert("users", userUuid, &user);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to create user: %s", username);
        return false;
    }

    LOG_INFO("Created user: %s", username);

    // Log in the user (create session)
    return loginUser(username, nodeId);
}

bool LoBBSDal::verifyPassword(const meshtastic_LoBBSUser *user, const char *password)
{
    uint8_t providedHash[32];
    hashPassword(password, providedHash);
    return memcmp(user->password_hash.bytes, providedHash, 32) == 0;
}

bool LoBBSDal::loginUser(const char *username, uint32_t nodeId)
{
    // Create session record
    meshtastic_LoBBSSession session = meshtastic_LoBBSSession_init_zero;
    session.user_uuid = lodb_new_uuid(username, hostNodeId);
    session.node_id = nodeId;
    session.last_login_time = getTime();

    // Use node ID directly as UUID
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;

    // Delete existing session if any (upsert pattern)
    db->deleteRecord("sessions", sessionUuid);

    // Insert new session
    LoDbError err = db->insert("sessions", sessionUuid, &session);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to create session for node 0x%08x", nodeId);
        return false;
    }

    LOG_INFO("Created session for user %s (UUID: " LODB_UUID_FMT ") on node 0x%08x", username, LODB_UUID_ARGS(session.user_uuid),
             nodeId);
    return true;
}

bool LoBBSDal::logoutUser(uint32_t nodeId)
{
    // Use node ID directly as UUID
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;

    LoDbError err = db->deleteRecord("sessions", sessionUuid);
    if (err == LODB_OK) {
        LOG_INFO("Logged out node 0x%08x", nodeId);
        return true;
    }
    LOG_WARN("No session found to log out for node 0x%08x", nodeId);
    return false;
}
