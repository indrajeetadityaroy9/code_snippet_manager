#pragma once

#include <variant>
#include <string>
#include <stdexcept>
#include <utility>

namespace dam {

// Error codes for the document store
enum class ErrorCode {
    OK = 0,
    NOT_FOUND,
    ALREADY_EXISTS,
    IO_ERROR,
    CORRUPTION,
    INVALID_ARGUMENT,
    BUFFER_POOL_FULL,
    PAGE_PINNED,
    TRANSACTION_ABORTED,
    WAL_ERROR,
    OUT_OF_SPACE,
    PERMISSION_DENIED,
    INTERNAL_ERROR
};

// Error with code and message
class Error {
public:
    Error() : code_(ErrorCode::OK) {}
    Error(ErrorCode code, std::string message = "")
        : code_(code), message_(std::move(message)) {}

    ErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }

    bool ok() const { return code_ == ErrorCode::OK; }
    explicit operator bool() const { return !ok(); }

    std::string to_string() const {
        if (message_.empty()) {
            return std::string(error_code_name(code_));
        }
        return std::string(error_code_name(code_)) + ": " + message_;
    }

    static const char* error_code_name(ErrorCode code) {
        switch (code) {
            case ErrorCode::OK: return "OK";
            case ErrorCode::NOT_FOUND: return "NOT_FOUND";
            case ErrorCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
            case ErrorCode::IO_ERROR: return "IO_ERROR";
            case ErrorCode::CORRUPTION: return "CORRUPTION";
            case ErrorCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
            case ErrorCode::BUFFER_POOL_FULL: return "BUFFER_POOL_FULL";
            case ErrorCode::PAGE_PINNED: return "PAGE_PINNED";
            case ErrorCode::TRANSACTION_ABORTED: return "TRANSACTION_ABORTED";
            case ErrorCode::WAL_ERROR: return "WAL_ERROR";
            case ErrorCode::OUT_OF_SPACE: return "OUT_OF_SPACE";
            case ErrorCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
            case ErrorCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
            default: return "UNKNOWN";
        }
    }

private:
    ErrorCode code_;
    std::string message_;
};

// Result type for operations that can fail
// Similar to Rust's Result<T, E> or C++23's std::expected
template<typename T>
class Result {
public:
    // Success constructor
    Result(T value) : data_(std::move(value)) {}

    // Error constructors
    Result(Error error) : data_(std::move(error)) {}
    Result(ErrorCode code, std::string message = "")
        : data_(Error(code, std::move(message))) {}

    // Check if result is successful
    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    // Access value (throws if error)
    T& value() & {
        if (!ok()) {
            throw std::runtime_error(error().to_string());
        }
        return std::get<T>(data_);
    }

    const T& value() const& {
        if (!ok()) {
            throw std::runtime_error(error().to_string());
        }
        return std::get<T>(data_);
    }

    T&& value() && {
        if (!ok()) {
            throw std::runtime_error(error().to_string());
        }
        return std::move(std::get<T>(data_));
    }

    // Access value with default
    T value_or(T default_value) const {
        if (ok()) {
            return std::get<T>(data_);
        }
        return default_value;
    }

    // Access error (throws if success)
    const Error& error() const {
        if (ok()) {
            throw std::logic_error("Result has no error");
        }
        return std::get<Error>(data_);
    }

    ErrorCode error_code() const {
        if (ok()) {
            return ErrorCode::OK;
        }
        return error().code();
    }

    // Pointer-like access
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> data_;
};

// Specialization for void results
template<>
class Result<void> {
public:
    Result() : error_() {}
    Result(Error error) : error_(std::move(error)) {}
    Result(ErrorCode code, std::string message = "")
        : error_(Error(code, std::move(message))) {}

    bool ok() const { return error_.ok(); }
    explicit operator bool() const { return ok(); }

    const Error& error() const { return error_; }
    ErrorCode error_code() const { return error_.code(); }

    void value() const {
        if (!ok()) {
            throw std::runtime_error(error_.to_string());
        }
    }

private:
    Error error_;
};

// Helper for creating successful void results
inline Result<void> Ok() { return Result<void>(); }

// Helper for creating errors
inline Error Err(ErrorCode code, std::string message = "") {
    return Error(code, std::move(message));
}

}  // namespace dam

// ============================================================================
// Precondition Guard Macros
// ============================================================================

/**
 * Guard macro for checking if store is open.
 * Use in methods that return Result<T>.
 *
 * Example:
 *   Result<FileId> store_file(...) {
 *       DOCSTORE_REQUIRE_OPEN(is_open_);
 *       // ... rest of implementation
 *   }
 */
#define DAM_REQUIRE_OPEN(is_open) \
    do { \
        if (!(is_open)) { \
            return ::dam::Error(::dam::ErrorCode::INVALID_ARGUMENT, "Store is not open"); \
        } \
    } while(0)

/**
 * Guard macro for methods returning empty containers when store is closed.
 * Use in const methods that return vectors, sets, etc.
 *
 * Example:
 *   std::vector<FileMetadata> get_all_files() const {
 *       DOCSTORE_REQUIRE_OPEN_OR_EMPTY(is_open_, {});
 *       // ... rest of implementation
 *   }
 */
#define DAM_REQUIRE_OPEN_OR_EMPTY(is_open, empty_value) \
    do { \
        if (!(is_open)) { \
            return empty_value; \
        } \
    } while(0)

/**
 * Guard macro for checking if a condition holds, returning an error if not.
 *
 * Example:
 *   DOCSTORE_REQUIRE(file_id != INVALID_FILE_ID, INVALID_ARGUMENT, "Invalid file ID");
 */
#define DAM_REQUIRE(condition, error_code, message) \
    do { \
        if (!(condition)) { \
            return ::dam::Error(::dam::ErrorCode::error_code, message); \
        } \
    } while(0)
