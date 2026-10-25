// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Section.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kSmoothJoinCos = 0.9396926207859084; // cos 20 degrees
constexpr double kNoCornerCos = 0.8660254037844387;   // cos 30 degrees

double distanceToSegment(const Vec2d& a, const Vec2d& b, const Vec2d& p) noexcept {
    SketchCurve line;
    line.points = {a, b};
    return distanceTo(line, p);
}

double angleOf(const Vec2d& v) noexcept {
    return std::atan2(v.y, v.x);
}

/// Anticlockwise turn from angle a to angle b, in [0, 2 pi).
double turnBetween(const double a, const double b) noexcept {
    const double t = std::fmod(b - a, kTwoPi);
    return t < 0.0 ? t + kTwoPi : t;
}

/// Least-squares circle through the points: the algebraic (Kasa) fit, then Gauss-Newton steps on
/// the distances. Empty if the points are (nearly) on a line.
std::optional<SketchCurve> fitCircle(const std::span<const Vec2d> points) {
    Vec2d mean;
    for (const Vec2d& p : points) mean = mean + p;
    mean = mean * (1.0 / static_cast<double>(points.size()));
    Eigen::Matrix3d a = Eigen::Matrix3d::Zero();
    Eigen::Vector3d b = Eigen::Vector3d::Zero();
    for (const Vec2d& p : points) {
        const Vec2d q = p - mean;
        const Eigen::Vector3d row(q.x, q.y, 1.0);
        a += row * row.transpose();
        b -= row * dot(q, q);
    }
    const Eigen::Vector3d k = a.ldlt().solve(b);
    Eigen::Vector3d c(-k.x() / 2.0, -k.y() / 2.0, 0.0);
    c.z() = std::sqrt(std::max(0.0, c.x() * c.x() + c.y() * c.y() - k.z()));
    for (int step = 0; step < 8; ++step) {
        Eigen::Matrix3d jtj = Eigen::Matrix3d::Zero();
        Eigen::Vector3d jtr = Eigen::Vector3d::Zero();
        for (const Vec2d& p : points) {
            const Vec2d d = p - mean - Vec2d{.x = c.x(), .y = c.y()};
            const double len = length(d);
            if (len <= 0.0) continue;
            const Eigen::Vector3d j(-d.x / len, -d.y / len, -1.0);
            jtj += j * j.transpose();
            jtr += j * (len - c.z());
        }
        c -= jtj.ldlt().solve(jtr);
    }
    if (!c.allFinite() || !(c.z() > 0.0)) return std::nullopt;
    SketchCurve circle;
    circle.kind = CurveKind::Arc;
    circle.centre = mean + Vec2d{.x = c.x(), .y = c.y()};
    circle.radiusMm = c.z();
    return circle;
}

/// The arc from q[i] to q[j] nearest the points between, by Gauss-Newton on its centre's position
/// along the chord's perpendicular bisector, starting from the circle through the point farthest
/// from the chord. Empty if it is not within `tol` of every point or, with `mustBow`, does not bow
/// away from its chord by more than `tol` (scan noise can make a point that far from a straight
/// chord, so the arc's own height above the chord counts).
std::optional<SketchCurve> arcThrough(const std::span<const Vec2d> q,
                                      const std::size_t i,
                                      const std::size_t j,
                                      const double tol,
                                      const bool mustBow) {
    if (j < i + 2) return std::nullopt;
    const Vec2d a = q[i], b = q[j];
    const double chord = length(b - a);
    if (!(chord > 0.0)) return std::nullopt;
    Vec2d mid = q[i + 1];
    double bow = 0.0;
    for (std::size_t k = i + 1; k < j; ++k) {
        if (const double dist = distanceToSegment(a, b, q[k]); dist > bow) {
            bow = dist;
            mid = q[k];
        }
    }
    const Vec2d m = (a + b) * 0.5;
    const Vec2d d = Vec2d{.x = a.y - b.y, .y = b.x - a.x} * (1.0 / chord);
    const double h2 = chord * chord / 4.0;
    // The centre M + d t is as far from `mid` as from a and b.
    const double across = dot(d, m - mid);
    if (std::abs(across) < 1e-12) return std::nullopt;
    double t = (h2 - dot(m - mid, m - mid)) / (2.0 * across);
    for (int step = 0; step < 8 && std::isfinite(t); ++step) {
        const Vec2d c = m + d * t;
        const double r = std::sqrt(h2 + t * t);
        double jtj = 0.0, jtr = 0.0;
        for (std::size_t k = i + 1; k < j; ++k) {
            const Vec2d v = q[k] - c;
            const double len = length(v);
            if (len <= 0.0) continue;
            const double dr = -dot(v, d) / len - t / r;
            jtj += dr * dr;
            jtr += dr * (len - r);
        }
        if (!(jtj > 0.0)) break;
        t -= jtr / jtj;
    }
    if (!std::isfinite(t)) return std::nullopt;
    SketchCurve arc;
    arc.kind = CurveKind::Arc;
    arc.centre = m + d * t;
    arc.radiusMm = std::sqrt(h2 + t * t);
    arc.startRad = angleOf(a - arc.centre);
    const double toEnd = turnBetween(arc.startRad, angleOf(b - arc.centre));
    const double toMid = turnBetween(arc.startRad, angleOf(mid - arc.centre));
    arc.sweepRad = toMid < toEnd ? toEnd : toEnd - kTwoPi;
    if (mustBow && !(arc.radiusMm * (1.0 - std::cos(std::abs(arc.sweepRad) / 2.0)) > tol)) {
        return std::nullopt;
    }
    for (std::size_t k = i + 1; k < j; ++k) {
        if (distanceTo(arc, q[k]) > tol) return std::nullopt;
    }
    return arc;
}

