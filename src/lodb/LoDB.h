#pragma once

#include <cstddef>
#include <cstdint>
#include <pb.h>
#include <vector>
#include <string>

/**
 * LoDB - Cooperative Protobuf Database
 * 
 * A filesystem-based database for protobuf records stored in /lodb/<table_name>/<uuid>.pr files.
 * 
 * COOPERATIVE DESIGN:
 * - All operations work on single records at a time
 * - SELECT uses cursors that advance one record per call (perfect for OSThread runOnce())
 * - Operations yield control between records for true cooperative multitasking
 */

// UUID type - 64-bit unsigned integer
typedef uint64_t lodb_uuid_t;

// UUID formatting macros for platforms without %llx support
#define LODB_UUID_FMT "%08x%08x"
#define LODB_UUID_ARGS(uuid) (uint32_t)((uuid) >> 32), (uint32_t)((uuid) & 0xFFFFFFFF)

/**
 * Error codes returned by LoDB operations
 */
typedef enum {
    LODB_OK = 0,               // Success
    LODB_ERR_NOT_FOUND,        // UUID doesn't exist
    LODB_ERR_IO,               // Filesystem error
    LODB_ERR_DECODE,           // Protobuf decode failed
    LODB_ERR_ENCODE,           // Protobuf encode failed
    LODB_ERR_INVALID           // Invalid parameters
} LoDbError;

/**
 * Table configuration
 */
typedef struct {
    const char *table_name;              // Table directory name (e.g., "users")
    const pb_msgdesc_t *pb_descriptor;   // Nanopb message descriptor
    size_t record_size;                  // sizeof(struct) for allocation
    char table_path[128];                // Full path: /lodb/<table_name>/
} LoDbTable;

/**
 * Filter function: returns true to select/include record
 * @param record Pointer to the decoded protobuf record
 * @param context User-provided context data
 * @return true to include record in results, false to skip
 */
typedef bool (*LoDbFilter)(const void *record, void *context);

/**
 * Cursor for iterating through selected records
 * Opaque structure - use cursor functions to interact
 */
typedef struct LoDbCursor LoDbCursor;

/**
 * Initialize a table and create necessary directories
 * @param table Table structure to initialize
 * @param table_name Name of the table (directory name)
 * @param pb_descriptor Nanopb message descriptor for the protobuf type
 * @param record_size Size of the in-memory struct (sizeof)
 * @return LODB_OK on success, error code otherwise
 */
LoDbError lodb_init_table(LoDbTable *table, const char *table_name, const pb_msgdesc_t *pb_descriptor, size_t record_size);

/**
 * Convert UUID to 16-character hex string for filenames
 * @param uuid UUID to convert
 * @param hex_out Buffer to store hex string (must be at least 17 bytes for null terminator)
 */
void lodb_uuid_to_hex(lodb_uuid_t uuid, char hex_out[17]);

/**
 * Generate or derive a UUID
 * @param str String to hash into UUID (NULL for auto-generated)
 * @param salt Optional salt value (0 for none, typically node ID for user-specific UUIDs)
 * @return 64-bit UUID - auto-generated if str is NULL, otherwise SHA256(str + salt)
 */
lodb_uuid_t lodb_new_uuid(const char *str, uint64_t salt);

/**
 * Insert a new record with a UUID
 * @param table Table to insert into
 * @param uuid UUID to use for this record
 * @param record Pointer to the protobuf record to insert
 * @return LODB_OK on success, LODB_ERR_INVALID if UUID exists, error code otherwise
 */
LoDbError lodb_insert(LoDbTable *table, lodb_uuid_t uuid, const void *record);

/**
 * Get a record by UUID
 * @param table Table to read from
 * @param uuid UUID of the record to retrieve
 * @param record_out Buffer to store decoded record (must be at least table->record_size bytes)
 * @return LODB_OK on success, LODB_ERR_NOT_FOUND if UUID doesn't exist, error code otherwise
 */
LoDbError lodb_get(LoDbTable *table, lodb_uuid_t uuid, void *record_out);

/**
 * Update a single record by UUID
 * @param table Table to update
 * @param uuid UUID of the record to update
 * @param record Pointer to the updated protobuf record
 * @return LODB_OK on success, LODB_ERR_NOT_FOUND if UUID doesn't exist, error code otherwise
 */
LoDbError lodb_update(LoDbTable *table, lodb_uuid_t uuid, const void *record);

/**
 * Delete a single record by UUID
 * @param table Table to delete from
 * @param uuid UUID of the record to delete
 * @return LODB_OK on success, LODB_ERR_NOT_FOUND if UUID doesn't exist, error code otherwise
 */
LoDbError lodb_delete(LoDbTable *table, lodb_uuid_t uuid);

/**
 * Create a cursor for iterating through records matching a filter
 * This is the cooperative way to scan a table - call lodb_cursor_next() from OSThread::runOnce()
 * 
 * @param table Table to scan
 * @param filter Filter function (returns true to include record), NULL to select all
 * @param context User context passed to filter function
 * @return Cursor pointer on success, NULL on error
 * 
 * USAGE:
 *   cursor = lodb_select_cursor(&table, filter, &ctx);
 *   while (lodb_cursor_next(cursor)) {
 *       const void *record = lodb_cursor_get(cursor);
 *       const char *uuid = lodb_cursor_get_uuid(cursor);
 *       // Process one record, then return from runOnce() for cooperation
 *   }
 *   lodb_cursor_close(cursor);
 */
LoDbCursor *lodb_select_cursor(LoDbTable *table, LoDbFilter filter, void *context);

/**
 * Advance cursor to next file and check if it matches filter
 * COOPERATIVE: Processes exactly ONE file from directory, then returns immediately
 * 
 * Call this from OSThread::runOnce() for true cooperation. If the current file doesn't
 * match the filter, this returns false (not true with next match). Caller should call
 * again until either a match is found or lodb_cursor_is_exhausted() returns true.
 * 
 * @param cursor Cursor to advance
 * @return true if current file matches filter, false if no match OR iteration complete
 * 
 * USAGE:
 *   while (!lodb_cursor_is_exhausted(cursor)) {
 *       if (lodb_cursor_next(cursor)) {
 *           // Process matching record
 *           const void *record = lodb_cursor_get(cursor);
 *           // ... do work, then return from runOnce() for cooperation
 *       }
 *       // No match, but more files remain - return from runOnce() anyway
 *       return 0;
 *   }
 */
bool lodb_cursor_next(LoDbCursor *cursor);

/**
 * Check if cursor has exhausted all files in the directory
 * Use this to distinguish "no match" from "no more files"
 * 
 * @param cursor Cursor to check
 * @return true if no more files to process, false if more files remain
 */
bool lodb_cursor_is_exhausted(LoDbCursor *cursor);

/**
 * Get the current record from the cursor
 * Valid only after lodb_cursor_next() returns true
 * 
 * @param cursor Cursor to query
 * @return Pointer to decoded record (owned by cursor, valid until next call or close)
 */
const void *lodb_cursor_get(LoDbCursor *cursor);

/**
 * Get the UUID of the current record from the cursor
 * Valid only after lodb_cursor_next() returns true
 * 
 * @param cursor Cursor to query
 * @return UUID (owned by cursor, valid until next call or close)
 */
lodb_uuid_t lodb_cursor_get_uuid(LoDbCursor *cursor);

/**
 * Close cursor and free all resources
 * @param cursor Cursor to close (can be NULL)
 */
void lodb_cursor_close(LoDbCursor *cursor);
