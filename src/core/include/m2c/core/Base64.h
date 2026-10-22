// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Base64 (RFC 4648, with padding), for pictures returned inside JSON results.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace m2c {

[[nodiscard]] inline std::string encodeBase64(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        const auto at = [&](std::size_t k) { return k < left ? unsigned{bytes[i + k]} : 0u; };
        const unsigned triple = at(0) << 16 | at(1) << 8 | at(2);
        out += kDigits[triple >> 18 & 63];
        out += kDigits[triple >> 12 & 63];
        out += left > 1 ? kDigits[triple >> 6 & 63] : '=';
        out += left > 2 ? kDigits[triple & 63] : '=';
    }
    return out;
}

} // namespace m2c
