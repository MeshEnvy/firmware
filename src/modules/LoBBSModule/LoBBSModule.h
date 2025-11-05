#pragma once

#include "SinglePortModule.h"
#include "lodb/LoDB.h"
#include "TextMessageSender.h"
#include "concurrency/OSThreadWorkerPool.h"
#include "lobbs.pb.h"
#include <vector>
#include <string>

/**
 * LoBBS (Lo-Fi Bulletin Board System) Module
 * 
 * A simple BBS-style messaging system for Meshtastic.
 * Handles text messages on TEXT_MESSAGE_APP port.
 * 
 * Uses LoDB for storage:
 * - Users table: /lodb/lobbs/users/<username>.pr
 * - Sessions table: /lodb/lobbs/sessions/<nodeid_hex>.pr
 */
class LoBBSModule : public SinglePortModule
{
  public:
    LoBBSModule();

  protected:
    /**
     * Handle an incoming message
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    // LoDB database instance
    LoDb *db;
    
    TextMessageSender *messageSender;
    concurrency::OSThreadWorkerPool *workerPool;
    
    // Host node ID used as salt for user UUID generation
    uint32_t hostNodeId;

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
     * Hash a password using SHA256
     */
    static void hashPassword(const char *password, uint8_t *hash);

    /**
     * Send a text reply to a node (immediate, single message)
     */
    void sendTextReply(uint32_t toNode, const char *message);

    /**
     * Send a potentially large message (queued, auto-fragmented)
     */
    void sendLargeMessage(uint32_t toNode, const std::string &message);
};