/// The largest j in [first, n] for which fits(j) holds, searching by doubling then halving, or 0
/// if fits(first) fails.
template <class Fits>
std::size_t extend(std::size_t first, const std::size_t n, Fits&& fits) {
    if (first > n || !fits(first)) return 0;
    std::size_t good = first, step = 1;
    std::size_t bad = n + 1;
    while (good < n) {
        const std::size_t next = std::min(n, good + step);
        if (!fits(next)) {
            bad = next;
            break;
        }
        good = next;
        step *= 2;
    }
    while (bad <= n && bad - good > 1) {
        const std::size_t mid = good + (bad - good) / 2;
        (fits(mid) ? good : bad) = mid;
    }
    return good;
}

/// Unit tangent at the start (atEnd false) or end of a line or arc, in its direction.
Vec2d tangent(const SketchCurve& c, const bool atEnd) noexcept {
    if (c.kind == CurveKind::Arc) {
        const double angle = c.startRad + (atEnd ? c.sweepRad : 0.0);
        const double sign = c.sweepRad < 0.0 ? -1.0 : 1.0;
        return Vec2d{.x = -std::sin(angle), .y = std::cos(angle)} * sign;
    }
    const Vec2d d = c.end() - c.start();
    return d * (1.0 / length(d));
}

/// Douglas-Peucker: the points of q[first..last] needed to stay within tol of them all.
std::vector<Vec2d>
thin(const std::span<const Vec2d> q, std::size_t first, std::size_t last, const double tol) {
    std::vector keep(last - first + 1, false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{first, last}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        std::size_t split = a;
        double farthest = tol;
        for (std::size_t k = a + 1; k < b; ++k) {
            if (const double dist = distanceToSegment(q[a], q[b], q[k]); dist > farthest) {
                farthest = dist;
                split = k;
            }
        }
        if (split == a) continue;
        keep[split - first] = true;
        stack.emplace_back(a, split);
        stack.emplace_back(split, b);
    }
    std::vector<Vec2d> out;
    for (std::size_t k = first; k <= last; ++k) {
        if (keep[k - first]) out.push_back(q[k]);
    }
    return out;
}

struct Piece {
    SketchCurve curve;
    std::size_t first{0}, last{0}; ///< The points it covers, first and last included.
};

bool lineFits(const std::span<const Vec2d> q,
              const std::size_t i,
              const std::size_t j,
              const double tol) {
    if (!(length(q[j] - q[i]) > 0.0)) return false;
    for (std::size_t k = i + 1; k < j; ++k) {
        if (distanceToSegment(q[i], q[j], q[k]) > tol) return false;
    }
    return true;
}

/// A curve of the given kind from q[i] to q[j] within tol of the points between, if there is one.
std::optional<SketchCurve> curveThrough(std::span<const Vec2d> q,
                                        const CurveKind kind,
                                        const std::size_t i,
                                        const std::size_t j,
                                        const double tol) {
    if (kind == CurveKind::Arc) return arcThrough(q, i, j, tol, false);
    if (j <= i || !lineFits(q, i, j, tol)) return std::nullopt;
    SketchCurve line;
    line.points = {q[i], q[j]};
    return line;
}

