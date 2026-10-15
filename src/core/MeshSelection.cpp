// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/MeshSelection.h"

#include <bit>

namespace m2c {

std::size_t MeshSelection::count() const noexcept {
    std::size_t n = 0;
    for (const std::uint64_t w : words_) n += static_cast<std::size_t>(std::popcount(w));
    return n;
}

std::vector<std::uint32_t> MeshSelection::indices() const {
    std::vector<std::uint32_t> out;
    out.reserve(count());
    for (std::size_t w = 0; w < words_.size(); ++w) {
        std::uint64_t bits = words_[w];
        while (bits != 0) {
            const int b = std::countr_zero(bits);
            out.push_back(static_cast<std::uint32_t>(w * 64 + static_cast<std::size_t>(b)));
            bits &= bits - 1;
        }
    }
    return out;
}

void MeshSelection::invert() noexcept {
    for (std::uint64_t& w : words_) w = ~w;
    clearTail();
}

MeshSelection MeshSelection::fromWords(std::vector<std::uint64_t> words, std::size_t faceCount) {
    MeshSelection s;
    words.resize((faceCount + 63) / 64, 0);
    s.words_ = std::move(words);
    s.size_ = faceCount;
    s.clearTail();
    return s;
}

void MeshSelection::clearTail() noexcept {
    const std::size_t used = size_ & 63;
    if (used != 0 && !words_.empty()) words_.back() &= (std::uint64_t{1} << used) - 1;
}

} // namespace m2c
