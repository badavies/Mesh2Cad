// SPDX-License-Identifier: GPL-3.0-or-later
#include "detail/Parse.h"

#include <bit>
#include <limits>
#include <unordered_map>

namespace m2c::io_detail {

std::optional<std::string_view> LineReader::next() {
    while (true) {
        const std::size_t nl = buffer_.find('\n', pos_);
        if (nl != std::string::npos) {
            std::string_view line(buffer_.data() + pos_, nl - pos_);
            pos_ = nl + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            ++line_;
            return line;
        }
        if (eof_) {
            if (pos_ >= buffer_.size()) return std::nullopt;
            std::string_view line(buffer_.data() + pos_, buffer_.size() - pos_);
            pos_ = buffer_.size();
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            ++line_;
            return line;
        }
        // Keep the unfinished line and append the next chunk.
        buffer_.erase(0, pos_);
        pos_ = 0;
        const std::size_t old = buffer_.size();
        buffer_.resize(old + chunk_);
        in_.read(buffer_.data() + old, static_cast<std::streamsize>(chunk_));
        const auto got = static_cast<std::size_t>(in_.gcount());
        buffer_.resize(old + got);
        if (got < chunk_) eof_ = true;
    }
}

namespace {

struct PositionKey {
    std::uint64_t x, y, z;
    friend bool operator==(const PositionKey&, const PositionKey&) = default;
};

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint64_t v : {k.x, k.y, k.z}) {
            h ^= v;
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h ^ (h >> 32));
    }
};

} // namespace

Result<TriMesh> weldAndCompact(const std::vector<Vec3d>& positions,
                               const std::vector<std::uint32_t>& indices,
                               std::string_view format) {
    if (indices.empty()) {
        return fail(ErrorCode::Parse, std::string(format) + " file contains no faces");
    }
    std::vector<bool> used(positions.size(), false);
    for (const std::uint32_t i : indices) {
        if (i >= positions.size()) {
            return fail(ErrorCode::Parse,
                        std::string(format) + " face references vertex " + std::to_string(i) +
                            " but the file has only " + std::to_string(positions.size()));
        }
        used[i] = true;
    }

    // Walk vertices in file order, so a clean file keeps its vertex numbering.
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> remap(positions.size(), kUnset);
    std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> lookup;
    lookup.reserve(safeReserve(positions.size()));
    std::vector<Vec3d> outPositions;
    outPositions.reserve(safeReserve(positions.size()));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (!used[i]) continue;
        const Vec3d& p = positions[i];
        // "+ 0.0" folds -0.0 into +0.0 so both weld together.
        const PositionKey key{std::bit_cast<std::uint64_t>(p.x + 0.0),
                              std::bit_cast<std::uint64_t>(p.y + 0.0),
                              std::bit_cast<std::uint64_t>(p.z + 0.0)};
        const auto [it, inserted] =
            lookup.try_emplace(key, static_cast<std::uint32_t>(outPositions.size()));
        if (inserted) outPositions.push_back(p);
        remap[i] = it->second;
    }

    std::vector<std::uint32_t> outIndices;
    outIndices.reserve(indices.size());
    for (const std::uint32_t i : indices) outIndices.push_back(remap[i]);
    return TriMesh::create(std::move(outPositions), std::move(outIndices));
}

} // namespace m2c::io_detail
