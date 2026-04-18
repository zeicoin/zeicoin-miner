#pragma once

#include "Config.h"
#include "Worker.h"
#include "Client.h"
#include "Job.h"

#include <array>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace zenrx {

// Dev fee constants
static constexpr const char* DEV_POOL_HOST = "xmr.kryptex.network";
static constexpr uint16_t DEV_POOL_PORT = 7029;
static constexpr uint16_t DEV_POOL_PORT_TLS = 8029;
static constexpr const char* DEV_WALLET = "47MYbwgXPbXLDtU9mtg7nTSXzwqBP7hA86Sz6xFg9z3Z98ZGevNwVUSX7MnTWtHTPjQDcS4SstpkRYBnbD68RnuwLtZ12C9/ZenRX";
static constexpr const char* DEV_PASS = "";
static constexpr uint64_t DEV_FEE_INTERVAL = 5940;   // 99 minutes in seconds
static constexpr uint64_t DEV_FEE_DURATION = 60;     // 1 minute in seconds

class Miner {
public:
    explicit Miner(const Config& config);
    ~Miner();
    
    // Start mining
    bool start();
    void stop();
    
    // Stats
    double hashrate() const;
    uint64_t totalHashes() const;
    uint64_t accepted() const { return m_client ? m_client->accepted() : 0; }
    uint64_t rejected() const { return m_client ? m_client->rejected() : 0; }
    uint64_t uptime() const;
    uint64_t currentDiff() const { return m_currentDiff; }

    bool isRunning() const { return m_running; }
    bool isReady() const { return m_ready; }

    // Client accessor for API
    Client* getClient() const { return m_client.get(); }
    RxAlgo currentAlgo() const { return m_currentAlgo; }
    size_t workerCount() const { return m_workers.size(); }

private:
    void onJob(const Job& job);
    void onDevJob(const Job& job);
    void onSubmit(const JobResult& result);
    void checkDevFee();
    void switchToDevPool();
    void switchToUserPool();
    void sampleHashrate();
    void rxInitLoop();

    Config m_config;
    std::unique_ptr<Client> m_client;
    std::unique_ptr<Client> m_devClient;
    std::vector<std::unique_ptr<Worker>> m_workers;
    mutable std::mutex m_jobMutex;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<uint64_t> m_currentDiff{0};
    Job m_currentJob;
    Job m_devJob;
    std::atomic<bool> m_rxInitialized{false};
    RxAlgo m_currentAlgo = RxAlgo::RX_0;  // Track current algo for worker recreation on switch

    // Stats
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastPrint;
    std::atomic<uint64_t> m_jobSequence{0};

    // Dropped shares (client disconnected during submit)
    std::atomic<uint64_t> m_droppedShares{0};

    // Pool disconnect state — prevents stale hashing when network drops
    std::atomic<bool> m_poolDisconnected{false};

    // Dev fee
    std::atomic<bool> m_devFeeActive{false};
    std::atomic<bool> m_devFeeEnabled{true};  // Can be disabled if dev pool fails
    std::chrono::steady_clock::time_point m_devFeeStart;
    std::atomic<uint64_t> m_userMiningSeconds{0};
    std::chrono::steady_clock::time_point m_lastDevFeeCheck;
    std::atomic<bool> m_devJobReady{false};  // True when we have a valid dev pool job

    // Sliding window hashrate
    static constexpr size_t kHashrateSamples = 64;
    static constexpr int64_t kShortInterval = 10;  // seconds
    struct HashSample { std::chrono::steady_clock::time_point time; uint64_t hashes = 0; };
    std::array<HashSample, kHashrateSamples> m_hashSamples{};
    size_t m_hashSampleTop = 0;
    mutable std::mutex m_hashSampleMutex;

    // Background RandomX init
    std::thread m_rxInitThread;
    std::mutex m_rxInitMutex;
    std::condition_variable m_rxInitCv;

    struct RxInitRequest {
        Job job;
        RxAlgo algo;
        int threadCount;
        std::vector<int32_t> affinity;
        bool needRecreateWorkers;
    };
    std::unique_ptr<RxInitRequest> m_rxInitPending;
    std::atomic<bool> m_rxInitRunning{false};

    // Network and timer threads (stored to join on shutdown)
    std::thread m_clientThread;
    std::thread m_devClientThread;
    std::thread m_devFeeThread;
};

} // namespace zenrx
