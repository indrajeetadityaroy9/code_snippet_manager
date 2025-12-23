#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dam {

/**
 * CRC32 utility functions.
 */
class CRC32 {
public:
    /**
     * Compute CRC32 checksum of data.
     */
    static uint32_t compute(const uint8_t* data, size_t len);
    static uint32_t compute(const char* data, size_t len);
    static uint32_t compute(const std::string& data);

    /**
     * Update a running CRC32 with more data.
     */
    static uint32_t update(uint32_t crc, const uint8_t* data, size_t len);

private:
    static void init_table();
    static uint32_t table_[256];
    static bool table_initialized_;
};

}  // namespace dam
