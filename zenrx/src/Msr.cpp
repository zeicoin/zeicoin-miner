#include "Msr.h"
#include "Log.h"
#include "CpuInfo.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#   include <windows.h>
#   include <winioctl.h>
#   include <vector>
#   include <string>
#   include "WinRing0Driver.h"
#else
#   include <fcntl.h>
#   include <unistd.h>
#   include <sys/stat.h>
#endif

namespace zenrx {

#ifdef _WIN32

// WinRing0 service and IOCTL definitions
#define SERVICE_NAME    L"WinRing0_1_2_0"
#define IOCTL_READ_MSR  CTL_CODE(40000, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WRITE_MSR CTL_CODE(40000, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Extract embedded driver to temp file
static std::wstring extractDriver()
{
    wchar_t tempPath[MAX_PATH];
    wchar_t tempFile[MAX_PATH];
    
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        return L"";
    }
    
    // Create unique filename
    if (!GetTempFileNameW(tempPath, L"wr0", 0, tempFile)) {
        return L"";
    }
    
    // Delete the temp file and use .sys extension
    DeleteFileW(tempFile);
    std::wstring driverPath = tempFile;
    driverPath = driverPath.substr(0, driverPath.length() - 4) + L".sys";
    
    // Write embedded driver to file
    HANDLE hFile = CreateFileW(driverPath.c_str(), GENERIC_WRITE, 0, nullptr, 
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return L"";
    }
    
    DWORD written;
    BOOL success = WriteFile(hFile, WinRing0_sys, WinRing0_sys_len, &written, nullptr);
    CloseHandle(hFile);
    
    if (!success || written != WinRing0_sys_len) {
        DeleteFileW(driverPath.c_str());
        return L"";
    }
    
    return driverPath;
}

// Windows MSR implementation using WinRing0 driver
class MsrPrivate {
public:
    MsrPrivate() : driver(INVALID_HANDLE_VALUE), manager(nullptr), service(nullptr), reuse(false) {}
    
    bool uninstall()
    {
        if (driver != INVALID_HANDLE_VALUE) {
            CloseHandle(driver);
            driver = INVALID_HANDLE_VALUE;
        }

        if (!service) {
            // Clean up extracted driver file
            if (!extractedDriverPath.empty()) {
                DeleteFileW(extractedDriverPath.c_str());
                extractedDriverPath.clear();
            }
            return true;
        }

        bool result = true;

        if (!reuse) {
            SERVICE_STATUS serviceStatus;
            ControlService(service, SERVICE_CONTROL_STOP, &serviceStatus);
            DeleteService(service);
        }

        CloseServiceHandle(service);
        service = nullptr;

        if (manager) {
            CloseServiceHandle(manager);
            manager = nullptr;
        }
        
        // Clean up extracted driver file
        if (!extractedDriverPath.empty()) {
            DeleteFileW(extractedDriverPath.c_str());
            extractedDriverPath.clear();
        }

        return result;
    }
    
    HANDLE driver;
    SC_HANDLE manager;
    SC_HANDLE service;
    bool reuse;
    std::wstring extractedDriverPath;
};

Msr::Msr() : m_priv(new MsrPrivate())
{
    DWORD err = 0;

    m_priv->manager = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!m_priv->manager) {
        m_available = false;
        return;
    }

    // Try to connect to existing driver first
    m_priv->driver = CreateFileW(L"\\\\.\\" SERVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_priv->driver != INVALID_HANDLE_VALUE) {
        m_priv->reuse = true;
        m_available = true;
        return;
    }

    // Check if service exists but not running
    m_priv->service = OpenServiceW(m_priv->manager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (m_priv->service) {
        SERVICE_STATUS status;
        if (QueryServiceStatus(m_priv->service, &status) && status.dwCurrentState == SERVICE_RUNNING) {
            m_priv->reuse = true;
            m_priv->driver = CreateFileW(L"\\\\.\\" SERVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_priv->driver != INVALID_HANDLE_VALUE) {
                m_available = true;
                return;
            }
        }
        m_priv->uninstall();
    }

    // Extract embedded driver to temp location
    std::wstring driverPath = extractDriver();
    if (driverPath.empty()) {
        m_available = false;
        return;
    }
    m_priv->extractedDriverPath = driverPath;

    // Install and start the driver
    m_priv->service = CreateServiceW(m_priv->manager, SERVICE_NAME, SERVICE_NAME, SERVICE_ALL_ACCESS, 
                                     SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, 
                                     driverPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!m_priv->service) {
        err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            m_priv->service = OpenServiceW(m_priv->manager, SERVICE_NAME, SERVICE_ALL_ACCESS);
        }
        if (!m_priv->service) {
            m_available = false;
            return;
        }
    }

    if (!StartService(m_priv->service, 0, nullptr)) {
        err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            m_priv->uninstall();
            m_available = false;
            return;
        }
    }

    // Connect to driver
    m_priv->driver = CreateFileW(L"\\\\.\\" SERVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_priv->driver == INVALID_HANDLE_VALUE) {
        m_priv->uninstall();
        m_available = false;
        return;
    }

    m_available = true;
}

Msr::~Msr()
{
    if (m_priv) {
        m_priv->uninstall();
        delete m_priv;
    }
}

bool Msr::modprobe() { return false; }
bool Msr::allowWrites() { return false; }

