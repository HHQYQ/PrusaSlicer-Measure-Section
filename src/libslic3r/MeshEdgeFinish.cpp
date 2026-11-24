///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "MeshEdgeFinish.hpp"

#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace Slic3r::MeshEdgeFinish {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double plane_angle = 0.999962; // cos(0.5 degrees)
using Vec2 = Eigen::Vector2d;
using Neighbors = std::vector<std::array<int, 3>>;
using Borders = std::vector<std::vector<Segment>>;

double signed_volume(const Surface &mesh)
{
    double volume = 0.;
    for (const auto &f : mesh.indices)
        volume += mesh.vertices[f[0]].dot(mesh.vertices[f[1]].cross(mesh.vertices[f[2]]));
    return volume / 6.;
}

bool topology(const Surface &mesh, Neighbors &neighbors, std::vector<std::vector<int>> &incident, int64_t &euler)
{
    struct Use { int face, slot, from; bool paired = false; };
    std::unordered_map<uint64_t, Use> uses;
    uses.reserve(mesh.indices.size() * 2);
    neighbors.assign(mesh.indices.size(), {{-1, -1, -1}});
    incident.resize(mesh.vertices.size());
    for (int f = 0; f < int(mesh.indices.size()); ++f) {
        const auto &v = mesh.indices[f];
        if (v.minCoeff() < 0 || size_t(v.maxCoeff()) >= mesh.vertices.size() ||
            v[0] == v[1] || v[1] == v[2] || v[2] == v[0])
            return false;
        for (int i = 0; i < 3; ++i) {
            const int a = v[i], b = v[(i + 1) % 3];
            incident[a].push_back(f);
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint32_t(std::max(a, b));
            auto [it, inserted] = uses.emplace(key, Use{f, i, a});
            if (inserted)
                continue;
            Use &first = it->second;
            if (first.paired || first.from == a)
                return false;
            first.paired = true;
            neighbors[f][i] = first.face;
            neighbors[first.face][first.slot] = f;
        }
    }
    for (const auto &n : neighbors)
        if (std::find(n.begin(), n.end(), -1) != n.end())
            return false;

    std::vector<int> queue{0};
    std::vector<int> seen(neighbors.size(), -2);
    seen[0] = -1;
    for (size_t i = 0; i < queue.size(); ++i)
        for (int next : neighbors[queue[i]])
            if (seen[next] == -2) {
                seen[next] = -1;
                queue.push_back(next);
            }
    if (queue.size() != neighbors.size())
        return false;

    int64_t used_vertices = 0;
    for (int vertex = 0; vertex < int(incident.size()); ++vertex) {
        if (incident[vertex].empty())
            continue;
        ++used_vertices;
        queue.assign(1, incident[vertex].front());
        seen[queue.front()] = vertex;
        for (size_t i = 0; i < queue.size(); ++i) {
            const int face = queue[i];
            const auto &f = mesh.indices[face];
            for (int slot = 0; slot < 3; ++slot) {
                const int next = neighbors[face][slot];
                if ((f[slot] == vertex || f[(slot + 1) % 3] == vertex) && seen[next] != vertex) {
                    seen[next] = vertex;
                    queue.push_back(next);
                }
            }
        }
        if (queue.size() != incident[vertex].size())
            return false; // disconnected fans sharing a vertex
    }
    euler = used_vertices - int64_t(uses.size()) + int64_t(mesh.indices.size());
    return true;
}

