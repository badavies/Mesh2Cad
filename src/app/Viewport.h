// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QOpenGLFunctions_3_3_Core>
#include <QVector3D>

#include "m2c/core/DeviationField.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Vec3.h"

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

/// A point under the cursor on a visible object: a mesh, or (pickObject) a solid or primitive.
struct Hit {
    ObjectId meshId{0};    ///< The mesh hit; 0 when it is a solid or primitive.
    std::uint32_t face{0}; ///< Of the mesh, or of the solid's or primitive's display mesh.
    Vec3d point;
    ObjectId objectId{0}; ///< The object hit, mesh or not.
};

/// A marker at a mesh's most outside or most inside vertex under its shown deviation.
struct DeviationMarker {
    ObjectId meshId{0};
    std::uint32_t vertex{0};
    Vec3d point;
    double valueMm{0.0};
    bool largest{false}; ///< The largest (most positive) distance, else the smallest.
};

/// What the viewport draws for a sketch: its curves as segments, each coloured by kind (lines,
/// arcs and splines), an arrow along the plane normal from the plane origin (the side a positive
/// solid.extrude goes), and a dot at both ends of every open chain (where a curve does not start
/// within 1e-6 mm of where the one before it ends, as solid.extrude requires).
struct SketchScene {
    std::vector<Vec3d> points;
    std::vector<std::uint32_t> segments; ///< Pairs of point indices.
    std::vector<QVector3D> colors;       ///< One per point.
    Vec3d arrowTail;                     ///< The plane origin.
    Vec3d arrowTip;              ///< 0.3 times the curves' bounding diagonal along the normal.
    std::vector<Vec3d> openEnds; ///< Two per open chain.
};

[[nodiscard]] SketchScene buildSketchScene(const Sketch& sketch);

/// The colour a sketch draws a curve of this kind in.
[[nodiscard]] QVector3D sketchCurveColor(CurveKind kind) noexcept;

