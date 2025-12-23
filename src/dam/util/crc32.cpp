#include <dam/util/crc32.hpp>

namespace dam {

uint32_t CRC32::table_[256];
bool CRC32::table_initialized_ = false;

void CRC32::init_table() {
    if (table_initialized_) return;

    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        table_[i] = crc;
    }

    table_initialized_ = true;
}

uint32_t CRC32::compute(const uint8_t* data, size_t len) {
    init_table();

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = table_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

uint32_t CRC32::compute(const char* data, size_t len) {
    return compute(reinterpret_cast<const uint8_t*>(data), len);
}

uint32_t CRC32::compute(const std::string& data) {
    return compute(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

uint32_t CRC32::update(uint32_t crc, const uint8_t* data, size_t len) {
    init_table();

    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc = table_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace dam
