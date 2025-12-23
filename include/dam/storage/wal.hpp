#pragma once

#include <dam/core_types.hpp>
#include <dam/result.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace dam {

namespace fs = std::filesystem;

/**
 * Log record structure for WAL.
 */
struct LogRecord {
    LSN lsn;
    LSN prev_lsn;           // Previous LSN for this transaction
    TxnId txn_id;
    LogRecordType type;
    PageId page_id;
    std::string key;
    std::string value;
    std::string old_value;  // For UPDATE records

    size_t serialized_size() const;
    void serialize(char* buffer) const;
    static LogRecord deserialize(const char* buffer, size_t len);
};

/**
 * WriteAheadLog - Provides durability through logging.
 *
 * Features:
 * - Transaction begin/commit/abort
 * - Operation logging (insert, delete, update)
 * - Crash recovery (ARIES-style)
 * - Checkpointing
 */
class WriteAheadLog {
public:
    /**
     * Create or open a WAL.
     *
     * @param log_path Path to the WAL file
     * @param buffer_size Size of the in-memory log buffer
     */
    explicit WriteAheadLog(const fs::path& log_path, size_t buffer_size = 64 * 1024);

    ~WriteAheadLog();

    // Prevent copying
    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    /**
     * Begin a new transaction.
     *
     * @return The transaction ID
     */
    TxnId begin_transaction();

    /**
     * Commit a transaction.
     *
     * @param txn_id The transaction to commit
     * @return Result indicating success or failure
     */
    Result<void> commit(TxnId txn_id);

    /**
     * Abort a transaction.
     *
     * @param txn_id The transaction to abort
     * @return Result indicating success or failure
     */
    Result<void> abort(TxnId txn_id);

    /**
     * Log an insert operation.
     *
     * @param txn_id The transaction ID
     * @param page_id The page being modified
     * @param key The key being inserted
     * @param value The value being inserted
     * @return The LSN of the log record
     */
    LSN log_insert(TxnId txn_id, PageId page_id,
                   const std::string& key, const std::string& value);

    /**
     * Log a delete operation.
     */
    LSN log_delete(TxnId txn_id, PageId page_id,
                   const std::string& key, const std::string& old_value);

    /**
     * Log an update operation.
     */
    LSN log_update(TxnId txn_id, PageId page_id,
                   const std::string& key,
                   const std::string& old_value,
                   const std::string& new_value);

    /**
     * Flush the log buffer to disk up to the given LSN.
     *
     * @param lsn Flush all records up to and including this LSN
     */
    Result<void> flush_to(LSN lsn);

    /**
     * Flush all buffered log records to disk.
     */
    Result<void> flush();

    /**
     * Create a checkpoint.
     */
    Result<void> checkpoint();

    /**
     * Recover from the log after a crash.
     *
     * @return List of operations to redo
     */
    Result<void> recover();

    /**
     * Get the current LSN.
     */
    LSN get_current_lsn() const { return current_lsn_; }

    /**
     * Get the flushed LSN.
     */
    LSN get_flushed_lsn() const { return flushed_lsn_; }

    /**
     * Check if WAL is open and valid.
     */
    bool is_open() const { return is_open_; }

private:
    // Append a log record to the buffer
    LSN append_record(const LogRecord& record);

    // Write buffer to disk
    Result<void> write_buffer();

    fs::path log_path_;
    std::fstream log_file_;
    size_t buffer_size_;

    std::vector<char> buffer_;
    size_t buffer_offset_;

    LSN current_lsn_;
    LSN flushed_lsn_;
    TxnId next_txn_id_;

    // Track active transactions
    std::unordered_map<TxnId, LSN> active_txns_;  // txn_id -> last_lsn

    bool is_open_;
    mutable std::mutex mutex_;
};

}  // namespace dam
