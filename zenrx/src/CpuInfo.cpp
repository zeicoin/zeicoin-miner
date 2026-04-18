#include "CpuInfo.h"
#include "Log.h"
#include "crypto/Algorithm.h"

#include <hwloc.h>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef _MSC_VER
#   include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#   include <cpuid.h>
#endif

namespace zenrx {

static CpuInfo s_cpuInfo;

const CpuInfo& CpuInfo::instance()
{
    return s_cpuInfo;
}

// CPUID helpers for feature detection
#if defined(_MSC_VER)
static inline void cpuid(uint32_t level, int32_t output[4])
{
    __cpuid(output, static_cast<int>(level));
}

static inline void cpuidex(uint32_t level, uint32_t subleaf, int32_t output[4])
{
    __cpuidex(output, static_cast<int>(level), static_cast<int>(subleaf));
}

static inline uint64_t xgetbv()
{
    return _xgetbv(0);
}

#elif defined(__x86_64__) || defined(__i386__)
static inline void cpuid(uint32_t level, int32_t output[4])
{
    memset(output, 0, sizeof(int32_t) * 4);
    __cpuid_count(level, 0, output[0], output[1], output[2], output[3]);
}

static inline void cpuidex(uint32_t level, uint32_t subleaf, int32_t output[4])
{
    memset(output, 0, sizeof(int32_t) * 4);
    __cpuid_count(level, subleaf, output[0], output[1], output[2], output[3]);
}

static inline uint64_t xgetbv()
{
    uint32_t eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}
#endif

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
static inline bool has_feature(uint32_t level, uint32_t reg, int32_t bit)
{
    int32_t cpu_info[4] = {0};
    cpuid(level, cpu_info);
    return (cpu_info[reg] & bit) != 0;
}

static inline bool has_osxsave()    { return has_feature(1, 2, 1 << 27); }
static inline bool has_aes_ni()     { return has_feature(1, 2, 1 << 25); }
static inline bool has_avx()        { return has_feature(1, 2, 1 << 28) && has_osxsave() && ((xgetbv() & 0x06) == 0x06); }
static inline bool has_avx2()       { int32_t info[4]; cpuidex(7, 0, info); return (info[1] & (1 << 5)) && has_avx(); }
static inline bool has_avx512f()    { int32_t info[4]; cpuidex(7, 0, info); return (info[1] & (1 << 16)) && has_osxsave() && ((xgetbv() & 0xE6) == 0xE6); }
static inline bool has_pdpe1gb()    { return has_feature(0x80000001, 3, 1 << 26); }

static inline int32_t get_masked(int32_t val, int32_t h, int32_t l)
{
    val &= (0x7FFFFFFF >> (31 - (h - l))) << l;
    return val >> l;
}
#endif

// hwloc tree walking helpers (ported from xmrig HwlocCpuInfo.cpp)

// Recursively find all objects of a given type under parent.
// When a matching object is found, it is NOT recursed into (same as xmrig).
static void findByType(hwloc_obj_t parent, hwloc_obj_type_t type, std::vector<hwloc_obj_t>& results)
{
    for (unsigned i = 0; i < parent->arity; i++) {
        hwloc_obj_t child = parent->children[i];
        if (child->type == type) {
            results.push_back(child);
        } else {
            findByType(child, type, results);
        }
    }
}

// Check if a cache is exclusive (not inclusive). Ported from xmrig.
// An exclusive L3 means L2 data is NOT duplicated in L3, so the effective
// cache available for scratchpads is L3 + some L2.
static bool isCacheExclusive(hwloc_obj_t obj)
{
    const char* value = hwloc_obj_get_info_by_name(obj, "Inclusive");
    return value == nullptr || value[0] != '1';
}

CpuInfo::CpuInfo()
{
    detectVendor();     // Detect vendor first (needed for L3 logic)
    detectTopology();   // Then topology with hwloc
    detectFeatures();   // Then CPU features
    detectMsrPreset();  // Finally MSR preset
    detectArch();       // Determine microarchitecture
}

CpuInfo::~CpuInfo()
{
    if (m_topology) {
        hwloc_topology_destroy(m_topology);
        m_topology = nullptr;
    }
}

void CpuInfo::detectVendor()
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    int32_t cpu_info[4] = {0};
    cpuid(0, cpu_info);
    char vendor[13] = {0};
    memcpy(vendor + 0, &cpu_info[1], 4);
    memcpy(vendor + 4, &cpu_info[3], 4);
    memcpy(vendor + 8, &cpu_info[2], 4);
    
