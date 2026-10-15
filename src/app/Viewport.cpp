// SPDX-License-Identifier: GPL-3.0-or-later
#include "Viewport.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

#include "m2c/core/DisplayProxy.h"
#include "m2c/core/SpatialIndex.h"

namespace m2c::app {

namespace {

constexpr std::size_t kProxyThreshold =
    2'000'000; ///< Faces above which a proxy is drawn while moving.
constexpr std::size_t kProxyFaces = 400'000;

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 position;
uniform mat4 mvp;
uniform mat4 mv;
out vec3 viewPos;
void main() {
    viewPos = (mv * vec4(position, 1.0)).xyz;
    gl_Position = mvp * vec4(position, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 viewPos;
uniform vec3 baseColor;
uniform vec3 backColor;
uniform int hasSelection;
uniform int lineMode;
uniform usamplerBuffer selection;
out vec4 fragColor;
void main() {
    if (lineMode == 1) { fragColor = vec4(0.08, 0.09, 0.10, 1.0); return; }
    // Flat shading: the face normal from screen-space derivatives of the view position.
    vec3 n = normalize(cross(dFdx(viewPos), dFdy(viewPos)));
    vec3 toEye = normalize(-viewPos);
    float diffuse = abs(dot(n, toEye));
    vec3 color = gl_FrontFacing ? baseColor : backColor;
    if (hasSelection == 1) {
        uint s = texelFetch(selection, gl_PrimitiveID).r;
        if (s == 1u) color = mix(color, vec3(1.0, 0.50, 0.10), 0.8);
        else if (s == 2u) color = mix(color, vec3(0.25, 0.60, 1.00), 0.6);
    }
    fragColor = vec4(color * (0.25 + 0.75 * diffuse), 1.0);
}
)";

QMatrix4x4 toQt(const Mat4d& m) {
    QMatrix4x4 q;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) q(r, c) = static_cast<float>(m.at(r, c));
    }
    return q;
}

Mat4d translation(const Vec3d& t) {
    Mat4d m;
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

bool isMesh(const DocumentObject& o) {
    return o.mesh() != nullptr;
}

} // namespace

Viewport::Viewport(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    interactionTimer_.setSingleShot(true);
    interactionTimer_.setInterval(250);
    connect(&interactionTimer_, &QTimer::timeout, this, [this] {
        interacting_ = false;
        update();
    });
}

Viewport::~Viewport() {
    // The base class destroys the context after this body runs; its aboutToBeDestroyed signal
    // must not reach this half-destroyed object.
    if (context() != nullptr) disconnect(context(), nullptr, this, nullptr);
    cleanupGl();
}

void Viewport::cleanupGl() {
    if (!glReady_) return;
    makeCurrent();
    for (auto& [id, gpu] : gpu_) release(gpu);
    gpu_.clear();
    program_.removeAllShaders();
    glReady_ = false;
    doneCurrent();
}

void Viewport::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        qWarning("OpenGL 3.3 core is not available");
        return;
    }
    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    if (!program_.link()) {
        qWarning("shader link failed: %s", qPrintable(program_.log()));
        return;
    }
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureBuffer_);
    glReady_ = true;
    // The context is destroyed before the widget when it moves between windows; free buffers.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &Viewport::cleanupGl);
}

void Viewport::resizeGL(int, int) {}

double Viewport::aspect() const {
    return std::max(1, width()) / static_cast<double>(std::max(1, height()));
}

// ---- Document ---------------------------------------------------------------------------------

void Viewport::setSnapshot(std::shared_ptr<const DocSnapshot> snapshot) {
    const bool hadMeshes =
        snapshot_ && std::any_of(snapshot_->objects.begin(), snapshot_->objects.end(), isMesh);
    snapshot_ = std::move(snapshot);
    const bool hasMeshes =
        std::any_of(snapshot_->objects.begin(), snapshot_->objects.end(), isMesh);
    if (!hadMeshes && hasMeshes) fitPending_ = true;
    if (hover_ && snapshot_->find(hover_->meshId) == nullptr) hover_.reset();
    update();
}