bool planar_patches(Analysis &out, const Neighbors &neighbors, Borders &borders, const Cancelled &cancelled)
{
    const auto &mesh = out.mesh;
    std::vector<Vec3> normals;
    normals.reserve(mesh.indices.size());
    for (const auto &f : mesh.indices) {
        const Vec3 cross = (mesh.vertices[f[1]] - mesh.vertices[f[0]]).cross(mesh.vertices[f[2]] - mesh.vertices[f[0]]);
        if (!cross.allFinite() || cross.squaredNorm() == 0.)
            return false;
        normals.push_back(cross.normalized());
    }
    out.face_patch.assign(mesh.indices.size(), -1);
    std::vector<int> queue;
    for (int f = 0; f < int(mesh.indices.size()); ++f) {
        if (out.face_patch[f] != -1)
            continue;
        if (cancelled())
            return false;
        const int patch = int(out.patch_normals.size());
        const Vec3 &normal = normals[f];
        const Vec3 &origin = mesh.vertices[mesh.indices[f][0]];
        out.patch_normals.push_back(normal);
        queue.assign(1, f);
        out.face_patch[f] = patch;
        for (size_t i = 0; i < queue.size(); ++i)
            for (int n : neighbors[queue[i]]) {
                if (out.face_patch[n] != -1 || normals[n].dot(normal) < plane_angle)
                    continue;
                const auto &ids = mesh.indices[n];
                if (std::abs(normal.dot(mesh.vertices[ids[0]] - origin)) <= out.tolerance &&
                    std::abs(normal.dot(mesh.vertices[ids[1]] - origin)) <= out.tolerance &&
                    std::abs(normal.dot(mesh.vertices[ids[2]] - origin)) <= out.tolerance) {
                    out.face_patch[n] = patch;
                    queue.push_back(n);
                }
            }
    }
    borders.resize(out.patch_normals.size());
    for (int face = 0; face < int(mesh.indices.size()); ++face)
        for (int slot = 0; slot < 3; ++slot) {
            const int other = neighbors[face][slot], patch = out.face_patch[face];
            if (out.face_patch[other] != patch)
                borders[patch].push_back({mesh.indices[face][slot], mesh.indices[face][(slot + 1) % 3], face, other});
        }
    return true;
}

// Distance to the first boundary in a support plane, across a strip swept
// perpendicular to the edge. Subsequent 3D validation also checks its corners.
double strip_limit(const Analysis &a, const std::vector<Segment> &border, int from, int to, const Vec3 &side,
    const std::vector<int> &excluded, bool skip_vertical_ends = false)
{
    const Vec3 origin = a.mesh.vertices[from];
    const Vec3 axis = (a.mesh.vertices[to] - origin).normalized();
    const double length = (a.mesh.vertices[to] - origin).norm();
    const double eps = a.tolerance;
    double best = std::numeric_limits<double>::infinity();
    for (const Segment &s : border) {
        if (std::find(excluded.begin(), excluded.end(), a.face_patch[s.other_face]) != excluded.end())
            continue;
        const Vec3 u = a.mesh.vertices[s.from] - origin, v = a.mesh.vertices[s.to] - origin;
        const double x = u.dot(axis), y = v.dot(axis), du = u.dot(side), dv = v.dot(side);
        if (skip_vertical_ends && std::abs(x - y) <= eps &&
            (std::abs(x) <= eps || std::abs(x - length) <= eps))
            continue;
        if (std::max(x, y) < eps || std::min(x, y) > length - eps || std::max(du, dv) <= eps)
            continue;
        double lo = 0., hi = 1.;
        if (std::abs(y - x) > eps) {
            const double t0 = (eps - x) / (y - x), t1 = (length - eps - x) / (y - x);
            lo = std::max(lo, std::min(t0, t1));
            hi = std::min(hi, std::max(t0, t1));
        }
        if (lo <= hi)
            best = std::min(best, std::max(0., std::min(du + lo * (dv - du), du + hi * (dv - du))));
    }
    return best;
}

