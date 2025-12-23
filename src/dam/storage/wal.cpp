#include <dam/storage/wal.hpp>
#include <cstring>

namespace dam {

// LogRecord serialization format:
// [lsn:8][prev_lsn:8][txn_id:8][type:1][page_id:4][key_len:4][key:*][val_len:4][value:*][old_val_len:4][old_value:*]

size_t LogRecord::serialized_size() const {
    return 8 + 8 + 8 + 1 + 4 + 4 + key.size() + 4 + value.size() + 4 + old_value.size();
}

void LogRecord::serialize(char* buffer) const {
    size_t offset = 0;

    std::memcpy(buffer + offset, &lsn, 8); offset += 8;
    std::memcpy(buffer + offset, &prev_lsn, 8); offset += 8;
    std::memcpy(buffer + offset, &txn_id, 8); offset += 8;
    std::memcpy(buffer + offset, &type, 1); offset += 1;
    std::memcpy(buffer + offset, &page_id, 4); offset += 4;

    uint32_t key_len = static_cast<uint32_t>(key.size());
    std::memcpy(buffer + offset, &key_len, 4); offset += 4;
    std::memcpy(buffer + offset, key.data(), key.size()); offset += key.size();

    uint32_t val_len = static_cast<uint32_t>(value.size());
    std::memcpy(buffer + offset, &val_len, 4); offset += 4;
    std::memcpy(buffer + offset, value.data(), value.size()); offset += value.size();

    uint32_t old_len = static_cast<uint32_t>(old_value.size());
    std::memcpy(buffer + offset, &old_len, 4); offset += 4;
    std::memcpy(buffer + offset, old_value.data(), old_value.size());
}

LogRecord LogRecord::deserialize(const char* buffer, size_t len) {
    LogRecord record;
    size_t offset = 0;

    if (len < 33) return record;  // Minimum size

    std::memcpy(&record.lsn, buffer + offset, 8); offset += 8;
    std::memcpy(&record.prev_lsn, buffer + offset, 8); offset += 8;
    std::memcpy(&record.txn_id, buffer + offset, 8); offset += 8;
    std::memcpy(&record.type, buffer + offset, 1); offset += 1;
    std::memcpy(&record.page_id, buffer + offset, 4); offset += 4;

    // Read key_len - check bounds first
    if (offset + 4 > len) return record;
    uint32_t key_len;
    std::memcpy(&key_len, buffer + offset, 4); offset += 4;
    // Overflow-safe check: key_len <= len - offset (which is equivalent but safe)
    if (key_len <= len - offset) {
        record.key = std::string(buffer + offset, key_len);
        offset += key_len;
    } else {
        return record;  // Corrupted or truncated
    }

    // Read val_len - check bounds first
    if (offset + 4 > len) return record;
    uint32_t val_len;
    std::memcpy(&val_len, buffer + offset, 4); offset += 4;
    if (val_len <= len - offset) {
        record.value = std::string(buffer + offset, val_len);
        offset += val_len;
    } else {
        return record;
    }

    // Read old_len - check bounds first
    if (offset + 4 > len) return record;
    uint32_t old_len;
    std::memcpy(&old_len, buffer + offset, 4); offset += 4;
    if (old_len <= len - offset) {
        record.old_value = std::string(buffer + offset, old_len);
    }

    return record;
}

WriteAheadLog::WriteAheadLog(const fs::path& log_path, size_t buffer_size)
    : log_path_(log_path)
    , buffer_size_(buffer_size)
    , buffer_(buffer_size)
    , buffer_offset_(0)
    , current_lsn_(1)
    , flushed_lsn_(0)
    , next_txn_id_(1)
    , is_open_(false)
{
    fs::create_directories(log_path_.parent_path());

    log_file_.open(log_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!log_file_.is_open()) {
        log_file_.open(log_path_, std::ios::out | std::ios::binary | std::ios::trunc);
        log_file_.close();
        log_file_.open(log_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    }

    if (log_file_.is_open()) {
        is_open_ = true;
    }
}

WriteAheadLog::~WriteAheadLog() {
    if (is_open_) {
        flush();
        log_file_.close();
    }
}

TxnId WriteAheadLog::begin_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);

    TxnId txn_id = next_txn_id_++;

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::BEGIN;
    record.page_id = INVALID_PAGE_ID;
    record.prev_lsn = INVALID_LSN;

    LSN lsn = append_record(record);
    active_txns_[txn_id] = lsn;

    return txn_id;
}

Result<void> WriteAheadLog::commit(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_txns_.find(txn_id);
    if (it == active_txns_.end()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Transaction not found");
    }

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::COMMIT;
    record.page_id = INVALID_PAGE_ID;
    record.prev_lsn = it->second;

    append_record(record);
    active_txns_.erase(it);

    // Force flush on commit for durability
    return write_buffer();
}