void Viewport::setCurrent(ObjectId meshId, ObjectId selectionId) {
    currentMesh_ = meshId;
    currentSelection_ = selectionId;
    update();
}

void Viewport::setMode(Mode mode) {
    mode_ = mode;
    measureStart_.reset();
    lasso_.clear();
    stroke_.clear();
    drag_ = Drag::None;
    setCursor(mode == Mode::Navigate ? Qt::ArrowCursor : Qt::CrossCursor);
    update();
}

void Viewport::setBrushRadius(double mm) {
    const double r = std::max(mm, 1e-6);
    if (r == brushRadius_) return;
    brushRadius_ = r;
    emit brushRadiusChanged(r);
    update();
}

void Viewport::setWireframe(bool on) {
    wireframe_ = on;
    update();
}

void Viewport::setShowBackFaces(bool on) {
    showBackFaces_ = on;
    update();
}

Aabb Viewport::visibleBounds(std::optional<ObjectId> only) const {
    Aabb box{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    if (!snapshot_) return box;
    for (const DocumentObject& o : snapshot_->objects) {
        if (!isMesh(o) || (only ? o.id != *only : !o.visible)) continue;
        const Aabb& b = o.mesh()->mesh->bounds();
        box.min = {std::min(box.min.x, b.min.x),
                   std::min(box.min.y, b.min.y),
                   std::min(box.min.z, b.min.z)};
        box.max = {std::max(box.max.x, b.max.x),
                   std::max(box.max.y, b.max.y),
                   std::max(box.max.z, b.max.z)};
    }
    return box;
}

void Viewport::fitAll() {
    const Aabb box = visibleBounds();
    if (box.min.x > box.max.x) return;
    camera_.fit(box);
    update();
}

void Viewport::fitObject(ObjectId id) {
    const Aabb box = visibleBounds(id);
    if (box.min.x > box.max.x) return;
    camera_.fit(box);
    update();
}

void Viewport::setStandardView(double yaw, double pitch) {
    camera_.setView(yaw, pitch);
    update();
}

Mat4d Viewport::viewProjection() const {
    return camera_.projection(aspect()) * camera_.view();
}

// ---- GPU buffers ------------------------------------------------------------------------------

void Viewport::release(GpuMesh& gpu) {
    const GLuint vaos[] = {gpu.vao, gpu.pvao};
    const GLuint buffers[] = {gpu.vbo, gpu.ebo, gpu.pvbo, gpu.pebo, gpu.selBuffer};
    glDeleteVertexArrays(2, vaos);
    glDeleteBuffers(5, buffers);
    glDeleteTextures(1, &gpu.selTexture);
    gpu = GpuMesh{};
}

void Viewport::upload(
    GpuMesh& gpu, const TriMesh& mesh, GLuint& vao, GLuint& vbo, GLuint& ebo, GLsizei& count) {
    std::vector<float> xyz;
    xyz.reserve(mesh.vertexCount() * 3);
    for (const Vec3d& p : mesh.positions()) {
        const Vec3d r = p - gpu.offset;
        xyz.insert(xyz.end(),
                   {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.z)});
    }
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(xyz.size() * sizeof(float)),
                 xyz.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.indices().size() * sizeof(std::uint32_t)),
                 mesh.indices().data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
    count = static_cast<GLsizei>(mesh.indices().size());
}

void Viewport::syncSelection(GpuMesh& gpu, const DocumentObject& mesh) {
    std::vector<std::pair<const MeshSelection*, bool>> key;
    for (const DocumentObject& o : snapshot_->objects) {
        const SelectionData* s = o.selection();
        if (s == nullptr || s->meshId != mesh.id || !o.visible) continue;
        key.emplace_back(s->faces.get(), o.id == currentSelection_);
    }
    if (key == gpu.selectionKey && (gpu.hasSelection || key.empty())) return;
    gpu.selectionKey = key;
    gpu.hasSelection = false;
    const std::size_t faces = mesh.mesh()->mesh->faceCount();
    if (key.empty() || faces > static_cast<std::size_t>(maxTextureBuffer_)) return;
    std::vector<std::uint8_t> bytes(faces, 0);
    // Other selections first, so the current one wins where they overlap.
    std::stable_sort(
        key.begin(), key.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [sel, current] : key) {
        const std::uint8_t value = current ? 1 : 2;
        for (const std::uint32_t f : sel->indices()) bytes[f] = value;
    }
    if (gpu.selBuffer == 0) {
        glGenBuffers(1, &gpu.selBuffer);
        glGenTextures(1, &gpu.selTexture);
    }
    glBindBuffer(GL_TEXTURE_BUFFER, gpu.selBuffer);
    glBufferData(
        GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, gpu.selTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R8UI, gpu.selBuffer);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    gpu.hasSelection = true;
}