void recognize_rim(Analysis &a, const Borders &borders, int patch, const std::vector<Segment> &loop,
    std::vector<bool> &curved)
{
    if (loop.size() < 24)
        return;
    const Vec3 normal = a.patch_normals[patch];
    std::vector<int> corners;
    Vec3 area = Vec3::Zero();
    const Vec3 origin = a.mesh.vertices[loop.front().from];
    for (size_t i = 0; i < loop.size(); ++i) {
        const Vec3 p = a.mesh.vertices[loop[i].from];
        const Vec3 u = p - a.mesh.vertices[loop[(i + loop.size() - 1) % loop.size()].from];
        const Vec3 v = a.mesh.vertices[loop[i].to] - p;
        if (u.dot(v) <= 0. || u.cross(v).norm() > a.tolerance * std::max(u.norm(), v.norm()))
            corners.push_back(loop[i].from);
        area += (p - origin).cross(a.mesh.vertices[loop[i].to] - origin);
    }
    if (corners.size() < 24 || std::abs(area.dot(normal)) <= a.tolerance * a.tolerance)
        return;
    const bool inner = area.dot(normal) < 0.;
    const Vec3 x = (a.mesh.vertices[corners[1]] - a.mesh.vertices[corners[0]]).normalized();
    const Vec3 y = normal.cross(x).normalized();
    std::vector<Vec2> xy;
    Vec2 mean = Vec2::Zero();
    for (int i : corners) {
        const Vec3 p = a.mesh.vertices[i] - origin;
        xy.emplace_back(p.dot(x), p.dot(y));
        mean += xy.back();
    }
    mean /= double(xy.size());
    Eigen::Matrix2d fit = Eigen::Matrix2d::Zero();
    Vec2 rhs = Vec2::Zero();
    for (const Vec2 &p : xy) {
        const Vec2 q = p - mean;
        fit += q * q.transpose();
        rhs += q.squaredNorm() * q;
    }
    if (fit.determinant() <= 0.)
        return;
    const Vec2 c = mean + 0.5 * (fit.inverse() * rhs);
    double radius = 0.;
    for (const Vec2 &p : xy)
        radius += (p - c).norm();
    radius /= xy.size();
    if (!std::isfinite(radius) || radius <= a.tolerance)
        return;
    double angle_sum = 0.;
    for (size_t i = 0; i < xy.size(); ++i) {
        const Vec2 u = xy[i] - c, v = xy[(i + 1) % xy.size()] - c;
        const double angle = std::atan2((inner ? -1. : 1.) * (u.x() * v.y() - u.y() * v.x()), u.dot(v));
        if (std::abs(u.norm() - radius) > std::max(3. * a.tolerance, 0.002 * radius) ||
            angle <= 0. || angle > pi / 12. + 1e-5)
            return;
        angle_sum += angle;
    }
    if (std::abs(angle_sum - 2. * pi) > 1e-5)
        return;

    Edge e;
    e.kind = Kind::Rim;
    e.normal = normal;
    e.center = origin + c.x() * x + c.y() * y;
    e.corners = std::move(corners);
    e.patches.push_back(patch);
    e.retreat = e.depth = std::numeric_limits<double>::infinity();
    for (const Segment &s : loop) {
        const int wall = a.face_patch[s.other_face];
        const Vec3 &wall_normal = a.patch_normals[wall];
        const Vec3 p = a.mesh.vertices[s.from], q = a.mesh.vertices[s.to];
        if (std::abs(wall_normal.dot(normal)) > a.tolerance / std::max(radius, 1.) ||
            (inner ? 1. : -1.) * wall_normal.dot(e.center - p) <= a.tolerance)
            return;
        e.patches.push_back(wall);
        if (inner) {
            e.depth = std::min(e.depth, strip_limit(a, borders[wall], s.from, s.to, -normal, {patch}, true));
            const Vec3 outward = normal.cross(q - p).normalized();
            e.retreat = std::min(e.retreat, strip_limit(a, borders[patch], s.from, s.to, outward, {}));
        }
    }
    for (size_t i = 1; i < e.patches.size(); ++i)
        curved[e.patches[i]] = true;
    if (inner && std::isfinite(e.depth) && e.depth > a.tolerance && e.retreat > a.tolerance) {
        e.segments = loop;
        a.edges.push_back(std::move(e));
    }
}

void rims(Analysis &out, const Borders &borders, std::vector<bool> &curved, const Cancelled &cancelled)
{
    for (int patch = 0; patch < int(borders.size()); ++patch) {
        if (cancelled())
            return;
        const auto &boundary = borders[patch];
        if (boundary.size() < 24)
            continue;
        std::map<int, int> next;
        for (int i = 0; i < int(boundary.size()); ++i) {
            auto [it, unique] = next.emplace(boundary[i].from, i);
            if (!unique)
                it->second = -1;
        }
        std::vector<bool> used(boundary.size(), false);
        for (int i = 0; i < int(boundary.size()); ++i) {
            if (used[i])
                continue;
            std::vector<Segment> loop;
            int current = i;
            do {
                used[current] = true;
                const Segment &s = boundary[current];
                loop.push_back(s);
                const auto it = next.find(s.to);
                current = it == next.end() ? -1 : it->second;
            } while (current >= 0 && !used[current]);
            if (current == i)
                recognize_rim(out, borders, patch, loop, curved);
        }
    }
}

