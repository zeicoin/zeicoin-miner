#include "App.h"
#include "Log.h"
#include "CpuInfo.h"
#include "Msr.h"
#include "Api.h"
#include "MoBenchmark.h"
#include "crypto/RandomX.h"
#include "version.h"

extern "C" {
#include "crypto/argon2/include/argon2.h"
}

#include <csignal>
#include <thread>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <unistd.h>
#   include <fcntl.h>
#   ifdef __GLIBC__
#       include <execinfo.h>
#       define HAVE_BACKTRACE 1
#   endif
#endif

namespace zenrx {

#ifdef _WIN32
// Enable Windows console colors (ANSI escape codes)
static void enableWindowsConsoleColors()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;
    
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}

// Enable SeLockMemoryPrivilege for huge pages on Windows
static bool enableLockMemoryPrivilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    
    BOOL result = AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    DWORD error = GetLastError();
    CloseHandle(token);
    
    return result && (error == ERROR_SUCCESS);
}

// Check if huge pages are available on Windows
static bool checkWindowsHugePages()
{
    // Try to enable the privilege
    if (!enableLockMemoryPrivilege()) {
        return false;
    }
    
    // Try to allocate a small huge page to test
    SIZE_T largePageSize = GetLargePageMinimum();
    if (largePageSize == 0) {
        return false;
    }
    
    void* mem = VirtualAlloc(NULL, largePageSize, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (mem == NULL) {
        return false;
    }
    
    VirtualFree(mem, 0, MEM_RELEASE);
    return true;
}
#endif

// Check if running as root/admin
static bool isRoot()
{
#ifdef _WIN32
    // Windows: check if running as administrator
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
#else
    // Linux/Unix: check if running as root
    return geteuid() == 0;
#endif
}

#ifndef _WIN32
// Async-signal-safe crash handler — writes backtrace to stderr and /tmp/zenrx_crash.log
static void crashHandler(int sig)
{
    // Use only async-signal-safe functions
    const char* msg = "CRASH: unknown signal\n";
    switch (sig) {
        case SIGSEGV: msg = "CRASH: SIGSEGV (segmentation fault)\n"; break;
        case SIGABRT: msg = "CRASH: SIGABRT (aborted)\n"; break;
        case SIGBUS:  msg = "CRASH: SIGBUS (bus error)\n"; break;
        case SIGFPE:  msg = "CRASH: SIGFPE (floating point exception)\n"; break;
    }
    // strlen is not technically async-signal-safe, but works in practice on all targets
    size_t msgLen = 0;
    while (msg[msgLen]) ++msgLen;

    write(STDERR_FILENO, msg, msgLen);

#ifdef HAVE_BACKTRACE
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif

    int fd = open("/tmp/zenrx_crash.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, msg, msgLen);
#ifdef HAVE_BACKTRACE
        backtrace_symbols_fd(frames, n, fd);
#endif
        close(fd);
    }

    _exit(128 + sig);
}
#endif

#ifndef _WIN32
// Setup huge pages on Linux with retry and verification
static constexpr int HUGEPAGES_RETRY_COUNT = 3;
static constexpr int HUGEPAGES_RETRY_DELAY_MS = 500;

static int64_t getHugePagesValue(const char* pageSize, const std::string& filename)
{
    // Try all NUMA nodes and sum their values
    uint32_t nodes = cpu().numaNodes();
    int64_t total = 0;
    bool found = false;
    for (uint32_t n = 0; n < nodes; n++) {
        std::string path = "/sys/devices/system/node/node" + std::to_string(n)
                         + "/hugepages/hugepages-" + pageSize + "/" + filename;
        std::ifstream file(path);
        if (file.is_open()) {
            int64_t value = 0;
            file >> value;
            total += value;
            found = true;
        }
    }
    if (found) return total;

    // Fallback to global path
    std::string path = std::string("/sys/kernel/mm/hugepages/hugepages-") + pageSize + "/" + filename;
    std::ifstream file(path);
    if (file.is_open()) {
        int64_t value = 0;
        file >> value;
        return value;
    }
    return -1;
}

static int64_t getHugePages(const char* pageSize)
{
    return getHugePagesValue(pageSize, "nr_hugepages");
}

static int64_t getFreeHugePages(const char* pageSize)
{
    return getHugePagesValue(pageSize, "free_hugepages");
}

static bool writeHugePages(const char* pageSize, size_t count)
{
    uint32_t nodes = cpu().numaNodes();
    if (nodes > 1) {
        // Multi-node: distribute evenly across all nodes
        size_t perNode = count / nodes;
        size_t remainder = count % nodes;
        bool anySuccess = false;
        for (uint32_t n = 0; n < nodes; n++) {
            size_t nodeCount = perNode + (n < remainder ? 1 : 0);
            std::string path = "/sys/devices/system/node/node" + std::to_string(n)
                             + "/hugepages/hugepages-" + pageSize + "/nr_hugepages";
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            if (file.is_open()) {
                file << nodeCount;
                file.flush();
                if (file.good()) anySuccess = true;
            }
        }
        return anySuccess;
    }

    // Single node: write to node0, fallback to global
    std::string paths[] = {
        std::string("/sys/devices/system/node/node0/hugepages/hugepages-") + pageSize + "/nr_hugepages",
        std::string("/sys/kernel/mm/hugepages/hugepages-") + pageSize + "/nr_hugepages"
    };

    for (const auto& path : paths) {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << count;
            file.flush();
            if (file.good()) {
                return true;
            }
        }
    }
    return false;
}