/// Curve by curve from q[0] to q[n], the line or arc that covers the most points.
std::vector<Piece> greedyPieces(std::span<const Vec2d> q, const double tol) {
    const std::size_t n = q.size() - 1;
    std::vector<Piece> pieces;
    for (std::size_t i = 0; i < n;) {
        const std::size_t lineEnd = std::max(
            i + 1, extend(i + 1, n, [&](const std::size_t j) { return lineFits(q, i, j, tol); }));
        // An arc must bow away from its chord, or the line would do.
        const std::size_t arcEnd = extend(i + 2, n, [&](const std::size_t j) {
            return arcThrough(q, i, j, tol, false).has_value();
        });
        const auto arc = arcEnd > lineEnd ? arcThrough(q, i, arcEnd, tol, true) : std::nullopt;
        Piece piece{.curve = {}, .first = i, .last = arc ? arcEnd : lineEnd};
        if (arc) {
            piece.curve = *arc;
        } else {
            piece.curve.points = {q[i], q[lineEnd]};
        }
        i = piece.last;
        pieces.push_back(std::move(piece));
    }
    return pieces;
}

double squaredError(const std::span<const Vec2d> q,
                    const SketchCurve& c,
                    const std::size_t i,
                    const std::size_t j) {
    double sum = 0.0;
    for (std::size_t k = i; k <= j; ++k) sum += distanceTo(c, q[k]) * distanceTo(c, q[k]);
    return sum;
}

/// Greedy curves run past where they should end: a line on into a tangent arc, an arc on into a
/// tangent line, as far as the limit allows. Moves each junction between two curves, up to 32
/// points either way, to where the two refitted curves fit the points best. An arc that ran on
/// at both ends fits better than either end moved alone, so each curve's two junctions are also
/// moved towards or away from each other together. Repeats until nothing moves (at most 16
/// rounds).
void refineJunctions(std::span<const Vec2d> q, std::vector<Piece>& pieces, double tol) {
    bool moved = true;
    for (int round = 0; round < 16 && moved; ++round) {
        constexpr std::size_t kWindow = 32;
        moved = false;
        for (std::size_t k = 1; k < pieces.size(); ++k) {
            Piece& before = pieces[k - 1];
            Piece& after = pieces[k];
            const std::size_t lo =
                std::max(before.first + 2, after.first - std::min(after.first, kWindow));
            const std::size_t hi =
                std::min(after.last - std::min(after.last, std::size_t{2}), after.first + kWindow);
            double best = squaredError(q, before.curve, before.first, before.last) +
                          squaredError(q, after.curve, after.first, after.last);
            for (std::size_t j = lo; j <= hi; ++j) {
                if (j == after.first) continue;
                auto a = curveThrough(q, before.curve.kind, before.first, j, tol);
                auto b = a ? curveThrough(q, after.curve.kind, j, after.last, tol) : std::nullopt;
                if (!b) continue;
                const double error =
                    squaredError(q, *a, before.first, j) + squaredError(q, *b, j, after.last);
                if (error < best) {
                    best = error;
                    before.curve = std::move(*a);
                    after.curve = std::move(*b);
                    before.last = after.first = j;
                    moved = true;
                }
            }
        }
        for (std::size_t k = 1; k + 1 < pieces.size(); ++k) {
            Piece& before = pieces[k - 1];
            Piece& piece = pieces[k];
            Piece& after = pieces[k + 1];
            double best = squaredError(q, before.curve, before.first, before.last) +
                          squaredError(q, piece.curve, piece.first, piece.last) +
                          squaredError(q, after.curve, after.first, after.last);
            const std::size_t first = piece.first, last = piece.last;
            for (std::size_t step = 1; step <= kWindow; ++step) {
                for (const bool grow : {false, true}) {
                    if (grow ? first < before.first + 2 + step || last + step + 2 > after.last
                             : first + 2 * step + 2 > last) {
                        continue;
                    }
                    const std::size_t i = grow ? first - step : first + step;
                    const std::size_t j = grow ? last + step : last - step;
                    auto a = curveThrough(q, before.curve.kind, before.first, i, tol);
                    auto b = a ? curveThrough(q, piece.curve.kind, i, j, tol) : std::nullopt;
                    auto c =
                        b ? curveThrough(q, after.curve.kind, j, after.last, tol) : std::nullopt;
                    if (!c) continue;
                    const double error = squaredError(q, *a, before.first, i) +
                                         squaredError(q, *b, i, j) +
                                         squaredError(q, *c, j, after.last);
                    if (error < best) {
                        best = error;
                        before.curve = std::move(*a);
                        piece.curve = std::move(*b);
                        after.curve = std::move(*c);
                        before.last = piece.first = i;
                        piece.last = after.first = j;
                        moved = true;
                    }
                }
            }
        }
    }
}

/// q is the polyline from `start`, closed: q[n] is q[0].
std::vector<Vec2d> closedFrom(const std::span<const Vec2d> p, const std::size_t start) {
    std::vector<Vec2d> q(p.size() + 1);
    for (std::size_t k = 0; k < q.size(); ++k) q[k] = p[(start + k) % p.size()];
    return q;
}

} // namespace

