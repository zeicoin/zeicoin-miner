#pragma once

#include "crypto/cpuType.hpp"

#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <cstring>
#include <thread>

namespace zenrx {

class ICpuInfo {
public:
    enum Vendor { VENDOR_UNKNOWN, VENDOR_INTEL, VENDOR_AMD };
    enum Arch {
        ARCH_UNKNOWN,
        ARCH_ZEN, ARCH_ZEN_PLUS, ARCH_ZEN2, ARCH_ZEN3, ARCH_ZEN4, ARCH_ZEN5
    };

    virtual ~ICpuInfo() = default;
    virtual bool hasBMI2() const = 0;
    virtual bool jccErratum() const = 0;
    virtual bool hasAVX() const = 0;
    virtual bool hasAVX2() const = 0;
    virtual bool hasXOP() const = 0;
    virtual Vendor vendor() const = 0;
    virtual Arch arch() const = 0;
    virtual int cores() const = 0;
    virtual int threads() const = 0;
};

class CpuInfoShim : public ICpuInfo {
public:
    CpuInfoShim() { detect(); }

    bool hasBMI2() const override { return zenrx::cpu::hasBMI2(); }
    bool jccErratum() const override { return false; }
    bool hasAVX() const override { return m_avx; }
    bool hasAVX2() const override { return m_avx2; }
    bool hasXOP() const override { return m_xop; }
    Vendor vendor() const override { return m_vendor; }
    Arch arch() const override { return m_arch; }
    int cores() const override { return m_cores; }
    int threads() const override { return m_threads; }

private:
    void detect() {
        int32_t info[4] = {};
        char vendorStr[13] = {};

#ifdef _WIN32
        __cpuid(info, 0);
#else
        __cpuid_count(0, 0, info[0], info[1], info[2], info[3]);
#endif
        memcpy(vendorStr, &info[1], 4);
        memcpy(vendorStr + 4, &info[3], 4);
        memcpy(vendorStr + 8, &info[2], 4);

        if (strcmp(vendorStr, "GenuineIntel") == 0) m_vendor = VENDOR_INTEL;
        else if (strcmp(vendorStr, "AuthenticAMD") == 0) m_vendor = VENDOR_AMD;

        // Check AVX/AVX2/XOP
#ifdef _WIN32
        __cpuid(info, 1);
#else
        __cpuid_count(1, 0, info[0], info[1], info[2], info[3]);
#endif
        bool osxsave = (info[2] >> 27) & 1;
        m_avx = osxsave && ((info[2] >> 28) & 1);

        int family = ((info[0] >> 8) & 0xF);
        int model = ((info[0] >> 4) & 0xF) | (((info[0] >> 16) & 0xF) << 4);
        if (family == 0xF) family += (info[0] >> 20) & 0xFF;

#ifdef _WIN32
        __cpuidex(info, 7, 0);
#else
        __cpuid_count(7, 0, info[0], info[1], info[2], info[3]);
#endif
        m_avx2 = m_avx && ((info[1] >> 5) & 1);

        // XOP (AMD extended feature)
#ifdef _WIN32
        __cpuid(info, 0x80000001);
#else
        __cpuid_count(0x80000001, 0, info[0], info[1], info[2], info[3]);
#endif
        m_xop = (info[2] >> 11) & 1;

        // AMD Zen architecture detection
        if (m_vendor == VENDOR_AMD && family == 0x17) {
            if (model <= 0x0F) m_arch = ARCH_ZEN;
            else if (model <= 0x2F) m_arch = ARCH_ZEN_PLUS;
            else m_arch = ARCH_ZEN2;
        } else if (m_vendor == VENDOR_AMD && family == 0x19) {
            m_arch = ARCH_ZEN3;
        } else if (m_vendor == VENDOR_AMD && family == 0x1A) {
            m_arch = ARCH_ZEN5;
        }

        m_threads = static_cast<int>(std::thread::hardware_concurrency());
        m_cores = m_threads; // Approximation (no SMT detection needed for JIT decisions)
    }

    bool m_avx = false;
    bool m_avx2 = false;
    bool m_xop = false;
    Vendor m_vendor = VENDOR_UNKNOWN;
    Arch m_arch = ARCH_UNKNOWN;
    int m_cores = 1;
    int m_threads = 1;
};

class Cpu {
public:
    static ICpuInfo* info() {
        static CpuInfoShim instance;
        return &instance;
    }
};

} // namespace zenrx
