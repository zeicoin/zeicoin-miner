#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <vector>
#include <mutex>
#include <thread>

#include "Platform.h"

#include "Config.h"
#include "Job.h"
#include <map>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st BIO;

namespace zenrx {

class Client {
public:
    using JobCallback = std::function<void(const Job&)>;
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;
    
    Client(bool silent = false);
    ~Client();
    
    // Set pool
    void setPool(const PoolConfig& pool);

    // Connect to pool
    bool connect(const PoolConfig& pool);
    bool connect();  // Connect using configured pool
    void disconnect();
    bool isConnected() const { return m_connected; }
    
    // Submit share
    bool submit(const JobResult& result);
    
    // Callbacks
    void onJob(JobCallback cb) { m_onJob = std::move(cb); }
    void onConnected(ConnectedCallback cb) { m_onConnected = std::move(cb); }
    void onDisconnected(DisconnectedCallback cb) { m_onDisconnected = std::move(cb); }
    
    // Stats
    uint64_t accepted() const { return m_accepted; }
    uint64_t rejected() const { return m_rejected; }

    // MoneroOcean algo-perf
    void setAlgoPerf(const std::map<std::string, double>& perf) { m_algoPerf = perf; }
    void setAlgoMinTime(int t) { m_algoMinTime = t; }

    // Current pool info
    const PoolConfig& currentPool() const { return m_pool; }
    
    // Run network loop (call from dedicated thread)
    void run();
    void stop();

private:
    bool login();
    void handleMessage(const std::string& message);
    void handleJob(const std::string& json);
    bool send(const std::string& message);
    void reconnect();
    void sendKeepalive();

    PoolConfig m_pool;
    socket_t m_socket = SOCKET_INVALID;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_authenticated{false};
    
    std::string m_rpcId;
    std::atomic<uint64_t> m_sequence{0};
    
    std::atomic<uint64_t> m_accepted{0};
    std::atomic<uint64_t> m_rejected{0};
    
    JobCallback m_onJob;
    ConnectedCallback m_onConnected;
    DisconnectedCallback m_onDisconnected;
    
    std::mutex m_sendMutex;
    std::thread m_thread;
    
    // Keepalive
    std::chrono::steady_clock::time_point m_lastActivity;
    std::chrono::steady_clock::time_point m_keepaliveSentTime;
    std::chrono::steady_clock::time_point m_lastRecvTime;
    bool m_keepaliveInFlight = false;
    int  m_missedKeepalives = 0;
    static constexpr int KEEPALIVE_INTERVAL = 60;          // seconds idle before sending keepalive
    static constexpr int KEEPALIVE_RESPONSE_TIMEOUT = 30;  // seconds to wait for any recv after keepalive
    static constexpr int MAX_MISSED_KEEPALIVES = 3;        // consecutive misses before declaring dead
    static constexpr int SILENCE_TIMEOUT = 300;            // seconds of total silence before disconnect
    
    std::atomic<uint64_t> m_submitId{1};
    
    // Duplicate share prevention
    std::mutex m_submitHistoryMutex;
    std::vector<std::pair<std::string, uint32_t>> m_submitHistory;  // jobId + nonce
    static constexpr size_t MAX_SUBMIT_HISTORY = 100;

    // Silent mode (no job logs)
    bool m_silent = false;

    // MoneroOcean algo-perf data for stratum login
    std::map<std::string, double> m_algoPerf;
    int m_algoMinTime = 0;

    // TLS
    bool m_tls = false;
    SSL_CTX* m_sslCtx = nullptr;
    SSL* m_ssl = nullptr;
    BIO* m_readBio = nullptr;
    BIO* m_writeBio = nullptr;
    bool tlsHandshake();
    void tlsClose();
    bool flushTlsWrite();
};

} // namespace zenrx
