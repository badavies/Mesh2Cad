// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-alone fuzz driver for compilers without libFuzzer.
//
//   m2c_fuzz_mesh_parsers [--iterations N] [--seed S] [--save-last FILE] [corpus files...]
//
// Runs every seed input unchanged, then N mutated inputs. Mutations come from a fixed-seed
// std::mt19937_64 (its output sequence is defined by the standard), so a run is reproducible on
// every platform: rerun with the same --seed and --save-last to capture the input that crashed.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "m2c/io/Obj.h"
#include "m2c/io/Ply.h"
#include "m2c/io/Stl.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

using Bytes = std::string;
using Writer = m2c::Result<void> (*)(const m2c::TriMesh&, std::ostream&);

Bytes written(const m2c::TriMesh& mesh, Writer write) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    if (!write(mesh, out)) {
        std::cerr << "fuzz: could not write a seed mesh\n";
        std::exit(2);
    }
    return out.str();
}

std::vector<Bytes> builtinSeeds() {
    auto tetra = m2c::TriMesh::create({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                      {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3});
    if (!tetra) {
        std::cerr << "fuzz: could not build the seed mesh\n";
        std::exit(2);
    }
    return {
        written(*tetra, m2c::writeStl),
        written(*tetra, m2c::writeObj),
        written(*tetra, m2c::writePly),
        "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
        "endloop\nendfacet\nendsolid t\n",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
        "f 1/1/1 2/1/1 3/1/1 4/1/1\nf -4 -3 -2\n",
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
        "property float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n"
        "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n",
    };
}

// Number strings that tend to break header and index handling.
constexpr std::string_view kInteresting[] = {"0",
                                             "-1",
                                             "4294967295",
                                             "4294967296",
                                             "2147483648",
                                             "1e308",
                                             "nan",
                                             "inf",
                                             "-0",
                                             "999999999999"};

class Mutator {
public:
    explicit Mutator(std::uint64_t seed) : rng_(seed) {}

    Bytes mutate(Bytes data) {
        const int count = 1 + static_cast<int>(below(4));
        for (int i = 0; i < count; ++i) {
            mutateOnce(data);
        }
        return data;
    }

private:
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : rng_() % n; }

    void mutateOnce(Bytes& data) {
        const std::size_t size = data.size();
        switch (below(7)) {
        case 0: // flip one bit
            if (size > 0) {
                data[below(size)] ^= static_cast<char>(1u << below(8));
            }
            break;
        case 1: // overwrite one byte
            if (size > 0) {
                data[below(size)] = static_cast<char>(below(256));
            }
            break;
        case 2: // insert random bytes
            data.insert(below(size + 1), randomBytes(1 + below(8)));
            break;
        case 3: // erase a range
            if (size > 0) {
                const std::size_t at = below(size);
                data.erase(at, 1 + below(size - at));
            }
            break;
        case 4: // duplicate a range
            if (size > 0) {
                const std::size_t at = below(size);
                const std::size_t length = 1 + below(std::min<std::size_t>(64, size - at));
                data.insert(below(size + 1), data.substr(at, length));
            }
            break;
        case 5: // truncate
            data.resize(below(size + 1));
            break;
        default: // replace a run of digits with an interesting number
            replaceNumber(data);
            break;
        }
    }

    Bytes randomBytes(std::uint64_t n) {
        Bytes out;
        for (std::uint64_t i = 0; i < n; ++i) {
            out.push_back(static_cast<char>(below(256)));
        }
        return out;
    }

    void replaceNumber(Bytes& data) {
        const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        const std::string_view number = kInteresting[below(std::size(kInteresting))];
        const std::size_t start = data.empty() ? 0 : below(data.size());
        std::size_t at = start;
        while (at < data.size() && !isDigit(data[at])) {
            ++at;
        }
        if (at == data.size()) {
            data.insert(start, number);
            return;
        }
        std::size_t end = at;
        while (end < data.size() && isDigit(data[end])) {
            ++end;
        }
        data.replace(at, end - at, number);
    }

    std::mt19937_64 rng_;
};

void runOne(const Bytes& input, const std::string& saveLast) {
    if (!saveLast.empty()) {
        std::ofstream(saveLast, std::ios::binary | std::ios::trunc) << input;
    }
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 10000;
    std::uint64_t seed = 1;
    std::string saveLast;
    std::vector<Bytes> corpus = builtinSeeds();

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--iterations" || arg == "--seed" || arg == "--save-last") && i + 1 < argc) {
            const char* value = argv[++i];
            if (arg == "--iterations") {
                iterations = std::strtoull(value, nullptr, 10);
            } else if (arg == "--seed") {
                seed = std::strtoull(value, nullptr, 10);
            } else {
                saveLast = value;
            }
        } else if (arg.starts_with("--")) {
            std::cerr << "usage: " << argv[0]
                      << " [--iterations N] [--seed S] [--save-last FILE] [corpus files...]\n";
            return 2;
        } else {
            std::ifstream in(argv[i], std::ios::binary);
            if (!in) {
                std::cerr << "fuzz: cannot read " << arg << "\n";
                return 2;
            }
            corpus.emplace_back(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
        }
    }

    for (const Bytes& input : corpus) {
        runOne(input, saveLast);
    }
    Mutator mutator(seed);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        runOne(mutator.mutate(corpus[i % corpus.size()]), saveLast);
    }
    std::cout << "fuzz: " << corpus.size() << " seeds and " << iterations
              << " mutated inputs, seed " << seed << ", no failures\n";
    return 0;
}
