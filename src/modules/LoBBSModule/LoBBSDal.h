#pragma once

#include "lobbs.pb.h"
#include "lodb/LoDB.h"
#include <stdint.h>

// Maximum username length (from lobbs.options: meshtastic.LoBBSUser.username max_size:32)
#define LOBBS_MAX_USERNAME_LEN 32
#define LOBBS_USERNAME_BUFFER_SIZE (LOBBS_MAX_USERNAME_LEN + 1) // +1 for null terminator

// Stringify macro for converting numeric defines to string literals
#define LOBBS_XSTR(x) LOBBS_STR(x)
#define LOBBS_STR(x) #x

/**
 * LoBBS Data Access Layer
 *
 * Handles all database operations for LoBBS:
 * - User management (creation, lookup, authentication)
 * - Session management (login, logout)
 * - Password hashing and verification
 */
class LoBBSDal
{
  public:
    /**
     * Constructor
     * @param hostNodeId Node ID used as salt for UUID generation
     */
    LoBBSDal(uint32_t hostNodeId);

    /**
     * Destructor
     */
    ~LoBBSDal();

    /**
     * Validate username format
     */
    bool isValidUsername(const char *username);

    /**
     * Validate password format
     */
    bool isValidPassword(const char *password);

    /**
     * Load user by username from LoDB
     */
    bool loadUserByUsername(const char *username, meshtastic_LoBBSUser *user);

    /**
     * Load user by node ID (via session lookup)
     */
    bool loadUserByNodeId(uint32_t nodeId, meshtastic_LoBBSUser *user);

    /**
     * Create a new user account
     */
    bool createUser(const char *username, const char *password, uint32_t nodeId);

    /**
     * Verify password for a user
     */
    bool verifyPassword(const meshtastic_LoBBSUser *user, const char *password);

    /**
     * Log in a user (create session)
     */
    bool loginUser(const char *username, uint32_t nodeId);

    /**
     * Log out a user (delete session)
     */
    bool logoutUser(uint32_t nodeId);

    /**
     * Get the underlying database instance
     */
    LoDb *getDb() { return db; }

  private:
    /**
     * Hash a password using SHA256
     */
    static void hashPassword(const char *password, uint8_t *hash);

    // LoDB database instance
    LoDb *db;

    // Host node ID used as salt for user UUID generation
    uint32_t hostNodeId;
};
