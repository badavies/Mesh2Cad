// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

namespace m2c::app {

class Viewport;

/// Moves a viewport's camera with a 3Dconnexion SpaceMouse through the 3DxWare Navigation
/// Library (ADR 0039). Built only with MESH2CAD_SPACEMOUSE=ON on Windows; it stays idle when the
/// 3DxWare driver is not installed.
///
/// - The camera keeps +Z up: the 3D mouse's roll is not followed, as if 3DxWare's Lock Horizon
///   were on, and the view stops short of straight up or down.
/// - The 3D mouse turns the view around the point it hits on the visible meshes, solids and
///   primitives at the centre of the view, drawn as a blue dot while it moves.
/// - 3DxWare's own view commands (Fit, Front, Top, Right and the rest) work; the app adds none.
class SpaceMouse {
public:
    explicit SpaceMouse(Viewport& viewport);
    ~SpaceMouse();
    SpaceMouse(const SpaceMouse&) = delete;
    SpaceMouse& operator=(const SpaceMouse&) = delete;

    /// Whether the connection to the 3DxWare driver opened.
    [[nodiscard]] bool connected() const noexcept;

private:
    class Navigation;
    std::unique_ptr<Navigation> navigation_;
};

} // namespace m2c::app
