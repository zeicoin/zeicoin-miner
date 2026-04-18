#pragma once

#include "CpuInfo.h"
#include <cstdint>
#include <vector>
#include <functional>

namespace zenrx {

// Forward declaration for Windows implementation
class MsrPrivate;

class Msr {
public:
    using Callback = std::function<bool(int32_t cpu)>;
    
    Msr();
    ~Msr();
    
    bool isAvailable() const { return m_available; }
    
    // Read MSR register
    bool rdmsr(uint32_t reg, int32_t cpu, uint64_t& value) const;
    
    // Write MSR register
    bool wrmsr(uint32_t reg, uint64_t value, int32_t cpu);
    
    // Write MSR item with mask
    bool write(const MsrItem& item, int32_t cpu);
    
    // Write to all CPUs
    bool write(Callback&& callback);
    
    // Apply MSR preset for RandomX
    static bool applyPreset(const std::vector<MsrItem>& preset);
    
    // Apply default preset based on CPU
    static bool applyDefault();

private:
    bool m_available = false;
    MsrPrivate* m_priv = nullptr;  // Windows WinRing0 driver handle
    
    bool modprobe();
    bool allowWrites();
};

} // namespace zenrx
