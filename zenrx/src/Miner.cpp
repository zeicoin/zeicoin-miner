#include "Miner.h"
#include "Log.h"
#include "CpuInfo.h"
#include "crypto/RandomX.h"

#include <thread>

namespace zenrx {

// Build per-worker NUMA node list from CPU affinity
static std::vector<int32_t> buildNumaNodes(int threadCount, const std::vector<int32_t>& affinity)
{
    std::vector<int32_t> numaNodes;
    numaNodes.reserve(threadCount);
    for (int i = 0; i < threadCount; i++) {
        int32_t cpuIdx = (i < static_cast<int>(affinity.size())) ? affinity[i] : -1;
        numaNodes.push_back(cpuIdx >= 0 ? cpu().numaNodeForCpu(cpuIdx) : 0);
    }
    return numaNodes;
}

Miner::Miner(const Config& config)
    : m_config(config)
    , m_startTime(std::chrono::steady_clock::now())
{
}

Miner::~Miner()
{
    stop();
}

bool Miner::start()
{
    if (m_running) return true;

    if (m_config.pool.host.empty()) {
        Log::error("No pool configured");
        return false;
    }

    // Use per-algo thread count and affinity for the initial algorithm
    m_currentAlgo = m_config.rxAlgo;
    int startThreads = m_config.threadsForAlgo(m_currentAlgo);
    const auto& startAffinity = m_config.affinityForAlgo(m_currentAlgo);

    Log::debug("Starting %d worker threads (algo: %s)", startThreads, rxAlgoName(m_currentAlgo));

    for (int i = 0; i < startThreads; i++) {
        int64_t aff = (i < static_cast<int>(startAffinity.size())) ? startAffinity[i] : -1;
        auto worker = std::make_unique<Worker>(i, aff, [this](const JobResult& result) {
            onSubmit(result);
        });
        m_workers.push_back(std::move(worker));
    }

    // Start worker threads once — they spin-wait for a valid job.
    // Workers stay alive until Miner::stop(); seed changes use pause/resume.
    for (auto& worker : m_workers) {
        worker->start();
    }

    m_running = true;
    m_startTime = std::chrono::steady_clock::now();
    m_lastPrint = m_startTime;
    m_lastDevFeeCheck = m_startTime;
    m_userMiningSeconds = 0;
    m_devFeeActive = false;

    m_client = std::make_unique<Client>();
    m_devClient = std::make_unique<Client>(true);

    // Pass algo-perf data for MoneroOcean stratum login
    m_client->setAlgoPerf(m_config.algoPerf);
    m_client->setAlgoMinTime(m_config.algoMinTime);

    m_client->onJob([this](const Job& job) { onJob(job); });
    m_client->onDisconnected([this]() {
        Log::warn("Pool disconnected - pausing miners");
        m_poolDisconnected.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_jobMutex);
        for (auto& w : m_workers) {
            w->pause();
        }
    });

    m_devClient->onJob([this](const Job& job) { onDevJob(job); });
    m_devClient->onDisconnected([this]() { Log::debug("Dev pool disconnected"); });

    Log::info("Connecting to pool...");

    m_client->setPool(m_config.pool);

    if (!m_client->connect()) {
        Log::error("Failed to connect to pool");
        return false;
    }

    PoolConfig devPool;
    devPool.host = DEV_POOL_HOST;
    devPool.port = DEV_POOL_PORT;
    devPool.user = DEV_WALLET;
    devPool.pass = DEV_PASS;

    // Mirror user TLS setting to dev pool (use actual connected state, not config,
    // in case TLS was auto-detected)
    bool userTls = m_client->currentPool().tls;
    if (userTls) {
        devPool.tls = true;
        devPool.port = DEV_POOL_PORT_TLS;
    }

    if (!m_devClient->connect(devPool)) {
        Log::debug("Dev pool unavailable");
        m_devFeeEnabled = false;
    } else {
        m_devFeeEnabled = true;
    }

    // Start background RandomX init thread (must be before client threads so it's
    // ready to handle init requests from onJob)
    m_rxInitThread = std::thread([this]() { rxInitLoop(); });

    m_clientThread = std::thread([this]() { m_client->run(); });
    
    if (m_devFeeEnabled) {
        m_devClientThread = std::thread([this]() { m_devClient->run(); });
    }

    m_devFeeThread = std::thread([this]() {
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!m_running) break;
            sampleHashrate();
            if (m_devFeeEnabled) {
                checkDevFee();
            }
        }
    });

    return true;
}

