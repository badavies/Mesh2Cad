// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpaceMouse.h"

#include <QCursor>
#include <QPointF>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <system_error>

// The 3DxWare SDK. It is not in the repository: its licence keeps the sources confidential.
#include <SpaceMouse/CNavigation3D.hpp>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Camera.h"
#include "Viewport.h"

namespace m2c::app {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

long noData() {
    return navlib::make_result_code(navlib::navlib_errc::no_data_available);
}

long notSupported() {
    return navlib::make_result_code(navlib::navlib_errc::function_not_supported);
}

long perspectiveOnly() {
    return navlib::make_result_code(navlib::navlib_errc::invalid_operation);
}

// Mat4d and navlib::matrix_t store the camera-to-world transform the same way: the right, up
// and back axes, then the eye, each as four consecutive values.
navlib::matrix_t toNavlib(const Mat4d& m) {
    navlib::matrix_t r{};
    for (std::size_t i = 0; i < 16; ++i) r[i] = m.m[i];
    return r;
}

Mat4d fromNavlib(const navlib::matrix_t& m) {
    Mat4d r;
    for (std::size_t i = 0; i < 16; ++i) r.m[i] = m[i];
    return r;
}

navlib::point_t toNavlib(const Vec3d& p) {
    return {p.x, p.y, p.z};
}

Vec3d fromNavlib(const navlib::point_t& p) {
    return {.x = p.x, .y = p.y, .z = p.z};
}

} // namespace

/// The Navigation Library's accessors on the viewport. Single-threaded: the library calls them
/// on the UI thread from the Windows message loop.
class SpaceMouse::Navigation final : public TDx::SpaceMouse::Navigation3D::CNavigation3D {
public:
    explicit Navigation(Viewport& viewport) : CNavigation3D(false, false), viewport_(viewport) {
        PutProfileHint("Mesh2Cad"); // the name in 3Dconnexion Settings
        std::error_code ec;
        EnableNavigation(true, ec);
        connected_ = !ec;
    }

    ~Navigation() override {
        std::error_code ec;
        EnableNavigation(false, ec);
    }

    Navigation(const Navigation&) = delete;
    Navigation& operator=(const Navigation&) = delete;

    [[nodiscard]] bool connected() const noexcept { return connected_; }

protected:
    // ISpace3D: X right, Y into the screen, Z up, so Lock Horizon keeps Z up.
    long GetCoordinateSystem(navlib::matrix_t& matrix) const override {
        matrix = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
        return 0;
    }

    // View > Front: looking along +Y.
    long GetFrontView(navlib::matrix_t& matrix) const override {
        Camera front;
        front.setView(-90, 0);
        Mat4d m = front.cameraToWorld();
        m.at(0, 3) = m.at(1, 3) = m.at(2, 3) = 0.0;
        matrix = toNavlib(m);
        return 0;
    }

    // IView. The camera is always perspective.
    long GetCameraMatrix(navlib::matrix_t& matrix) const override {
        matrix = toNavlib(viewport_.camera().cameraToWorld());
        return 0;
    }

    long SetCameraMatrix(const navlib::matrix_t& matrix) override {
        Camera camera = viewport_.camera();
        camera.setCameraToWorld(fromNavlib(matrix), pivot_);
        viewport_.setCamera(camera);
        return 0;
    }

    // The cursor on the near plane.
    long GetPointerPosition(navlib::point_t& position) const override {
        const QPointF pos(viewport_.mapFromGlobal(QCursor::pos()));
        if (!viewport_.rect().contains(pos.toPoint())) return noData();
        const Camera& camera = viewport_.camera();
        Vec3d origin;
        Vec3d direction;
        camera.ray(2.0 * pos.x() / std::max(1, viewport_.width()) - 1.0,
                   1.0 - 2.0 * pos.y() / std::max(1, viewport_.height()),
                   viewport_.aspect(),
                   origin,
                   direction);
        position =
            toNavlib(origin + direction * (camera.nearPlane() / dot(direction, camera.forward())));
        return 0;
    }

    long GetViewFOV(double& fov) const override {
        fov = viewport_.camera().fovYDeg * kDeg;
        return 0;
    }

    long SetViewFOV(const double fov) override {
        Camera camera = viewport_.camera();
        camera.fovYDeg = std::clamp(fov / kDeg, 5.0, 120.0);
        viewport_.setCamera(camera);
        return 0;
    }

