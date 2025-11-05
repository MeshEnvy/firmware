#include "LoDB.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include <Arduino.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <cstring>

/**
 * LoDB Implementation - Cursor-based Cooperative Design
 * 
 * Threading Model:
 * - All filesystem operations use LockGuard(spiLock) for thread safety
 * - Single-record operations (insert, get, update, delete) complete immediately
 * - SELECT uses cursors that advance one record at a time for OSThread cooperation
 * - Each cursor advance is a natural yield point for cooperative scheduling
 */

// Cursor structure for cooperative iteration
struct LoDbCursor {
    LoDbTable *table;
    LoDbFilter filter;
    void *filter_context;
    
    // Streaming directory iteration (one file at a time!)
#ifdef FSCom
    File *dir_handle;                // Pointer to open directory handle for streaming
#endif
    bool dir_exhausted;              // True when we've read all files
    
    // Current record state
    uint8_t *current_record;         // Buffer for decoded record
    char current_uuid[LODB_UUID_LEN];
    bool has_current;
};

// Generate a unique 12-character hex UUID
// Format: 8 chars timestamp + 4 chars random
void lodb_generate_uuid(char *uuid_out)
{
    uint32_t timestamp = getTime();
    uint16_t rand_part = random(0x10000); // 16-bit random value

    // Format: 8 hex chars for timestamp + 4 hex chars for random
    snprintf(uuid_out, LODB_UUID_LEN, "%08x%04x", timestamp, rand_part);
}

// Initialize a table and create necessary directories
LoDbError lodb_init_table(LoDbTable *table, const char *table_name, const pb_msgdesc_t *pb_descriptor, size_t record_size)
{
    if (!table || !table_name || !pb_descriptor || record_size == 0) {
        return LODB_ERR_INVALID;
    }

    table->table_name = table_name;
    table->pb_descriptor = pb_descriptor;
    table->record_size = record_size;

    // Build table path
    snprintf(table->table_path, sizeof(table->table_path), "/lodb/%s", table_name);

#ifdef FSCom
    // Create directories
    concurrency::LockGuard g(spiLock);
    FSCom.mkdir("/lodb");
    if (!FSCom.mkdir(table->table_path)) {
        LOG_DEBUG("Table directory may already exist or created: %s", table->table_path);
    }
#else
    LOG_ERROR("Filesystem not available");
    return LODB_ERR_IO;
#endif

    LOG_INFO("Initialized LoDB table: %s", table->table_path);
    return LODB_OK;
}

// Insert a new record with auto-generated UUID
LoDbError lodb_insert(LoDbTable *table, const void *record, char *uuid_out)
{
    if (!table || !record || !uuid_out) {
        return LODB_ERR_INVALID;
    }

#ifdef FSCom
    // Generate UUID
    lodb_generate_uuid(uuid_out);

    // Build file path
    char file_path[160];
    snprintf(file_path, sizeof(file_path), "%s/%s.pr", table->table_path, uuid_out);

    // Encode to buffer
    uint8_t buffer[2048];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, table->pb_descriptor, record)) {
        LOG_ERROR("Failed to encode protobuf for insert");
        return LODB_ERR_ENCODE;
    }

    size_t encoded_size = stream.bytes_written;
    LOG_DEBUG("Encoded record: %d bytes", encoded_size);

    // Write to file
    {
        concurrency::LockGuard g(spiLock);
        auto file = FSCom.open(file_path, FILE_O_WRITE);
        if (!file) {
            LOG_ERROR("Failed to open file for writing: %s", file_path);
            return LODB_ERR_IO;
        }

        size_t written = file.write(buffer, encoded_size);
        if (written != encoded_size) {
            LOG_ERROR("Failed to write file, wrote %d of %d bytes", written, encoded_size);
            file.close();
            return LODB_ERR_IO;
        }

        file.flush();
        file.close();
        LOG_DEBUG("Wrote record to: %s (%d bytes)", file_path, encoded_size);
    }

    LOG_INFO("Inserted record with UUID: %s", uuid_out);
    return LODB_OK;
#else
    LOG_ERROR("Filesystem not available");
    return LODB_ERR_IO;
#endif
}

