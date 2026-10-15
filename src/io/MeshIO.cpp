// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/MeshIO.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#include "m2c/io/Obj.h"
#include "m2c/io/Ply.h"
#include "m2c/io/Stl.h"

namespace m2c {

namespace {

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

} // namespace

Result<TriMesh> readMesh(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext != ".stl" && ext != ".obj" && ext != ".ply") {
        return fail(ErrorCode::Unsupported, "unsupported mesh file type '" + ext + "'");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for reading");
    if (ext == ".stl") return readStl(file);
    if (ext == ".obj") return readObj(file);
    return readPly(file);
}

Result<void> writeMesh(const TriMesh& mesh, const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext != ".stl" && ext != ".obj" && ext != ".ply") {
        return fail(ErrorCode::Unsupported, "writing '" + ext + "' files is not supported");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for writing");
    auto written = ext == ".stl"   ? writeStl(mesh, file)
                   : ext == ".obj" ? writeObj(mesh, file)
                                   : writePly(mesh, file);
    if (!written) return written;
    file.close();
    if (!file) return fail(ErrorCode::Io, "failed to finish writing '" + path.string() + "'");
    return {};
}

} // namespace m2c
