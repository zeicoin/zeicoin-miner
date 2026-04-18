#include "Worker.h"
#include "Log.h"
#include "crypto/RandomX.h"

#include "crypto/randomx/randomx.h"
#include "crypto/Algorithm.h"
#include <cstring>

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace zenrx {


// Pin current thread to a specific CPU (by OS index from hwloc topology)
static void pinToProcessor(int workerId, int64_t affinity)
{
    if (affinity < 0) {
        return;
    }

    int cpuId = static_cast<int>(affinity);

#ifdef _WIN32
    DWORD_PTR mask = 1ULL << cpuId;
    SetThreadAffinityMask(GetCurrentThread(), mask);
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpuId, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    Log::debug("Worker %d pinned to CPU %d", workerId, cpuId);
}

Worker::Worker(int id, int64_t affinity, SubmitCallback onSubmit)
    : m_id(id)
    , m_affinity(affinity)
    , m_onSubmit(std::move(onSubmit))
{
    static_assert(sizeof(int) >= 4, "int must be at least 32 bits");

    if (id >= 256) {
        Log::warn("Worker %d: id >= 256, nonce ranges will overlap with worker %d", id, id % 256);
    }

    // Each worker gets a unique nonce range (0x01000000 per worker).
    // With 256 workers the full 32-bit space is covered; beyond that, ranges overlap.
    m_nonceStart = static_cast<uint32_t>(id % 256) * 0x01000000;
}

Worker::~Worker()
{
    stop();
}

void Worker::start()
{
    if (m_running) return;
    
    m_running = true;
    m_thread = std::thread(&Worker::run, this);
    
    Log::debug("Worker %d started", m_id);
}

void Worker::stop()
{
    m_running = false;
    m_paused = false;  // Unblock pause loop so thread can exit
    m_pauseCv.notify_one();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    Log::debug("Worker %d stopped", m_id);
}

void Worker::pause()
{
    m_pauseAck.store(false, std::memory_order_release);
    m_paused.store(true, std::memory_order_release);
}

void Worker::resume()
{
    m_paused.store(false, std::memory_order_release);
    m_pauseCv.notify_one();
}

void Worker::setJob(const Job& job)
{
    // Double-buffer: write to the inactive slot, then flip the read index.
    // setJob() is always called with Miner's m_jobMutex held, so single-writer is guaranteed.
    const uint8_t writeIdx = 1 - m_jobReadIndex.load(std::memory_order_relaxed);
    m_jobs[writeIdx] = job;
    std::atomic_thread_fence(std::memory_order_release);  // Ensure job data is visible before index flip
    m_jobReadIndex.store(writeIdx, std::memory_order_release);
    m_jobSequence.fetch_add(1, std::memory_order_release);
}

void Worker::run()
{
    // Pin this worker to a specific CPU for cache locality
    pinToProcessor(m_id, m_affinity);

    alignas(64) uint8_t hash[32];  // Cache-line aligned
    alignas(16) uint64_t tempHash[8];  // Pipeline state for _first/_next
    Job localJob;
    uint32_t nonce = m_nonceStart;
    uint32_t prevNonce = 0;
    std::string currentJobId;
    RxInstanceId currentRxId = RxInstanceId::User;
    randomx_vm* vm = nullptr;
    zenrx::Algorithm algo;
    bool first = true;
    uint64_t localSequence = 0;

    while (m_running) {
        // Wait when paused (during RandomX dataset init)
        if (m_paused.load(std::memory_order_acquire)) {
            m_pauseAck.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lock(m_pauseMutex);
            m_pauseCv.wait(lock, [this] {
                return !m_paused.load(std::memory_order_acquire) || !m_running;
            });
            continue;
        }
        if (m_pauseAck.load(std::memory_order_relaxed)) {
            m_pauseAck.store(false, std::memory_order_release);
        }

        // Lock-free job check via double-buffer sequence number
        const uint64_t seq = m_jobSequence.load(std::memory_order_acquire);
        if (seq != localSequence) {
            localSequence = seq;
            const uint8_t readIdx = m_jobReadIndex.load(std::memory_order_acquire);
            localJob = m_jobs[readIdx];

            // Only reset nonce if job ID actually changed
            if (localJob.id() != currentJobId) {
                currentJobId = localJob.id();
                nonce = m_nonceStart;
                first = true;  // Reset pipeline on job change

                // Use User VM when possible (avoids L3 cache thrashing on AMD)
                // Fall back to Dev VM only when seeds differ
                if (localJob.rxInstanceId() == 0) {
                    currentRxId = RxInstanceId::User;
                } else if (randomx().isSeedValid(RxInstanceId::User, localJob.seedHash())) {
                    currentRxId = RxInstanceId::User;  // Same seed — reuse User VM
                } else {
                    currentRxId = RxInstanceId::Dev;
                }
                vm = randomx().getVM(currentRxId, m_id);

                algo = zenrx::Algorithm(zenrx::Algorithm::fromRxAlgo(localJob.rxAlgo()));

                Log::debug("Worker %d: new job %s (rx=%d)", m_id, localJob.id().c_str(), static_cast<int>(currentRxId));
            }
        }

        // Wait for a valid job
        if (!localJob.isValid() || !vm) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (first) {
            // Pipeline priming: set up scratchpad for first hash, no output yet
            localJob.setNonce(nonce);
            randomx_calculate_hash_first(vm, tempHash, localJob.blob(), localJob.blobSize(), algo);
            prevNonce = nonce;
            nonce++;
            first = false;

            if (nonce >= m_nonceStart + 0x01000000) {
                nonce = m_nonceStart;
                Log::debug("Worker %d: nonce range exhausted, wrapping", m_id);
            }
            continue;
        }

        // Pipelined: produce hash for prevNonce while priming next nonce
        localJob.setNonce(nonce);
        randomx_calculate_hash_next(vm, tempHash, localJob.blob(), localJob.blobSize(), hash, algo);

        m_hashCount.fetch_add(1, std::memory_order_relaxed);

        // Check if hash meets target (hash corresponds to prevNonce)
        uint64_t hashValue;
        memcpy(&hashValue, hash + 24, sizeof(hashValue));

        if (__builtin_expect(hashValue < localJob.target(), 0)) {
            JobResult result;
            result.jobId = currentJobId;
            result.nonce = prevNonce;
            result.rxInstanceId = localJob.rxInstanceId();
            memcpy(result.hash.data(), hash, 32);
            result.actualDiff = hashValue > 0 ? 0xFFFFFFFFFFFFFFFFULL / hashValue : 0;

            if (result.rxInstanceId == 0) {
                Log::share(result.actualDiff);
            } else {
                Log::debug("Dev share found | diff: %lu", result.actualDiff);
            }

            if (m_onSubmit) {
                m_onSubmit(result);
            }
        }

        prevNonce = nonce;
        nonce++;

        // Wrap nonce if exhausted
        if (nonce >= m_nonceStart + 0x01000000) {
            nonce = m_nonceStart;
            Log::debug("Worker %d: nonce range exhausted, wrapping", m_id);
        }
    }
}

} // namespace zenrx