// Get a record by UUID
LoDbError lodb_get(LoDbTable *table, const char *uuid, void *record_out)
{
    if (!table || !uuid || !record_out) {
        return LODB_ERR_INVALID;
    }

#ifdef FSCom
    // Build file path
    char file_path[160];
    snprintf(file_path, sizeof(file_path), "%s/%s.pr", table->table_path, uuid);

    // Read file into buffer
    uint8_t buffer[2048];
    size_t file_size = 0;

    {
        concurrency::LockGuard g(spiLock);
        auto file = FSCom.open(file_path, FILE_O_READ);
        if (!file) {
            LOG_DEBUG("Record not found: %s", uuid);
            return LODB_ERR_NOT_FOUND;
        }

        file_size = file.read(buffer, sizeof(buffer));
        file.close();

        if (file_size == 0) {
            LOG_ERROR("Record file is empty: %s", uuid);
            return LODB_ERR_IO;
        }

        LOG_DEBUG("Read record file: %s (%d bytes)", file_path, file_size);
    }

    // Decode from buffer
    pb_istream_t stream = pb_istream_from_buffer(buffer, file_size);
    memset(record_out, 0, table->record_size);

    if (!pb_decode(&stream, table->pb_descriptor, record_out)) {
        LOG_ERROR("Failed to decode protobuf from %s", uuid);
        return LODB_ERR_DECODE;
    }

    LOG_DEBUG("Retrieved record: %s", uuid);
    return LODB_OK;
#else
    LOG_ERROR("Filesystem not available");
    return LODB_ERR_IO;
#endif
}

// Update a single record by UUID
LoDbError lodb_update(LoDbTable *table, const char *uuid, const void *record)
{
    if (!table || !uuid || !record) {
        return LODB_ERR_INVALID;
    }

#ifdef FSCom
    // Build file path
    char file_path[160];
    snprintf(file_path, sizeof(file_path), "%s/%s.pr", table->table_path, uuid);

    // Check if record exists first
    {
        concurrency::LockGuard g(spiLock);
        auto file = FSCom.open(file_path, FILE_O_READ);
        if (!file) {
            LOG_DEBUG("Record not found for update: %s", uuid);
            return LODB_ERR_NOT_FOUND;
        }
        file.close();
    }

    // Encode to buffer
    uint8_t buffer[2048];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, table->pb_descriptor, record)) {
        LOG_ERROR("Failed to encode updated record: %s", uuid);
        return LODB_ERR_ENCODE;
    }

    size_t encoded_size = stream.bytes_written;

    // Write to file
    {
        concurrency::LockGuard g(spiLock);
        FSCom.remove(file_path); // Remove old file
        auto file = FSCom.open(file_path, FILE_O_WRITE);
        if (!file) {
            LOG_ERROR("Failed to open file for update: %s", file_path);
            return LODB_ERR_IO;
        }

        size_t written = file.write(buffer, encoded_size);
        if (written != encoded_size) {
            LOG_ERROR("Failed to write updated file");
            file.close();
            return LODB_ERR_IO;
        }

        file.flush();
        file.close();
    }

    LOG_INFO("Updated record: %s", uuid);
    return LODB_OK;
#else
    LOG_ERROR("Filesystem not available");
    return LODB_ERR_IO;
#endif
}

// Delete a single record by UUID
LoDbError lodb_delete(LoDbTable *table, const char *uuid)
{
    if (!table || !uuid) {
        return LODB_ERR_INVALID;
    }

#ifdef FSCom
    char file_path[160];
    snprintf(file_path, sizeof(file_path), "%s/%s.pr", table->table_path, uuid);

    {
        concurrency::LockGuard g(spiLock);
        if (FSCom.remove(file_path)) {
            LOG_DEBUG("Deleted record: %s", uuid);
            return LODB_OK;
        } else {
            LOG_WARN("Failed to delete record (may not exist): %s", uuid);
            return LODB_ERR_NOT_FOUND;
        }
    }
#else
    LOG_ERROR("Filesystem not available");
    return LODB_ERR_IO;
#endif
}

// Create a cursor for iterating through records
LoDbCursor *lodb_select_cursor(LoDbTable *table, LoDbFilter filter, void *context)
{
    if (!table) {
        return nullptr;
    }

    // Allocate cursor
    LoDbCursor *cursor = new LoDbCursor();
    if (!cursor) {
        LOG_ERROR("Failed to allocate cursor");
        return nullptr;
    }

    cursor->table = table;
    cursor->filter = filter;
    cursor->filter_context = context;
    cursor->dir_exhausted = false;
    cursor->has_current = false;
    cursor->current_record = new uint8_t[table->record_size];
    
    if (!cursor->current_record) {
        LOG_ERROR("Failed to allocate cursor record buffer");
        delete cursor;
        return nullptr;
    }

#ifdef FSCom
    // Open directory for streaming (one file at a time!)
    {
        concurrency::LockGuard g(spiLock);
        
        File dir = FSCom.open(table->table_path, FILE_O_READ);
        if (!dir) {
            LOG_DEBUG("Table directory not found: %s", table->table_path);
            cursor->dir_handle = nullptr;
            cursor->dir_exhausted = true; // Empty table, mark as done
            return cursor;
        }

        if (!dir.isDirectory()) {
            LOG_ERROR("Table path is not a directory: %s", table->table_path);
            dir.close();
            delete[] cursor->current_record;
            delete cursor;
            return nullptr;
        }
        
        // Allocate File on heap and move/copy into it
        cursor->dir_handle = new File(dir);
    }

    LOG_DEBUG("Cursor created for streaming directory: %s", table->table_path);
#else
    cursor->dir_handle = nullptr;
    cursor->dir_exhausted = true; // No filesystem, mark as done
#endif

    return cursor;
}