void straight_edges(Analysis &out, const Borders &borders, const std::vector<std::vector<int>> &incident,
    const std::vector<bool> &curved, const Cancelled &cancelled)
{
    const auto &v = out.mesh.vertices;
    for (int p = 0; p < int(borders.size()); ++p) {
        if (cancelled())
            return;
        if (curved[p])
            continue;
        std::map<int, std::vector<Segment>> groups;
        const Vec3 &n = out.patch_normals[p];
        for (const Segment &s : borders[p]) {
            const int other = out.face_patch[s.other_face];
            if (other <= p || curved[other] || std::abs(n.dot(out.patch_normals[other])) >= plane_angle)
                continue;
            const auto &f = out.mesh.indices[s.other_face];
            for (int k = 0; k < 3; ++k) {
                const int i = f[k];
                if (i != s.from && i != s.to && n.dot(v[i] - v[s.from]) < -out.tolerance)
                    groups[other].push_back(s);
            }
        }
        for (const auto &[q, segments] : groups) {
            const Vec3 &m = out.patch_normals[q];
            std::map<int, std::vector<size_t>> at;
            for (size_t i = 0; i < segments.size(); ++i) {
                at[segments[i].from].push_back(i);
                at[segments[i].to].push_back(i);
            }
            std::vector<bool> used(segments.size(), false);
            for (const auto &[start, hits] : at) {
                if (hits.size() != 1 || used[hits.front()])
                    continue;
                Edge e;
                e.patches = {p, q};
                int end = start;
                do {
                    const auto &options = at.at(end);
                    const auto next = std::find_if(options.begin(), options.end(), [&](size_t i) { return !used[i]; });
                    if (next == options.end())
                        break;
                    used[*next] = true;
                    Segment s = segments[*next];
                    if (s.from != end)
                        std::swap(s.from, s.to);
                    e.segments.push_back(s);
                    end = s.to;
                } while (at.at(end).size() == 2);
                if (end == start || at.at(end).size() != 1)
                    continue;
                const Vec3 direction = (v[end] - v[start]).normalized();
                if ((v[end] - v[start]).norm() <= out.tolerance ||
                    std::any_of(e.segments.begin(), e.segments.end(), [&](const Segment &s) {
                        return (v[s.from] - v[start]).cross(direction).norm() > out.tolerance;
                    }))
                    continue;
                e.corners = {start, end};
                e.side_a = (-m + n * n.dot(m)).normalized();
                e.side_b = (-n + m * m.dot(n)).normalized();
                e.angle = std::acos(std::clamp(e.side_a.dot(e.side_b), -1., 1.));
                for (size_t i = 0; i < 2; ++i) {
                    std::set<int> ends;
                    for (int face : incident[e.corners[i]]) {
                        const int id = out.face_patch[face];
                        if (id != p && id != q)
                            ends.insert(id);
                    }
                    if (ends.size() == 1 &&
                        (i == 0 ? -1. : 1.) * out.patch_normals[*ends.begin()].dot(direction) > 0.01)
                        e.end_patches[i] = *ends.begin();
                }
                if (e.end_patches[0] < 0 || e.end_patches[1] < 0)
                    continue;
                const auto [cap_a, cap_b] = e.end_patches;
                e.retreat = std::min(strip_limit(out, borders[p], start, end, e.side_a, {q, cap_a, cap_b}),
                    strip_limit(out, borders[q], start, end, e.side_b, {p, cap_a, cap_b}));
                if (e.retreat <= out.tolerance)
                    continue;
                e.patches.insert(e.patches.end(), {cap_a, cap_b});
                out.edges.push_back(std::move(e));
            }
        }
    }
}

