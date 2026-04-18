#include "Job.h"
#include "HexUtils.h"
#include <cstring>

namespace zenrx {

void Job::setBlob(const std::string& hex)
{
    m_blobSize = hex.length() / 2;
    if (m_blobSize > kMaxBlobSize) {
        m_blobSize = kMaxBlobSize;
    }
    
    m_blob.fill(0);
    hexToBytes(hex, m_blob.data(), m_blobSize);
}

void Job::setTarget(const std::string& hex)
{
    // For 4-byte (8 hex char) target:
    //   target = 0xFFFFFFFFFFFFFFFF / (0xFFFFFFFF / compact_target)
    // For 8-byte (16 hex char) target:
    //   target = raw 64-bit value (little-endian)
    
    const size_t size = hex.length();
    
    if (size < 4) {
        m_target = 0;
        m_diff = 0;
        return;
    }
    
    // Convert hex to bytes
    uint8_t raw[8] = {0};
    size_t rawLen = hexToBytes(hex, raw, 8);
    
    if (rawLen == 4) {
        // 4-byte compact target (most common)
        // Read as little-endian uint32
        uint32_t compact = raw[0] | 
                          (static_cast<uint32_t>(raw[1]) << 8) |
                          (static_cast<uint32_t>(raw[2]) << 16) |
                          (static_cast<uint32_t>(raw[3]) << 24);
        
        if (compact == 0) {
            m_target = 0xFFFFFFFFFFFFFFFFULL;
            m_diff = 0;
        } else {
            // target = 0xFFFFFFFFFFFFFFFF / (0xFFFFFFFF / compact)
            m_target = 0xFFFFFFFFFFFFFFFFULL / (0xFFFFFFFFULL / static_cast<uint64_t>(compact));
            m_diff = 0xFFFFFFFFFFFFFFFFULL / m_target;
        }
    }
    else if (rawLen == 8) {
        // 8-byte target - read as little-endian uint64
        m_target = raw[0] |
                  (static_cast<uint64_t>(raw[1]) << 8) |
                  (static_cast<uint64_t>(raw[2]) << 16) |
                  (static_cast<uint64_t>(raw[3]) << 24) |
                  (static_cast<uint64_t>(raw[4]) << 32) |
                  (static_cast<uint64_t>(raw[5]) << 40) |
                  (static_cast<uint64_t>(raw[6]) << 48) |
                  (static_cast<uint64_t>(raw[7]) << 56);
        
        if (m_target == 0) {
            m_diff = 0;
        } else {
            m_diff = 0xFFFFFFFFFFFFFFFFULL / m_target;
        }
    }
    else {
        m_target = 0;
        m_diff = 0;
    }
}

} // namespace zenrx
