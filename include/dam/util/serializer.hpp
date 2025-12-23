#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <set>
#include <vector>

namespace dam {

/**
 * BinaryWriter - Efficient binary serialization utility.
 *
 * Provides type-safe methods for writing primitive types and strings
 * to a binary buffer. Used across tag_index, file_index, and WAL
 * for consistent serialization.
 */
class BinaryWriter {
public:
    BinaryWriter() = default;

    // Reserve buffer capacity to avoid reallocations
    void reserve(size_t size) { buffer_.reserve(size); }

    // Write primitive types
    void write_uint8(uint8_t v) {
        buffer_.push_back(static_cast<char>(v));
    }

    void write_uint16(uint16_t v) {
        buffer_.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    void write_uint32(uint32_t v) {
        buffer_.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    void write_uint64(uint64_t v) {
        buffer_.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    void write_int64(int64_t v) {
        buffer_.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    // Write length-prefixed string (uint32 length + data)
    void write_string(const std::string& s) {
        write_uint32(static_cast<uint32_t>(s.size()));
        buffer_.append(s);
    }

    // Write raw bytes
    void write_raw(const void* data, size_t size) {
        buffer_.append(reinterpret_cast<const char*>(data), size);
    }

    // Get the serialized data
    const std::string& data() const { return buffer_; }
    std::string&& release() { return std::move(buffer_); }

    // Current size
    size_t size() const { return buffer_.size(); }

    // Clear and reuse
    void clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

/**
 * BinaryReader - Efficient binary deserialization utility.
 *
 * Provides type-safe methods for reading primitive types and strings
 * from a binary buffer with bounds checking.
 */
class BinaryReader {
public:
    explicit BinaryReader(const std::string& data)
        : ptr_(data.data())
        , end_(data.data() + data.size())
    {}

    BinaryReader(const char* data, size_t size)
        : ptr_(data)
        , end_(data + size)
    {}

    // Check if there's enough data remaining
    bool has_remaining(size_t size) const {
        return static_cast<size_t>(end_ - ptr_) >= size;
    }

    // Remaining bytes
    size_t remaining() const {
        return static_cast<size_t>(end_ - ptr_);
    }

    // Read primitive types (returns false if not enough data)
    bool read_uint8(uint8_t* v) {
        if (!has_remaining(sizeof(*v))) return false;
        *v = static_cast<uint8_t>(*ptr_);
        ptr_ += sizeof(*v);
        return true;
    }

    bool read_uint16(uint16_t* v) {
        if (!has_remaining(sizeof(*v))) return false;
        std::memcpy(v, ptr_, sizeof(*v));
        ptr_ += sizeof(*v);
        return true;
    }

    bool read_uint32(uint32_t* v) {
        if (!has_remaining(sizeof(*v))) return false;
        std::memcpy(v, ptr_, sizeof(*v));
        ptr_ += sizeof(*v);
        return true;
    }

    bool read_uint64(uint64_t* v) {
        if (!has_remaining(sizeof(*v))) return false;
        std::memcpy(v, ptr_, sizeof(*v));
        ptr_ += sizeof(*v);
        return true;
    }

    bool read_int64(int64_t* v) {
        if (!has_remaining(sizeof(*v))) return false;
        std::memcpy(v, ptr_, sizeof(*v));
        ptr_ += sizeof(*v);
        return true;
    }

    // Read length-prefixed string
    bool read_string(std::string* s) {
        uint32_t len;
        if (!read_uint32(&len)) return false;
        if (!has_remaining(len)) return false;
        s->assign(ptr_, len);
        ptr_ += len;
        return true;
    }

    // Read raw bytes
    bool read_raw(void* data, size_t size) {
        if (!has_remaining(size)) return false;
        std::memcpy(data, ptr_, size);
        ptr_ += size;
        return true;
    }

    // Skip bytes
    bool skip(size_t size) {
        if (!has_remaining(size)) return false;
        ptr_ += size;
        return true;
    }

private:
    const char* ptr_;
    const char* end_;
};

/**
 * Convenience functions for common serialization patterns
 */

// Serialize a set of FileIds (used by TagIndex)
template<typename T>
inline std::string serialize_id_set(const std::set<T>& ids) {
    BinaryWriter writer;
    writer.reserve(ids.size() * sizeof(T));
    for (const T& id : ids) {
        writer.write_raw(&id, sizeof(T));
    }
    return writer.release();
}

// Deserialize a set of FileIds
template<typename T>
inline std::set<T> deserialize_id_set(const std::string& data) {
    std::set<T> result;
    BinaryReader reader(data);
    T id;
    while (reader.read_raw(&id, sizeof(T))) {
        result.insert(id);
    }
    return result;
}

}  // namespace dam
