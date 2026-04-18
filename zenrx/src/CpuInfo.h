#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Config.h"

// Forward declare hwloc types
struct hwloc_topology;
typedef struct hwloc_topology* hwloc_topology_t;

namespace zenrx {

enum CpuVendor {
    VENDOR_UNKNOWN,
    VENDOR_INTEL,
    VENDOR_AMD
};

enum CpuArch {
    ARCH_UNKNOWN,
    ARCH_ZEN,
    ARCH_ZEN_PLUS,
    ARCH_ZEN2,
    ARCH_ZEN3,
    ARCH_ZEN4,
    ARCH_ZEN5,
    ARCH_INTEL
};

enum MsrMod {
    MSR_MOD_NONE,
    MSR_MOD_RYZEN_17H,
    MSR_MOD_RYZEN_19H,
    MSR_MOD_RYZEN_19H_ZEN4,
    MSR_MOD_RYZEN_1AH_ZEN5,
    MSR_MOD_INTEL,
    MSR_MOD_MAX
};

struct MsrItem {
    uint32_t reg;
    uint64_t value;
    uint64_t mask;
    
    MsrItem(uint32_t r, uint64_t v, uint64_t m = 0xFFFFFFFFFFFFFFFFULL)
        : reg(r), value(v), mask(m) {}
};

class CpuInfo {
public:
    CpuInfo();
    ~CpuInfo();
    
    // Basic info
    const std::string& brand() const { return m_brand; }
    CpuVendor vendor() const { return m_vendor; }
    
    // Thread/core info
    uint32_t threads() const { return m_threads; }
    uint32_t cores() const { return m_cores; }
    uint32_t packages() const { return m_packages; }
    
    // Cache info (in bytes)
    size_t L2() const { return m_l2Cache; }
    size_t L3() const { return m_l3Cache; }
    
    // Feature flags
    bool hasAES() const { return m_aes; }
    bool hasAVX2() const { return m_avx2; }
    bool hasAVX512() const { return m_avx512; }
    bool has1GbPages() const { return m_1gbPages; }
    
    // MSR
    MsrMod msrMod() const { return m_msrMod; }
    std::vector<MsrItem> msrPreset() const;
    
    // CPU architecture classification (for cache tuning)
    CpuArch arch() const { return m_arch; }

    // Calculate optimal threads for RandomX (2MB L3 per thread) — legacy fallback
    uint32_t optimalThreads() const;

    // Get mining threads respecting both cache and system thread reserve
    uint32_t miningThreads() const;

    // Algorithm-aware thread calculation using hwloc cache topology (port of xmrig processTopLevelCache)
    uint32_t threadsForAlgo(RxAlgo algo) const;

    // Generate ordered affinity list for a specific algorithm.
    // Returns PU os_indices: physical cores first, then HT, per-L3 distribution.
    std::vector<int32_t> affinityForAlgo(RxAlgo algo) const;

    // NUMA
    uint32_t numaNodes() const { return m_numaNodes; }
    int32_t numaNodeForCpu(int32_t cpuIndex) const;

    // CPU units (for MSR writing)
    const std::vector<int32_t>& units() const { return m_units; }
    
    // Singleton
    static const CpuInfo& instance();

private:
    void detectVendor();        // CPUID vendor detection (called first)
    void detectTopology();      // hwloc-based topology detection
    void detectFeatures();      // CPUID-based feature detection
    void detectMsrPreset();     // Determine MSR preset from family/model
    void detectArch();          // Determine CPU microarchitecture from family/model
    
    std::string m_brand;
    CpuVendor m_vendor = VENDOR_UNKNOWN;
    MsrMod m_msrMod = MSR_MOD_NONE;
    
    uint32_t m_threads = 0;
    uint32_t m_cores = 0;
    uint32_t m_packages = 1;
    
    size_t m_l2Cache = 0;
    size_t m_l3Cache = 0;
    
    bool m_aes = false;
    bool m_avx2 = false;
    bool m_avx512 = false;
    bool m_1gbPages = false;
    
    uint32_t m_family = 0;
    uint32_t m_model = 0;
    CpuArch m_arch = ARCH_UNKNOWN;

    std::vector<int32_t> m_units;

    uint32_t m_numaNodes = 1;
    std::map<int32_t, int32_t> m_cpuToNode;  // PU os_index → NUMA node os_index

    hwloc_topology_t m_topology = nullptr;
};

// Convenience function
inline const CpuInfo& cpu() { return CpuInfo::instance(); }

} // namespace zenrx
