#include "crypto/RxInstance.h"
#include "Log.h"
#include "CpuInfo.h"
#include "HexUtils.h"

#include "crypto/randomx/randomx.h"
#include "crypto/randomx/blake2/blake2.h"
#include "crypto/VirtualMemory.h"

#include <cstring>
#include <thread>
#include <cpuid.h>
#include "crypto/randomx/blake2/avx2/blake2b.h"

// blake2b function pointers required by randomx library — select best at startup
static bool hasSSE41() {
    uint32_t eax, ebx, ecx, edx;
    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    return (ecx >> 19) & 1;
}

static bool hasAVX2() {
    uint32_t eax, ebx, ecx, edx;
    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    if (!((ecx >> 27) & 1)) return false;  // OSXSAVE
    uint32_t xcr0_lo, xcr0_hi;
    __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x06) != 0x06) return false;  // AVX state
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return (ebx >> 5) & 1;  // AVX2
}

void (*rx_blake2b_compress)(blake2b_state* S, const uint8_t* block) =
    hasSSE41() ? rx_blake2b_compress_sse41 : rx_blake2b_compress_integer;
int (*rx_blake2b)(void* out, size_t outlen, const void* in, size_t inlen) =
    hasAVX2() ? blake2b_avx2 : rx_blake2b_default;

// RANDOMX_CACHE_MAX_SIZE is defined in dataset.hpp but we need the value here
// Cache max = ArgonMemory(262144) * 1024 = 268435456 bytes (256MB)
static constexpr size_t CACHE_MAX_SIZE = 268435456;