Result<void> WriteAheadLog::abort(TxnId txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_txns_.find(txn_id);
    if (it == active_txns_.end()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Transaction not found");
    }

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::ABORT;
    record.page_id = INVALID_PAGE_ID;
    record.prev_lsn = it->second;

    append_record(record);
    active_txns_.erase(it);

    return write_buffer();
}

LSN WriteAheadLog::log_insert(TxnId txn_id, PageId page_id,
                               const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_txns_.find(txn_id);
    LSN prev_lsn = (it != active_txns_.end()) ? it->second : INVALID_LSN;

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::INSERT;
    record.page_id = page_id;
    record.key = key;
    record.value = value;
    record.prev_lsn = prev_lsn;

    LSN lsn = append_record(record);

    if (it != active_txns_.end()) {
        it->second = lsn;
    }

    return lsn;
}

LSN WriteAheadLog::log_delete(TxnId txn_id, PageId page_id,
                               const std::string& key, const std::string& old_value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_txns_.find(txn_id);
    LSN prev_lsn = (it != active_txns_.end()) ? it->second : INVALID_LSN;

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::DELETE;
    record.page_id = page_id;
    record.key = key;
    record.old_value = old_value;
    record.prev_lsn = prev_lsn;

    LSN lsn = append_record(record);

    if (it != active_txns_.end()) {
        it->second = lsn;
    }

    return lsn;
}

LSN WriteAheadLog::log_update(TxnId txn_id, PageId page_id,
                               const std::string& key,
                               const std::string& old_value,
                               const std::string& new_value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_txns_.find(txn_id);
    LSN prev_lsn = (it != active_txns_.end()) ? it->second : INVALID_LSN;

    LogRecord record;
    record.txn_id = txn_id;
    record.type = LogRecordType::UPDATE;
    record.page_id = page_id;
    record.key = key;
    record.old_value = old_value;
    record.value = new_value;
    record.prev_lsn = prev_lsn;

    LSN lsn = append_record(record);

    if (it != active_txns_.end()) {
        it->second = lsn;
    }

    return lsn;
}

LSN WriteAheadLog::append_record(const LogRecord& record) {
    LogRecord rec = record;
    rec.lsn = current_lsn_++;

    size_t size = rec.serialized_size();

    // Check if single record is too large for buffer (would overflow)
    if (size + 4 > buffer_size_) {
        // Record too large - cannot be written to WAL buffer
        // Decrement LSN since we're not actually writing this record
        --current_lsn_;
        return INVALID_LSN;
    }

    // Check if we need to flush
    if (buffer_offset_ + size + 4 > buffer_size_) {
        write_buffer();
    }

    // Write record size prefix
    uint32_t size32 = static_cast<uint32_t>(size);
    std::memcpy(buffer_.data() + buffer_offset_, &size32, 4);
    buffer_offset_ += 4;

    // Write record
    rec.serialize(buffer_.data() + buffer_offset_);
    buffer_offset_ += size;

    return rec.lsn;
}

Result<void> WriteAheadLog::flush_to(LSN lsn) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (lsn <= flushed_lsn_) {
        return Ok();
    }

    return write_buffer();
}

Result<void> WriteAheadLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_buffer();
}

Result<void> WriteAheadLog::write_buffer() {
    if (buffer_offset_ == 0) {
        return Ok();
    }

    log_file_.write(buffer_.data(), static_cast<std::streamsize>(buffer_offset_));
    log_file_.flush();

    if (!log_file_.good()) {
        return Error(ErrorCode::IO_ERROR, "Failed to write WAL");
    }

    flushed_lsn_ = current_lsn_ - 1;
    buffer_offset_ = 0;

    return Ok();
}

Result<void> WriteAheadLog::checkpoint() {
    std::lock_guard<std::mutex> lock(mutex_);

    LogRecord begin_record;
    begin_record.txn_id = INVALID_TXN_ID;
    begin_record.type = LogRecordType::CHECKPOINT_BEGIN;
    begin_record.page_id = INVALID_PAGE_ID;
    begin_record.prev_lsn = INVALID_LSN;
    append_record(begin_record);

    // In a full implementation, we would:
    // 1. Record all active transactions
    // 2. Record dirty page table
    // 3. Flush all dirty pages

    LogRecord end_record;
    end_record.txn_id = INVALID_TXN_ID;
    end_record.type = LogRecordType::CHECKPOINT_END;
    end_record.page_id = INVALID_PAGE_ID;
    end_record.prev_lsn = INVALID_LSN;
    append_record(end_record);

    return write_buffer();
}

Result<void> WriteAheadLog::recover() {
    // Simplified recovery - in a full implementation, this would:
    // 1. Analysis phase: scan log from last checkpoint
    // 2. Redo phase: redo all committed operations
    // 3. Undo phase: undo all uncommitted operations

    // For now, just reset state
    current_lsn_ = 1;
    flushed_lsn_ = 0;
    active_txns_.clear();

    return Ok();
}

}  // namespace dam
