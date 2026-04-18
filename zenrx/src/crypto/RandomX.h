#pragma once

#include "crypto/RxInstance.h"
#include "Config.h"
#include <memory>
#include <unordered_map>
#include <shared_mutex>

namespace zenrx {

// Instance identifiers
enum class RxInstanceId {
    User,   // User pool mining
    Dev     // Dev fee mining
};

// Manager for multiple RandomX instances
class RandomX {
public:
    RandomX();
    ~RandomX();

    bool init(RxInstanceId id, const std::string& seedHash, int threads,
              RxAlgo algo = RxAlgo::RX_0,
              bool hugePages = true, bool hardwareAES = true, int initThreads = -1,
              bool oneGbPages = false, const std::vector<int32_t>& numaNodes = {});

    bool init(const std::string& seedHash, int threads,
              RxAlgo algo = RxAlgo::RX_0,
              bool hugePages = true, bool hardwareAES = true, int initThreads = -1,
              bool oneGbPages = false, const std::vector<int32_t>& numaNodes = {});

    bool reinit(RxInstanceId id, const std::string& seedHash, int threads,
                bool hardwareAES = true, int initThreads = -1,
                const std::vector<int32_t>& numaNodes = {});

    bool isSeedValid(const std::string& seedHash) const;
    bool isSeedValid(RxInstanceId id, const std::string& seedHash) const;

    randomx_vm* getVM(int index);
    randomx_vm* getVM(RxInstanceId id, int index);

    RxInstance* getInstance(RxInstanceId id);

    void release();
    void release(RxInstanceId id);

    bool allHugePagesEnabled() const;

    static bool hasAES();
    static bool hasAVX2();

private:
    RxInstance* resolve(RxInstanceId id) const;

    std::unique_ptr<RxInstance> m_userInstance;
    std::unique_ptr<RxInstance> m_devInstance;
    mutable std::shared_mutex m_mutex;
};

// Singleton instance
RandomX& randomx();

} // namespace zenrx