namespace zenrx {

RxInstance::RxInstance()
{
}

RxInstance::~RxInstance()
{
    release();
}

void RxInstance::applyAlgoConfig(RxAlgo algo)
{
    // Defense-in-depth: randomx_apply_config() modifies the global RandomX_CurrentConfig.
    // The caller (RandomX::init/reinit) already serializes via RandomX::m_mutex, but we add
    // a local mutex to guard against any future code path that bypasses the outer lock.
    static std::mutex s_configMutex;
    std::lock_guard<std::mutex> guard(s_configMutex);

    switch (algo) {
        case RxAlgo::RX_0:     randomx_apply_config(RandomX_MoneroConfig);     break;
        case RxAlgo::RX_WOW:   randomx_apply_config(RandomX_WowneroConfig);    break;
        case RxAlgo::RX_ARQ:   randomx_apply_config(RandomX_ArqmaConfig);      break;
        case RxAlgo::RX_XEQ:   randomx_apply_config(RandomX_EquilibriaConfig); break;
        case RxAlgo::RX_GRAFT: randomx_apply_config(RandomX_GraftConfig);      break;
        case RxAlgo::RX_SFX:   randomx_apply_config(RandomX_SafexConfig);      break;
        case RxAlgo::RX_XLA:   randomx_apply_config(RandomX_ScalaConfig);      break;
    }
    Log::debug("Applied RandomX config for %s", rxAlgoName(algo));
}

int RxInstance::detectFlags(bool hardwareAES)
{
    int flags = RANDOMX_FLAG_DEFAULT;

    // Detect hardware AES via CPUID
    {
        uint32_t eax, ebx, ecx, edx;
        __cpuid(1, eax, ebx, ecx, edx);
        if (hardwareAES && ((ecx >> 25) & 1)) {
            flags |= RANDOMX_FLAG_HARD_AES;
        }
    }

    // Always enable JIT on x86_64
    flags |= RANDOMX_FLAG_JIT;

    return flags;
}

void RxInstance::destroyVMs()
{
    for (auto* vm : m_vms) {
        if (vm) randomx_destroy_vm(vm);
    }
    m_vms.clear();

    for (auto* sp : m_scratchpads) {
        delete sp;
    }
    m_scratchpads.clear();
}

bool RxInstance::createVMs(int threads, int vmFlags, const std::vector<int32_t>& numaNodes)
{
    m_vms.resize(threads);
    m_scratchpads.resize(threads);

    for (int i = 0; i < threads; i++) {
        m_scratchpads[i] = new zenrx::VirtualMemory(
            RandomX_CurrentConfig.ScratchpadL3_Size, m_allocatedWithHugePages);

        int32_t node = (i < static_cast<int>(numaNodes.size())) ? numaNodes[i] : 0;

        // Bind scratchpad to the worker's local NUMA node
        if (cpu().numaNodes() > 1 && m_scratchpads[i]->scratchpad() && node >= 0) {
            zenrx::VirtualMemory::bindToNode(m_scratchpads[i]->scratchpad(),
                                             m_scratchpads[i]->size(), node);
        }

        m_vms[i] = randomx_create_vm(
            static_cast<randomx_flags>(vmFlags),
            m_cache, m_dataset,
            m_scratchpads[i]->scratchpad(), static_cast<uint32_t>(node));

        if (!m_vms[i]) {
            Log::error("Failed to create RandomX VM %d", i);
            return false;
        }
    }
    return true;
}

bool RxInstance::init(const std::string& seedHash, int threads, RxAlgo algo,
                      bool hugePages, bool hardwareAES, int initThreads,
                      bool oneGbPages, const std::vector<int32_t>& numaNodes)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized && seedHash == m_seedHash && algo == m_algo) {
        return true;
    }

    if (m_initialized) {
        destroyVMs();

        if (m_dataset) {
            randomx_release_dataset(m_dataset);
            m_dataset = nullptr;
        }

        if (m_cache) {
            randomx_release_cache(m_cache);
            m_cache = nullptr;
        }

        if (m_datasetMemory) {
            if (m_allocatedWithHugePages) {
                zenrx::VirtualMemory::freeLargePagesMemory(m_datasetMemory, m_datasetMemorySize);
            } else {
                free(m_datasetMemory);
            }
            m_datasetMemory = nullptr;
            m_datasetMemorySize = 0;
        }

        if (m_cacheMemory) {
            if (m_cacheAllocatedWithHugePages) {
                zenrx::VirtualMemory::freeLargePagesMemory(m_cacheMemory, m_cacheMemorySize);
            } else {
                free(m_cacheMemory);
            }
            m_cacheMemory = nullptr;
            m_cacheMemorySize = 0;
        }

        m_initialized = false;
        m_seedHash.clear();
        m_allocatedWithHugePages = false;
        m_cacheAllocatedWithHugePages = false;
        m_using1GbPages = false;
    }

    m_seedHash = seedHash;
    m_hugePages = hugePages;
    m_hardwareAES = hardwareAES;
    m_algo = algo;

    // Apply algorithm-specific configuration before any allocations
    applyAlgoConfig(algo);

    uint8_t seed[32];
    memset(seed, 0, sizeof(seed));
    hexToBytes(seedHash, seed, 32);

    int flags = detectFlags(hardwareAES);
    bool useHugePages = hugePages;

    // Allocate cache memory
    m_cacheMemorySize = CACHE_MAX_SIZE;
    if (useHugePages) {
        m_cacheMemory = static_cast<uint8_t*>(
            zenrx::VirtualMemory::allocateLargePagesMemory(m_cacheMemorySize));
    }
    if (!m_cacheMemory) {
        if (useHugePages) {
            Log::debug("Cache hugepages failed, retrying without");
            useHugePages = false;
        }
        m_cacheMemory = static_cast<uint8_t*>(malloc(m_cacheMemorySize));
    }
    if (!m_cacheMemory) {
        Log::error("Failed to allocate RandomX cache memory");
        return false;
    }

    m_cacheAllocatedWithHugePages = useHugePages;

    // NUMA-interleave cache memory across all nodes for balanced latency
    if (cpu().numaNodes() > 1 && m_cacheMemory) {
        zenrx::VirtualMemory::bindInterleave(m_cacheMemory, m_cacheMemorySize);
    }

    m_cache = randomx_create_cache(static_cast<randomx_flags>(flags | RANDOMX_FLAG_JIT), m_cacheMemory);
    if (!m_cache) {
        Log::error("Failed to create RandomX cache");
        if (m_cacheAllocatedWithHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_cacheMemory, m_cacheMemorySize);
        } else {
            free(m_cacheMemory);
        }
        m_cacheMemory = nullptr;
        return false;
    }

    randomx_init_cache(m_cache, seed, sizeof(seed));

    // Allocate dataset memory
    // Dataset size = DatasetBaseSize + DatasetExtraSize
    // For rx/0: 2147483648 + 33554368 = ~2.03GB
    m_datasetMemorySize = static_cast<size_t>(RandomX_CurrentConfig.DatasetBaseSize) +
                          RandomX_ConfigurationBase::DatasetExtraSize;

    // Try 1GB huge pages first (reduces TLB misses: 3 entries vs ~1024 for 2MB pages)
    if (useHugePages && oneGbPages) {
        // Round up to 1GB boundary
        constexpr size_t ONE_GB = 1ULL << 30;
        size_t alignedSize = ((m_datasetMemorySize + ONE_GB - 1) / ONE_GB) * ONE_GB;
        m_datasetMemory = static_cast<uint8_t*>(
            zenrx::VirtualMemory::allocateLargePagesMemory(alignedSize, 1024));
        if (m_datasetMemory) {
            m_datasetMemorySize = alignedSize;
            m_using1GbPages = true;
            Log::info("Dataset allocated with 1GB hugepages (%zu GB)", alignedSize / ONE_GB);
        } else {
            Log::debug("1GB hugepages failed for dataset, falling back to 2MB");
        }
    }

    // Fall back to 2MB huge pages
    if (!m_datasetMemory && useHugePages) {
        m_datasetMemory = static_cast<uint8_t*>(
            zenrx::VirtualMemory::allocateLargePagesMemory(m_datasetMemorySize));
    }

    // Fall back to regular malloc
    if (!m_datasetMemory) {
        if (useHugePages) {
            Log::debug("Dataset hugepages failed, retrying without");
        }
        useHugePages = false;
        m_datasetMemory = static_cast<uint8_t*>(malloc(m_datasetMemorySize));
    }
    if (!m_datasetMemory) {
        Log::error("Failed to allocate RandomX dataset memory");
        randomx_release_cache(m_cache);
        m_cache = nullptr;
        if (m_cacheAllocatedWithHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_cacheMemory, m_cacheMemorySize);
        } else {
            free(m_cacheMemory);
        }
        m_cacheMemory = nullptr;
        return false;
    }

    // NUMA-interleave dataset memory across all nodes for balanced latency
    if (cpu().numaNodes() > 1 && m_datasetMemory) {
        zenrx::VirtualMemory::bindInterleave(m_datasetMemory, m_datasetMemorySize);
        Log::debug("Dataset NUMA interleaved across %u nodes", cpu().numaNodes());
    }

    m_dataset = randomx_create_dataset(m_datasetMemory);
    if (!m_dataset) {
        Log::error("Failed to create RandomX dataset");
        randomx_release_cache(m_cache);
        m_cache = nullptr;
        if (useHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_datasetMemory, m_datasetMemorySize);
        } else {
            free(m_datasetMemory);
        }
        if (m_cacheAllocatedWithHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_cacheMemory, m_cacheMemorySize);
        } else {
            free(m_cacheMemory);
        }
        m_datasetMemory = nullptr;
        m_cacheMemory = nullptr;
        return false;
    }

    m_allocatedWithHugePages = useHugePages;

    initDataset(initThreads > 0 ? initThreads : threads);

    // Build VM flags
    int vmFlags = flags | RANDOMX_FLAG_FULL_MEM;
    if (m_allocatedWithHugePages) {
        vmFlags |= RANDOMX_FLAG_LARGE_PAGES;
    }
    if (cpu().vendor() == VENDOR_AMD) {
        vmFlags |= RANDOMX_FLAG_AMD;
    }

    // Create VMs with externally managed scratchpads
    if (!createVMs(threads, vmFlags, numaNodes)) {
        release();
        return false;
    }

    m_initialized = true;

    Log::debug("RandomX initialized | algo: %s | hugepages: %s | 1GB pages: %s | JIT: yes",
               rxAlgoName(algo), m_allocatedWithHugePages ? "yes" : "no",
               m_using1GbPages ? "yes" : "no");

    return true;
}

