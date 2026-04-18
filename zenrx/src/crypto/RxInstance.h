#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#include "Config.h"

struct randomx_cache;
struct randomx_dataset;
class randomx_vm;

namespace zenrx { class VirtualMemory; }

namespace zenrx {

class RxInstance {
public:
    RxInstance();
    ~RxInstance();

    bool init(const std::string& seedHash, int threads, RxAlgo algo = RxAlgo::RX_0,
              bool hugePages = true, bool hardwareAES = true, int initThreads = -1,
              bool oneGbPages = false, const std::vector<int32_t>& numaNodes = {});
    bool reinit(const std::string& seedHash, int threads, bool hardwareAES = true,
                int initThreads = -1, const std::vector<int32_t>& numaNodes = {});

    bool isValidForSeed(const std::string& seedHash) const;
    randomx_vm* getVM(int index);
    void release();

    const std::string& seedHash() const { return m_seedHash; }
    bool isInitialized() const { return m_initialized; }
    bool hasHugePages() const { return m_allocatedWithHugePages && m_cacheAllocatedWithHugePages; }
    int threadCount() const { return static_cast<int>(m_vms.size()); }
    RxAlgo algo() const { return m_algo; }

private:
    void initDataset(int threads);
    void applyAlgoConfig(RxAlgo algo);
    void destroyVMs();
    bool createVMs(int threads, int vmFlags, const std::vector<int32_t>& numaNodes = {});
    static int detectFlags(bool hardwareAES);

    std::string m_seedHash;
    randomx_cache* m_cache = nullptr;
    randomx_dataset* m_dataset = nullptr;
    uint8_t* m_cacheMemory = nullptr;
    size_t m_cacheMemorySize = 0;
    uint8_t* m_datasetMemory = nullptr;
    size_t m_datasetMemorySize = 0;
    std::vector<randomx_vm*> m_vms;
    std::vector<zenrx::VirtualMemory*> m_scratchpads;

    RxAlgo m_algo = RxAlgo::RX_0;
    bool m_hugePages = true;
    bool m_hardwareAES = true;
    bool m_allocatedWithHugePages = false;      // Dataset hugepages
    bool m_cacheAllocatedWithHugePages = false;  // Cache hugepages (may differ from dataset)
    bool m_using1GbPages = false;                // Dataset uses 1GB hugepages
    std::atomic<bool> m_initialized{false};
    mutable std::mutex m_mutex;
};

} // namespace zenrx
