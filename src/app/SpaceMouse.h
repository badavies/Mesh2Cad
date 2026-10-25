// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

namespace m2c::app {

class Viewport;

/// Moves a viewport's camera with a 3Dconnexion SpaceMouse through the 3DxWare Navigation
/// Library (ADR 0039). Built only with MESH2CAD_SPACEMOUSE=ON on Windows; it stays idle when the
/// 3DxWare driver is not installed.
///
/// - The 3D mouse turns the view freely: it can roll it and turn it over the top (3DxWare's
///   Lock Horizon setting keeps it level). A standard view levels it again (ADR 0040).
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