Result<LoopFit> fitLoop(std::span<const Vec2d> points, const SectionParams& params) {
    const double tol = params.maxErrorMm;
    if (!(tol > 0.0)) return fail(ErrorCode::InvalidArgument, "maximum error must be positive");
    std::vector<Vec2d> p;
    for (const Vec2d& v : points) {
        if (p.empty() || length(v - p.back()) > 1e-9) p.push_back(v);
    }
    while (p.size() > 1 && length(p.back() - p.front()) <= 1e-9) p.pop_back();
    const std::size_t n = p.size();
    if (n < 3) return fail(ErrorCode::InvalidArgument, "a loop needs at least three points");

    double area = 0.0;
    for (std::size_t k = 0; k < n; ++k) area += cross(p[k], p[(k + 1) % n]);
    if (const auto circle = fitCircle(p)) {
        double worst = 0.0;
        for (const Vec2d& v : p)
            worst = std::max(worst, std::abs(length(v - circle->centre) - circle->radiusMm));
        if (worst <= tol) {
            LoopFit out;
            SketchCurve full = *circle;
            full.startRad = angleOf(p.front() - full.centre);
            full.sweepRad = area < 0.0 ? -kTwoPi : kTwoPi;
            out.loop.curves.push_back(full);
            out.maxErrorMm = worst;
            return out;
        }
    }

    // Start at the sharpest corner. With no corner sharper than 30 degrees the start may be in
    // the middle of a curve, so start again where the first curve ends.
    std::size_t start = 0;
    double sharpest = 2.0;
    for (std::size_t k = 0; k < n; ++k) {
        const Vec2d in = p[k] - p[(k + n - 1) % n], out = p[(k + 1) % n] - p[k];
        if (const double c = dot(in, out) / (length(in) * length(out)); c < sharpest) {
            sharpest = c;
            start = k;
        }
    }
    std::vector<Vec2d> q = closedFrom(p, start);
    std::vector<Piece> pieces = greedyPieces(q, tol);
    refineJunctions(q, pieces, tol);
    if (sharpest > kNoCornerCos && pieces.size() > 1) {
        q = closedFrom(p, (start + pieces.front().last) % n);
        pieces = greedyPieces(q, tol);
        refineJunctions(q, pieces, tol);
    }

    LoopFit out;
    const auto isShort = [&](const Piece& c) {
        return c.last - c.first + 1 < static_cast<std::size_t>(params.minCurvePoints);
    };
    for (std::size_t k = 0; k < pieces.size();) {
        std::size_t end = k + 1;
        while (isShort(pieces[k]) && end < pieces.size() && isShort(pieces[end]) &&
               dot(tangent(pieces[end - 1].curve, true), tangent(pieces[end].curve, false)) >=
                   kSmoothJoinCos) {
            ++end;
        }
        SketchCurve curve = pieces[k].curve;
        if (end > k + 1) {
            curve = SketchCurve{};
            curve.kind = CurveKind::Spline;
            curve.points = thin(q, pieces[k].first, pieces[end - 1].last, tol / 2.0);
        }
        for (std::size_t v = pieces[k].first; v <= pieces[end - 1].last; ++v) {
            out.maxErrorMm = std::max(out.maxErrorMm, distanceTo(curve, q[v]));
        }
        out.loop.curves.push_back(std::move(curve));
        k = end;
    }
    return out;
}

Result<SectionResult>
sectionMesh(const TriMesh& mesh, const SketchPlane& plane, const SectionParams& params) {
    auto cut = cgal::slice(mesh, plane.origin, plane.normal());
    if (!cut) return std::unexpected(cut.error());
    SectionResult out;
    out.sketch.plane = plane;
    for (const auto& polyline : *cut) {
        if (polyline.size() < 4 || polyline.front() != polyline.back()) {
            ++out.openPieces;
            continue;
        }
        std::vector<Vec2d> flat;
        flat.reserve(polyline.size());
        for (const Vec3d& v : polyline) flat.push_back(plane.toPlane(v));
        auto fit = fitLoop(flat, params);
        if (!fit) return std::unexpected(fit.error());
        out.maxErrorMm = std::max(out.maxErrorMm, fit->maxErrorMm);
        out.sketch.loops.push_back(std::move(fit->loop));
    }
    if (out.sketch.loops.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    out.openPieces > 0
                        ? "the plane cuts the mesh only where it is open, so no profile closes"
                        : "the plane does not cut the mesh");
    }
    return out;
}

} // namespace m2c