bool Msr::rdmsr(uint32_t reg, int32_t /*cpu*/, uint64_t& value) const
{
    if (!m_priv || m_priv->driver == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD size = 0;
    return DeviceIoControl(m_priv->driver, IOCTL_READ_MSR, &reg, sizeof(reg), &value, sizeof(value), &size, nullptr) != 0;
}

bool Msr::wrmsr(uint32_t reg, uint64_t value, int32_t /*cpu*/)
{
    if (!m_priv || m_priv->driver == INVALID_HANDLE_VALUE) {
        return false;
    }

    struct {
        uint32_t reg;
        uint32_t value[2];
    } input;

    static_assert(sizeof(input) == 12, "Invalid struct size for WinRing0 driver");

    input.reg = reg;
    memcpy(input.value, &value, sizeof(value));

    DWORD output;
    DWORD k;

    return DeviceIoControl(m_priv->driver, IOCTL_WRITE_MSR, &input, sizeof(input), &output, sizeof(output), &k, nullptr) != 0;
}

bool Msr::write(const MsrItem& item, int32_t cpu)
{
    uint64_t value = 0;
    
    if (item.mask != 0xFFFFFFFFFFFFFFFFULL) {
        if (!rdmsr(item.reg, cpu, value)) {
            return false;
        }
        value = (value & ~item.mask) | (item.value & item.mask);
    } else {
        value = item.value;
    }
    
    return wrmsr(item.reg, value, cpu);
}

bool Msr::write(Callback&& callback)
{
    return callback(-1);
}

bool Msr::applyPreset(const std::vector<MsrItem>& preset)
{
    if (preset.empty()) {
        return false;
    }
    
    Msr msr;
    if (!msr.isAvailable()) {
        return false;
    }
    
    bool success = true;
    for (const auto& item : preset) {
        if (!msr.write(item, -1)) {
            success = false;
        }
    }
    
    return success;
}

bool Msr::applyDefault()
{
    auto preset = zenrx::cpu().msrPreset();
    if (preset.empty()) {
        return false;
    }
    
    return applyPreset(preset);
}

#else // Linux implementation

static constexpr int MSR_RETRY_COUNT = 5;
static constexpr int MSR_RETRY_DELAY_MS = 200;

static bool waitForMsrDevice(int timeoutMs = 2000)
{
    const char* path = "/dev/cpu/0/msr";
    int elapsed = 0;
    const int checkInterval = 100;
    
    while (elapsed < timeoutMs) {
        struct stat st;
        if (stat(path, &st) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(checkInterval));
        elapsed += checkInterval;
    }
    return false;
}

Msr::Msr() : m_priv(nullptr)
{
    m_available = false;
    
    // Try to enable MSR access with retries
    for (int attempt = 0; attempt < MSR_RETRY_COUNT && !m_available; ++attempt) {
        if (attempt > 0) {
            Log::debug("MSR init retry %d/%d", attempt + 1, MSR_RETRY_COUNT);
            std::this_thread::sleep_for(std::chrono::milliseconds(MSR_RETRY_DELAY_MS));
        }
        
        // Try allowWrites first (module already loaded)
        if (allowWrites()) {
            if (waitForMsrDevice(500)) {
                m_available = true;
                break;
            }
        }
        
        // Try modprobe
        if (modprobe()) {
            if (waitForMsrDevice(2000)) {
                m_available = true;
                break;
            }
        }
    }
    
    if (!m_available) {
        Log::warn("MSR module not available");
    }
}

Msr::~Msr()
{
}

bool Msr::modprobe()
{
    int ret = system("/sbin/modprobe msr allow_writes=on > /dev/null 2>&1");
    if (ret != 0) {
        // Try without allow_writes (older kernels)
        ret = system("/sbin/modprobe msr > /dev/null 2>&1");
    }
    return ret == 0;
}

bool Msr::allowWrites()
{
    std::ofstream file("/sys/module/msr/parameters/allow_writes", 
                       std::ios::out | std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        file << "on";
        file.flush();
        return file.good();
    }
    return false;
}

bool Msr::rdmsr(uint32_t reg, int32_t cpu, uint64_t& value) const
{
    char path[64];
    if (cpu < 0) {
        cpu = zenrx::cpu().units().front();
    }
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    
    bool success = pread(fd, &value, sizeof(value), reg) == sizeof(value);
    close(fd);
    
    return success;
}

bool Msr::wrmsr(uint32_t reg, uint64_t value, int32_t cpu)
{
    char path[64];
    if (cpu < 0) {
        cpu = zenrx::cpu().units().front();
    }
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return false;
    }
    
    bool success = pwrite(fd, &value, sizeof(value), reg) == sizeof(value);
    close(fd);
    
    return success;
}

bool Msr::write(const MsrItem& item, int32_t cpu)
{
    uint64_t value = 0;
    
    if (item.mask != 0xFFFFFFFFFFFFFFFFULL) {
        if (!rdmsr(item.reg, cpu, value)) {
            return false;
        }
        value = (value & ~item.mask) | (item.value & item.mask);
    } else {
        value = item.value;
    }
    
    return wrmsr(item.reg, value, cpu);
}

bool Msr::write(Callback&& callback)
{
    const auto& units = zenrx::cpu().units();
    
    for (int32_t cpu : units) {
        if (!callback(cpu)) {
            return false;
        }
    }
    
    return true;
}

bool Msr::applyPreset(const std::vector<MsrItem>& preset)
{
    if (preset.empty()) {
        return false;
    }
    
    Msr msr;
    if (!msr.isAvailable()) {
        Log::warn("MSR not available");
        return false;
    }
    
    bool success = msr.write([&msr, &preset](int32_t cpu) {
        for (const auto& item : preset) {
            if (!msr.write(item, cpu)) {
                return false;
            }
        }
        return true;
    });
    
    if (success) {
        Log::info("MSR tweaks applied successfully");
    }
    
    return success;
}

bool Msr::applyDefault()
{
    auto preset = zenrx::cpu().msrPreset();
    if (preset.empty()) {
        Log::debug("No MSR preset for this CPU");
        return false;
    }
    
    return applyPreset(preset);
}

#endif // _WIN32

} // namespace zenrx
