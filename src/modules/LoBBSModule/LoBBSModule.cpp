#include "LoBBSModule.h"
#include "CommandParser.h"
#include "MeshService.h"
#include "configuration.h"
#include "airtime.h"
#include "gps/RTC.h"
#include "lobbs.pb.h"

#include <SHA256.h>
#include <assert.h>
#include <cctype>
#include <algorithm>

LoBBSModule::LoBBSModule() 
    : SinglePortModule("LoBBS", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    // Get host node ID for use as salt in UUID generation
    hostNodeId = nodeDB->getNodeNum();
    
    // Initialize LoDB tables
    lodb_init_table(&usersTable, "lobbs_users", &meshtastic_LoBBSUser_msg, sizeof(meshtastic_LoBBSUser));
    lodb_init_table(&sessionsTable, "lobbs_sessions", &meshtastic_LoBBSSession_msg, sizeof(meshtastic_LoBBSSession));
    
    // Create message sender with callback to send replies
    messageSender = new TextMessageSender([this](uint32_t nodeId, const char *message) {
        this->sendTextReply(nodeId, message);
    });
    
    // Create worker pool for long-running operations
    workerPool = new concurrency::OSThreadWorkerPool("LoBBSWorker", 10);
}

bool LoBBSModule::isValidUsername(const char *username)
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

bool LoBBSModule::isValidPassword(const char *password)
{
    // Check each character: alphanumeric, underscore, or common special chars
    for (size_t i = 0; password[i] != '\0'; i++) {
        char c = password[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.' && 
            c != '!' && c != '@' && c != '#' && c != '$' && c != '%') {
            return false;
        }
    }
    return true;
}

void LoBBSModule::hashPassword(const char *password, uint8_t *hash)
{
    SHA256 sha256;
    sha256.reset();
    sha256.update(password, strlen(password));
    sha256.finalize(hash, 32);
}

bool LoBBSModule::loadUserByUsername(const char *username, meshtastic_LoBBSUser *user)
{
    // Convert username to UUID with host node ID as salt
    lodb_uuid_t userUuid = lodb_new_uuid(username, hostNodeId);
    LoDbError err = lodb_get(&usersTable, userUuid, user);
    if (err == LODB_OK) {
        LOG_DEBUG("Loaded user by username: %s", username);
        return true;
    }
    LOG_DEBUG("User not found: %s", username);
    return false;
}

bool LoBBSModule::loadUserByNodeId(uint32_t nodeId, meshtastic_LoBBSUser *user)
{
    // First, lookup session by node ID (use node ID directly as UUID)
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;
    
    meshtastic_LoBBSSession session = meshtastic_LoBBSSession_init_zero;
    LoDbError err = lodb_get(&sessionsTable, sessionUuid, &session);
    if (err != LODB_OK) {
        LOG_DEBUG("No session found for node 0x%08x", nodeId);
        return false;
    }
    
    // Now load the user by UUID from session
    err = lodb_get(&usersTable, session.user_uuid, user);
    if (err == LODB_OK) {
        LOG_DEBUG("Loaded user by node ID: 0x%08x -> UUID: %016llx", nodeId, (unsigned long long)session.user_uuid);
        return true;
    }
    LOG_DEBUG("User UUID not found: %016llx", (unsigned long long)session.user_uuid);
    return false;
}

bool LoBBSModule::createUser(const char *username, const char *password, uint32_t nodeId)
{
    // Create user record
    meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
    strncpy(user.username, username, sizeof(user.username) - 1);
    user.uid = 0; // Can be used for future features
    user.password_hash.size = 32;
    hashPassword(password, user.password_hash.bytes);
    
    // Insert with username converted to UUID with host node ID as salt
    lodb_uuid_t userUuid = lodb_new_uuid(username, hostNodeId);
    LoDbError err = lodb_insert(&usersTable, userUuid, &user);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to create user: %s", username);
        return false;
    }
    
    LOG_INFO("Created user: %s", username);
    
    // Log in the user (create session)
    return loginUser(username, nodeId);
}