    long GetViewFrustum(navlib::frustum_t& frustum) const override {
        const Camera& camera = viewport_.camera();
        const double n = camera.nearPlane();
        const double h = n * std::tan(camera.fovYDeg * kDeg / 2.0);
        const double w = h * viewport_.aspect();
        frustum = {-w, w, -h, h, n, camera.farPlane()};
        return 0;
    }

    long SetViewFrustum(const navlib::frustum_t&) override { return notSupported(); }

    long GetIsViewPerspective(navlib::bool_t& perspective) const override {
        perspective = 1;
        return 0;
    }

    long GetViewExtents(navlib::box_t&) const override { return perspectiveOnly(); }
    long SetViewExtents(const navlib::box_t&) override { return perspectiveOnly(); }

    // IModel. Millimetres; the 3D mouse moves the view, never the selection.
    long GetModelExtents(navlib::box_t& extents) const override {
        const Aabb box = viewport_.visibleBounds();
        if (box.min.x > box.max.x) return noData();
        extents = {toNavlib(box.min), toNavlib(box.max)};
        return 0;
    }

    long GetUnitsToMeters(double& meters) const override {
        meters = 0.001;
        return 0;
    }

    long GetIsSelectionEmpty(navlib::bool_t& empty) const override {
        empty = 1;
        return 0;
    }

    long GetSelectionExtents(navlib::box_t&) const override { return noData(); }
    long GetSelectionTransform(navlib::matrix_t&) const override { return noData(); }
    long SetSelectionTransform(const navlib::matrix_t&) override { return notSupported(); }

    // IPivot. The library places the pivot; until it does, the camera target is the pivot. The
    // camera's target follows it (setCameraToWorld), so a mouse orbit afterwards turns around it.
    long GetPivotPosition(navlib::point_t& position) const override {
        position = toNavlib(pivot_.value_or(viewport_.camera().target));
        return 0;
    }

    long IsUserPivot(navlib::bool_t& userPivot) const override {
        userPivot = 0;
        return 0;
    }

    long SetPivotPosition(const navlib::point_t& position) override {
        pivot_ = fromNavlib(position);
        if (pivotVisible_) viewport_.setNavigationPivot(pivot_);
        return 0;
    }

    long GetPivotVisible(navlib::bool_t& visible) const override {
        visible = pivotVisible_ ? 1 : 0;
        return 0;
    }

    long SetPivotVisible(const bool visible) override {
        pivotVisible_ = visible;
        viewport_.setNavigationPivot(visible ? pivot_ : std::nullopt);
        return 0;
    }

    // IHit: one ray, so the aperture is not used.
    long SetHitAperture(double) override { return 0; }

    long SetHitDirection(const navlib::vector_t& direction) override {
        hitDirection_ = {.x = direction.x, .y = direction.y, .z = direction.z};
        return 0;
    }

    long SetHitLookFrom(const navlib::point_t& eye) override {
        hitFrom_ = fromNavlib(eye);
        return 0;
    }

    long SetHitSelectionOnly(const bool onlySelection) override {
        hitSelectionOnly_ = onlySelection;
        return 0;
    }

    long GetHitLookAt(navlib::point_t& position) const override {
        if (hitSelectionOnly_ || length(hitDirection_) == 0.0) return noData();
        const auto hit = viewport_.pickRay(hitFrom_, normalized(hitDirection_));
        if (!hit) return noData();
        position = toNavlib(hit->point);
        return 0;
    }

    // IState. A frame's setters each call update(), which Qt draws once, so frames need no
    // batching; huge meshes use their coarse proxy until the motion ends.
    long SetTransaction(long) override { return 0; }

    long SetMotionFlag(const bool motion) override {
        viewport_.setNavigating(motion);
        return 0;
    }

    // IEvents: the app adds no commands to the 3D mouse's buttons.
    long SetActiveCommand(std::string) override { return notSupported(); }

private:
    Viewport& viewport_;
    bool connected_{false};
    std::optional<Vec3d> pivot_;
    bool pivotVisible_{false};
    Vec3d hitFrom_;
    Vec3d hitDirection_;
    bool hitSelectionOnly_{false};
};

SpaceMouse::SpaceMouse(Viewport& viewport) : navigation_(std::make_unique<Navigation>(viewport)) {}

SpaceMouse::~SpaceMouse() = default;

bool SpaceMouse::connected() const noexcept {
    return navigation_->connected();
}

} // namespace m2c::app
