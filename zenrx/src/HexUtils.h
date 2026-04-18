#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace zenrx {

inline uint8_t hexCharToNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

inline size_t hexToBytes(const std::string& hex, uint8_t* out, size_t maxLen)
{
    size_t len = hex.length() / 2;
    if (len > maxLen) len = maxLen;

    for (size_t i = 0; i < len; i++) {
        out[i] = (hexCharToNibble(hex[i*2]) << 4) | hexCharToNibble(hex[i*2 + 1]);
    }

    return len;
}

} // namespace zenrx