    if (memcmp(vendor, "AuthenticAMD", 12) == 0) {
        m_vendor = VENDOR_AMD;
    } else if (memcmp(vendor, "GenuineIntel", 12) == 0) {
        m_vendor = VENDOR_INTEL;
    }
    
    // Get family and model
    cpuid(1, cpu_info);
    int32_t procInfo = cpu_info[0];
    m_family = get_masked(procInfo, 12, 8) + get_masked(procInfo, 28, 20);
    m_model = (get_masked(procInfo, 20, 16) << 4) | get_masked(procInfo, 8, 4);
    
    Log::debug("CPUID: vendor=%s family=0x%x model=0x%x", 
              m_vendor == VENDOR_AMD ? "AMD" : (m_vendor == VENDOR_INTEL ? "Intel" : "Unknown"),
              m_family, m_model);
#endif
}

void CpuInfo::detectTopology()
{
    // Initialize hwloc
    if (hwloc_topology_init(&m_topology) < 0) {
        Log::error("Failed to initialize hwloc topology");
        m_threads = 1;
        m_cores = 1;
        m_brand = "Unknown CPU";
        return;
    }
    
    if (hwloc_topology_load(m_topology) < 0) {
        Log::error("Failed to load hwloc topology");
        hwloc_topology_destroy(m_topology);
        m_topology = nullptr;
        m_threads = 1;
        m_cores = 1;
        m_brand = "Unknown CPU";
        return;
    }
    
    // Get thread count (Processing Units)
    int puCount = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_PU);
    m_threads = puCount > 0 ? static_cast<uint32_t>(puCount) : 1;
    
    // Get core count
    int coreCount = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_CORE);
    m_cores = coreCount > 0 ? static_cast<uint32_t>(coreCount) : m_threads;
    
    // Get package count
    int pkgCount = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_PACKAGE);
    m_packages = pkgCount > 0 ? static_cast<uint32_t>(pkgCount) : 1;
    
    // Detect NUMA nodes and build PU → NUMA node mapping
    int numaCount = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_NUMANODE);
    if (numaCount > 0) {
        m_numaNodes = static_cast<uint32_t>(numaCount);
        for (int i = 0; i < numaCount; i++) {
            hwloc_obj_t node = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_NUMANODE, i);
            if (!node || !node->cpuset) continue;
            int32_t nodeId = static_cast<int32_t>(node->os_index);
            for (uint32_t p = 0; p < m_threads; p++) {
                hwloc_obj_t pu = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_PU, p);
                if (pu && hwloc_bitmap_isset(node->cpuset, pu->os_index)) {
                    m_cpuToNode[static_cast<int32_t>(pu->os_index)] = nodeId;
                }
            }
        }
    }

    // Build CPU units list (for MSR writing)
    m_units.clear();
    for (uint32_t i = 0; i < m_threads; ++i) {
        hwloc_obj_t pu = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_PU, i);
        if (pu) {
            m_units.push_back(static_cast<int32_t>(pu->os_index));
        } else {
            m_units.push_back(static_cast<int32_t>(i));
        }
    }
    
    // Get L2 cache (per-core value)
    int l2Count = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_L2CACHE);
    if (l2Count > 0) {
        hwloc_obj_t l2 = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_L2CACHE, 0);
        if (l2 && l2->attr) {
            m_l2Cache = l2->attr->cache.size;
        }
    }
    
    // Get L3 cache
    // Intel: single shared L3, hwloc may report multiple objects pointing to same cache
    // AMD: multiple CCDs each with own L3, need to sum
    int l3Count = hwloc_get_nbobjs_by_type(m_topology, HWLOC_OBJ_L3CACHE);
    if (l3Count > 0) {
        if (m_vendor == VENDOR_AMD) {
            // AMD: sum all L3 caches (multi-CCD like Ryzen 9)
            m_l3Cache = 0;
            for (int i = 0; i < l3Count; ++i) {
                hwloc_obj_t l3 = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_L3CACHE, i);
                if (l3 && l3->attr) {
                    m_l3Cache += l3->attr->cache.size;
                }
            }
        } else {
            // Intel: shared L3, just get first one (all objects point to same cache)
            hwloc_obj_t l3 = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_L3CACHE, 0);
            if (l3 && l3->attr) {
                m_l3Cache = l3->attr->cache.size;
            }
        }
    }
    
    // Get CPU brand from hwloc
    hwloc_obj_t root = hwloc_get_root_obj(m_topology);
    if (root) {
        const char* cpuModel = hwloc_obj_get_info_by_name(root, "CPUModel");
        if (cpuModel && strlen(cpuModel) > 0) {
            m_brand = cpuModel;
        }
    }
    
    // Fallback: get brand from package object
    if (m_brand.empty()) {
        hwloc_obj_t pkg = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_PACKAGE, 0);
        if (pkg) {
            const char* cpuModel = hwloc_obj_get_info_by_name(pkg, "CPUModel");
            if (cpuModel && strlen(cpuModel) > 0) {
                m_brand = cpuModel;
            }
        }
    }
    
    // Fallback: get brand from CPUID
    if (m_brand.empty()) {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
        int32_t cpu_info[4] = {0};
        char brand[64] = {0};
        
        cpuid(0x80000000, cpu_info);
        if (static_cast<uint32_t>(cpu_info[0]) >= 0x80000004) {
            for (uint32_t i = 0; i < 3; i++) {
                cpuid(0x80000002 + i, cpu_info);
                memcpy(brand + (i * 16), cpu_info, sizeof(cpu_info));
            }
        }
        
        // Trim leading spaces
        char* p = brand;
        while (*p == ' ') p++;
        m_brand = p;
#endif
    }
    
    if (m_brand.empty()) {
        m_brand = "Unknown CPU";
    }
    
    // Detect vendor from brand string
    if (m_brand.find("AMD") != std::string::npos || 
        m_brand.find("Ryzen") != std::string::npos ||
        m_brand.find("EPYC") != std::string::npos ||
        m_brand.find("Threadripper") != std::string::npos) {
        m_vendor = VENDOR_AMD;
    } else if (m_brand.find("Intel") != std::string::npos ||
               m_brand.find("Xeon") != std::string::npos ||
               m_brand.find("Core") != std::string::npos ||
               m_brand.find("Pentium") != std::string::npos ||
               m_brand.find("Celeron") != std::string::npos) {
        m_vendor = VENDOR_INTEL;
    }
    
    // Fallback cache estimates if hwloc failed
    if (m_l3Cache == 0) {
        if (m_vendor == VENDOR_AMD) {
            m_l3Cache = (m_threads / 8 + 1) * 32 * 1024 * 1024;
        } else if (m_vendor == VENDOR_INTEL) {
            m_l3Cache = m_threads * 2 * 1024 * 1024;
        } else {
            m_l3Cache = m_threads * 2 * 1024 * 1024;
        }
    }
    
    if (m_l2Cache == 0) {
        m_l2Cache = m_cores * 256 * 1024;
    }
    
    Log::debug("hwloc: %u threads, %u cores, %u packages, %u NUMA nodes", m_threads, m_cores, m_packages, m_numaNodes);
    Log::debug("hwloc: L2=%zu KB, L3=%zu MB", m_l2Cache / 1024, m_l3Cache / (1024 * 1024));
}

