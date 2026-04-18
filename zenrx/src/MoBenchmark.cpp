#include "MoBenchmark.h"
#include "Log.h"
#include "Worker.h"
#include "crypto/RandomX.h"
#include "crypto/randomx/randomx.h"
#include "crypto/Algorithm.h"
#include "Job.h"

#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>

namespace zenrx {

// Algorithms to actually benchmark
static const RxAlgo BENCH_ALGOS[] = {
    RxAlgo::RX_0,
    RxAlgo::RX_WOW,
    RxAlgo::RX_GRAFT,
    RxAlgo::RX_ARQ,
    RxAlgo::RX_XLA,
};
static constexpr size_t BENCH_ALGO_COUNT = sizeof(BENCH_ALGOS) / sizeof(BENCH_ALGOS[0]);

// Standard benchmark seed (all zeros + 1)
static const char* BENCH_SEED = "0000000000000000000000000000000000000000000000000000000000000001";

// Standard benchmark blob
static const char* BENCH_BLOB = "0707f7a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2a4f0d4d6f2";


bool MoBenchmark::isNeeded(const Config& config)
{
    for (size_t i = 0; i < BENCH_ALGO_COUNT; i++) {
        const char* name = rxAlgoName(BENCH_ALGOS[i]);
        if (config.algoPerf.find(name) == config.algoPerf.end()) {
            return true;
        }
    }
    // Also check derived algos
    if (config.algoPerf.find("rx/sfx") == config.algoPerf.end()) return true;
    if (config.algoPerf.find("rx/xeq") == config.algoPerf.end()) return true;
    return false;
}

double MoBenchmark::benchmarkAlgo(RxAlgo algo, int threads, int durationSec, bool hugePages)
{
    Log::info("  Benchmarking %s ...", rxAlgoName(algo));

    // Initialize a dedicated RandomX instance for benchmarking
    if (!randomx().init(RxInstanceId::User, BENCH_SEED, threads, algo, hugePages, true)) {
        Log::error("  Failed to init RandomX for %s", rxAlgoName(algo));
        return 0.0;
    }

    // Create a synthetic job blob (76 bytes of zeros is fine for benchmarking)
    alignas(64) uint8_t blob[76];
    memset(blob, 0, sizeof(blob));
    alignas(64) uint8_t hash[32];

    zenrx::Algorithm xmAlgo(zenrx::Algorithm::fromRxAlgo(algo));

    // Warmup: 2 seconds
    auto warmupEnd = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    std::atomic<uint64_t> totalHashes{0};
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;

    auto workerFunc = [&](int id) {
        randomx_vm* vm = randomx().getVM(RxInstanceId::User, id);
        if (!vm) return;

        alignas(64) uint8_t localBlob[76];
        memcpy(localBlob, blob, sizeof(blob));
        alignas(64) uint8_t localHash[32];
        alignas(16) uint64_t tempHash[8];

        uint32_t nonce = static_cast<uint32_t>(id) * 0x01000000;

        // Prime the pipeline
        memcpy(localBlob + 39, &nonce, sizeof(nonce));
        randomx_calculate_hash_first(vm, tempHash, localBlob, sizeof(localBlob), xmAlgo);
        nonce++;

        while (running.load(std::memory_order_relaxed)) {
            memcpy(localBlob + 39, &nonce, sizeof(nonce));
            randomx_calculate_hash_next(vm, tempHash, localBlob, sizeof(localBlob), localHash, xmAlgo);
            totalHashes.fetch_add(1, std::memory_order_relaxed);
            nonce++;
        }
    };

    // Warmup phase
    running = true;
    totalHashes = 0;
    for (int i = 0; i < threads; i++) {
        workers.emplace_back(workerFunc, i);
    }

    // Wait for warmup
    while (std::chrono::steady_clock::now() < warmupEnd) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Reset counters and measure
    totalHashes = 0;
    auto measureStart = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::seconds(durationSec));

    auto measureEnd = std::chrono::steady_clock::now();
    running = false;

    for (auto& t : workers) {
        t.join();
    }

    double elapsed = std::chrono::duration<double>(measureEnd - measureStart).count();
    uint64_t hashes = totalHashes.load();
    double hashrate = (elapsed > 0) ? static_cast<double>(hashes) / elapsed : 0.0;

    Log::info("  %s: %.1f H/s", rxAlgoName(algo), hashrate);

    // Release the instance so we can reinit with a different algo
    randomx().release(RxInstanceId::User);

    return hashrate;
}

std::map<std::string, double> MoBenchmark::runAll(Config& config)
{
    std::map<std::string, double> result = config.algoPerf;

    for (size_t i = 0; i < BENCH_ALGO_COUNT; i++) {
        const char* name = rxAlgoName(BENCH_ALGOS[i]);

        // Skip if already benchmarked
        if (result.find(name) != result.end() && result[name] > 0) {
            Log::info("  %s: %.1f H/s (cached)", name, result[name]);
            continue;
        }

        config.autotuneAlgo = name;

        int algoThreads = config.threadsForAlgo(BENCH_ALGOS[i]);
        double hr = benchmarkAlgo(BENCH_ALGOS[i], algoThreads, config.benchAlgoTime, config.hugePagesEnabled);
        if (hr > 0) {
            result[name] = hr;
        }
    }

    config.autotuneAlgo = nullptr;

    // Fill derived algorithms:
    // rx/sfx uses same config as rx/0, so same performance
    if (result.find("rx/0") != result.end() && result.find("rx/sfx") == result.end()) {
        result["rx/sfx"] = result["rx/0"];
    }
    // rx/xeq uses same config as rx/arq, so same performance
    if (result.find("rx/arq") != result.end() && result.find("rx/xeq") == result.end()) {
        result["rx/xeq"] = result["rx/arq"];
    }

    return result;
}

} // namespace zenrx