int arc_segments(double radius, double angle)
{
    // Reserve some of the 0.02 mm budget for seam and output rounding.
    const double half_step = 2. * std::asin(std::sqrt(std::min(1., 0.008 / radius)));
    return std::max(3, int(std::ceil(angle / (2. * half_step))));
}

void join_rings(Surface &mesh, int count, int first_fan_corner = 0)
{
    const int rings = int(mesh.vertices.size()) / count;
    for (int i = 0; i < rings - 1; ++i)
        for (int j = 0; j < count; ++j) {
            const int next = (j + 1) % count, k = i * count;
            mesh.indices.emplace_back(k + j, k + next, k + count + next);
            mesh.indices.emplace_back(k + j, k + count + next, k + count + j);
        }
    const int end = (rings - 1) * count;
    for (int j = 1; j < count - 1; ++j) {
        const int i0 = first_fan_corner, i1 = (i0 + j) % count, i2 = (i0 + j + 1) % count;
        mesh.indices.emplace_back(i0, i2, i1);
        mesh.indices.emplace_back(end + i0, end + i1, end + i2);
    }
    if (signed_volume(mesh) < 0.)
        for (auto &f : mesh.indices)
            std::swap(f[1], f[2]);
}

} // namespace

std::shared_ptr<Analysis> analyze(const indexed_triangle_set &mesh, const Eigen::Matrix3d &linear, const Cancelled &cancelled)
{
    if (mesh.indices.size() < 4 || mesh.vertices.size() < 4 || !linear.allFinite() || linear.determinant() == 0.)
        return {};
    auto out = std::make_shared<Analysis>();
    out->local_origin = mesh.vertices.front().cast<double>();
    // Strip the rigid part of the transform before building exact planes.
    // Transforming a coplanar STL into world space first can introduce slivers.
    const Eigen::Matrix3d gram = linear.transpose() * linear;
    if (!gram.allFinite())
        return {};
    Eigen::Matrix3d metric = gram.diagonal().cwiseSqrt().asDiagonal();
    const Eigen::Matrix3d diagonal = gram.diagonal().asDiagonal();
    if ((gram - diagonal).norm() > 32. * std::numeric_limits<double>::epsilon() * diagonal.norm()) {
        const Eigen::JacobiSVD<Eigen::Matrix3d> svd(linear, Eigen::ComputeFullV);
        metric = svd.matrixV() * svd.singularValues().asDiagonal() * svd.matrixV().transpose();
    }
    out->from_work = metric.inverse();
    if (!out->from_work.allFinite())
        return {};
    out->mirrored = linear.determinant() < 0.;
    out->mesh.indices = mesh.indices;
    Vec3 lower = Vec3::Zero(), upper = Vec3::Zero();
    for (const stl_vertex &v : mesh.vertices) {
        const Vec3 p = metric * (v.cast<double>() - out->local_origin);
        if (!p.allFinite())
            return {};
        lower = lower.cwiseMin(p);
        upper = upper.cwiseMax(p);
        out->mesh.vertices.push_back(p);
    }
    out->tolerance = std::max(1e-5, (upper - lower).norm() * 8. * std::numeric_limits<float>::epsilon());
    if (!std::isfinite(out->tolerance))
        return {};
    Neighbors neighbors;
    std::vector<std::vector<int>> incident;
    Borders borders;
    if (!topology(out->mesh, neighbors, incident, out->euler) || signed_volume(out->mesh) <= 0. ||
        !planar_patches(*out, neighbors, borders, cancelled))
        return {};
    std::vector<bool> curved(borders.size(), false);
    rims(*out, borders, curved, cancelled);
    straight_edges(*out, borders, incident, curved, cancelled);
    if (cancelled())
        return {};
    const double extent = (upper - lower).norm();
    for (Edge &edge : out->edges)
        edge.retreat = std::min(edge.retreat, extent);
    out->facet_segments.resize(mesh.indices.size());
    for (size_t i = 0; i < out->edges.size(); ++i)
        for (size_t j = 0; j < out->edges[i].segments.size(); ++j) {
            const Segment &s = out->edges[i].segments[j];
            for (int f : {s.face, s.other_face}) {
                out->facet_segments[f].emplace_back(i, j);
                for (int next : neighbors[f])
                    out->facet_segments[next].emplace_back(i, j);
            }
        }
    return out;
}