void Miner::stop()
{
    m_running = false;

    // Wake init thread so it can exit
    m_rxInitCv.notify_one();
    if (m_rxInitThread.joinable()) {
        m_rxInitThread.join();
    }

    // Stop workers
    for (auto& worker : m_workers) {
        worker->stop();
    }
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_workers.clear();
    }

    // Stop user client
    if (m_client) {
        m_client->stop();
    }

    // Stop dev client
    if (m_devClient) {
        m_devClient->stop();
    }

    // Join network threads (must be done after stop() signals)
    if (m_clientThread.joinable()) {
        m_clientThread.join();
    }
    if (m_devClientThread.joinable()) {
        m_devClientThread.join();
    }
    if (m_devFeeThread.joinable()) {
        m_devFeeThread.join();
    }

    // Reset clients after threads are joined
    m_client.reset();
    m_devClient.reset();

    // Release RandomX instances
    randomx().release();
}

void Miner::onJob(const Job& job)
{
    // Determine algo: prefer pool-supplied, fallback to config
    RxAlgo algo = job.algo().empty() ? m_config.rxAlgo : job.rxAlgo();

    // Check if seed changed or algo changed
    auto* userInst = randomx().getInstance(RxInstanceId::User);
    bool needsInit = !job.seedHash().empty() &&
        (!randomx().isSeedValid(RxInstanceId::User, job.seedHash()) ||
         (userInst && userInst->algo() != algo));

    // Detect algorithm change requiring worker recreation
    bool algoChanged = (algo != m_currentAlgo);

    if (needsInit || algoChanged) {
        int newThreadCount = m_config.threadsForAlgo(algo);
        const auto& newAffinity = m_config.affinityForAlgo(algo);
        bool needRecreateWorkers = algoChanged;

        Log::debug("New seed/algo detected (algo: %s, threads: %d->%d%s), queuing init...",
                   rxAlgoName(algo), static_cast<int>(m_workers.size()), newThreadCount,
                   needRecreateWorkers ? ", recreating workers" : "");

        {
            std::lock_guard<std::mutex> jlock(m_jobMutex);

            if (needRecreateWorkers) {
                // Stop and destroy old workers — algo changed, need new thread count + affinity
                for (auto& worker : m_workers) {
                    worker->stop();
                }
                m_workers.clear();
            } else {
                // Same algo, just seed change — pause workers for dataset reinit
                for (auto& worker : m_workers) {
                    worker->pause();
                }
                // Wait for all workers to confirm they've stopped hashing
                for (auto& worker : m_workers) {
                    while (!worker->isPauseAcknowledged()) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
            }
        }

        // Queue async init (returns immediately, frees network thread)
        {
            std::lock_guard<std::mutex> lock(m_rxInitMutex);
            m_rxInitPending = std::make_unique<RxInitRequest>();
            m_rxInitPending->job = job;
            m_rxInitPending->algo = algo;
            m_rxInitPending->threadCount = newThreadCount;
            m_rxInitPending->affinity = newAffinity;
            m_rxInitPending->needRecreateWorkers = needRecreateWorkers;
        }
        m_rxInitCv.notify_one();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_currentJob = job;
        m_currentJob.setSequence(++m_jobSequence);
        m_currentJob.setRxInstanceId(0);
        m_currentDiff = job.diff();

        if (!m_devFeeActive) {
            for (auto& worker : m_workers) {
                worker->setJob(m_currentJob);
            }
        }

        // Resume workers if they were paused due to pool disconnect
        if (m_poolDisconnected.exchange(false, std::memory_order_acq_rel)) {
            Log::info("Pool reconnected - resuming miners");
            for (auto& worker : m_workers) {
                worker->resume();
            }
        }
    }
}

void Miner::onDevJob(const Job& job)
{
    if (!job.seedHash().empty() && !randomx().isSeedValid(RxInstanceId::Dev, job.seedHash())) {
        Log::debug("Dev seed changed, reinitializing dev instance...");

        int devThreads = m_config.threadsForAlgo(m_currentAlgo);
        const auto& devAffinity = m_config.affinityForAlgo(m_currentAlgo);
        auto devNumaNodes = buildNumaNodes(devThreads, devAffinity);

        auto* devInstance = randomx().getInstance(RxInstanceId::Dev);
        if (devInstance && devInstance->isInitialized()) {
            if (!randomx().reinit(RxInstanceId::Dev, job.seedHash(), devThreads, true, -1, devNumaNodes)) {
                Log::debug("Failed to reinit dev RandomX - trying full init");
                if (!randomx().init(RxInstanceId::Dev, job.seedHash(), devThreads,
                                   RxAlgo::RX_0, m_config.hugePagesEnabled, true, -1,
                                   m_config.oneGbHugePagesEnabled, devNumaNodes)) {
                    Log::debug("Failed to initialize dev RandomX - dev fee disabled");
                    m_devFeeEnabled = false;
                    return;
                }
            }
        } else {
            if (!randomx().init(RxInstanceId::Dev, job.seedHash(), devThreads,
                               RxAlgo::RX_0, m_config.hugePagesEnabled, true, -1,
                               m_config.oneGbHugePagesEnabled, devNumaNodes)) {
                Log::debug("Failed to initialize dev RandomX - dev fee disabled");
                m_devFeeEnabled = false;
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_devJob = job;
        m_devJob.setSequence(++m_jobSequence);
        m_devJob.setRxInstanceId(1);
        m_devJobReady = true;

        if (m_devFeeActive) {
            for (auto& worker : m_workers) {
                worker->setJob(m_devJob);
            }
        }
    }
}

void Miner::onSubmit(const JobResult& result)
{
    // Route share based on rxInstanceId - this is the most reliable way
    if (result.rxInstanceId == 1) {
        // Dev instance share
        if (m_devClient && m_devClient->isConnected()) {
            if (!m_devClient->submit(result)) {
                m_droppedShares.fetch_add(1, std::memory_order_relaxed);
                Log::warn("Failed to submit dev share | total dropped: %lu",
                          m_droppedShares.load(std::memory_order_relaxed));
            }
        } else {
            m_droppedShares.fetch_add(1, std::memory_order_relaxed);
            Log::warn("Dropped dev share (pool disconnected) | total dropped: %lu",
                      m_droppedShares.load(std::memory_order_relaxed));
        }
    } else {
        // User instance share
        if (m_client && m_client->isConnected()) {
            if (!m_client->submit(result)) {
                m_droppedShares.fetch_add(1, std::memory_order_relaxed);
                Log::warn("Failed to submit user share | diff: %lu | total dropped: %lu",
                          result.actualDiff, m_droppedShares.load(std::memory_order_relaxed));
            }
        } else {
            m_droppedShares.fetch_add(1, std::memory_order_relaxed);
            Log::warn("Dropped user share (pool disconnected) | diff: %lu | total dropped: %lu",
                      result.actualDiff, m_droppedShares.load(std::memory_order_relaxed));
        }
    }
}

void Miner::sampleHashrate()
{
    std::lock_guard<std::mutex> lock(m_hashSampleMutex);
    auto& s = m_hashSamples[m_hashSampleTop % kHashrateSamples];
    s.time = std::chrono::steady_clock::now();
    s.hashes = totalHashes();
    ++m_hashSampleTop;
}

double Miner::hashrate() const
{
    std::lock_guard<std::mutex> lock(m_hashSampleMutex);

    if (m_hashSampleTop < 2) {
        // Fall back to lifetime average
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count();
        if (elapsed <= 0) return 0.0;
        return static_cast<double>(totalHashes()) / elapsed;
    }

    const size_t newest = (m_hashSampleTop - 1) % kHashrateSamples;
    const auto& newestSample = m_hashSamples[newest];

    // Scan backwards to find oldest sample within kShortInterval
    size_t count = std::min(m_hashSampleTop, kHashrateSamples);
    size_t oldestIdx = newest;

    for (size_t i = 1; i < count; ++i) {
        size_t idx = (m_hashSampleTop - 1 - i) % kHashrateSamples;
        auto dt = std::chrono::duration_cast<std::chrono::seconds>(
            newestSample.time - m_hashSamples[idx].time).count();
        if (dt > kShortInterval) break;
        oldestIdx = idx;
    }

    if (oldestIdx == newest) {
        // Only one sample in window, fall back
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            newestSample.time - m_startTime).count();
        if (elapsed <= 0) return 0.0;
        return static_cast<double>(newestSample.hashes) / elapsed;
    }

    const auto& oldestSample = m_hashSamples[oldestIdx];
    double dt = std::chrono::duration<double>(newestSample.time - oldestSample.time).count();
    if (dt <= 0.0) return 0.0;

    return static_cast<double>(newestSample.hashes - oldestSample.hashes) / dt;
}

uint64_t Miner::totalHashes() const
{
    if (!m_running) return 0;

    // Safe without lock: m_workers is stable during normal mining.
    // Algo changes that modify m_workers hold m_jobMutex which
    // serializes with callers that also hold it.
    uint64_t total = 0;
    for (const auto& worker : m_workers) {
        total += worker->hashCount();
    }
    return total;
}

uint64_t Miner::uptime() const
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count();
}

void Miner::checkDevFee()
{
    auto now = std::chrono::steady_clock::now();

    if (m_devFeeActive) {
        // Check if dev fee period is over
        auto devFeeDuration = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_devFeeStart).count();

        if (static_cast<uint64_t>(devFeeDuration) >= DEV_FEE_DURATION) {
            switchToUserPool();
        }
    } else {
        // Update user mining time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_lastDevFeeCheck).count();

        if (elapsed > 0) {
            m_userMiningSeconds += elapsed;
            m_lastDevFeeCheck = now;
        }

        // Check if time to start dev fee
        if (m_userMiningSeconds >= DEV_FEE_INTERVAL) {
            switchToDevPool();
        }
    }
}