static bool setupHugePages(const char* pageSize, const char* label, size_t requiredPages)
{
    for (int attempt = 0; attempt < HUGEPAGES_RETRY_COUNT; ++attempt) {
        if (attempt > 0) {
            Log::debug("%s setup retry %d/%d", label, attempt + 1, HUGEPAGES_RETRY_COUNT);
            std::this_thread::sleep_for(std::chrono::milliseconds(HUGEPAGES_RETRY_DELAY_MS));
        }

        int64_t currentPages = getHugePages(pageSize);

        if (currentPages >= static_cast<int64_t>(requiredPages)) {
            int64_t freePages = getFreeHugePages(pageSize);
            if (freePages >= static_cast<int64_t>(requiredPages)) {
                Log::debug("%s: %ld allocated, %ld free", label, currentPages, freePages);
                return true;
            }
            Log::debug("%s: not enough free (%ld/%zu)", label, freePages, requiredPages);
        }

        if (!writeHugePages(pageSize, requiredPages)) {
            Log::debug("Failed to write %s value", label);
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int64_t allocated = getHugePages(pageSize);
        int64_t freePages = getFreeHugePages(pageSize);

        if (allocated >= static_cast<int64_t>(requiredPages) &&
            freePages >= static_cast<int64_t>(requiredPages)) {
            Log::debug("%s: %ld allocated, %ld free", label, allocated, freePages);
            return true;
        }

        Log::debug("%s incomplete: %ld/%zu (free: %ld)", label, allocated, requiredPages, freePages);
    }

    Log::debug("Failed to allocate %s after %d attempts", label, HUGEPAGES_RETRY_COUNT);
    return false;
}
#endif

App* App::s_instance = nullptr;
std::atomic<bool> App::s_signaled{false};

App::App(int argc, char** argv)
{
    s_instance = this;
    
    // Parse configuration
    if (!m_config.parseArgs(argc, argv)) {
        return;
    }
    
    Log::init(m_config.debug ? LogLevel::Debug : LogLevel::Info);

    // Initialize file logging from config (if not already set via --log-file CLI)
    if (!m_config.logFile.empty()) {
        Log::initFile(m_config.logFile);
    }
}

App::~App()
{
    stop();
    s_instance = nullptr;
}

int App::run()
{
    if (!m_config.isValid()) {
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signalHandler);
#ifndef _WIN32
    signal(SIGTERM, signalHandler);
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGBUS,  crashHandler);
    signal(SIGFPE,  crashHandler);
#else
    // Enable ANSI colors on Windows console
    enableWindowsConsoleColors();
#endif
    
    printBanner();
    printCpuInfo();
    
    // Try to setup huge pages
    if (isRoot()) {
#ifdef _WIN32
        m_config.hugePagesEnabled = checkWindowsHugePages();
#else
        // Dataset ~2GB = 1024 pages, Cache ~256MB = 128 pages, plus scratchpad per thread
        // Scratchpad: 2MB default (rx/0) = 1 page per thread, smaller for some algos
        const size_t pagesPerInstance = 1168 + static_cast<size_t>(m_config.maxThreads());
        const size_t requiredPages = pagesPerInstance * 2;

        Log::debug("Allocating %zu hugepages for %d max threads", requiredPages, m_config.maxThreads());

        m_hugePagesOriginal = getHugePages("2048kB");
        m_config.hugePagesEnabled = setupHugePages("2048kB", "Hugepages", requiredPages);

        // Try 1GB huge pages for dataset (reduces TLB misses significantly)
        if (cpu().has1GbPages()) {
            // Dataset ~2.03GB needs 3 x 1GB pages, cache ~256MB needs 1 x 1GB page
            // Two instances (user + dev) = (3 + 1) * 2 = 8 pages minimum
            const size_t required1GbPages = 8;
            m_1gbHugePagesOriginal = getHugePages("1048576kB");
            if (setupHugePages("1048576kB", "1GB hugepages", required1GbPages)) {
                m_config.oneGbHugePagesEnabled = true;
                Log::info("1GB hugepages: enabled (%zu pages)", required1GbPages);
            } else {
                Log::debug("1GB hugepages: not available, using 2MB pages");
            }
        }
#endif
    } else {
        m_config.hugePagesEnabled = false;
        Log::debug("Not running as root, huge pages disabled");
    }
    
    // Select argon2 implementation
    if (!m_config.argon2Impl.empty()) {
        if (!argon2_select_impl_by_name(m_config.argon2Impl.c_str())) {
            Log::warn("Argon2 implementation '%s' not found, using auto-detect", m_config.argon2Impl.c_str());
            argon2_select_impl();
        }
    }

    // Try to apply MSR tweaks if running as root/admin
    if (isRoot()) {
        m_config.msrEnabled = Msr::applyDefault();
    } else {
        m_config.msrEnabled = false;
        Log::debug("Not running as root, MSR tweaks disabled");
    }
    
    printSummary();
    
    // Create miner (but don't start yet)
    m_miner = std::make_unique<Miner>(m_config);

    // Start API early so it's reachable during autotune
    if (m_config.apiEnabled) {
        m_api = std::make_unique<Api>(&m_config, m_miner.get());
        m_api->start();
    }

    // Run algo-perf benchmark if needed (for MoneroOcean algo switching)
    bool configSaved = false;
    if (MoBenchmark::isNeeded(m_config)) {
        m_config.autotuning = true;
        Log::info("Starting algo performance calibration...");
        m_config.algoPerf = MoBenchmark::runAll(m_config);
        std::string savePath = m_config.configPath.empty() ? "zenrx.json" : m_config.configPath;
        m_config.saveFile(savePath);
        configSaved = true;
        m_config.autotuning = false;
        Log::info("Algo performance calibration complete");
    }

    // Auto-save config if enabled (skip if already saved by benchmark above)
    if (!configSaved && m_config.autosave && m_config.configPath.empty()) {
        m_config.saveFile("zenrx.json");
    }

    printf("\n");

    // Now start miner (connects to pools, jobs start arriving)
    if (!m_miner->start()) {
        Log::error("Failed to start miner");
        return 1;
    }

    m_running = true;

    // Main loop - print stats periodically (500ms poll for fast shutdown)
    auto lastPrint = std::chrono::steady_clock::now();
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!m_running || s_signaled.load(std::memory_order_relaxed)) break;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrint).count() >= m_config.printTime && m_miner) {
            lastPrint = now;
            double hr = m_miner->hashrate();
            uint64_t accepted = m_miner->accepted();
            uint64_t rejected = m_miner->rejected();
            Log::hashrate(hr, accepted, rejected);
        }
    }
    
    Log::info("Shutting down...");

    if (m_api) {
        m_api->stop();
    }

    m_miner->stop();

    releaseHugePages();

    return 0;
}