static int joint_support_plane(const Edge &a, const Edge &b, int vertex)
{
    if (&a == &b || a.kind != Kind::Straight || b.kind != Kind::Straight)
        return -1;
    const auto end_a = std::find(a.corners.begin(), a.corners.end(), vertex);
    const auto end_b = std::find(b.corners.begin(), b.corners.end(), vertex);
    if (end_a == a.corners.end() || end_b == b.corners.end())
        return -1;
    const int cap_a = a.end_patches[end_a - a.corners.begin()];
    const int cap_b = b.end_patches[end_b - b.corners.begin()];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (a.patches[i] == b.patches[j] && a.patches[1 - i] == cap_b && b.patches[1 - j] == cap_a)
                return a.patches[i];
    return -1;
}

Limits limits(const Analysis &a, const std::vector<size_t> &edges, Operation operation)
{
    Limits result;
    std::map<int, size_t> used;
    int joint_plane = -1;
    if (edges.empty())
        result.error = Error::NoEdges;
    for (size_t i = 0; i < edges.size(); ++i) {
        const size_t id = edges[i];
        const Edge &edge = a.edges.at(id);
        for (const Segment &s : edge.segments)
            for (int vertex : {s.from, s.to})
                if (const auto it = used.find(vertex); it != used.end()) {
                    const int plane = joint_support_plane(edge, a.edges[it->second], vertex);
                    if (plane < 0 || (joint_plane >= 0 && joint_plane != plane))
                        return {0., id, Error::Adjacent, it->second};
                    joint_plane = plane;
                    result.planar_joints.emplace_back(it->second, id);
                }
        for (const Segment &s : edge.segments) {
            used.emplace(s.from, id);
            used.emplace(s.to, id);
        }
        std::optional<size_t> other_edge;
        double max_size = edge.retreat - a.tolerance;
        if (edge.kind == Kind::Straight && operation == Operation::Fillet)
            max_size *= std::tan(0.5 * edge.angle);
        if (edge.kind == Kind::Rim) {
            max_size = std::min(max_size, edge.depth - 5. * a.tolerance);
            for (size_t j = 0; j < i; ++j) {
                const Edge &other = a.edges.at(edges[j]);
                if (other.kind != Kind::Rim || other.normal.dot(edge.normal) > -plane_angle)
                    continue;
                const Vec3 delta = other.center - edge.center;
                if (delta.cross(edge.normal).norm() > 3. * a.tolerance)
                    continue;
                const bool same_bore = std::any_of(edge.patches.begin() + 1, edge.patches.end(), [&](int patch) {
                    return std::find(other.patches.begin() + 1, other.patches.end(), patch) != other.patches.end();
                });
                const double pair_limit = 0.5 * std::abs(delta.dot(edge.normal)) - a.tolerance;
                if (same_bore && pair_limit < max_size) {
                    max_size = pair_limit;
                    other_edge = edges[j];
                }
            }
        }
        if (max_size < result.maximum) {
            result.maximum = max_size;
            result.limiting_edge = id;
            result.other_edge = other_edge;
        }
    }
    return result;
}

bool points_span_edge(const Analysis &a, size_t id, const std::vector<Vec3> &points)
{
    const Edge &edge = a.edges.at(id);
    if (edge.kind == Kind::Straight) {
        const double length = (a.mesh.vertices[edge.corners[1]] - a.mesh.vertices[edge.corners[0]]).norm();
        return points.size() == 2 && (points[0] - points[1]).norm() >= std::max(4. * a.tolerance, 0.10 * length);
    }
    if (points.size() != 3)
        return false;
    double largest = 0.;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3 u = (points[i] - edge.center).normalized();
        const Vec3 v = (points[(i + 1) % points.size()] - edge.center).normalized();
        const double angle = std::acos(std::clamp(u.dot(v), -1., 1.));
        if (angle < pi / 12.)
            return false;
        largest = std::max(largest, angle);
    }
    return largest >= pi / 3.;
}