// Advance cursor to next file and check if it matches
// COOPERATIVE: Processes exactly ONE file, then returns (no recursion!)
bool lodb_cursor_next(LoDbCursor *cursor)
{
    if (!cursor || cursor->dir_exhausted) {
        return false;
    }

    cursor->has_current = false;

#ifdef FSCom
    // Read exactly ONE file from directory (cooperative yield point!)
    std::string pathStr;
    {
        concurrency::LockGuard g(spiLock);
        if (!cursor->dir_handle) {
            cursor->dir_exhausted = true;
            return false;
        }
        
        File file = cursor->dir_handle->openNextFile();
        
        if (!file) {
            // No more files in directory
            cursor->dir_exhausted = true;
            LOG_DEBUG("Cursor exhausted all files");
            return false;
        }

        // Skip directories - return false but don't mark exhausted
        if (file.isDirectory()) {
            file.close();
            LOG_DEBUG("Skipped directory entry");
            return false; // Caller will call again for next file
        }

        // Extract UUID from filename
        pathStr = file.name();
        file.close(); // Done with file handle
    }

    // Extract just the filename (after last /)
    size_t lastSlash = pathStr.rfind('/');
    std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

    // Extract UUID (remove .pr extension)
    size_t prPos = filename.find(".pr");
    if (prPos == std::string::npos) {
        // Not a .pr file, skip it
        LOG_DEBUG("Skipped non-.pr file: %s", filename.c_str());
        return false; // Caller will call again for next file
    }

    std::string uuid = filename.substr(0, prPos);
    
    // Read and decode the record
    memset(cursor->current_record, 0, cursor->table->record_size);
    LoDbError err = lodb_get(cursor->table, uuid.c_str(), cursor->current_record);
    
    if (err != LODB_OK) {
        LOG_WARN("Failed to read record %s during cursor iteration", uuid.c_str());
        return false; // Caller will call again for next file
    }

    // Apply filter if provided
    if (cursor->filter && !cursor->filter(cursor->current_record, cursor->filter_context)) {
        LOG_DEBUG("Record %s filtered out", uuid.c_str());
        return false; // Doesn't match, caller will call again
    }

    // Found a matching record!
    strncpy(cursor->current_uuid, uuid.c_str(), LODB_UUID_LEN - 1);
    cursor->current_uuid[LODB_UUID_LEN - 1] = '\0';
    cursor->has_current = true;
    
    LOG_DEBUG("Cursor found matching record: %s", cursor->current_uuid);
    return true; // Match found!
#else
    cursor->dir_exhausted = true;
    return false;
#endif
}

// Check if cursor is exhausted (no more files to process)
bool lodb_cursor_is_exhausted(LoDbCursor *cursor)
{
    if (!cursor) {
        return true;
    }
    return cursor->dir_exhausted;
}

// Get the current record from cursor
const void *lodb_cursor_get(LoDbCursor *cursor)
{
    if (!cursor || !cursor->has_current) {
        return nullptr;
    }
    return cursor->current_record;
}

// Get the UUID of the current record
const char *lodb_cursor_get_uuid(LoDbCursor *cursor)
{
    if (!cursor || !cursor->has_current) {
        return nullptr;
    }
    return cursor->current_uuid;
}

// Close cursor and free resources
void lodb_cursor_close(LoDbCursor *cursor)
{
    if (!cursor) {
        return;
    }

#ifdef FSCom
    // Close directory handle if still open
    if (cursor->dir_handle) {
        {
            concurrency::LockGuard g(spiLock);
            cursor->dir_handle->close();
        }
        delete cursor->dir_handle;
        cursor->dir_handle = nullptr;
    }
#endif

    if (cursor->current_record) {
        delete[] cursor->current_record;
    }

    delete cursor;
    LOG_DEBUG("Cursor closed");
}