void Viewport::syncGpu() {
    if (!snapshot_) return;
    // Drop buffers of meshes that are gone.
    for (auto it = gpu_.begin(); it != gpu_.end();) {
        const DocumentObject* o = snapshot_->find(it->first);
        if (o == nullptr || !isMesh(*o)) {
            release(it->second);
            it = gpu_.erase(it);
        } else {
            ++it;
        }
    }
    for (const DocumentObject& o : snapshot_->objects) {
        if (!isMesh(o)) continue;
        const std::shared_ptr<const TriMesh>& mesh = o.mesh()->mesh;
        GpuMesh& gpu = gpu_[o.id];
        if (gpu.source != mesh.get()) {
            if (gpu.vao != 0) release(gpu);
            gpu.source = mesh.get();
            gpu.keepAlive = mesh;
            gpu.offset = (mesh->bounds().min + mesh->bounds().max) * 0.5;
            upload(gpu, *mesh, gpu.vao, gpu.vbo, gpu.ebo, gpu.indexCount);
            // Build the picking tree (and a proxy for huge meshes) off the UI thread.
            const bool wantProxy = mesh->faceCount() > kProxyThreshold;
            gpu.proxyRequested = wantProxy;
            QPointer<Viewport> self(this);
            const ObjectId id = o.id;
            std::thread([self, id, mesh, wantProxy] {
                (void)mesh->spatialIndex();
                if (!wantProxy) return;
                auto proxy = std::make_shared<const TriMesh>(buildDisplayProxy(*mesh, kProxyFaces));
                QMetaObject::invokeMethod(
                    qApp,
                    [self, id, mesh, proxy] {
                        if (!self) return;
                        const auto it = self->gpu_.find(id);
                        if (it == self->gpu_.end() || it->second.source != mesh.get()) return;
                        it->second.proxy = proxy;
                        self->update();
                    },
                    Qt::QueuedConnection);
            }).detach();
        }
        if (gpu.proxy && gpu.pvao == 0) {
            upload(gpu, *gpu.proxy, gpu.pvao, gpu.pvbo, gpu.pebo, gpu.proxyIndexCount);
        }
        syncSelection(gpu, o);
    }
}

// ---- Drawing ----------------------------------------------------------------------------------

void Viewport::paintGL() {
    if (fitPending_ && snapshot_) {
        fitPending_ = false;
        const Aabb box = visibleBounds();
        if (box.min.x <= box.max.x) camera_.fit(box);
    }
    glClearColor(0.17f, 0.18f, 0.20f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glReady_ && snapshot_) {
        syncGpu();
        glEnable(GL_DEPTH_TEST);
        if (showBackFaces_) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
        program_.bind();
        program_.setUniformValue("selection", 0);
        const Mat4d projection = camera_.projection(aspect());
        const Mat4d view = camera_.viewRelative();
        for (const DocumentObject& o : snapshot_->objects) {
            if (!isMesh(o) || !o.visible) continue;
            const auto it = gpu_.find(o.id);
            if (it == gpu_.end()) continue;
            const GpuMesh& gpu = it->second;
            const Mat4d mv = view * translation(gpu.offset - camera_.target);
            program_.setUniformValue("mv", toQt(mv));
            program_.setUniformValue("mvp", toQt(projection * mv));
            const bool current = o.id == currentMesh_;
            program_.setUniformValue("baseColor",
                                     current ? QVector3D(0.80f, 0.82f, 0.86f)
                                             : QVector3D(0.66f, 0.68f, 0.70f));
            program_.setUniformValue("backColor", QVector3D(0.72f, 0.30f, 0.30f));
            const bool proxy = interacting_ && gpu.proxyIndexCount > 0;
            program_.setUniformValue("hasSelection", (!proxy && gpu.hasSelection) ? 1 : 0);
            if (!proxy && gpu.hasSelection) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_BUFFER, gpu.selTexture);
            }
            glBindVertexArray(proxy ? gpu.pvao : gpu.vao);
            const GLsizei count = proxy ? gpu.proxyIndexCount : gpu.indexCount;
            program_.setUniformValue("lineMode", 0);
            if (wireframe_) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.0f, 1.0f);
            }
            glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
            if (wireframe_) {
                glDisable(GL_POLYGON_OFFSET_FILL);
                program_.setUniformValue("lineMode", 1);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            }
        }
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        program_.release();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
    }
    drawOverlay();
}

