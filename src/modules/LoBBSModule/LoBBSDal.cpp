#include "LoBBSDal.h"
#include "configuration.h"
#include "gps/RTC.h"
#include <SHA256.h>
#include <cctype>
#include <cstring>

// Helper: normalize username to lowercase for case-insensitive lookups
static void normalizeUsername(const char *username, char *normalized)
{
    size_t len = strlen(username);
    if (len > LOBBS_MAX_USERNAME_LEN)
        len = LOBBS_MAX_USERNAME_LEN;
    for (size_t i = 0; i < len; i++) {
        normalized[i] = tolower(username[i]);
    }
    normalized[len] = '\0';
}

LoBBSDal::LoBBSDal(uint32_t hostNodeId) : hostNodeId(hostNodeId)
{
    // Initialize LoDB database
    db = new LoDb("lobbs");
    db->registerTable("users", &meshtastic_LoBBSUser_msg, sizeof(meshtastic_LoBBSUser));
    db->registerTable("sessions", &meshtastic_LoBBSSession_msg, sizeof(meshtastic_LoBBSSession));
    db->registerTable("mail", &meshtastic_LoBBSMail_msg, sizeof(meshtastic_LoBBSMail));
    db->registerTable("news", &meshtastic_LoBBSNews_msg, sizeof(meshtastic_LoBBSNews));
    db->registerTable("news_reads", &meshtastic_LoBBSNewsRead_msg, sizeof(meshtastic_LoBBSNewsRead));
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
    // Normalize username to lowercase for case-insensitive lookup
    char normalized[LOBBS_USERNAME_BUFFER_SIZE];
    normalizeUsername(username, normalized);

    // Convert username to UUID with host node ID as salt
    lodb_uuid_t userUuid = lodb_new_uuid(normalized, hostNodeId);
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
    // Normalize username to lowercase for case-insensitive storage
    char normalized[LOBBS_USERNAME_BUFFER_SIZE];
    normalizeUsername(username, normalized);

    // Calculate UUID first (username with host node ID as salt)
    lodb_uuid_t userUuid = lodb_new_uuid(normalized, hostNodeId);

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
    // Normalize username to lowercase for case-insensitive lookup
    char normalized[LOBBS_USERNAME_BUFFER_SIZE];
    normalizeUsername(username, normalized);

    // Create session record
    meshtastic_LoBBSSession session = meshtastic_LoBBSSession_init_zero;
    session.user_uuid = lodb_new_uuid(normalized, hostNodeId);
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

uint64_t LoBBSDal::getUserUuidByUsername(const char *username)
{
    // Normalize username to lowercase for case-insensitive lookup
    char normalized[LOBBS_USERNAME_BUFFER_SIZE];
    normalizeUsername(username, normalized);

    // Convert username to UUID with host node ID as salt
    uint64_t userUuid = lodb_new_uuid(normalized, hostNodeId);

    // Verify user exists
    meshtastic_LoBBSUser user = meshtastic_LoBBSUser_init_zero;
    LoDbError err = db->get("users", userUuid, &user);
    if (err == LODB_OK) {
        LOG_DEBUG("Found user UUID for %s: " LODB_UUID_FMT, username, LODB_UUID_ARGS(userUuid));
        return userUuid;
    }
    LOG_DEBUG("User not found: %s", username);
    return 0;
}

bool LoBBSDal::sendMail(uint64_t fromUserUuid, uint64_t toUserUuid, const char *message)
{
    // Generate a unique UUID for the mail message (using timestamp and recipient UUID)
    lodb_uuid_t mailUuid = lodb_new_uuid((const char *)&toUserUuid, getTime());

    // Create mail record
    meshtastic_LoBBSMail mail = meshtastic_LoBBSMail_init_zero;
    mail.uuid = mailUuid;
    mail.from_user_uuid = fromUserUuid;
    mail.to_user_uuid = toUserUuid;
    strncpy(mail.message, message, sizeof(mail.message) - 1);
    mail.message[sizeof(mail.message) - 1] = '\0';
    mail.timestamp = getTime();
    mail.read = false;

    LoDbError err = db->insert("mail", mailUuid, &mail);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to send mail from " LODB_UUID_FMT " to " LODB_UUID_FMT, LODB_UUID_ARGS(fromUserUuid),
                  LODB_UUID_ARGS(toUserUuid));
        return false;
    }

    LOG_INFO("Sent mail from " LODB_UUID_FMT " to " LODB_UUID_FMT, LODB_UUID_ARGS(fromUserUuid), LODB_UUID_ARGS(toUserUuid));
    return true;
}

