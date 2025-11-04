#include "LoBBSModule.h"
#include "CommandParser.h"
#include "MeshService.h"
#include "configuration.h"

#include <assert.h>
#include <cctype>

LoBBSModule::LoBBSModule() : SinglePortModule("LoBBS", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    db = new LoBBSDb();
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
        bool isAuthenticated = db->loadUserByNodeId(mp.from, &existingUser);
        
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
        bool isAuthenticated = db->loadUserByNodeId(mp.from, &user);
        
        char helpMsg[256];
        if (isAuthenticated) {
            snprintf(helpMsg, sizeof(helpMsg), 
                     "LoBBS Commands:\n"
                     "/bye - Logout\n"
                     "/mail - Read mail (coming soon)\n"
                     "/news - Read news (coming soon)\n"
                     "/help - Show this help");
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
        if (db->loadUserByNodeId(mp.from, &user)) {
            if (db->logoutNodeId(mp.from)) {
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
        bool userExists = db->loadUserByUsername(username, &existingUser);
        
        if (userExists) {
            // User exists - verify password
            if (db->verifyPassword(&existingUser, password)) {
                LOG_INFO("User %s authenticated successfully from node 0x%0x", username, mp.from);
                db->saveUser(&existingUser, mp.from);
                
                char welcomeMsg[64];
                snprintf(welcomeMsg, sizeof(welcomeMsg), "Welcome back %s!", username);
                sendTextReply(mp.from, welcomeMsg);
            } else {
                LOG_WARN("Invalid password for user %s from node 0x%0x", username, mp.from);
                sendTextReply(mp.from, "Invalid password");
            }
        } else {
            // New user - create account
            LOG_INFO("Creating new user: %s for node 0x%0x", username, mp.from);
            
            meshtastic_LoBBSUser newUser = meshtastic_LoBBSUser_init_zero;
            if (db->createUser(username, password, mp.from, &newUser)) {
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