Surface make_cutter(const Analysis &a, size_t id, Operation operation, double size,
    std::pair<size_t, size_t> &profile_faces)
{
    const Edge &edge = a.edges.at(id);
    const bool fillet = operation == Operation::Fillet;
    const double pad = 4. * a.tolerance;
    Surface result;
    if (edge.kind == Kind::Rim) {
        const int count = int(edge.corners.size());
        std::vector<Vec3> offset;
        for (int i = 0; i < count; ++i) {
            const Vec3 &p = a.mesh.vertices[edge.corners[i]];
            const Vec3 before = edge.normal.cross(p - a.mesh.vertices[edge.corners[(i + count - 1) % count]]).normalized();
            const Vec3 after = edge.normal.cross(a.mesh.vertices[edge.corners[(i + 1) % count]] - p).normalized();
            offset.push_back((before + after) / (1. + before.dot(after)));
        }
        // Keep the measured bore polygon at the tangency seam.
        auto append = [&](double depth, double retreat) {
            for (int i = 0; i < count; ++i)
                result.vertices.push_back(a.mesh.vertices[edge.corners[i]] - depth * edge.normal + retreat * offset[i]);
        };
        append(-pad, size);
        const int n = fillet ? arc_segments(size, 0.5 * pi) : 1;
        for (int i = 0; i < n; ++i) {
            const double t = 0.5 * pi * i / n;
            append(size * (1. - std::cos(t)), size * (1. - std::sin(t)));
        }
        append(size, 0.);
        append(size + pad, -pad);
        join_rings(result, count);
        profile_faces = {2 * size_t(count), 2 * size_t(count) * (n + 1)};
    } else {
        const Vec3 normal_a = a.patch_normals[edge.patches[0]], normal_b = a.patch_normals[edge.patches[1]];
        const double setback = fillet ? size / std::tan(0.5 * edge.angle) : size;
        const Vec3 p = setback * edge.side_a, q = setback * edge.side_b;
        std::vector<Vec3> profile{p};
        if (fillet) {
            const Vec3 center = (edge.side_a + edge.side_b).normalized() * (size / std::sin(0.5 * edge.angle));
            const Vec3 u = (p - center).normalized(), v = (q - center).normalized();
            const double angle = pi - edge.angle;
            const int n = arc_segments(size, angle);
            for (int i = 1; i < n; ++i) {
                const double t = angle * i / n;
                profile.push_back(center + (size / std::sin(angle)) * (std::sin(angle - t) * u + std::sin(t) * v));
            }
        }
        profile.push_back(q);
        profile_faces = {0, 2 * (profile.size() - 1)};
        profile.push_back(q + pad * normal_b);
        const int cap_corner = int(profile.size());
        profile.push_back(pad * (normal_a + normal_b).normalized());
        profile.push_back(p + pad * normal_a);
        const Vec3 start = a.mesh.vertices[edge.corners[0]], end = a.mesh.vertices[edge.corners[1]];
        const Vec3 axis = (end - start).normalized(), cap_a = a.patch_normals[edge.end_patches[0]],
            cap_b = a.patch_normals[edge.end_patches[1]];
        for (const Vec3 &c : profile)
            result.vertices.push_back(start + c + ((pad - c.dot(cap_a)) / axis.dot(cap_a)) * axis);
        for (size_t i = 0; i < profile.size(); ++i) {
            const Vec3 &c = profile[i];
            const Vec3 last = end + c + ((pad - c.dot(cap_b)) / axis.dot(cap_b)) * axis;
            if ((last - result.vertices[i]).dot(axis) <= a.tolerance)
                return {};
            result.vertices.push_back(last);
        }
        join_rings(result, int(profile.size()), cap_corner);
    }
    return result;
}

