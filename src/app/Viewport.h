// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPolygonF>
#include <QTimer>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QOpenGLFunctions_3_3_Core>

#include "Camera.h"
#include "Engine.h"

namespace m2c::app {

enum class Mode {
    Navigate,
    Brush,
    Lasso,
    Crease,
    Measure
};

/// A point under the cursor on a visible mesh.
struct Hit {
    ObjectId meshId{0};
    std::uint32_t face{0};
    Vec3d point;
};

/// OpenGL 3.3 view of the document snapshot (ARCHITECTURE.md section 9).
///
/// - Meshes are uploaded once per mesh version, as floats relative to their own bounding-box
///   centre so large coordinates keep their precision; the camera matrix is built in doubles.
/// - Flat shading comes from screen-space derivatives, so no normals are stored or uploaded.
/// - Selections are a per-face byte buffer read by the fragment shader (gl_PrimitiveID), so
///   changing a selection uploads one byte per face, not the mesh.
/// - Meshes over two million faces get a coarse proxy built in the background; it is drawn
///   while the camera moves.
/// - Picking for brushing and hovering uses the mesh's own BVH at full resolution.
///
/// The viewport never changes the document: tools emit signals that the main window turns into
/// commands, so everything a person does here is a command an agent could also send.
class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;
    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;

    void setSnapshot(std::shared_ptr<const DocSnapshot> snapshot);
    void setCurrent(ObjectId meshId, ObjectId selectionId);
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    void setBrushRadius(double mm);
    [[nodiscard]] double brushRadius() const noexcept { return brushRadius_; }
    void setCreaseAngle(double degrees) { creaseAngle_ = degrees; }
    [[nodiscard]] double creaseAngle() const noexcept { return creaseAngle_; }
    void setWireframe(bool on);
    void setShowBackFaces(bool on);

    void fitAll();
    void fitObject(ObjectId id);
    void setStandardView(double yaw, double pitch);

    /// View-projection matrix (column-major, world coordinates) and camera position, as the
    /// select.lasso command expects.
    [[nodiscard]] Mat4d viewProjection() const;
    [[nodiscard]] Vec3d eye() const { return camera_.eye(); }

    /// True once every drawn mesh that needs a coarse proxy (see above) has one on the GPU, so
    /// camera moves draw the proxy. Used by the frame-rate test.
    [[nodiscard]] bool proxiesReady() const;

signals:
    /// A finished brush stroke on one mesh: the brush centres in order.
    void
    brushStroke(ObjectId meshId, const std::vector<m2c::Vec3d>& centers, bool subtract, bool add);
    /// A finished lasso, in normalised device coordinates.
    void lassoDrawn(const QPolygonF& ndc, bool subtract, bool add);
    void faceClicked(m2c::app::Hit hit, bool subtract, bool add);
    void measured(m2c::app::Hit a, m2c::app::Hit b);
    void hoverChanged(const QString& text);
    void brushRadiusChanged(double mm);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct GpuMesh {
        const TriMesh* source{nullptr}; ///< Identity of the uploaded version.
        std::shared_ptr<const TriMesh> keepAlive;
        Vec3d offset;
        GLuint vao{0}, vbo{0}, ebo{0};
        GLsizei indexCount{0};
        // Selection overlay.
        std::vector<std::pair<const MeshSelection*, bool>> selectionKey; ///< (faces, is current)
        GLuint selBuffer{0}, selTexture{0};
        bool hasSelection{false};
        // Coarse proxy for fast camera moves on huge meshes.
        std::shared_ptr<const TriMesh> proxy;
        GLuint pvao{0}, pvbo{0}, pebo{0};
        GLsizei proxyIndexCount{0};
        bool proxyRequested{false};
    };

    void cleanupGl();
    void syncGpu();
    void upload(
        GpuMesh& gpu, const TriMesh& mesh, GLuint& vao, GLuint& vbo, GLuint& ebo, GLsizei& count);
    void syncSelection(GpuMesh& gpu, const DocumentObject& mesh);
    void release(GpuMesh& gpu);
    void drawOverlay();

    [[nodiscard]] std::optional<Hit> pick(const QPointF& pos) const;
    [[nodiscard]] QPointF ndc(const QPointF& pos) const;
    [[nodiscard]] std::optional<QPointF> project(const Vec3d& p) const;
    [[nodiscard]] double pixelsPerMm(const Vec3d& at) const;
    [[nodiscard]] double aspect() const;
    [[nodiscard]] Aabb visibleBounds(std::optional<ObjectId> only = std::nullopt) const;
    void markInteracting();

    std::shared_ptr<const DocSnapshot> snapshot_;
    std::map<ObjectId, GpuMesh> gpu_;
    QOpenGLShaderProgram program_;
    bool glReady_{false};
    GLint maxTextureBuffer_{0};

    Camera camera_;
    bool fitPending_{true};
    Mode mode_{Mode::Navigate};
    ObjectId currentMesh_{0};
    ObjectId currentSelection_{0};
    double brushRadius_{2.0};
    double creaseAngle_{20.0};
    bool wireframe_{false};
    bool showBackFaces_{true};

    // Mouse state.
    enum class Drag {
        None,
        Orbit,
        Pan,
        Brush,
        Lasso
    };
    Drag drag_{Drag::None};
    QPointF lastPos_;
    QPointF pressPos_;
    Qt::KeyboardModifiers pressMods_;
    std::optional<Hit> hover_;
    std::vector<Vec3d> stroke_;
    ObjectId strokeMesh_{0};
    QPolygonF lasso_;
    std::optional<Hit> measureStart_;
    bool interacting_{false};
    QTimer interactionTimer_;
};

} // namespace m2c::app
