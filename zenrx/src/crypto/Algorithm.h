#pragma once

#include <cstdint>
#include "Config.h"

namespace zenrx {

class Algorithm
{
public:
    enum Id : uint32_t {
        INVALID  = 0,
        RX_0     = 0x72151200,
        RX_ARQ   = 0x72121061,
        RX_XEQ   = 0x72121000,
        RX_GRAFT = 0x72151267,
        RX_WOW   = 0x72141177,
        RX_SFX   = 0x72151273,
        RX_XLA   = 0x721211ff,
    };

    Algorithm() = default;
    Algorithm(Id id) : m_id(id) {}
    constexpr operator Id() const { return m_id; }

    // Extract L3 scratchpad size from algorithm ID encoding (bytes)
    static constexpr size_t l3(Id id) { return 1ULL << ((id >> 16) & 0xff); }
    // Extract L2 scratchpad size from algorithm ID encoding (bytes)
    static constexpr size_t l2(Id id) { return 1U << ((id >> 8) & 0xff); }

    size_t l3() const { return l3(m_id); }
    size_t l2() const { return l2(m_id); }

    // Convert RxAlgo enum to Algorithm::Id
    static Id fromRxAlgo(RxAlgo algo) {
        switch (algo) {
            case RxAlgo::RX_0:     return RX_0;
            case RxAlgo::RX_WOW:   return RX_WOW;
            case RxAlgo::RX_ARQ:   return RX_ARQ;
            case RxAlgo::RX_XEQ:   return RX_XEQ;
            case RxAlgo::RX_GRAFT: return RX_GRAFT;
            case RxAlgo::RX_SFX:   return RX_SFX;
            case RxAlgo::RX_XLA:   return RX_XLA;
        }
        return RX_0;
    }

private:
    Id m_id = RX_0;
};

} // namespace zenrx
