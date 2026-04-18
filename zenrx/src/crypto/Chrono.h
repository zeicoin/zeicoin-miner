#pragma once

#include <chrono>

namespace zenrx {

class Chrono {
public:
    static double highResolutionMSecs() {
        using namespace std::chrono;
        auto now = high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        return duration_cast<microseconds>(duration).count() / 1000.0;
    }
};

} // namespace zenrx