void Miner::switchToDevPool()
{
    if (!m_devJobReady || !m_devFeeEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_jobMutex);

    if (!randomx().isSeedValid(RxInstanceId::Dev, m_devJob.seedHash())) {
        Log::debug("Dev RandomX not ready, skipping");
        return;
    }

    m_devFeeActive = true;
    m_devFeeStart = std::chrono::steady_clock::now();
    Log::debug("Devfee Started");

    for (auto& worker : m_workers) {
        worker->setJob(m_devJob);
    }
}

void Miner::switchToUserPool()
{
    std::lock_guard<std::mutex> lock(m_jobMutex);

    m_devFeeActive = false;
    m_userMiningSeconds = 0;
    m_lastDevFeeCheck = std::chrono::steady_clock::now();
    Log::debug("Devfee Ended");

    for (auto& worker : m_workers) {
        worker->setJob(m_currentJob);
    }
}

void Miner::rxInitLoop()
{
    while (m_running) {
        std::unique_lock<std::mutex> lock(m_rxInitMutex);
        m_rxInitCv.wait(lock, [this] {
            return m_rxInitPending != nullptr || !m_running;
        });

        if (!m_running) break;

        // Take the pending request (last one wins if multiple arrived)
        auto req = std::move(m_rxInitPending);
        m_rxInitPending = nullptr;
        m_rxInitRunning = true;
        lock.unlock();

        // Build per-worker NUMA node list from affinity
        auto numaNodes = buildNumaNodes(req->threadCount, req->affinity);

        // Heavy operation — blocks for seconds, but NOT on network thread
        bool ok = randomx().init(RxInstanceId::User, req->job.seedHash(),
                                 req->threadCount, req->algo,
                                 m_config.hugePagesEnabled, true, -1,
                                 m_config.oneGbHugePagesEnabled, numaNodes);

        if (ok && m_devFeeEnabled && !m_rxInitialized) {
            Log::debug("Pre-allocating dev instance...");
            randomx().init(RxInstanceId::Dev, req->job.seedHash(),
                          req->threadCount, RxAlgo::RX_0,
                          m_config.hugePagesEnabled, true, -1,
                          m_config.oneGbHugePagesEnabled, numaNodes);
            m_rxInitialized = true;
        }

        if (!ok) {
            Log::error("Failed to initialize RandomX for %s", rxAlgoName(req->algo));
        }

        // Resume mining (create workers, set job)
        {
            std::lock_guard<std::mutex> jlock(m_jobMutex);

            // Check if a newer request superseded us
            {
                std::lock_guard<std::mutex> ilock(m_rxInitMutex);
                if (m_rxInitPending) {
                    // Newer request queued — skip resume, let next iteration handle it
                    m_rxInitRunning = false;
                    continue;
                }
            }

            if (ok) {
                m_ready = true;
                m_currentAlgo = req->algo;

                if (req->needRecreateWorkers) {
                    // Create new workers with correct affinity for the new algorithm
                    for (int i = 0; i < req->threadCount; i++) {
                        int64_t aff = (i < static_cast<int>(req->affinity.size())) ? req->affinity[i] : -1;
                        auto worker = std::make_unique<Worker>(i, aff,
                            [this](const JobResult& result) { onSubmit(result); });
                        m_workers.push_back(std::move(worker));
                    }
                    for (auto& w : m_workers) w->start();
                }

                m_currentJob = req->job;
                m_currentJob.setSequence(++m_jobSequence);
                m_currentJob.setRxInstanceId(0);
                m_currentDiff = req->job.diff();

                for (auto& w : m_workers) {
                    if (!m_devFeeActive) w->setJob(m_currentJob);
                    if (!req->needRecreateWorkers && !m_poolDisconnected.load(std::memory_order_acquire)) w->resume();
                }
            } else if (req->needRecreateWorkers) {
                // Init failed and workers are gone — recreate with previous algo settings
                Log::error("Workers lost during failed algo switch, recreating...");
                int prevThreads = m_config.threadsForAlgo(m_currentAlgo);
                const auto& prevAff = m_config.affinityForAlgo(m_currentAlgo);
                for (int i = 0; i < prevThreads; i++) {
                    int64_t aff = (i < static_cast<int>(prevAff.size())) ? prevAff[i] : -1;
                    auto worker = std::make_unique<Worker>(i, aff,
                        [this](const JobResult& result) { onSubmit(result); });
                    m_workers.push_back(std::move(worker));
                }
                for (auto& w : m_workers) w->start();
                // Workers will spin waiting for a valid VM until a successful init
            } else {
                // Seed change failed — resume workers with old state
                if (!m_poolDisconnected.load(std::memory_order_acquire)) {
                    for (auto& w : m_workers) w->resume();
                }
            }
        }

        m_rxInitRunning = false;
    }
}

} // namespace zenrx