void CpuInfo::detectFeatures()
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    // Detect CPU features (vendor/family/model already set in detectVendor)
    m_aes = has_aes_ni();
    m_avx2 = has_avx2();
    m_avx512 = has_avx512f();
    m_1gbPages = has_pdpe1gb();
#else
    // Non-x86 fallback - check /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("flags") != std::string::npos) {
            m_aes = line.find(" aes") != std::string::npos;
            m_avx2 = line.find(" avx2") != std::string::npos;
            m_avx512 = line.find(" avx512f") != std::string::npos;
            m_1gbPages = line.find(" pdpe1gb") != std::string::npos;
            break;
        }
    }
#endif

    Log::debug("Features: AES=%d AVX2=%d AVX512=%d 1GB=%d", 
              m_aes, m_avx2, m_avx512, m_1gbPages);
}

void CpuInfo::detectMsrPreset()
{
    m_msrMod = MSR_MOD_NONE;
    
    if (m_vendor == VENDOR_AMD && m_family >= 0x17) {
        switch (m_family) {
        case 0x17:
            m_msrMod = MSR_MOD_RYZEN_17H;
            break;
        case 0x19:
            if (m_model == 0x61 || m_model == 0x70 || m_model == 0x71 || 
                m_model == 0x74 || m_model == 0x75 || m_model == 0x78) {
                m_msrMod = MSR_MOD_RYZEN_19H_ZEN4;
            } else {
                m_msrMod = MSR_MOD_RYZEN_19H;
            }
            break;
        case 0x1a:
            m_msrMod = MSR_MOD_RYZEN_1AH_ZEN5;
            break;
        default:
            if (m_family > 0x1a) {
                m_msrMod = MSR_MOD_RYZEN_1AH_ZEN5;
            }
            break;
        }
    } else if (m_vendor == VENDOR_INTEL) {
        m_msrMod = MSR_MOD_INTEL;
    }
    
    Log::debug("MSR preset: family=0x%x model=0x%x mod=%d", m_family, m_model, m_msrMod);
}