bool RxInstance::reinit(const std::string& seedHash, int threads, bool hardwareAES,
                        int initThreads, const std::vector<int32_t>& numaNodes)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized && seedHash == m_seedHash) {
        return true;
    }

    if (!m_initialized || !m_cache || !m_dataset) {
        Log::error("Cannot reinit: instance not initialized");
        return false;
    }

    destroyVMs();

    m_seedHash = seedHash;
    m_hardwareAES = hardwareAES;

    uint8_t seed[32];
    memset(seed, 0, sizeof(seed));
    hexToBytes(seedHash, seed, 32);

    randomx_init_cache(m_cache, seed, sizeof(seed));

    m_initialized = false;
    initDataset(initThreads > 0 ? initThreads : threads);

    int vmFlags = detectFlags(hardwareAES) | RANDOMX_FLAG_FULL_MEM;

    if (m_allocatedWithHugePages) {
        vmFlags |= RANDOMX_FLAG_LARGE_PAGES;
    }
    if (cpu().vendor() == VENDOR_AMD) {
        vmFlags |= RANDOMX_FLAG_AMD;
    }

    if (!createVMs(threads, vmFlags, numaNodes)) {
        destroyVMs();
        return false;
    }

    m_initialized = true;

    Log::debug("RandomX reinit complete");

    return true;
}