bool LoBBSModule::verifyPassword(const meshtastic_LoBBSUser *user, const char *password)
{
    uint8_t providedHash[32];
    hashPassword(password, providedHash);
    return memcmp(user->password_hash.bytes, providedHash, 32) == 0;
}

bool LoBBSModule::loginUser(const char *username, uint32_t nodeId)
{
    // Create session record
    meshtastic_LoBBSSession session = meshtastic_LoBBSSession_init_zero;
    session.user_uuid = lodb_new_uuid(username, hostNodeId);
    session.node_id = nodeId;
    session.last_login_time = getTime();
    
    // Use node ID directly as UUID
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;
    
    // Delete existing session if any (upsert pattern)
    lodb_delete(&sessionsTable, sessionUuid);
    
    // Insert new session
    LoDbError err = lodb_insert(&sessionsTable, sessionUuid, &session);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to create session for node 0x%08x", nodeId);
        return false;
    }
    
    LOG_INFO("Created session for user %s (UUID: %016llx) on node 0x%08x", username, (unsigned long long)session.user_uuid, nodeId);
    return true;
}

bool LoBBSModule::logoutUser(uint32_t nodeId)
{
    // Use node ID directly as UUID
    lodb_uuid_t sessionUuid = (lodb_uuid_t)nodeId;
    
    LoDbError err = lodb_delete(&sessionsTable, sessionUuid);
    if (err == LODB_OK) {
        LOG_INFO("Logged out node 0x%08x", nodeId);
        return true;
    }
    LOG_WARN("No session found to log out for node 0x%08x", nodeId);
    return false;
}

void LoBBSModule::sendTextReply(uint32_t toNode, const char *message)
{
    auto reply = allocDataPacket();
    reply->decoded.payload.size = strlen(message);
    memcpy(reply->decoded.payload.bytes, message, reply->decoded.payload.size);
    reply->to = toNode;
    reply->decoded.want_response = false;
    
    LOG_INFO("Sending LoBBS reply to 0x%0x: %s", toNode, message);
    service->sendToMesh(reply, RX_SRC_LOCAL, true);
}

void LoBBSModule::sendLargeMessage(uint32_t toNode, const std::string &message)
{
    messageSender->send(toNode, message); // TextMessageSender wakes its own thread
}