void CpuInfo::detectArch()
{
    m_arch = ARCH_UNKNOWN;

    if (m_vendor == VENDOR_AMD) {
        switch (m_family) {
        case 0x17:
            if (m_model >= 0x31) {
                m_arch = ARCH_ZEN2;
            } else if (m_model >= 0x08) {
                m_arch = ARCH_ZEN_PLUS;
            } else {
                m_arch = ARCH_ZEN;
            }
            break;
        case 0x19:
            // Zen4 models: 0x61, 0x70, 0x71, 0x74, 0x75, 0x78 (same set as MSR_MOD_RYZEN_19H_ZEN4)
            if (m_model == 0x61 || m_model == 0x70 || m_model == 0x71 ||
                m_model == 0x74 || m_model == 0x75 || m_model == 0x78) {
                m_arch = ARCH_ZEN4;
            } else {
                m_arch = ARCH_ZEN3;
            }
            break;
        case 0x1a:
            m_arch = ARCH_ZEN5;
            break;
        default:
            if (m_family > 0x1a) {
                m_arch = ARCH_ZEN5;
            }
            break;
        }
    } else if (m_vendor == VENDOR_INTEL) {
        m_arch = ARCH_INTEL;
    }

    Log::debug("CPU arch: %d (family=0x%x model=0x%x)", m_arch, m_family, m_model);
}

int32_t CpuInfo::numaNodeForCpu(int32_t cpuIndex) const
{
    auto it = m_cpuToNode.find(cpuIndex);
    return (it != m_cpuToNode.end()) ? it->second : 0;
}

