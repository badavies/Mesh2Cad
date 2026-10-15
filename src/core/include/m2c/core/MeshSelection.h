// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace m2c {

/// A set of face indices of one mesh, stored as a bitset. Commands pass selections around by
/// document object id, never as index lists (ARCHITECTURE.md section 7).
class MeshSelection {
public:
    MeshSelection() = default;
    explicit MeshSelection(std::size_t faceCount)
        : words_((faceCount + 63) / 64, 0), size_(faceCount) {}

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool test(std::size_t face) const noexcept {
        return (words_[face >> 6] >> (face & 63)) & 1U;
    }
    void set(std::size_t face, bool on = true) noexcept {
        const std::uint64_t bit = std::uint64_t{1} << (face & 63);
        if (on) {
            words_[face >> 6] |= bit;
        } else {
            words_[face >> 6] &= ~bit;
        }
    }

    /// Number of selected faces.
    [[nodiscard]] std::size_t count() const noexcept;

    /// Selected face indices in increasing order.
    [[nodiscard]] std::vector<std::uint32_t> indices() const;

    void invert() noexcept;

    [[nodiscard]] const std::vector<std::uint64_t>& words() const noexcept { return words_; }

    /// Rebuilds a selection from saved words (project file). Bits past `faceCount` are cleared.
    [[nodiscard]] static MeshSelection fromWords(std::vector<std::uint64_t> words,
                                                 std::size_t faceCount);

    friend bool operator==(const MeshSelection&, const MeshSelection&) = default;

private:
    void clearTail() noexcept;

    std::vector<std::uint64_t> words_;
    std::size_t size_{0};
};

} // namespace m2c