// Comparator for sorting mail by timestamp descending (newest first)
static int compareMailByTimestamp(const void *a, const void *b)
{
    const meshtastic_LoBBSMail *m1 = (const meshtastic_LoBBSMail *)a;
    const meshtastic_LoBBSMail *m2 = (const meshtastic_LoBBSMail *)b;
    // Reverse order: newer (larger timestamp) first
    if (m2->timestamp > m1->timestamp)
        return 1;
    if (m2->timestamp < m1->timestamp)
        return -1;
    return 0;
}

std::vector<void *> LoBBSDal::getMailForUser(uint64_t userUuid, uint32_t offset, uint32_t limit)
{
    // Build filter lambda for mail matching recipient
    auto mail_filter = [userUuid](const void *rec) -> bool {
        const meshtastic_LoBBSMail *m = (const meshtastic_LoBBSMail *)rec;
        return m->to_user_uuid == userUuid;
    };

    // Execute select with filter and sort
    auto allMail = db->select("mail", mail_filter, compareMailByTimestamp);

    // Apply offset and limit
    std::vector<void *> result;
    for (size_t i = offset; i < allMail.size() && i < offset + limit; i++) {
        result.push_back(allMail[i]);
    }

    // Free records not included in result
    for (size_t i = 0; i < allMail.size(); i++) {
        if (i < offset || i >= offset + limit) {
            delete[] (uint8_t *)allMail[i];
        }
    }

    LOG_DEBUG("Retrieved %d mail messages for user " LODB_UUID_FMT " (offset=%d, limit=%d)", result.size(),
              LODB_UUID_ARGS(userUuid), offset, limit);
    return result;
}

bool LoBBSDal::markMailAsRead(uint64_t mailUuid)
{
    // Load the mail record
    meshtastic_LoBBSMail mail = meshtastic_LoBBSMail_init_zero;
    LoDbError err = db->get("mail", mailUuid, &mail);
    if (err != LODB_OK) {
        LOG_WARN("Mail not found: " LODB_UUID_FMT, LODB_UUID_ARGS(mailUuid));
        return false;
    }

    // Update read flag
    mail.read = true;

    // Delete old record and insert updated one
    db->deleteRecord("mail", mailUuid);
    err = db->insert("mail", mailUuid, &mail);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to mark mail as read: " LODB_UUID_FMT, LODB_UUID_ARGS(mailUuid));
        return false;
    }

    LOG_DEBUG("Marked mail as read: " LODB_UUID_FMT, LODB_UUID_ARGS(mailUuid));
    return true;
}

bool LoBBSDal::postNews(uint64_t authorUserUuid, const char *message)
{
    // Generate a unique UUID for the news item (using timestamp and author UUID)
    lodb_uuid_t newsUuid = lodb_new_uuid((const char *)&authorUserUuid, getTime());

    // Create news record
    meshtastic_LoBBSNews news = meshtastic_LoBBSNews_init_zero;
    news.uuid = newsUuid;
    news.author_user_uuid = authorUserUuid;
    strncpy(news.message, message, sizeof(news.message) - 1);
    news.message[sizeof(news.message) - 1] = '\0';
    news.timestamp = getTime();

    LoDbError err = db->insert("news", newsUuid, &news);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to post news from " LODB_UUID_FMT, LODB_UUID_ARGS(authorUserUuid));
        return false;
    }

    LOG_INFO("Posted news from " LODB_UUID_FMT, LODB_UUID_ARGS(authorUserUuid));
    return true;
}

