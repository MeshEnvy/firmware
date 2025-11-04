#pragma once

#include "lobbs.pb.h"
#include <cstdint>

/**
 * LoBBSDb - Database operations for LoBBS user management
 * 
 * Handles user authentication, storage, and retrieval using LittleFS
 */
class LoBBSDb
{
  public:
    LoBBSDb();

    /**
     * Load a user by username
     * @param username Username to look up
     * @param user Output: User record if found
     * @return true if user found and loaded successfully
     */
    bool loadUserByUsername(const char *username, meshtastic_LoBBSUser *user);

    /**
     * Load a user by node ID
     * @param nodeId Node ID to look up
     * @param user Output: User record if found
     * @return true if user found and loaded successfully
     */
    bool loadUserByNodeId(uint32_t nodeId, meshtastic_LoBBSUser *user);

    /**
     * Save a user record and associate with a node ID
     * Creates/updates user file and indexes
     * @param user User record to save
     * @param nodeId Node ID to associate with this user
     * @return true if saved successfully
     */
    bool saveUser(const meshtastic_LoBBSUser *user, uint32_t nodeId);

    /**
     * Create a new user account
     * @param username Username for the account
     * @param password Plain text password (will be hashed)
     * @param nodeId Node ID to associate with this user
     * @param user Output: Created user record
     * @return true if user created successfully
     */
    bool createUser(const char *username, const char *password, uint32_t nodeId, meshtastic_LoBBSUser *user);

    /**
     * Verify a user's password
     * @param user User record to verify against
     * @param password Plain text password to verify
     * @return true if password matches
     */
    bool verifyPassword(const meshtastic_LoBBSUser *user, const char *password);

    /**
     * Log out a node ID (remove node ID association)
     * User account remains, just removes the node->user mapping
     * @param nodeId Node ID to log out
     * @return true if logged out successfully
     */
    bool logoutNodeId(uint32_t nodeId);

    /**
     * Hash a password using SHA256
     * @param password Plain text password
     * @param hash Output: 32-byte hash
     */
    static void hashPassword(const char *password, uint8_t *hash);

    /**
     * Generate a deterministic UID from a username
     * @param username Username to hash
     * @return 32-bit user ID
     */
    static uint32_t generateUidFromUsername(const char *username);

  private:
    // Filesystem paths
    static constexpr const char *LOBBS_USERS_DIR = "/lobbs/users";
    static constexpr const char *LOBBS_INDEX_USERNAME_DIR = "/lobbs/indexes/uid_by_username";
    static constexpr const char *LOBBS_INDEX_NODEID_DIR = "/lobbs/indexes/uid_by_nodeid";

    /**
     * Ensure LoBBS directories exist
     */
    void createDirectories();

    /**
     * Load a user by UID
     * @param uid User ID
     * @param user Output: User record if found
     * @return true if user found and loaded successfully
     */
    bool loadUserByUid(uint32_t uid, meshtastic_LoBBSUser *user);
};

