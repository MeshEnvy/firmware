#include "LoBBSDb.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"

#include <SHA256.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <cstring>
#include <algorithm>
#include <cctype>

LoBBSDb::LoBBSDb()
{
    createDirectories();
}

void LoBBSDb::hashPassword(const char *password, uint8_t *hash)
{
    SHA256 sha256;
    sha256.reset();
    sha256.update(password, strlen(password));
    sha256.finalize(hash, 32);
}

uint32_t LoBBSDb::generateUidFromUsername(const char *username)
{
    // Hash username and take first 4 bytes as uint32
    uint8_t hash[32];
    SHA256 sha256;
    sha256.reset();
    sha256.update(username, strlen(username));
    sha256.finalize(hash, 32);
    
    // Convert first 4 bytes to uint32
    uint32_t uid = 0;
    uid |= ((uint32_t)hash[0]) << 24;
    uid |= ((uint32_t)hash[1]) << 16;
    uid |= ((uint32_t)hash[2]) << 8;
    uid |= ((uint32_t)hash[3]);
    
    return uid;
}

void LoBBSDb::createDirectories()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    FSCom.mkdir("/lobbs");
    FSCom.mkdir(LOBBS_USERS_DIR);
    FSCom.mkdir("/lobbs/indexes");
    FSCom.mkdir(LOBBS_INDEX_USERNAME_DIR);
    FSCom.mkdir(LOBBS_INDEX_NODEID_DIR);
#endif
}

