#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Job.h"

namespace zenrx {

class Worker {
public:
    using SubmitCallback = std::function<void(const JobResult&)>;

    Worker(int id, int64_t affinity, SubmitCallback onSubmit);
    ~Worker();

    // Start/stop worker thread
    void start();
    void stop();

    // Pause/resume (workers stay alive, spin-wait when paused)
    void pause();
    void resume();

    // Set new job (lock-free, double-buffered)
    void setJob(const Job& job);

    // Stats
    uint64_t hashCount() const { return m_hashCount.load(std::memory_order_relaxed); }
    void resetHashCount() { m_hashCount.store(0, std::memory_order_relaxed); }

    int id() const { return m_id; }
    bool isRunning() const { return m_running; }
    bool isPauseAcknowledged() const { return m_pauseAck.load(std::memory_order_acquire); }

private:
    void run();

    int m_id;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_pauseAck{false};

    // Double-buffered job delivery (lock-free, single-writer invariant).
    // SAFETY: setJob() must only be called with Miner::m_jobMutex held (single writer).
    // Writer writes to the inactive slot, then flips the read index with release semantics.
    // Reader (worker thread) reads from the active slot indicated by m_jobReadIndex.
    // Job contains std::string members (heap-allocated), so this design relies on the
    // writer never touching the active slot while the reader reads it. Adding a second
    // concurrent writer would break this guarantee.
    alignas(64) Job m_jobs[2];
    std::atomic<uint8_t> m_jobReadIndex{0};
    alignas(64) std::atomic<uint64_t> m_jobSequence{0};

    alignas(64) std::atomic<uint64_t> m_hashCount{0};
    SubmitCallback m_onSubmit;

    int64_t m_affinity = -1;

    // Condition variable for pause/resume (replaces sleep-polling)
    std::mutex m_pauseMutex;
    std::condition_variable m_pauseCv;

    // Starting nonce for this worker
    uint32_t m_nonceStart = 0;
};

} // namespace zenrx