std::optional<QPointF> Viewport::project(const Vec3d& p) const {
    const Mat4d vp = viewProjection();
    const double x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
    const double y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
    const double w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
    if (w <= 1e-12) return std::nullopt;
    return QPointF((x / w + 1.0) * 0.5 * width(), (1.0 - y / w) * 0.5 * height());
}

double Viewport::pixelsPerMm(const Vec3d& at) const {
    const double depth = std::max(dot(at - camera_.eye(), camera_.forward()), 1e-9);
    const double focal = height() / (2.0 * std::tan(camera_.fovYDeg * 3.14159265358979 / 360.0));
    return focal / depth;
}

void Viewport::drawOverlay() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent(255, 140, 40);

    if (!snapshot_ || std::none_of(snapshot_->objects.begin(), snapshot_->objects.end(), isMesh)) {
        painter.setPen(QColor(170, 175, 185));
        painter.drawText(rect(),
                         Qt::AlignCenter,
                         tr("Import a mesh (File > Import, or drop an STL, OBJ or PLY here)"));
    }

    // Measurements.
    if (snapshot_) {
        for (const DocumentObject& o : snapshot_->objects) {
            const MeasurementData* m = o.measurement();
            if (m == nullptr || !o.visible) continue;
            const auto a = project(m->measurement.a.position);
            const auto b = project(m->measurement.b.position);
            if (!a || !b) continue;
            painter.setPen(QPen(QColor(255, 220, 80), 2));
            painter.drawLine(*a, *b);
            painter.setBrush(QColor(255, 220, 80));
            painter.drawEllipse(*a, 3.5, 3.5);
            painter.drawEllipse(*b, 3.5, 3.5);
            const QString label = QStringLiteral("%1 mm").arg(m->measurement.distance(), 0, 'f', 3);
            const QPointF mid = (*a + *b) / 2.0 + QPointF(6, -6);
            const QRectF box = painter.fontMetrics().boundingRect(label).adjusted(-4, -2, 4, 2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(20, 20, 24, 200));
            painter.drawRoundedRect(box.translated(mid), 3, 3);
            painter.setPen(QColor(255, 230, 120));
            painter.drawText(mid, label);
        }
    }

    // Tool feedback.
    painter.setBrush(Qt::NoBrush);
    if (mode_ == Mode::Brush && hover_) {
        if (const auto c = project(hover_->point)) {
            const double r = brushRadius_ * pixelsPerMm(hover_->point);
            painter.setPen(QPen(accent, 1.5));
            painter.drawEllipse(*c, r, r);
        }
    }
    if (drag_ == Drag::Brush) {
        painter.setPen(QPen(accent, 1.0, Qt::DotLine));
        for (const Vec3d& p : stroke_) {
            if (const auto c = project(p)) painter.drawEllipse(*c, 2.0, 2.0);
        }
    }
    if (drag_ == Drag::Lasso && lasso_.size() > 1) {
        painter.setPen(QPen(accent, 1.5, Qt::DashLine));
        painter.setBrush(QColor(255, 140, 40, 40));
        painter.drawPolygon(lasso_);
        painter.setBrush(Qt::NoBrush);
    }
    if (mode_ == Mode::Measure) {
        if (measureStart_) {
            if (const auto a = project(measureStart_->point)) {
                painter.setPen(QPen(QColor(255, 220, 80), 2));
                painter.drawEllipse(*a, 4, 4);
                if (hover_) {
                    if (const auto b = project(hover_->point)) {
                        painter.setPen(QPen(QColor(255, 220, 80), 1, Qt::DashLine));
                        painter.drawLine(*a, *b);
                        painter.drawText(
                            *b + QPointF(10, -10),
                            QStringLiteral("%1 mm").arg(
                                length(hover_->point - measureStart_->point), 0, 'f', 3));
                    }
                }
            }
        } else if (hover_) {
            if (const auto b = project(hover_->point)) {
                painter.setPen(QPen(QColor(255, 220, 80), 1.5));
                painter.drawEllipse(*b, 4, 4);
            }
        }
    }
    if (mode_ == Mode::Crease && hover_) {
        if (const auto c = project(hover_->point)) {
            painter.setPen(QPen(accent, 1.5));
            painter.drawLine(*c - QPointF(6, 0), *c + QPointF(6, 0));
            painter.drawLine(*c - QPointF(0, 6), *c + QPointF(0, 6));
        }
    }

    // Axis triad, bottom left.
    const QPointF origin(42, height() - 42);
    const Mat4d v = camera_.viewRelative();
    const struct {
        Vec3d axis;
        QColor color;
        const char* name;
    } axes[] = {{{1, 0, 0}, QColor(230, 80, 80), "X"},
                {{0, 1, 0}, QColor(100, 200, 100), "Y"},
                {{0, 0, 1}, QColor(90, 140, 240), "Z"}};
    for (const auto& a : axes) {
        const double sx = v.at(0, 0) * a.axis.x + v.at(0, 1) * a.axis.y + v.at(0, 2) * a.axis.z;
        const double sy = v.at(1, 0) * a.axis.x + v.at(1, 1) * a.axis.y + v.at(1, 2) * a.axis.z;
        const QPointF tip = origin + QPointF(sx * 28.0, -sy * 28.0);
        painter.setPen(QPen(a.color, 2));
        painter.drawLine(origin, tip);
        painter.drawText(tip + QPointF(3, 4), QString::fromLatin1(a.name));
    }
}