/// OpenGL 3.3 view of the document snapshot (ARCHITECTURE.md section 9).
///
/// - Meshes are uploaded once per mesh version, as floats relative to their own bounding-box
///   centre so large coordinates keep their precision; the camera matrix is built in doubles.
/// - Flat shading comes from screen-space derivatives, so no normals are stored or uploaded.
/// - Selections are a per-face byte buffer read by the fragment shader (gl_PrimitiveID), so
///   changing a selection uploads one byte per face, not the mesh. The current selection is
///   orange, every other visible one its own colour (SelectionColors.h), as view.render draws it.
/// - Meshes over two million faces get a coarse proxy built in the background; it is drawn
///   while the camera moves.
/// - Picking for brushing and hovering uses the mesh's own BVH at full resolution.
/// - The hover text gives the shown deviation at the point, and markers show its extremes.
/// - Solids draw shaded from their display mesh, quad layouts as their edges and sketches as
///   their curves (SketchScene), over everything else. A visible deviation colours its mesh per
///   vertex, with a legend (DeviationColors.h).
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
    /// The current mesh and selection, and an object to draw highlighted (a primitive, solid,
    /// quad layout or sketch; a mesh shows as current instead).
    void setCurrent(ObjectId meshId, ObjectId selectionId, ObjectId highlightId = 0);
    void setMode(Mode mode);
    void setBrushRadius(double mm);
    [[nodiscard]] double brushRadius() const noexcept { return brushRadius_; }
    void setCreaseAngle(double degrees) { creaseAngle_ = degrees; }
    [[nodiscard]] double creaseAngle() const noexcept { return creaseAngle_; }
    /// How far from a point, in mm, the measure tool snaps to a vertex or edge: a few pixels.
    [[nodiscard]] double snapMm(const Vec3d& at) const;
    void setWireframe(bool on);
    void setShowBackFaces(bool on);
    void setColorBlindPalette(bool on);

    void fitAll();
    void fitObject(ObjectId id);
    void setStandardView(double yaw, double pitch);

    /// View-projection matrix (column-major, world coordinates) and camera position, as the
    /// select.lasso command expects.
    [[nodiscard]] Mat4d viewProjection() const;
    [[nodiscard]] Vec3d eye() const { return camera_.eye(); }

    /// The nearest visible mesh, solid or primitive under a widget position. The selection tools
    /// pick meshes only.
    [[nodiscard]] std::optional<Hit> pickObject(const QPointF& pos) const;

    /// The markers at the extremes of each visible mesh's shown deviation (the newest visible
    /// one, which colours it): its largest and smallest distance, each at the first vertex with
    /// it. Drawn over everything, so a worst point on the back or in a hole still shows.
    [[nodiscard]] std::vector<DeviationMarker> deviationMarkers() const;

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
    /// A click (no drag) in Navigate mode on a mesh, solid or primitive.
    void objectClicked(m2c::app::Hit hit);
    void measured(m2c::app::Hit a, m2c::app::Hit b);
    void hoverChanged(const QString& text);
    void brushRadiusChanged(double mm);
    /// The graphics driver gives no OpenGL 3.3 core context, so the viewport stays blank.
    void glUnavailable(const QString& reason);

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
    /// Selections a mesh's overlay was built from.
    struct SelectionKey {
        std::shared_ptr<const MeshSelection>
            faces; ///< Held, so a new one cannot reuse its address.
        ObjectId id{0};
        bool current{false};
        bool operator==(const SelectionKey&) const = default;
    };
    /// Selection colours the shader holds per mesh (selectionColors in Viewport.cpp).
    static constexpr std::size_t kSelectionSlots = 128;

    struct GpuMesh {
        const void* source{nullptr}; ///< Identity of the uploaded version.
        std::shared_ptr<const void> keepAlive;
        bool lines{false}; ///< A quad layout's edges rather than triangles.
        Vec3d offset;
        GLuint vao{0}, vbo{0}, ebo{0};
        GLsizei indexCount{0};
        // Selection overlay.
        std::vector<SelectionKey> selectionKey;
        std::vector<QVector3D> selectionColors; ///< Of values 2 onwards in the buffer.
        GLuint selBuffer{0}, selTexture{0};
        bool hasSelection{false};
        // Deviation colours, one per vertex.
        std::optional<DeviationField> colorField; ///< Its colours' field, held while shown.
        bool colorBlind{false};                   ///< Whether they use the colour-blind palette.
        GLuint colorBuffer{0};
        bool hasColors{false};
        std::vector<DeviationMarker> markers;
        // Coarse proxy for fast camera moves on huge meshes.
        std::shared_ptr<const TriMesh> proxy;
        GLuint pvao{0}, pvbo{0}, pebo{0};
        GLsizei proxyIndexCount{0};
        bool proxyRequested{false};
        // A sketch's open chain ends, drawn as dots over the view.
        std::vector<Vec3d> openEnds;
    };

    void cleanupGl();
    void syncGpu();
    void upload(const GpuMesh& gpu,
                const std::vector<Vec3d>& positions,
                const std::vector<std::uint32_t>& indices,
                GLuint& vao,
                GLuint& vbo,
                GLuint& ebo,
                GLsizei& count);
    void syncSelection(GpuMesh& gpu, const DocumentObject& mesh);
    void syncColors(GpuMesh& gpu, const DocumentObject& mesh);
    /// The hover text for a hit: the object, face and point, and the deviation there when the
    /// mesh shows one.
    [[nodiscard]] QString hoverText(const Hit& hit) const;
    /// The deviation whose legend is shown: on the current mesh if it has one, else the first.
    [[nodiscard]] const DocumentObject* legendDeviation() const;
    void drawLegend(QPainter& painter, const DocumentObject& deviation) const;
    void release(GpuMesh& gpu);
    void drawOverlay();

    [[nodiscard]] std::optional<Hit> pick(const QPointF& pos) const;
    /// The nearest hit on visible meshes, and with `surfaces` on solids and primitives too.
    [[nodiscard]] std::optional<Hit> pickAmong(const QPointF& pos, bool surfaces) const;
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
    ObjectId highlight_{0};
    double brushRadius_{2.0};
    double creaseAngle_{20.0};
    bool wireframe_{false};
    bool showBackFaces_{true};
    bool colorBlind_{false};

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
    Qt::MouseButton pressButton_{Qt::NoButton};
    std::optional<Hit> hover_;
    std::vector<Vec3d> stroke_;
    ObjectId strokeMesh_{0};
    QPolygonF lasso_;
    std::optional<Hit> measureStart_;
    bool interacting_{false};
    QTimer interactionTimer_;
};

} // namespace m2c::app