bool LoBBSDal::isNewsReadByUser(uint64_t newsUuid, uint64_t userUuid)
{
    // Generate UUID for the read record (combination of news and user)
    lodb_uuid_t readUuid = lodb_new_uuid((const char *)&newsUuid, userUuid);

    meshtastic_LoBBSNewsRead readRecord = meshtastic_LoBBSNewsRead_init_zero;
    LoDbError err = db->get("news_reads", readUuid, &readRecord);
    return (err == LODB_OK);
}

bool LoBBSDal::markNewsAsRead(uint64_t newsUuid, uint64_t userUuid)
{
    // Check if already marked as read
    if (isNewsReadByUser(newsUuid, userUuid)) {
        LOG_DEBUG("News " LODB_UUID_FMT " already marked as read by user " LODB_UUID_FMT, LODB_UUID_ARGS(newsUuid),
                  LODB_UUID_ARGS(userUuid));
        return true;
    }

    // Generate UUID for the read record
    lodb_uuid_t readUuid = lodb_new_uuid((const char *)&newsUuid, userUuid);

    // Create read record
    meshtastic_LoBBSNewsRead readRecord = meshtastic_LoBBSNewsRead_init_zero;
    readRecord.news_uuid = newsUuid;
    readRecord.user_uuid = userUuid;
    readRecord.read_timestamp = getTime();

    LoDbError err = db->insert("news_reads", readUuid, &readRecord);
    if (err != LODB_OK) {
        LOG_ERROR("Failed to mark news as read: " LODB_UUID_FMT, LODB_UUID_ARGS(newsUuid));
        return false;
    }

    LOG_DEBUG("Marked news " LODB_UUID_FMT " as read by user " LODB_UUID_FMT, LODB_UUID_ARGS(newsUuid), LODB_UUID_ARGS(userUuid));
    return true;
}

// Comparator for sorting news: unread first, then by timestamp descending
struct NewsWithReadStatus {
    meshtastic_LoBBSNews *news;
    bool isRead;
};

static int compareNewsByReadAndTimestamp(const void *a, const void *b)
{
    const NewsWithReadStatus *n1 = (const NewsWithReadStatus *)a;
    const NewsWithReadStatus *n2 = (const NewsWithReadStatus *)b;

    // Unread comes before read
    if (!n1->isRead && n2->isRead)
        return -1;
    if (n1->isRead && !n2->isRead)
        return 1;

    // Within same read status, sort by timestamp descending (newer first)
    if (n2->news->timestamp > n1->news->timestamp)
        return 1;
    if (n2->news->timestamp < n1->news->timestamp)
        return -1;
    return 0;
}

std::vector<void *> LoBBSDal::getNewsForUser(uint64_t userUuid, uint32_t offset, uint32_t limit)
{
    // Get all news items (no filter needed, news is for everyone)
    auto allNews = db->select("news", nullptr, nullptr);

    // Build array with read status
    std::vector<NewsWithReadStatus> newsWithStatus;
    for (auto *newsPtr : allNews) {
        NewsWithReadStatus item;
        item.news = (meshtastic_LoBBSNews *)newsPtr;
        item.isRead = isNewsReadByUser(item.news->uuid, userUuid);
        newsWithStatus.push_back(item);
    }

    // Sort: unread first, then by timestamp descending
    qsort(newsWithStatus.data(), newsWithStatus.size(), sizeof(NewsWithReadStatus), compareNewsByReadAndTimestamp);

    // Apply offset and limit
    std::vector<void *> result;
    for (size_t i = offset; i < newsWithStatus.size() && i < offset + limit; i++) {
        result.push_back(newsWithStatus[i].news);
    }

    // Free records not included in result
    for (size_t i = 0; i < newsWithStatus.size(); i++) {
        if (i < offset || i >= offset + limit) {
            delete[] (uint8_t *)newsWithStatus[i].news;
        }
    }

    LOG_DEBUG("Retrieved %d news items for user " LODB_UUID_FMT " (offset=%d, limit=%d)", result.size(), LODB_UUID_ARGS(userUuid),
              offset, limit);
    return result;
}