// ---- Picking ----------------------------------------------------------------------------------

QPointF Viewport::ndc(const QPointF& pos) const {
    return {2.0 * pos.x() / std::max(1, width()) - 1.0,
            1.0 - 2.0 * pos.y() / std::max(1, height())};
}

std::optional<Hit> Viewport::pick(const QPointF& pos) const {
    if (!snapshot_) return std::nullopt;
    const QPointF n = ndc(pos);
    Vec3d origin;
    Vec3d direction;
    camera_.ray(n.x(), n.y(), aspect(), origin, direction);
    std::optional<Hit> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const DocumentObject& o : snapshot_->objects) {
        if (!isMesh(o) || !o.visible) continue;
        const auto hit = o.mesh()->mesh->spatialIndex().raycast(origin, direction);
        if (hit && hit->distance < bestDistance) {
            bestDistance = hit->distance;
            best = Hit{o.id, hit->face, hit->point};
        }
    }
    return best;
}

// ---- Mouse ------------------------------------------------------------------------------------

void Viewport::markInteracting() {
    interacting_ = true;
    interactionTimer_.start();
}

void Viewport::mousePressEvent(QMouseEvent* event) {
    lastPos_ = pressPos_ = event->position();
    pressMods_ = event->modifiers();
    const bool left = event->button() == Qt::LeftButton;
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);

    if (event->button() == Qt::MiddleButton || (left && mode_ == Mode::Navigate && shift)) {
        drag_ = Drag::Pan;
        return;
    }
    if (event->button() == Qt::RightButton || (left && mode_ == Mode::Navigate)) {
        drag_ = shift ? Drag::Pan : Drag::Orbit;
        return;
    }
    if (!left) return;

    switch (mode_) {
    case Mode::Navigate:
        break;
    case Mode::Brush:
        if (const auto hit = pick(event->position())) {
            drag_ = Drag::Brush;
            strokeMesh_ = hit->meshId;
            stroke_ = {hit->point};
        } else {
            drag_ = Drag::Orbit;
        }
        break;
    case Mode::Lasso:
        drag_ = Drag::Lasso;
        lasso_ = QPolygonF{event->position()};
        break;
    case Mode::Crease:
        if (const auto hit = pick(event->position())) emit faceClicked(*hit, ctrl, shift);
        break;
    case Mode::Measure:
        if (const auto hit = pick(event->position())) {
            if (!measureStart_) {
                measureStart_ = hit;
            } else {
                emit measured(*measureStart_, *hit);
                measureStart_.reset();
            }
        }
        break;
    }
    update();
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const QPointF delta = pos - lastPos_;
    lastPos_ = pos;
    switch (drag_) {
    case Drag::Orbit:
        camera_.orbit(delta.x() * 0.4, delta.y() * 0.4);
        markInteracting();
        break;
    case Drag::Pan:
        camera_.pan(delta.x() / std::max(1, height()), delta.y() / std::max(1, height()));
        markInteracting();
        break;
    case Drag::Brush:
        if (const auto hit = pick(pos); hit && hit->meshId == strokeMesh_) {
            hover_ = hit;
            if (length(hit->point - stroke_.back()) > brushRadius_ * 0.35) {
                stroke_.push_back(hit->point);
            }
        }
        break;
    case Drag::Lasso:
        if (QLineF(lasso_.back(), pos).length() > 3.0) lasso_ << pos;
        break;
    case Drag::None:
        if (mode_ != Mode::Navigate && mode_ != Mode::Lasso) {
            hover_ = pick(pos);
            if (hover_) {
                emit hoverChanged(QStringLiteral("Object %1, face %2   (%3, %4, %5) mm")
                                      .arg(hover_->meshId)
                                      .arg(hover_->face)
                                      .arg(hover_->point.x, 0, 'f', 3)
                                      .arg(hover_->point.y, 0, 'f', 3)
                                      .arg(hover_->point.z, 0, 'f', 3));
            } else {
                emit hoverChanged({});
            }
        }
        break;
    }
    update();
}

