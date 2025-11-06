#include "LoBBSModule.h"
#include "LoBBSDal.h"
#include "MeshService.h"
#include "airtime.h"
#include "configuration.h"
#include "lobbs.pb.h"
#include <algorithm>
#include <cstring>
#include <string>

// Static helper: case-insensitive substring search
static const char *stristr(const char *haystack, const char *needle)
{
    if (!*needle)
        return haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && tolower(*h) == tolower(*n)) {
            h++;
            n++;
        }
        if (!*n)
            return haystack;
    }
    return nullptr;
}

// Static helper: comparator for case-insensitive alphabetical username sorting
static int compareUsernames(const void *a, const void *b)
{
    const meshtastic_LoBBSUser *u1 = (const meshtastic_LoBBSUser *)a;
    const meshtastic_LoBBSUser *u2 = (const meshtastic_LoBBSUser *)b;
    return strcasecmp(u1->username, u2->username);
}

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

    if (strcasecmp(cmdName, "/hi") == 0) {
        LOG_DEBUG("Processing /hi command from node=0x%0x", mp.from);

        // Get username
        char *username = strtok(NULL, " ");
        if (!username) {
            sendReply(mp.from, "Usage: /hi <username> <password>");
            return ProcessMessage::CONTINUE;
        }

        // Validate username length
        size_t usernameLen = strlen(username);
        if (usernameLen == 0 || usernameLen > LOBBS_MAX_USERNAME_LEN || !dal->isValidUsername(username)) {
            sendReply(
                mp.from,
                "Username not found or is invalid. Username must be 1-" LOBBS_XSTR(
                    LOBBS_MAX_USERNAME_LEN) " characters and contain only letters, numbers, and common special characters.");
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
            sendReply(mp.from, "Password incorrect or invalid. Password must be between 5 and 50 characters long and contain "
                               "only letters, numbers, and common "
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

    if (strcasecmp(cmdName, "/bye") == 0) {
        LOG_INFO("Processing /bye command from node=0x%0x", mp.from);

        meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
        dal->logoutUser(mp.from);
        sendReply(mp.from, "Goodbye!");

        return ProcessMessage::CONTINUE;
    }

    if (strcasecmp(cmdName, "/users") == 0) {
        LOG_INFO("Processing /users command from node=0x%0x", mp.from);

        char *filterStr = strtok(NULL, " ");

        // Parse optional filter argument
        if (filterStr && !dal->isValidUsername(filterStr)) {
            sendReply(mp.from, "Filter must contain only letters, numbers, and common special characters.");
            return ProcessMessage::CONTINUE;
        }

        // Build filter lambda for username matching (captures filterStr)
        auto username_filter = [filterStr](const void *rec) -> bool {
            const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)rec;
            return !filterStr || !filterStr[0] || stristr(u->username, filterStr) != nullptr;
        };

        // Execute synchronous select with filter and sort
        auto users = dal->getDb()->select("users", username_filter, compareUsernames);

        // Build and send response
        if (users.empty()) {
            std::string reply;
            LOG_DEBUG("No users match '%s'", filterStr);
            if (filterStr && filterStr[0]) {
                reply += "No users match '";
                reply += filterStr;
                reply += "'";
            } else {
                reply += "No users found";
            }
        } else {
            LOG_DEBUG("# users: %d", users.size());
            // Build user directory message
            std::string userListMsg = "User directory:\n";
            for (size_t i = 0; i < users.size(); i++) {
                const meshtastic_LoBBSUser *u = (const meshtastic_LoBBSUser *)users[i];
                if (i > 0) {
                    userListMsg += ", ";
                }
                userListMsg += u->username;
            }
            LOG_DEBUG("User list message: %s", userListMsg.c_str());

            sendReply(mp.from, userListMsg.c_str());

            // Free allocated records
            for (auto *userPtr : users) {
                delete[] (uint8_t *)userPtr;
            }
        }

        return ProcessMessage::CONTINUE;
    }

    std::string helpMsg = LOBBS_HEADER "/bye - Logout\n"
                                       "/users [filter] - List users (optional filter)\n"
                                       "/mail - Mail (soon)\n"
                                       "/news - News (soon)";
    LOG_DEBUG("Help message: %s", helpMsg);
    sendReply(mp.from, helpMsg);
    return ProcessMessage::CONTINUE;
}

void LoBBSModule::sendReply(NodeNum to, const std::string &msg)
{
    meshtastic_MeshPacket *reply = allocDataPacket();
    reply->decoded.payload.size = std::min(msg.size(), (size_t)sizeof(reply->decoded.payload.bytes));
    memcpy(reply->decoded.payload.bytes, msg.c_str(), reply->decoded.payload.size);
    reply->to = to;
    reply->decoded.want_response = false;
    service->sendToMesh(reply);
}