void App::stop()
{
    m_running = false;
    if (m_api) {
        m_api->stop();
    }
    if (m_miner) {
        m_miner->stop();
    }
    releaseHugePages();
}

void App::signalHandler(int sig)
{
    (void)sig;
    // Only set a flag here — calling stop() from a signal handler is unsafe
    // because it locks mutexes, joins threads, and frees memory (all non-async-signal-safe).
    // The main loop in App::run() checks s_signaled and performs the clean shutdown.
    s_signaled.store(true, std::memory_order_relaxed);
}

void App::printBanner()
{
    printf("\n");
    
    if (m_config.colors) {
        printf("\033[1;36m");
    }
    
    printf(" %s - RandomX CPU Miner\n", APP_VERSION_FULL);
    
    if (m_config.colors) {
        printf("\033[0m");
    }
    
    printf("\n");
}

void App::printCpuInfo()
{
    const auto& cpuInfo = cpu();
    
    Log::info("CPU: %s", cpuInfo.brand().c_str());
    Log::debug("  Cores: %u | Threads: %u", cpuInfo.cores(), cpuInfo.threads());
    Log::debug("  L2: %zu KB | L3: %zu MB", 
             cpuInfo.L2() / 1024, cpuInfo.L3() / (1024 * 1024));
    if (cpuInfo.numaNodes() > 1) {
        Log::info("NUMA: %u nodes detected", cpuInfo.numaNodes());
    }
    Log::debug("  AES: %s | AVX2: %s | AVX512: %s",
             cpuInfo.hasAES() ? "yes" : "no",
             cpuInfo.hasAVX2() ? "yes" : "no",
             cpuInfo.hasAVX512() ? "yes" : "no");
    
    const char* msrPreset = "none";
    switch (cpuInfo.msrMod()) {
        case MSR_MOD_RYZEN_17H: msrPreset = "ryzen (Zen/Zen+/Zen2)"; break;
        case MSR_MOD_RYZEN_19H: msrPreset = "ryzen (Zen3)"; break;
        case MSR_MOD_RYZEN_19H_ZEN4: msrPreset = "ryzen (Zen4)"; break;
        case MSR_MOD_RYZEN_1AH_ZEN5: msrPreset = "ryzen (Zen5)"; break;
        case MSR_MOD_INTEL: msrPreset = "intel"; break;
        default: break;
    }
    
    Log::debug("MSR Preset: %s", msrPreset);
}