Surface make_rectangular_perimeter_cutter(const Analysis &a, const std::vector<size_t> &ids,
    Operation operation, double size, std::pair<size_t, size_t> &profile_faces)
{
    if (ids.size() != 4)
        return {};
    const Edge &first = a.edges[ids.front()];
    int shared_patch = -1;
    for (size_t i = 0; i < 2; ++i) {
        const int patch = first.patches[i];
        if (std::all_of(ids.begin(), ids.end(), [&](size_t id) {
            const Edge &e = a.edges[id];
            return e.kind == Kind::Straight && (e.patches[0] == patch || e.patches[1] == patch) &&
                std::abs(e.angle - pi / 2.) < 1e-10;
        }))
            shared_patch = patch;
    }
    if (shared_patch < 0)
        return {};
    const Vec3 normal = a.patch_normals[shared_patch], origin = a.mesh.vertices[first.corners.front()];
    const Vec3 u = (a.mesh.vertices[first.corners.back()] - origin).normalized(), v = normal.cross(u).normalized();
    const double precision = std::min(1e-7, a.tolerance / 32.);
    if (std::abs(u.dot(normal)) > 1e-10)
        return {};
    std::array<Eigen::Vector2d, 8> points;
    Eigen::Vector2d lo = Eigen::Vector2d::Zero(), hi = Eigen::Vector2d::Zero();
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t end = 0; end < 2; ++end) {
            const Vec3 p = a.mesh.vertices[a.edges[ids[i]].corners[end]] - origin;
            if (std::abs(p.dot(normal)) > precision)
                return {};
            auto &projected = points[2 * i + end];
            projected = Eigen::Vector2d(u.dot(p), v.dot(p));
            lo = lo.cwiseMin(projected);
            hi = hi.cwiseMax(projected);
        }
    if ((hi - lo).minCoeff() <= 2. * size + a.tolerance)
        return {};
    std::array<bool, 4> found{};
    for (size_t i = 0; i < points.size(); i += 2)
        for (int axis = 0; axis < 2; ++axis) {
            const auto &p = points[i], &q = points[i + 1];
            const int other = 1 - axis;
            if (std::abs(std::abs(p[other] - q[other]) - (hi[other] - lo[other])) > precision)
                continue;
            for (int side = 0; side < 2; ++side) {
                const double boundary = side == 0 ? lo[axis] : hi[axis];
                if (std::abs(p[axis] - boundary) <= precision && std::abs(q[axis] - boundary) <= precision)
                    found[2 * axis + side] = true;
            }
        }
    if (std::find(found.begin(), found.end(), false) != found.end())
        return {};

    Surface result;
    auto add_ring = [&](double depth, double inset) {
        const int start = int(result.vertices.size());
        const Vec3 base = origin - depth * normal;
        const std::array<Eigen::Vector2d, 4> corners = {Eigen::Vector2d(lo.x() + inset, lo.y() + inset),
            Eigen::Vector2d(hi.x() - inset, lo.y() + inset), Eigen::Vector2d(hi.x() - inset, hi.y() - inset),
            Eigen::Vector2d(lo.x() + inset, hi.y() - inset)};
        for (const Eigen::Vector2d &p : corners)
            result.vertices.emplace_back(base + p.x() * u + p.y() * v);
        return start;
    };
    auto join = [&](int start, int end) {
        for (int i = 0; i < 4; ++i) {
            const int next = (i + 1) % 4;
            result.indices.emplace_back(start + i, start + next, end + next);
            result.indices.emplace_back(start + i, end + next, end + i);
        }
    };
    const double pad = 4. * a.tolerance;
    const int outer_top = add_ring(-pad, -pad), top = add_ring(-pad, size);
    int previous = add_ring(0., size);
    join(outer_top, top);
    join(top, previous);
    profile_faces.first = result.indices.size();
    const int n = operation == Operation::Fillet ? arc_segments(size, pi / 2.) : 1;
    for (int i = 1; i <= n; ++i) {
        const double t = (pi / 2.) * i / n;
        const int current = add_ring(i == n ? size : size * (1. - std::cos(t)), i == n ? 0. : size * (1. - std::sin(t)));
        join(previous, current);
        previous = current;
    }
    profile_faces.second = result.indices.size();
    const int bottom = add_ring(size + pad, -pad / 2.), outer_bottom = add_ring(size + pad, -pad);
    join(previous, bottom);
    join(bottom, outer_bottom);
    join(outer_bottom, outer_top);
    return result;
}

} // namespace Slic3r::MeshEdgeFinish
