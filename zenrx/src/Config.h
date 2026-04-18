#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <atomic>

namespace zenrx {

enum class RxAlgo {
    RX_0,       // rx/0   - Monero (default)
    RX_WOW,     // rx/wow - Wownero
    RX_ARQ,     // rx/arq - Arqma
    RX_XEQ,     // rx/xeq - Equilibria
    RX_GRAFT,   // rx/graft - Graft
    RX_SFX,     // rx/sfx - Safex
    RX_XLA,     // rx/xla / panthera - Scala
};

// Convert string to RxAlgo enum
inline RxAlgo parseRxAlgo(const std::string& str) {
    if (str == "rx/wow")    return RxAlgo::RX_WOW;
    if (str == "rx/arq")    return RxAlgo::RX_ARQ;
    if (str == "rx/xeq")    return RxAlgo::RX_XEQ;
    if (str == "rx/graft")  return RxAlgo::RX_GRAFT;
    if (str == "rx/sfx")    return RxAlgo::RX_SFX;
    if (str == "rx/xla")    return RxAlgo::RX_XLA;
    if (str == "panthera")  return RxAlgo::RX_XLA;
    return RxAlgo::RX_0;  // default
}

// Convert RxAlgo enum to string
inline const char* rxAlgoName(RxAlgo algo) {
    switch (algo) {
        case RxAlgo::RX_0:     return "rx/0";
        case RxAlgo::RX_WOW:   return "rx/wow";
        case RxAlgo::RX_ARQ:   return "rx/arq";
        case RxAlgo::RX_XEQ:   return "rx/xeq";
        case RxAlgo::RX_GRAFT: return "rx/graft";
        case RxAlgo::RX_SFX:   return "rx/sfx";
        case RxAlgo::RX_XLA:   return "rx/xla";
    }
    return "rx/0";
}

struct PoolConfig {
    std::string host;
    uint16_t port = 3333;
    std::string user;
    std::string pass = "x";
    bool tls = false;
};

struct Config {
    Config() = default;
    Config(const Config& o)
        : pool(o.pool), algoThreads(o.algoThreads), algoAffinities(o.algoAffinities),
          algo(o.algo), rxAlgo(o.rxAlgo), algoSpecified(o.algoSpecified),
          apiEnabled(o.apiEnabled), apiHost(o.apiHost), apiPort(o.apiPort),
          algoPerf(o.algoPerf), benchAlgoTime(o.benchAlgoTime), algoMinTime(o.algoMinTime),
          argon2Impl(o.argon2Impl),
          msrEnabled(o.msrEnabled), hugePagesEnabled(o.hugePagesEnabled),
          oneGbHugePagesEnabled(o.oneGbHugePagesEnabled),
          autotuning(o.autotuning.load(std::memory_order_relaxed)),
          autotuneAlgo(o.autotuneAlgo),
          colors(o.colors), printTime(o.printTime), debug(o.debug), autosave(o.autosave),
          logFile(o.logFile), configPath(o.configPath)
    {}
    Config& operator=(const Config&) = delete;

    // Pool settings
    PoolConfig pool;

    // Per-algorithm thread counts and affinities (pre-computed in autoDetect)
    std::map<RxAlgo, int> algoThreads;
    std::map<RxAlgo, std::vector<int32_t>> algoAffinities;

    // Lookup per-algo thread count
    int threadsForAlgo(RxAlgo algo) const;
    // Max thread count across all algos (for huge pages allocation)
    int maxThreads() const;
    // Lookup per-algo affinity list
    const std::vector<int32_t>& affinityForAlgo(RxAlgo algo) const;

    // Algorithm (RandomX variant)
    std::string algo = "";
    RxAlgo rxAlgo = RxAlgo::RX_0;
    bool algoSpecified = false;
    
    // HTTP API
    bool apiEnabled = true;       // Enable by default for agent.sh
    std::string apiHost = "127.0.0.1";
    uint16_t apiPort = 16000;     // Default port for ZenOS agent
    // MoneroOcean algo-perf benchmark results
    std::map<std::string, double> algoPerf;   // algo name -> H/s
    int benchAlgoTime = 20;                    // seconds per benchmark round
    int algoMinTime = 0;                       // minimum algo mining time for pool

    // CPU settings
    std::string argon2Impl;          // "argon2-impl" - argon2 implementation name

    // System status (set at runtime)
    bool msrEnabled = false;      // True if MSR was successfully applied
    bool hugePagesEnabled = false; // True if huge pages were successfully set
    bool oneGbHugePagesEnabled = false; // True if 1GB huge pages were successfully set
    std::atomic<bool> autotuning{false}; // True while algo-perf benchmark is running
    const char* autotuneAlgo = nullptr;  // Currently benchmarking algo name (pointer to string literal)
    
    // Misc
    bool colors = true;
    int printTime = 60;       // Hashrate print interval
    bool debug = false;       // Enable debug logging
    bool autosave = true;     // Auto-save config after detection
    
    // Logging
    std::string logFile;          // Path to log file (empty = no file logging)

    // Config file path
    std::string configPath;
    
    // Parse from command line
    bool parseArgs(int argc, char** argv);
    
    // Parse from JSON file
    bool parseFile(const std::string& path);
    
    // Save to JSON file
    bool saveFile(const std::string& path = "") const;
    
    // Validate configuration
    bool isValid() const;
    
    // Apply auto-detection for threads based on CPU
    void autoDetect();
    
    // Print help
    static void printHelp();
    
    // Print version
    static void printVersion();
};

} // namespace zenrx