void RxInstance::initDataset(int threads)
{
    if (!m_dataset || !m_cache) return;

    unsigned long datasetItemCount = randomx_dataset_item_count();
    const bool avx2 = cpu().hasAVX2();

    Log::debug("Initializing dataset (%d threads, AVX2: %s)", threads, avx2 ? "yes" : "no");

    if (threads <= 1) {
        if (avx2 && (datasetItemCount % 5)) {
            randomx_init_dataset(m_dataset, m_cache, 0, datasetItemCount - (datasetItemCount % 5));
            randomx_init_dataset(m_dataset, m_cache, datasetItemCount - 5, 5);
        } else {
            randomx_init_dataset(m_dataset, m_cache, 0, datasetItemCount);
        }
    } else {
        std::vector<std::thread> initThreads;

        for (int i = 0; i < threads; i++) {
            unsigned long start = (datasetItemCount * static_cast<unsigned long>(i)) / threads;
            unsigned long end   = (datasetItemCount * static_cast<unsigned long>(i + 1)) / threads;
            unsigned long count = end - start;

            initThreads.emplace_back([this, start, count, avx2]() {
                if (avx2 && (count % 5)) {
                    randomx_init_dataset(m_dataset, m_cache, start, count - (count % 5));
                    randomx_init_dataset(m_dataset, m_cache, start + count - 5, 5);
                } else {
                    randomx_init_dataset(m_dataset, m_cache, start, count);
                }
            });
        }

        for (auto& t : initThreads) {
            t.join();
        }
    }
}

bool RxInstance::isValidForSeed(const std::string& seedHash) const
{
    return m_initialized && m_seedHash == seedHash;
}

randomx_vm* RxInstance::getVM(int index)
{
    if (!m_initialized) {
        return nullptr;
    }
    if (index >= 0 && index < static_cast<int>(m_vms.size())) {
        return m_vms[index];
    }
    return nullptr;
}

void RxInstance::release()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto* vm : m_vms) {
        if (vm) randomx_destroy_vm(vm);
    }
    m_vms.clear();

    for (auto* sp : m_scratchpads) {
        delete sp;
    }
    m_scratchpads.clear();

    if (m_dataset) {
        randomx_release_dataset(m_dataset);
        m_dataset = nullptr;
    }

    if (m_cache) {
        randomx_release_cache(m_cache);
        m_cache = nullptr;
    }

    if (m_datasetMemory) {
        if (m_allocatedWithHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_datasetMemory, m_datasetMemorySize);
        } else {
            free(m_datasetMemory);
        }
        m_datasetMemory = nullptr;
        m_datasetMemorySize = 0;
    }

    if (m_cacheMemory) {
        if (m_cacheAllocatedWithHugePages) {
            zenrx::VirtualMemory::freeLargePagesMemory(m_cacheMemory, m_cacheMemorySize);
        } else {
            free(m_cacheMemory);
        }
        m_cacheMemory = nullptr;
        m_cacheMemorySize = 0;
    }

    m_initialized = false;
    m_allocatedWithHugePages = false;
    m_cacheAllocatedWithHugePages = false;
    m_using1GbPages = false;
    m_seedHash.clear();
}

} // namespace zenrx
