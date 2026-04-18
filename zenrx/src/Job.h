#pragma once

#include <string>
#include <cstdint>
#include <cstring>
#include <array>
#include <atomic>

#include "Config.h"

namespace zenrx {

// Forward declaration
enum class RxInstanceId;

class Job {
public:
    static constexpr size_t kMaxBlobSize = 128;
    
    Job() = default;
    
    Job(const std::string& id, const std::string& blob, const std::string& target, 
        uint64_t height = 0, const std::string& seedHash = "")
        : m_id(id), m_height(height), m_seedHash(seedHash)
    {
        setBlob(blob);
        setTarget(target);
    }
    
    bool isValid() const { return m_blobSize > 0 && m_diff > 0; }
    
    const std::string& id() const { return m_id; }
    const uint8_t* blob() const { return m_blob.data(); }
    uint8_t* blob() { return m_blob.data(); }  // Non-const for in-place nonce modification
    size_t blobSize() const { return m_blobSize; }
    uint64_t target() const { return m_target; }
    uint64_t diff() const { return m_diff; }
    uint64_t height() const { return m_height; }
    const std::string& seedHash() const { return m_seedHash; }
    const std::string& algo() const { return m_algo; }
    RxAlgo rxAlgo() const { return m_rxAlgo; }
    void setAlgo(const std::string& algo) { m_algo = algo; m_rxAlgo = parseRxAlgo(algo); }
    
    void setBlob(const std::string& hex);
    void setTarget(const std::string& hex);
    
    // Set nonce at offset 39 (standard position for CryptoNote)
    void setNonce(uint32_t nonce) {
        if (m_blobSize >= 43) {
            memcpy(m_blob.data() + 39, &nonce, sizeof(nonce));
        }
    }
    
    uint32_t getNonce() const {
        uint32_t nonce = 0;
        if (m_blobSize >= 43) {
            memcpy(&nonce, m_blob.data() + 39, sizeof(nonce));
        }
        return nonce;
    }
    
    // Sequence number for job ordering
    uint64_t sequence() const { return m_sequence; }
    void setSequence(uint64_t seq) { m_sequence = seq; }
    
    // Which RandomX instance to use for this job
    int rxInstanceId() const { return m_rxInstanceId; }
    void setRxInstanceId(int id) { m_rxInstanceId = id; }
    
    bool operator==(const Job& other) const {
        return m_id == other.m_id;
    }
    
    bool operator!=(const Job& other) const {
        return !(*this == other);
    }

private:
    std::string m_id;
    std::array<uint8_t, kMaxBlobSize> m_blob{};
    size_t m_blobSize = 0;
    uint64_t m_target = 0;
    uint64_t m_diff = 0;
    uint64_t m_height = 0;
    std::string m_seedHash;
    std::string m_algo;
    RxAlgo m_rxAlgo = RxAlgo::RX_0;
    uint64_t m_sequence = 0;
    int m_rxInstanceId = 0;  // 0 = User, 1 = Dev
};

// Result to submit
struct JobResult {
    std::string jobId;
    uint32_t nonce;
    std::array<uint8_t, 32> hash;
    uint64_t actualDiff;
    int rxInstanceId = 0;  // Track which instance was used
};

} // namespace zenrx
