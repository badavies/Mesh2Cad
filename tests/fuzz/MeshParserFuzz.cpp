// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target for the STL, OBJ and PLY readers (docs/ARCHITECTURE.md section 10).
//
// LLVMFuzzerTestOneInput is the libFuzzer entry point. FuzzDriver.cpp supplies a main() for
// compilers without libFuzzer (GCC, MSVC) that mutates a seed corpus with a fixed seed.
// A reader may reject any input with an error; it must never crash, throw or hang, and a mesh it
// accepts must pass the import gate (valid indices, finite coordinates).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "m2c/io/Obj.h"
#include "m2c/io/Ply.h"
#include "m2c/io/Stl.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

void checkAccepted(const m2c::Result<m2c::TriMesh>& mesh) {
    if (!mesh) {
        return;
    }
    if (mesh->faceCount() == 0 || mesh->vertexCount() == 0) {
        std::abort();
    }
    for (const std::uint32_t i : mesh->indices()) {
        if (i >= mesh->vertexCount()) {
            std::abort();
        }
    }
    for (const auto& p : mesh->positions()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            std::abort();
        }
    }
}

template <class Reader>
void readWith(Reader read, const std::string& bytes) {
    std::istringstream in(bytes, std::ios::in | std::ios::binary);
    checkAccepted(read(in));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    readWith(m2c::readStl, bytes);
    readWith(m2c::readObj, bytes);
    readWith(m2c::readPly, bytes);
    return 0;
}