void App::printSummary()
{
    Log::info("Threads: %d (%s) | MSR: %s | HugePages: %s | 1GB Pages: %s",
              m_config.threadsForAlgo(m_config.rxAlgo),
              rxAlgoName(m_config.rxAlgo),
              m_config.msrEnabled ? "ON" : "OFF",
              m_config.hugePagesEnabled ? "ON" : "OFF",
              m_config.oneGbHugePagesEnabled ? "ON" : "OFF");

    Log::info("Pool: %s:%d", m_config.pool.host.c_str(), m_config.pool.port);

    if (m_config.apiEnabled) {
        Log::debug("API: http://%s:%d/api", m_config.apiHost.c_str(), m_config.apiPort);
    }
}

void App::releaseHugePages()
{
#ifndef _WIN32
    // Restore 1GB hugepages
    int64_t original1Gb = m_1gbHugePagesOriginal.exchange(-1);
    if (original1Gb >= 0) {
        const size_t restore = static_cast<size_t>(original1Gb);
        if (writeHugePages("1048576kB", restore)) {
            Log::debug("1GB hugepages restored to %zu", restore);
        } else {
            Log::debug("Failed to restore 1GB hugepages to %zu", restore);
        }
    }

    // Restore 2MB hugepages
    int64_t original = m_hugePagesOriginal.exchange(-1);
    if (original < 0) {
        return;
    }

    const size_t restore = static_cast<size_t>(original);
    if (writeHugePages("2048kB", restore)) {
        Log::debug("Hugepages restored to %zu", restore);
    } else {
        Log::debug("Failed to restore hugepages to %zu", restore);
    }
#endif
}

} // namespace zenrx