ProcessMessage LoBBSModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    auto &p = mp.decoded;
    
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    LOG_INFO("Received LoBBS message from=0x%0x, id=%d, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif

    // Parse the command
    CommandParser parser(p.payload.bytes, p.payload.size);
    
    if (!parser.isCommand()) {
        // Not a command - send generic welcome message
        meshtastic_LoBBSUser existingUser = meshtastic_LoBBSUser_init_zero;
        bool isAuthenticated = loadUserByNodeId(mp.from, &existingUser);
        
        char welcomeMsg[256];
        if (isAuthenticated) {
            snprintf(welcomeMsg, sizeof(welcomeMsg), 
                     "Welcome back %s! You have 0 unread messages.\nType /help for commands", 
                     existingUser.username);
        } else {
            snprintf(welcomeMsg, sizeof(welcomeMsg), 
                     "Welcome to LoBBS!\nType /hi <user> <pass> to login or /help for more info");
        }
    
        LOG_INFO("Sending LoBBS welcome message to 0x%0x", mp.from);
        sendTextReply(mp.from, welcomeMsg);
        
        return ProcessMessage::CONTINUE;
    }
    
    // Get command name
    char cmdName[32];
    if (!parser.commandName(cmdName, sizeof(cmdName))) {
        sendTextReply(mp.from, "Invalid command");
        return ProcessMessage::CONTINUE;
    }
    
    // Handle commands
    if (strcmp(cmdName, "help") == 0) {
        LOG_INFO("Processing /help command from node=0x%0x", mp.from);
        
        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        bool isAuthenticated = loadUserByNodeId(mp.from, &user);
        
        char helpMsg[256];
        if (isAuthenticated) {
            snprintf(helpMsg, sizeof(helpMsg), 
                     "LoBBS Commands:\n"
                     "/bye - Logout\n"
                     "/users [filter] - List users (optional filter)\n"
                     "/mail - Mail (soon)\n"
                     "/news - News (soon)\n"
                     "/help - Show help");
        } else {
            snprintf(helpMsg, sizeof(helpMsg), 
                     "LoBBS - LoRa BBS\n"
                     "/hi <user> <pass> - Login or create account\n"
                     "/help - Show this help\n"
                     "Create an account to get started!");
        }
        
        sendTextReply(mp.from, helpMsg);
        return ProcessMessage::CONTINUE;
    }
    
    if (strcmp(cmdName, "bye") == 0) {
        LOG_INFO("Processing /bye command from node=0x%0x", mp.from);
        
        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        if (loadUserByNodeId(mp.from, &user)) {
            if (logoutUser(mp.from)) {
                char byeMsg[64];
                snprintf(byeMsg, sizeof(byeMsg), "Goodbye %s!", user.username);
                sendTextReply(mp.from, byeMsg);
            } else {
                sendTextReply(mp.from, "Error logging out");
            }
        } else {
            sendTextReply(mp.from, "You are not logged in");
        }
        
        return ProcessMessage::CONTINUE;
    }
    
    if (strcmp(cmdName, "users") == 0) {
        LOG_INFO("Processing /users command from node=0x%0x", mp.from);
        
        // Check authentication
        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        if (!loadUserByNodeId(mp.from, &user)) {
            sendTextReply(mp.from, "You must be logged in to use /users");
            return ProcessMessage::CONTINUE;
        }
        
        // Parse optional filter argument
        char filterStr[33] = "";
        parser.nextWord(filterStr, sizeof(filterStr)); // Optional, may be empty
        
        // Validate filter if provided
        if (filterStr[0] != '\0' && !isValidUsername(filterStr)) {
            sendTextReply(mp.from, "Filter contains invalid characters");
            return ProcessMessage::CONTINUE;
        }
        
        // Build filter function for username matching
        auto username_filter = [](const void *rec, void *ctx) -> bool {
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)rec;
            const char *filter = (const char *)ctx;
            
            // If no filter, include all
            if (filter[0] == '\0') {
                return true;
            }
            
            // Case-insensitive substring match
            char username_lower[33];
            strncpy(username_lower, u->username, sizeof(username_lower) - 1);
            for (size_t i = 0; username_lower[i]; i++) {
                username_lower[i] = tolower(username_lower[i]);
            }
            
            char filter_lower[33];
            strncpy(filter_lower, filter, sizeof(filter_lower) - 1);
            for (size_t i = 0; filter_lower[i]; i++) {
                filter_lower[i] = tolower(filter_lower[i]);
            }
            
            return strstr(username_lower, filter_lower) != nullptr;
        };
        
        // Use cursor to iterate and build user list incrementally via worker pool
        auto cursor = lodb_select_cursor(&usersTable, username_filter, filterStr);
        auto results = std::make_shared<std::vector<std::string>>();
        uint32_t fromNode = mp.from;
        std::string filterString(filterStr);
        
        // Add worker to process cursor incrementally
        workerPool->addWorker([cursor, results, fromNode, filterString, this]() mutable -> bool {
            // Process one cursor entry per worker invocation
            if (!lodb_cursor_is_exhausted(cursor)) {
            if (lodb_cursor_next(cursor)) {
                const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)lodb_cursor_get(cursor);
                    results->push_back(std::string(u->username));
            }
                return false; // Not done yet, continue on next cycle
            } else {
                // Cursor exhausted, close it and send results
        lodb_cursor_close(cursor);
        
                if (results->empty()) {
            char noMatchMsg[64];
                    if (!filterString.empty()) {
                        snprintf(noMatchMsg, sizeof(noMatchMsg), "No users match '%s'", filterString.c_str());
            } else {
                snprintf(noMatchMsg, sizeof(noMatchMsg), "No users found");
            }
                    sendTextReply(fromNode, noMatchMsg);
                } else {
        // Sort alphabetically
                    std::sort(results->begin(), results->end());
        
        // Build user directory message
        std::string userListMsg = "User directory: ";
                    for (size_t i = 0; i < results->size(); i++) {
            if (i > 0) {
                userListMsg += ", ";
            }
                        userListMsg += (*results)[i];
        }
        
                    // Send using message sender (will auto-fragment cooperatively)
                    sendLargeMessage(fromNode, userListMsg);
                }
                
                return true; // Done
            }
        });
        
        // Acknowledge that the request is being processed
        sendTextReply(mp.from, "Fetching user directory...");
        
        return ProcessMessage::CONTINUE;
    }
    
    if (strcmp(cmdName, "hi") == 0) {
        // Parse arguments: /hi <username> <password>
        char username[33];
        char password[256];
        
        if (!parser.nextWord(username, sizeof(username))) {
            sendTextReply(mp.from, "Usage: /hi <username> <password>");
            return ProcessMessage::CONTINUE;
        }
        
        if (!parser.rest(password, sizeof(password))) {
            sendTextReply(mp.from, "Usage: /hi <username> <password>");
            return ProcessMessage::CONTINUE;
        }
        
        // Validate username length
        size_t usernameLen = strlen(username);
        if (usernameLen == 0 || usernameLen > 32) {
            sendTextReply(mp.from, "Username must be 1-32 characters");
            return ProcessMessage::CONTINUE;
        }

        // Validate username format
        if (!isValidUsername(username)) {
            sendTextReply(mp.from, "Username must start with a letter and contain only letters, numbers, or underscores");
            return ProcessMessage::CONTINUE;
        }
        
        // Validate password
        size_t passwordLen = strlen(password);
        if (passwordLen == 0) {
            sendTextReply(mp.from, "Password cannot be empty");
            return ProcessMessage::CONTINUE;
        }

        // Validate password is at least 5 characters long
        if (passwordLen < 5) {
            sendTextReply(mp.from, "Password must be at least 5 characters long");
            return ProcessMessage::CONTINUE;
        }

        // Validate password format
        if (!isValidPassword(password)) {
            sendTextReply(mp.from, "Password contains invalid characters");
            return ProcessMessage::CONTINUE;
        }

        // Maximum password length is 50 characters
        if (passwordLen > 50) {
            sendTextReply(mp.from, "Password must be at most 50 characters long");
            return ProcessMessage::CONTINUE;
        }
        
        LOG_INFO("Processing /hi command: username=%s from node=0x%0x", username, mp.from);
        
        // Try to load existing user
        meshtastic_LoBBSUser existingUser = meshtastic_LoBBSUser_init_zero;
        bool userExists = loadUserByUsername(username, &existingUser);
        
        if (userExists) {
            // User exists - verify password
            if (verifyPassword(&existingUser, password)) {
                LOG_INFO("User %s authenticated successfully from node 0x%0x", username, mp.from);
                
                // Log in user (create/update session)
                if (loginUser(username, mp.from)) {
                    char welcomeMsg[64];
                    snprintf(welcomeMsg, sizeof(welcomeMsg), "Welcome back %s!", username);
                    sendTextReply(mp.from, welcomeMsg);
                } else {
                    sendTextReply(mp.from, "Error creating session");
                }
            } else {
                LOG_WARN("Invalid password for user %s from node 0x%0x", username, mp.from);
                sendTextReply(mp.from, "Invalid password");
            }
        } else {
            // New user - create account
            LOG_INFO("Creating new user: %s for node 0x%0x", username, mp.from);
            
            if (createUser(username, password, mp.from)) {
                char welcomeMsg[64];
                snprintf(welcomeMsg, sizeof(welcomeMsg), "Welcome %s!", username);
                sendTextReply(mp.from, welcomeMsg);
            } else {
                sendTextReply(mp.from, "Error creating account");
            }
        }
        
        return ProcessMessage::CONTINUE;
    }
    
    // Unknown command
    char errorMsg[64];
    snprintf(errorMsg, sizeof(errorMsg), "Unknown command: %s", cmdName);
    sendTextReply(mp.from, errorMsg);
    
    return ProcessMessage::CONTINUE;
}