std::vector<int32_t> CpuInfo::affinityForAlgo(RxAlgo rxAlgo) const
{
    Algorithm algo(Algorithm::fromRxAlgo(rxAlgo));
    const size_t scratchpad = algo.l3();
    const size_t algoL2     = algo.l2();

    // Fallback if no topology available
    if (!m_topology || (m_l2Cache == 0 && m_l3Cache == 0)) {
        uint32_t count = m_threads > 1 ? m_threads - 1 : 1;
        // Simple heuristic: L3 / scratchpad, capped at PU count - 1
        if (m_l3Cache > 0) {
            uint32_t cacheThreads = static_cast<uint32_t>((m_l3Cache + scratchpad / 2) / scratchpad);
            count = std::min(cacheThreads, count);
        }
        std::vector<int32_t> result;
        for (uint32_t i = 0; i < count && i < m_units.size(); i++) {
            result.push_back(m_units[i]);
        }
        return result;
    }

    // Determine top-level cache type (L3 preferred, L2 fallback)
    hwloc_obj_type_t cacheType = HWLOC_OBJ_L3CACHE;
    int cacheCount = hwloc_get_nbobjs_by_type(m_topology, cacheType);
    if (cacheCount <= 0) {
        cacheType = HWLOC_OBJ_L2CACHE;
        cacheCount = hwloc_get_nbobjs_by_type(m_topology, cacheType);
    }
    if (cacheCount <= 0) {
        // No cache info — use all PUs minus system reserve
        uint32_t count = m_threads > 1 ? m_threads - 1 : 1;
        std::vector<int32_t> result;
        for (uint32_t i = 0; i < count && i < m_units.size(); i++) {
            result.push_back(m_units[i]);
        }
        return result;
    }

    std::vector<int32_t> result;
    constexpr size_t oneMiB = 1024U * 1024U;

    // Process each top-level cache independently (multi-CCD AMD)
    for (int ci = 0; ci < cacheCount; ci++) {
        hwloc_obj_t cache = hwloc_get_obj_by_type(m_topology, cacheType, ci);
        if (!cache || !cache->attr) continue;

        // Count PUs under this cache
        std::vector<hwloc_obj_t> pus;
        findByType(cache, HWLOC_OBJ_PU, pus);
        size_t PUs = pus.size();
        if (PUs == 0) continue;

        // Find cores under this cache
        std::vector<hwloc_obj_t> cores;
        findByType(cache, HWLOC_OBJ_CORE, cores);
        if (cores.empty()) continue;

        bool L3_exclusive = isCacheExclusive(cache);

        size_t L3              = cache->attr->cache.size;
        size_t L2              = 0;
        int    L2_associativity = 0;
        size_t extra           = 0;

        if (cacheType == HWLOC_OBJ_L3CACHE) {
            // Collect L2 caches under this L3 and compute extra from exclusive L2
            std::vector<hwloc_obj_t> l2objs;
            findByType(cache, HWLOC_OBJ_L2CACHE, l2objs);

            for (auto* l2 : l2objs) {
                if (!l2->attr) continue;
                L2 += l2->attr->cache.size;
                L2_associativity = l2->attr->cache.associativity;

                if (L3_exclusive) {
                    if (m_vendor == VENDOR_AMD && (m_arch == ARCH_ZEN4 || m_arch == ARCH_ZEN5)) {
                        // Zen4/5: use half of exclusive L2, capped at scratchpad size
                        extra += std::min<size_t>(l2->attr->cache.size / 2, scratchpad);
                    } else if (l2->attr->cache.size >= scratchpad) {
                        extra += scratchpad;
                    }
                }
            }
        }

        // Intel special case: certain CPUs with L2=cores*1MB, 16-way associative, and L3>=L2
        // benefit from treating L2 as the effective cache (ported from xmrig)
        if (m_vendor == VENDOR_INTEL && scratchpad == 2 * oneMiB) {
            if (L2 > 0 && (cores.size() * oneMiB) == L2 && L2_associativity == 16 && L3 >= L2) {
                L3    = L2;
                extra = L2;
            }
        }

        size_t cacheHashes = (L3 + extra + scratchpad / 2) / scratchpad;

        // RandomX-specific adjustments (ported from xmrig)
        // Intel exclusive L3 with P+E cores: use all L3+L2
        if (m_vendor == VENDOR_INTEL && L3_exclusive && PUs < cores.size() * 2) {
            cacheHashes = (L3 + L2) / scratchpad;
        }

        // L2 constraint: when no exclusive L2 extra was applied, limit threads
        // to max(L2/algo_l2, cores) but not more than cacheHashes
        if (extra == 0 && algoL2 > 0) {
            cacheHashes = std::min<size_t>(
                std::max<size_t>(L2 / algoL2, cores.size()),
                cacheHashes);
        }

        // rx/xla (Panthera): always 1 thread per core
        if (rxAlgo == RxAlgo::RX_XLA) {
            cacheHashes = cores.size();
        }

        // Generate affinity list for this cache group
        if (cacheHashes >= PUs) {
            // Use all PUs (all cores + HT)
            for (auto* core : cores) {
                std::vector<hwloc_obj_t> units;
                findByType(core, HWLOC_OBJ_PU, units);
                for (auto* pu : units) {
                    result.push_back(static_cast<int32_t>(pu->os_index));
                }
            }
        } else {
            // Fill physical cores first (pu_id=0), then HT (pu_id=1+) from reverse
            // This is xmrig's strategy: alternate direction each HT pass for Zen optimization
            size_t pu_id = 0;
            while (cacheHashes > 0 && PUs > 0) {
                bool allocated = false;
                std::vector<int32_t> batch;

                for (auto* core : cores) {
                    std::vector<hwloc_obj_t> units;
                    findByType(core, HWLOC_OBJ_PU, units);
                    if (units.size() <= pu_id) continue;

                    cacheHashes--;
                    PUs--;
                    allocated = true;
                    batch.push_back(static_cast<int32_t>(units[pu_id]->os_index));

                    if (cacheHashes == 0) break;
                }

                // Reverse HT pass for better Zen optimization (xmrig pattern)
                if (pu_id & 1) {
                    std::reverse(batch.begin(), batch.end());
                }

                for (int32_t idx : batch) {
                    result.push_back(idx);
                }

                if (!allocated) break;
                pu_id++;
                std::reverse(cores.begin(), cores.end());
            }
        }
    }

    // System reserve: cap at total_PUs - 1
    if (result.size() >= m_threads && m_threads > 1) {
        result.resize(m_threads - 1);
    }

    // Safety: ensure at least 1 thread
    if (result.empty() && !m_units.empty()) {
        result.push_back(m_units[0]);
    }

    return result;
}