void Viewport::mouseReleaseEvent(QMouseEvent*) {
    const bool shift = pressMods_.testFlag(Qt::ShiftModifier);
    const bool ctrl = pressMods_.testFlag(Qt::ControlModifier);
    if (drag_ == Drag::Brush && !stroke_.empty()) {
        emit brushStroke(strokeMesh_, stroke_, ctrl, shift);
    } else if (drag_ == Drag::Lasso && lasso_.size() >= 3) {
        QPolygonF polygon;
        for (const QPointF& p : lasso_) polygon << ndc(p);
        emit lassoDrawn(polygon, ctrl, shift);
    }
    drag_ = Drag::None;
    stroke_.clear();
    lasso_.clear();
    update();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-click sets the orbit centre to the point under the cursor.
    if (event->button() != Qt::LeftButton || mode_ != Mode::Navigate) return;
    if (const auto hit = pick(event->position())) {
        const Vec3d eye = camera_.eye();
        camera_.target = hit->point;
        camera_.distance = std::max(length(eye - hit->point), camera_.sceneRadius * 1e-3);
        update();
    }
}

void Viewport::wheelEvent(QWheelEvent* event) {
    camera_.zoom(std::pow(0.85, event->angleDelta().y() / 120.0));
    markInteracting();
    update();
}

void Viewport::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_F:
        fitAll();
        break;
    case Qt::Key_Escape:
        measureStart_.reset();
        lasso_.clear();
        stroke_.clear();
        drag_ = Drag::None;
        update();
        break;
    case Qt::Key_BracketLeft:
        setBrushRadius(brushRadius_ / 1.25);
        break;
    case Qt::Key_BracketRight:
        setBrushRadius(brushRadius_ * 1.25);
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
    }
}

void Viewport::leaveEvent(QEvent*) {
    hover_.reset();
    emit hoverChanged({});
    update();
}

} // namespace m2c::app
