#include "LoBBSModule.h"
#include "MeshService.h"
#include "airtime.h"
#include "configuration.h"
#include "lobbs.pb.h"
#include <algorithm>
#include <cstring>
#include <string>

LoBBSModule::LoBBSModule() : SinglePortModule("LoBBS", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    // Create data access layer with host node ID as salt
    dal = new LoBBSDal(nodeDB->getNodeNum());
}

ProcessMessage LoBBSModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    LOG_DEBUG("LoBBS received DM from=0x%0x, id=%d, msg=%.*s", mp.from, mp.id, mp.decoded.payload.size, mp.decoded.payload.bytes);

    meshtastic_LoBBSUser existingUser = meshtastic_LoBBSUser_init_zero;
    bool isAuthenticated = dal->loadUserByNodeId(mp.from, &existingUser);
    if (isAuthenticated) {
        LOG_DEBUG("User node ID: %08x", mp.from);
        LOG_DEBUG("User UUID: " LODB_UUID_FMT, LODB_UUID_ARGS(existingUser.uuid));
        LOG_DEBUG("User: %s", existingUser.username);
        LOG_DEBUG("User is authenticated: %d", isAuthenticated);
    } else {
        LOG_DEBUG("User node ID: %08x", mp.from);
        LOG_DEBUG("User is not authenticated");
    }

    // Copy payload to mutable buffer for strtok and null terminate (there is no null terminator in the payload)
    memcpy(msgBuffer, mp.decoded.payload.bytes, mp.decoded.payload.size);
    msgBuffer[mp.decoded.payload.size] = '\0';
    char *cmdName = strtok(msgBuffer, " ");
    LOG_DEBUG("Token: %s", cmdName);

    if (strcmp(cmdName, "/hi") == 0) {
        LOG_DEBUG("Processing /hi command from node=0x%0x", mp.from);

        // Get username
        char *username = strtok(NULL, " ");
        if (!username) {
            sendReply(mp.from, "Usage: /hi <username> <password>");
            return ProcessMessage::CONTINUE;
        }

        // Validate username length
        size_t usernameLen = strlen(username);
        if (usernameLen == 0 || usernameLen > 32) {
            sendReply(mp.from, "Username must be 1-32 characters");
            return ProcessMessage::CONTINUE;
        }

        // Validate username format
        if (!dal->isValidUsername(username)) {
            sendReply(mp.from, "Username must start with a letter and contain only letters, numbers, or underscores");
            return ProcessMessage::CONTINUE;
        }

        // Get password (rest of the string)
        char *password = strtok(NULL, "");
        if (!password) {
            sendReply(mp.from, "Usage: /hi <username> <password>");
            return ProcessMessage::CONTINUE;
        }

        // Validate password is at least 5 characters long
        if (strlen(password) < 5 || strlen(password) > 50 || !dal->isValidPassword(password)) {
            sendReply(mp.from, "Password must be between 5 and 50 characters long and contain only letters, numbers, and common "
                               "special characters.");
            return ProcessMessage::CONTINUE;
        }

        LOG_DEBUG("Processing /hi command: username=%s from node=0x%0x", username, mp.from);

        // Try to load existing user
        meshtastic_LoBBSUser existingUser = meshtastic_LoBBSUser_init_zero;
        bool userExists = dal->loadUserByUsername(username, &existingUser);

        if (userExists) {
            // User exists - verify password
            if (dal->verifyPassword(&existingUser, password)) {
                LOG_DEBUG("User %s authenticated successfully from node 0x%0x", username, mp.from);

                // Log in user (create/update session)
                if (dal->loginUser(username, mp.from)) {
                    snprintf(replyBuffer, sizeof(replyBuffer), "Welcome back %s!", username);
                    sendReply(mp.from, replyBuffer);
                } else {
                    sendReply(mp.from, "Error creating session");
                }
            } else {
                LOG_WARN("Invalid password for user %s from node 0x%0x", username, mp.from);
                sendReply(mp.from, "Invalid password");
            }
        } else {
            // New user - create account
            LOG_DEBUG("Creating new user: %s for node 0x%0x", username, mp.from);

            if (dal->createUser(username, password, mp.from)) {
                snprintf(replyBuffer, sizeof(replyBuffer), "Welcome %s!", username);
                sendReply(mp.from, replyBuffer);
            } else {
                sendReply(mp.from, "Error creating account");
            }
        }

        return ProcessMessage::CONTINUE;
    }

    if (!isAuthenticated) {
        const char *helpMsg = LOBBS_HEADER "/hi <user> <pass> - Login or create account\n";
        LOG_DEBUG("Help message: %s", helpMsg);
        sendReply(mp.from, helpMsg);
        return ProcessMessage::CONTINUE;
    }

    /**
     * ================================
     * Authenticated commands
     * ================================
     */

    if (strcmp(cmdName, "/bye") == 0) {
        LOG_INFO("Processing /bye command from node=0x%0x", mp.from);

        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        dal->logoutUser(mp.from);
        sendReply(mp.from, "Goodbye!");

        return ProcessMessage::CONTINUE;
    }

    if (strcmp(cmdName, "/users") == 0) {
        LOG_INFO("Processing /users command from node=0x%0x", mp.from);

        char *filterStr = strtok(NULL, " ");

        // Parse optional filter argument
        if (filterStr && !dal->isValidUsername(filterStr)) {
            sendReply(mp.from, "Filter must contain only letters, numbers, and common special characters.");
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
        auto cursor = dal->getDb()->selectCursor("users", username_filter, filterStr);
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
                        snprintf(noMatchMsg, sizeof(noMatchMsg), "No users match '%s", filterString.c_str());
                    } else {
                        snprintf(noMatchMsg, sizeof(noMatchMsg), "No users found");
                    }
                    messageSender->send(fromNode, noMatchMsg);
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
                    messageSender->send(fromNode, userListMsg);
                }

                return true; // Done
            }
        });

        // Acknowledge that the request is being processed
        messageSender->send(mp.from, "Fetching user directory...");

        return ProcessMessage::CONTINUE;
    }

    if (isAuthenticated) {
        const char *helpMsg = LOBBS_HEADER "/bye - Logout\n"
                                           "/users [filter] - List users (optional filter)\n"
                                           "/mail - Mail (soon)\n"
                                           "/news - News (soon)\n"
                                           "/help - Show help";
        LOG_DEBUG("Help message: %s", helpMsg);
        sendReply(mp.from, helpMsg);
        return ProcessMessage::CONTINUE;
    }

    const char *helpMsg = LOBBS_HEADER "/hi <user> <pass> - Login or create account\n";
    LOG_DEBUG("Help message: %s", helpMsg);
    sendReply(mp.from, helpMsg);
    return ProcessMessage::CONTINUE;
}

void LoBBSModule::sendReply(NodeNum to, const char *msg)
{
    meshtastic_MeshPacket *reply = allocDataPacket();
    reply->decoded.payload.size = strlen(msg);
    memcpy(reply->decoded.payload.bytes, msg, reply->decoded.payload.size);
    reply->to = to;
    reply->decoded.want_response = false;
    service->sendToMesh(reply);
}