uint32_t CpuInfo::threadsForAlgo(RxAlgo algo) const
{
    auto affinity = affinityForAlgo(algo);
    return affinity.empty() ? 1 : static_cast<uint32_t>(affinity.size());
}

uint32_t CpuInfo::optimalThreads() const
{
    return threadsForAlgo(RxAlgo::RX_0);
}

uint32_t CpuInfo::miningThreads() const
{
    return optimalThreads();
}

std::vector<MsrItem> CpuInfo::msrPreset() const
{
    std::vector<MsrItem> preset;
    
    switch (m_msrMod) {
    case MSR_MOD_RYZEN_17H:
        preset.emplace_back(0xC0011020, 0ULL);
        preset.emplace_back(0xC0011021, 0x40ULL, ~0x20ULL);
        preset.emplace_back(0xC0011022, 0x1510000ULL);
        preset.emplace_back(0xC001102b, 0x2000cc16ULL);
        break;
        
    case MSR_MOD_RYZEN_19H:
        preset.emplace_back(0xC0011020, 0x0004480000000000ULL);
        preset.emplace_back(0xC0011021, 0x001c000200000040ULL, ~0x20ULL);
        preset.emplace_back(0xC0011022, 0xc000000401570000ULL);
        preset.emplace_back(0xC001102b, 0x2000cc10ULL);
        break;
        
    case MSR_MOD_RYZEN_19H_ZEN4:
    case MSR_MOD_RYZEN_1AH_ZEN5:
        preset.emplace_back(0xC0011020, 0x0004400000000000ULL);
        preset.emplace_back(0xC0011021, 0x0004000000000040ULL, ~0x20ULL);
        preset.emplace_back(0xC0011022, 0x8680000401570000ULL);
        preset.emplace_back(0xC001102b, 0x2040cc10ULL);
        break;
        
    case MSR_MOD_INTEL:
        preset.emplace_back(0x1a4, 0xf);
        break;
        
    default:
        break;
    }
    
    return preset;
}

} // namespace zenrx
