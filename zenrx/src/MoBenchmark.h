#pragma once

#include "Config.h"
#include <map>
#include <string>

namespace zenrx {

class MoBenchmark {
public:
    // Returns true if any benchmark algo is missing from config.algoPerf
    static bool isNeeded(const Config& config);

    // Run all benchmarks, returns complete algo-perf map
    static std::map<std::string, double> runAll(Config& config);

private:
    // Benchmark a single algorithm, returns H/s
    static double benchmarkAlgo(RxAlgo algo, int threads, int durationSec, bool hugePages);
};

} // namespace zenrx