bool LoBBSDb::loadUserByUid(uint32_t uid, meshtastic_LoBBSUser *user)
{
#ifdef FSCom
    char userPath[128];
    snprintf(userPath, sizeof(userPath), "%s/%u.pbin", LOBBS_USERS_DIR, uid);
    
    // Read file into buffer first
    uint8_t buffer[meshtastic_LoBBSUser_size];
    size_t fileSize = 0;
    
    {
        concurrency::LockGuard g(spiLock);
        auto userFile = FSCom.open(userPath, FILE_O_READ);
        if (!userFile) {
            LOG_DEBUG("User file not found: %s", userPath);
            return false;
        }
        
        fileSize = userFile.read(buffer, sizeof(buffer));
        userFile.close();
        
        if (fileSize == 0) {
            LOG_ERROR("User file is empty: %s", userPath);
            return false;
        }
        
        LOG_DEBUG("Read user file: %s (%d bytes)", userPath, fileSize);
    }
    
    // Decode from buffer
    pb_istream_t stream = pb_istream_from_buffer(buffer, fileSize);
    memset(user, 0, sizeof(meshtastic_LoBBSUser));
    
    bool success = pb_decode(&stream, &meshtastic_LoBBSUser_msg, user);
    
    if (!success) {
        LOG_ERROR("Failed to decode user protobuf from %s", userPath);
        return false;
    }
    
    return true;
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

bool LoBBSDb::loadUserByUsername(const char *username, meshtastic_LoBBSUser *user)
{
#ifdef FSCom
    // First, load the UID from the username index
    char indexPath[128];
    snprintf(indexPath, sizeof(indexPath), "%s/%s.pbin", LOBBS_INDEX_USERNAME_DIR, username);
    
    uint32_t uid;
    {
        concurrency::LockGuard g(spiLock);
        auto indexFile = FSCom.open(indexPath, FILE_O_READ);
        if (!indexFile) {
            LOG_DEBUG("Username index not found: %s", username);
            return false;
        }
        
        size_t bytesRead = indexFile.read((uint8_t *)&uid, sizeof(uid));
        indexFile.close();
        
        if (bytesRead != sizeof(uid)) {
            LOG_ERROR("Failed to read UID from index");
            return false;
        }
    }
    
    // Now load the user record (loadUserByUid will acquire its own lock)
    bool result = loadUserByUid(uid, user);
    if (result) {
        LOG_INFO("Loaded user: %s (UID: %u)", user->username, user->uid);
    }
    return result;
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

bool LoBBSDb::loadUserByNodeId(uint32_t nodeId, meshtastic_LoBBSUser *user)
{
#ifdef FSCom
    // Load the UID from the node ID index
    char indexPath[128];
    snprintf(indexPath, sizeof(indexPath), "%s/%u.pbin", LOBBS_INDEX_NODEID_DIR, nodeId);
    
    uint32_t uid;
    {
        concurrency::LockGuard g(spiLock);
        LOG_DEBUG("Loading user by node ID from: %s", indexPath);
        auto indexFile = FSCom.open(indexPath, FILE_O_READ);
        if (!indexFile) {
            LOG_DEBUG("Node ID index not found for node 0x%0x at path: %s", nodeId, indexPath);
            return false;
        }
        
        size_t bytesRead = indexFile.read((uint8_t *)&uid, sizeof(uid));
        indexFile.close();
        
        if (bytesRead != sizeof(uid)) {
            LOG_ERROR("Failed to read UID from node index, got %d bytes", bytesRead);
            return false;
        }
        LOG_DEBUG("Found UID %u for node 0x%0x", uid, nodeId);
    }
    
    // Now load the user record (loadUserByUid will acquire its own lock)
    bool result = loadUserByUid(uid, user);
    if (result) {
        LOG_INFO("Loaded user by node ID 0x%0x: %s", nodeId, user->username);
    }
    return result;
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

bool LoBBSDb::saveUser(const meshtastic_LoBBSUser *user, uint32_t nodeId)
{
#ifdef FSCom
    createDirectories();
    
    // Save user record - encode to buffer first, then write directly
    char userPath[128];
    snprintf(userPath, sizeof(userPath), "%s/%u.pbin", LOBBS_USERS_DIR, user->uid);
    
    // Encode to a buffer first
    uint8_t buffer[meshtastic_LoBBSUser_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    
    if (!pb_encode(&stream, &meshtastic_LoBBSUser_msg, user)) {
        LOG_ERROR("Failed to encode user protobuf");
        return false;
    }
    
    size_t encoded_size = stream.bytes_written;
    LOG_DEBUG("Encoded user protobuf: %d bytes", encoded_size);
    
    // Write directly to file
    {
        concurrency::LockGuard g(spiLock);
        FSCom.remove(userPath); // Remove old file
        auto userFile = FSCom.open(userPath, FILE_O_WRITE);
        if (!userFile) {
            LOG_ERROR("Failed to open user file for writing: %s", userPath);
            return false;
        }
        
        size_t written = userFile.write(buffer, encoded_size);
        if (written != encoded_size) {
            LOG_ERROR("Failed to write user file, wrote %d of %d bytes", written, encoded_size);
            userFile.close();
            return false;
        }
        
        userFile.flush();
        userFile.close();
        LOG_DEBUG("Wrote user file: %s (%d bytes)", userPath, encoded_size);
    }
    
    // Save username index
    char usernameIndexPath[128];
    snprintf(usernameIndexPath, sizeof(usernameIndexPath), "%s/%s.pbin", LOBBS_INDEX_USERNAME_DIR, user->username);
    
    {
        concurrency::LockGuard g(spiLock);
        auto usernameIndexFile = FSCom.open(usernameIndexPath, FILE_O_WRITE);
        if (!usernameIndexFile) {
            LOG_ERROR("Failed to create username index at %s", usernameIndexPath);
            return false;
        }
        size_t written = usernameIndexFile.write((uint8_t *)&user->uid, sizeof(user->uid));
        if (written != sizeof(user->uid)) {
            LOG_ERROR("Failed to write username index, wrote %d bytes", written);
            usernameIndexFile.close();
            return false;
        }
        usernameIndexFile.flush();
        usernameIndexFile.close();
        LOG_DEBUG("Saved username index: %s -> UID %u", user->username, user->uid);
    }
    
    // Save node ID index
    char nodeIndexPath[128];
    snprintf(nodeIndexPath, sizeof(nodeIndexPath), "%s/%u.pbin", LOBBS_INDEX_NODEID_DIR, nodeId);
    
    {
        concurrency::LockGuard g(spiLock);
        auto nodeIndexFile = FSCom.open(nodeIndexPath, FILE_O_WRITE);
        if (!nodeIndexFile) {
            LOG_ERROR("Failed to create node ID index at %s", nodeIndexPath);
            return false;
        }
        size_t written = nodeIndexFile.write((uint8_t *)&user->uid, sizeof(user->uid));
        if (written != sizeof(user->uid)) {
            LOG_ERROR("Failed to write node ID index, wrote %d bytes", written);
            nodeIndexFile.close();
            return false;
        }
        nodeIndexFile.flush();
        nodeIndexFile.close();
        LOG_DEBUG("Saved node ID index: 0x%0x -> UID %u", nodeId, user->uid);
    }
    
    LOG_INFO("Saved user: %s (UID: %u) for node 0x%0x", user->username, user->uid, nodeId);
    return true;
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

bool LoBBSDb::createUser(const char *username, const char *password, uint32_t nodeId, meshtastic_LoBBSUser *user)
{
    // Initialize user record
    memset(user, 0, sizeof(meshtastic_LoBBSUser));
    strncpy(user->username, username, sizeof(user->username) - 1);
    user->uid = generateUidFromUsername(username);
    user->password_hash.size = 32;
    hashPassword(password, user->password_hash.bytes);
    
    // Save to filesystem
    return saveUser(user, nodeId);
}

bool LoBBSDb::verifyPassword(const meshtastic_LoBBSUser *user, const char *password)
{
    uint8_t providedHash[32];
    hashPassword(password, providedHash);
    return memcmp(user->password_hash.bytes, providedHash, 32) == 0;
}

bool LoBBSDb::logoutNodeId(uint32_t nodeId)
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    
    // Remove the node ID index file
    char nodeIndexPath[128];
    snprintf(nodeIndexPath, sizeof(nodeIndexPath), "%s/%u.pbin", LOBBS_INDEX_NODEID_DIR, nodeId);
    
    if (FSCom.remove(nodeIndexPath)) {
        LOG_INFO("Logged out node 0x%0x", nodeId);
        return true;
    } else {
        LOG_WARN("Failed to remove node ID index for 0x%0x", nodeId);
        return false;
    }
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

bool LoBBSDb::listUsers(const char *filter, std::vector<std::string> &results)
{
#ifdef FSCom
    results.clear();
    
    // Convert filter to lowercase for case-insensitive comparison
    std::string filterLower = filter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), 
                   [](unsigned char c){ return std::tolower(c); });
    
    concurrency::LockGuard g(spiLock);
    
    // Open username index directory
    File root = FSCom.open(LOBBS_INDEX_USERNAME_DIR, FILE_O_READ);
    if (!root) {
        LOG_DEBUG("Username index directory not found");
        return true; // Not an error, just no users yet
    }
    
    if (!root.isDirectory()) {
        LOG_ERROR("Username index path is not a directory");
        root.close();
        return false;
    }
    
    // Iterate through all username index files
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            // Get filename (might be full path on ESP32, just name on others)
            const char *fullPath = file.name();
            std::string pathStr = fullPath;
            
            // Extract just the filename (after last /)
            size_t lastSlash = pathStr.rfind('/');
            std::string filename = (lastSlash != std::string::npos) ? 
                                   pathStr.substr(lastSlash + 1) : pathStr;
            
            // Extract username (remove .pbin extension)
            size_t pbinPos = filename.find(".pbin");
            if (pbinPos != std::string::npos) {
                std::string username = filename.substr(0, pbinPos);
                
                // Apply filter (case-insensitive substring match)
                std::string usernameLower = username;
                std::transform(usernameLower.begin(), usernameLower.end(), usernameLower.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                
                if (usernameLower.find(filterLower) != std::string::npos) {
                    results.push_back(username);
                    LOG_DEBUG("Found matching user: %s (from file: %s)", username.c_str(), filename.c_str());
                }
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    
    // Sort results alphabetically
    std::sort(results.begin(), results.end());
    
    LOG_INFO("Found %d users matching filter '%s'", results.size(), filter);
    return true;
#else
    LOG_ERROR("Filesystem not available");
    return false;
#endif
}

