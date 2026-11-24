///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "libslic3r/MeshEdgeFinish.hpp"
#ifdef EDGE_FINISH_TEST_PRINT_CONFIG
#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace Slic3r::MeshEdgeFinish;

constexpr double pi = 3.14159265358979323846;
const auto never_cancel = [] { return false; };

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

void quad(indexed_triangle_set &mesh, int a, int b, int c, int d)
{
    mesh.indices.emplace_back(a, b, c);
    mesh.indices.emplace_back(a, c, d);
}

indexed_triangle_set prism(const std::vector<Eigen::Vector2f> &profile)
{
    indexed_triangle_set mesh;
    const int n = int(profile.size());
    for (float z : {0.f, 10.f, 20.f})
        for (const auto &p : profile)
            mesh.vertices.emplace_back(p.x(), p.y(), z);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        quad(mesh, i, j, n + j, n + i);
        quad(mesh, n + i, n + j, 2 * n + j, 2 * n + i);
    }
    for (int i = 1; i + 1 < n; ++i) {
        mesh.indices.emplace_back(0, i + 1, i);
        mesh.indices.emplace_back(2 * n, 2 * n + i, 2 * n + i + 1);
    }
    return mesh;
}

indexed_triangle_set plate(int n, bool through)
{
    indexed_triangle_set mesh;
    for (int ring = 0; ring < 4; ++ring)
        for (int i = 0; i < n; ++i) {
            const double t = 2. * pi * i / n;
            const double x = std::cos(t), y = std::sin(t);
            const double r = ring < 2 ? 16. / std::max(std::abs(x), std::abs(y)) : 5.;
            mesh.vertices.emplace_back(float(r * x), float(r * y), ring == 0 ? 0.f : ring == 3 ? (through ? 0.f : 2.f) : 6.f);
        }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        quad(mesh, i, j, n + j, n + i);
        quad(mesh, n + i, n + j, 2 * n + j, 2 * n + i);
        quad(mesh, 2 * n + i, 2 * n + j, 3 * n + j, 3 * n + i);
        if (through)
            quad(mesh, j, i, 3 * n + i, 3 * n + j);
    }
    if (!through)
        for (int i = 1; i + 1 < n; ++i) {
            mesh.indices.emplace_back(3 * n, 3 * n + i, 3 * n + i + 1);
        }
    if (!through) {
        const int center = int(mesh.vertices.size());
        mesh.vertices.emplace_back(0.f, 0.f, 0.f);
        for (int i = 0; i < n; ++i)
            mesh.indices.emplace_back(center, (i + 1) % n, i);
    }
    return mesh;
}

std::vector<size_t> rims(const Analysis &a)
{
    std::vector<size_t> ids;
    for (size_t i = 0; i < a.edges.size(); ++i)
        if (a.edges[i].kind == Kind::Rim)
            ids.push_back(i);
    return ids;
}

size_t upright_at(const Analysis &a, double x, double y)
{
    for (size_t i = 0; i < a.edges.size(); ++i) {
        const Edge &e = a.edges[i];
        const Vec3 p = a.mesh.vertices[e.corners.front()], q = a.mesh.vertices[e.corners.back()];
        if (e.kind == Kind::Straight && std::abs(p.x() - x) < 1e-6 && std::abs(p.y() - y) < 1e-6 &&
            (p - q).head<2>().norm() < 1e-6 && std::abs(p.z() - q.z()) > 19.)
            return i;
    }
    throw std::runtime_error("missing vertical straight edge");
}

void check_geometry()
{
    const auto box = prism({{0.f, 0.f}, {40.f, 0.f}, {40.f, 30.f}, {0.f, 30.f}});
    const auto a = analyze(box, Eigen::Matrix3d::Identity(), never_cancel);
    check(a && a->edges.size() == 12, "box must have 12 edges despite triangle diagonals and subdivisions");
    const size_t first = upright_at(*a, 0., 0.);
    check(a->edges[first].segments.size() == 2, "two collinear mesh segments must be one edge");
    check(points_span_edge(*a, first, {{0., 0., 4.}, {0., 0., 16.}}), "spaced points on straight edge");
    check(!points_span_edge(*a, first, {{0., 0., 4.}, {0., 0., 4.1}}), "reject near-duplicate points");
    check(std::abs(limits(*a, {first}, Operation::Fillet).maximum - 30.) < 0.001, "box setback limit");
    const auto skew = prism({{0.f, 0.f}, {3.f, 0.f}, {1.5f, float(1.5 * std::sqrt(3.))}});
    const auto skew_a = analyze(skew, Eigen::Matrix3d::Identity(), never_cancel);
    check(skew_a && std::abs(skew_a->edges[upright_at(*skew_a, 0., 0.)].angle - pi / 3.) < 1e-6, "acute corner angle");
    const auto wire = analyze(plate(24, true), Eigen::Matrix3d::Identity(), never_cancel);
    check(wire && rims(*wire).size() == 2, "24-segment through hole has independently selectable rims");
    const auto ids = rims(*wire);
    const Limits paired_limit = limits(*wire, ids, Operation::Fillet);
    check(std::abs(paired_limit.maximum - 3.) < 0.001 && paired_limit.other_edge == ids.front(),
        "paired rims share the usable bore depth and report both edges");
    const auto &rim = wire->edges[ids.front()];
    check(points_span_edge(*wire, ids.front(), {wire->mesh.vertices[rim.corners[0]], wire->mesh.vertices[rim.corners[8]],
        wire->mesh.vertices[rim.corners[16]]}), "three separated points on a hole");
    check(!points_span_edge(*wire, ids.front(), {wire->mesh.vertices[rim.corners[0]], wire->mesh.vertices[rim.corners[1]],
        wire->mesh.vertices[rim.corners[2]]}), "reject neighboring samples within a short arc");
    const auto blind = analyze(plate(64, false), Eigen::Matrix3d::Identity(), never_cancel);
    check(blind && rims(*blind).size() == 1, "blind hole floor is not an opening");
    check(std::abs(limits(*blind, rims(*blind), Operation::Chamfer).maximum - 4.) < 0.001, "blind hole axial limit");
    const auto hex = analyze(plate(6, true), Eigen::Matrix3d::Identity(), never_cancel);
    check(hex && rims(*hex).empty(), "reject hexagon despite exact circumcircle");
    const Eigen::Matrix3d stretch = Eigen::Vector3d(1., 2., 1.).asDiagonal();
    const auto ellipse = analyze(plate(64, true), stretch, never_cancel);
    check(ellipse && rims(*ellipse).empty(), "reject physically elliptical hole after scaling");
    const Eigen::Matrix3d reflected = Eigen::Vector3d(-2., 2., 1.).asDiagonal();
    const auto mirror = analyze(box, reflected, never_cancel);
    check(mirror && mirror->mirrored && std::abs(limits(*mirror, {upright_at(*mirror, 0., 0.)}, Operation::Chamfer).maximum - 60.) < 0.001,
        "mirrored scaled coordinates use actual printing dimensions");
    auto open = box;
    open.indices.pop_back();
    check(!analyze(open, Eigen::Matrix3d::Identity(), never_cancel), "reject open mesh");
}

#ifndef EDGE_FINISH_GEOMETRY_ONLY
double volume(const indexed_triangle_set &mesh)
{
    double sum = 0.;
    for (const auto &f : mesh.indices) {
        const Vec3 a = mesh.vertices[f[0]].cast<double>(), b = mesh.vertices[f[1]].cast<double>(), c = mesh.vertices[f[2]].cast<double>();
        sum += a.dot(b.cross(c));
    }
    return sum / 6.;
}

double bore(const indexed_triangle_set &mesh, double z)
{
    const Vec3 origin(0., 0.321, z);
    double first = std::numeric_limits<double>::infinity();
    for (const auto &f : mesh.indices) {
        const Vec3 a = mesh.vertices[f[0]].cast<double>(), u = mesh.vertices[f[1]].cast<double>() - a,
            v = mesh.vertices[f[2]].cast<double>() - a;
        const Vec3 normal = u.cross(v);
        if (std::abs(normal.x()) < 1e-10)
            continue;
        const double x = normal.dot(a - origin) / normal.x();
        if (x <= 0. || x >= first)
            continue;
        const Vec3 disp = origin + x * Vec3::UnitX() - a;
        const double s = disp.cross(v).dot(normal) / normal.squaredNorm();
        const double t = u.cross(disp).dot(normal) / normal.squaredNorm();
        if (s >= -1e-9 && t >= -1e-9 && s + t <= 1. + 1e-9)
            first = x;
    }
    return first;
}

Result require_finish(const Analysis &a, const std::vector<size_t> &edges, Operation op, double size, double ref = 0.)
{
    Result result = finish(a, edges, op, size, ref, never_cancel);
    if (result.error != Error::None)
        throw std::runtime_error("unexpected machining failure " + std::to_string(int(result.error)) + " on edge " + std::to_string(result.edge));
    return result;
}

void check_booleans()
{
    const auto box = prism({{0.f, 0.f}, {40.f, 0.f}, {40.f, 30.f}, {0.f, 30.f}});
    const auto a = analyze(box, Eigen::Matrix3d::Identity(), never_cancel);
    const size_t first = upright_at(*a, 0., 0.), opposite = upright_at(*a, 40., 30.);
    const auto chamfer = require_finish(*a, {first, opposite}, Operation::Chamfer, 2.);
    check(std::abs(volume(box) - volume(chamfer.mesh) - 80.) < 0.001, "two 2 mm chamfers remove two known triangular prisms");
    for (const auto &input : {box, plate(64, true)}) {
        const auto solid = analyze(input, Eigen::Matrix3d::Identity(), never_cancel);
        const bool is_box = input.indices.size() == box.indices.size();
        const double top_z = is_box ? 20. : 6.;
        std::vector<size_t> perimeter;
        for (size_t i = 0; i < solid->edges.size(); ++i) {
            const Edge &edge = solid->edges[i];
            if (edge.kind == Kind::Straight && std::all_of(edge.corners.begin(), edge.corners.end(), [&](int corner) {
                return std::abs(solid->mesh.vertices[corner].z() + solid->local_origin.z() - top_z) < 1e-6;
            }))
                perimeter.push_back(i);
        }
        check(perimeter.size() == 4, "rectangular top has four straight edges");
        const double size = is_box ? 2. : 0.5;
        check(limits(*solid, perimeter, Operation::Chamfer).planar_joints.size() == 4, "allow four chamfer miters on one face");
        check(limits(*solid, perimeter, Operation::Fillet).planar_joints.size() == 4, "allow four fillet miters on one face");
        if (is_box) {
            auto three_way_corner = perimeter;
            three_way_corner.push_back(first);
            check(limits(*solid, three_way_corner, Operation::Chamfer).error == Error::Adjacent,
                "reject a triple chamfer at one corner");
            check(limits(*solid, three_way_corner, Operation::Fillet).error == Error::Adjacent,
                "reject a triple fillet at one corner");
        }
        auto batch = require_finish(*solid, perimeter, Operation::Chamfer, size);
        const double removed = (is_box ? 70. : 64.) * size * size - (4. / 3.) * size * size * size;
        check(std::abs(volume(input) - volume(batch.mesh) - removed) < 0.003, "perimeter chamfers meet in equal-setback miters");
        check(batch.cut_faces.size() == batch.mesh.indices.size() &&
            std::find(batch.cut_faces.begin(), batch.cut_faces.end(), true) != batch.cut_faces.end() &&
            std::find(batch.cut_faces.begin(), batch.cut_faces.end(), false) != batch.cut_faces.end(),
            "chamfer triangles and retained faces are tagged separately");
        double differently_shaded_area = 0.;
        for (size_t f = 0; f < batch.mesh.indices.size(); ++f) {
            const auto &triangle = batch.mesh.indices[f];
            const auto &p = batch.mesh.vertices[triangle[0]];
            const Vec3 twice_area = (batch.mesh.vertices[triangle[1]] - p).cast<double>()
                .cross((batch.mesh.vertices[triangle[2]] - p).cast<double>());
            const Vec3 n = twice_area.normalized();
            const bool is_miter = std::abs(n.z()) > 1e-5 && n.head<2>().norm() > 1e-5;
            if (batch.cut_faces[f] != is_miter)
                differently_shaded_area += 0.5 * twice_area.norm();
        }
        // Very small corner triangles can change normal when their vertices round to float.
        if (differently_shaded_area >= 0.001)
            throw std::runtime_error("perimeter preview color disagrees with the chamfer area by " + std::to_string(differently_shaded_area));
        const auto rounded = require_finish(*solid, perimeter, Operation::Fillet, size);
        const double lengths = 2. * (is_box ? 70. : 64.);
        const double exact_removed = lengths * size * size * (1. - pi / 4.) - 4. * size * size * size * (5. / 3. - pi / 2.);
        const double rounded_removed = volume(input) - volume(rounded.mesh);
        check(rounded_removed >= exact_removed - 0.001 && rounded_removed <= exact_removed + lengths * size * 0.02,
            "four radius profiles meet at miters within the tessellation tolerance");
        check(std::find(rounded.cut_faces.begin(), rounded.cut_faces.end(), true) != rounded.cut_faces.end(),
            "mark the four fillet bands for preview");
        if (!is_box) {
            for (auto part : {input, batch.mesh}) {
                const auto setup = analyze(part, Eigen::Matrix3d::Identity(), never_cancel);
                const auto bottom = std::find_if(setup->edges.begin(), setup->edges.end(), [&](const Edge &edge) {
                    return edge.kind == Kind::Straight && std::all_of(edge.corners.begin(), edge.corners.end(), [&](int v) {
                        return std::abs(setup->mesh.vertices[v].z() + setup->local_origin.z()) < 1e-6;
                    });
                });
                check(bottom != setup->edges.end(), "recognize the bottom outer edge after machining the top");
                const auto cut = require_finish(*setup, {size_t(bottom - setup->edges.begin())}, Operation::Fillet, 2., 0.42);
                const double removed = volume(part) - volume(cut.mesh);
                const double exact_removed = 32. * 4. * (1. - pi / 4.);
                check(removed >= exact_removed - 0.001 && removed <= exact_removed + 32. * 2. * 0.02,
                    "store the requested 2 mm single-edge radius as a solid");
                check(std::abs(bore(cut.mesh, 3.25) - bore(part, 3.25)) < 1e-5, "single-edge repair preserves the bore");
            }
        }
    }
    const auto fillet = require_finish(*a, {first}, Operation::Fillet, 2.);
    check(std::abs(volume(box) - volume(fillet.mesh) - 80. * (1. - pi / 4.)) < 0.7, "straight round matches a 2 mm radius");
    const Eigen::Matrix3d rotation = Eigen::AngleAxisd(0.43, Eigen::Vector3d(1., 2., 3.).normalized()).toRotationMatrix();
    const Eigen::Matrix3d pose = rotation * Eigen::Vector3d(-2., 2., 1.).asDiagonal();
    const auto posed_box = analyze(box, pose, never_cancel);
    const auto vertical = std::find_if(posed_box->edges.begin(), posed_box->edges.end(), [&](const Edge &e) {
        return e.kind == Kind::Straight && e.segments.size() == 2 &&
            (posed_box->mesh.vertices[e.corners[0]].norm() < 1e-8 || posed_box->mesh.vertices[e.corners[1]].norm() < 1e-8);
    });
    check(vertical != posed_box->edges.end(), "rotated reflected straight edge");
    const auto posed_cut = require_finish(*posed_box, {size_t(vertical - posed_box->edges.begin())}, Operation::Chamfer, 2.);
    check(std::abs(volume(box) - volume(posed_cut.mesh) - 10.) < 0.003, "2 mm remains 2 mm in rotated scaled mirrored instance");
    for (double s : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        check(finish(*a, {first}, Operation::Chamfer, s, 0., never_cancel).error == Error::InvalidSize, "reject invalid size");
    const double limit = limits(*a, {first}, Operation::Chamfer).maximum;
    const double near_limit = limit - 5. * a->tolerance;
    const auto large = require_finish(*a, {first}, Operation::Chamfer, near_limit);
    check(std::abs(volume(box) - volume(large.mesh) - 10. * near_limit * near_limit) < 0.01,
        "a legal cut just below the upper limit uses its submitted size");
    check(finish(*a, {first}, Operation::Chamfer, limit + a->tolerance, 0., never_cancel).error == Error::TooLarge,
        "reject a size just above the limit");
    check(finish(*a, {first}, Operation::Chamfer, 30., 0., never_cancel).error == Error::TooLarge, "reject protected wall width");
    check(finish(*a, {first, first}, Operation::Chamfer, 1., 0., never_cancel).error == Error::Adjacent, "reject intersecting selection");
    check(finish(*a, {first}, Operation::Chamfer, 1., 0., [] { return true; }).error == Error::Cancelled, "cancel without result");
    const auto small = analyze(prism({{0.f, 0.f}, {6.f, 0.f}, {6.f, 6.f}, {0.f, 6.f}}), Eigen::Matrix3d::Identity(), never_cancel);
    check(finish(*small, {upright_at(*small, 0., 0.), upright_at(*small, 6., 0.)}, Operation::Chamfer, 3.2, 0., never_cancel).error == Error::Overlap,
        "reject intersecting cuts on edges that do not share endpoints");
    for (bool through : {false, true})
        for (int segments : {24, 64}) {
            const auto input = plate(segments, through);
            const auto ring = analyze(input, Eigen::Matrix3d::Identity(), never_cancel);
            const auto picks = rims(*ring);
            for (Operation op : {Operation::Chamfer, Operation::Fillet}) {
                const Result edited = require_finish(*ring, picks, op, 1.);
                check(std::find(edited.cut_faces.begin(), edited.cut_faces.end(), true) != edited.cut_faces.end(),
                    "tag the hole chamfer or fillet band");
                check(volume(edited.mesh) < volume(input), "hole treatment removes material");
                check(std::abs(bore(edited.mesh, 3.25) - bore(input, 3.25)) < 1e-5, "the bore outside the cut stays the original polygon");
                check(bore(edited.mesh, 5.95) > bore(input, 5.95) + 0.5, "hole mouth widens");
            }
        }
    const auto blind = analyze(plate(64, false), Eigen::Matrix3d::Identity(), never_cancel);
    const auto soft = require_finish(*blind, rims(*blind), Operation::Chamfer, 3.8, 0.42);
    check(soft.thin_wall, "thin material gives a warning without blocking Apply");
    const auto tilted_input = plate(32, true);
    const auto tilted = analyze(tilted_input, rotation, never_cancel);
    check(tilted && rims(*tilted).size() == 2, "a rotated hole retains its two rims");
    const auto tilted_round = require_finish(*tilted, rims(*tilted), Operation::Fillet, 1.);
    check(std::abs(bore(tilted_round.mesh, 3.25) - bore(tilted_input, 3.25)) < 1e-5, "rotated rim does not change lower bore");
}
#endif
} // namespace

int main()
{
    try {
        check_geometry();
#ifdef EDGE_FINISH_TEST_PRINT_CONFIG
        const std::string width_key = "external_perimeter_extrusion_width";
        const Slic3r::DynamicPrintConfig viewport_config;
        bool width_missing = false;
        try {
            Slic3r::Flow::extrusion_width(width_key, viewport_config, 0);
        } catch (const Slic3r::FlowErrorMissingVariable &) {
            width_missing = true;
        }
        check(width_missing, "a viewport config without the advisory width raises a recoverable error");
        const auto print_config = Slic3r::DynamicPrintConfig::full_print_config_with(width_key, "0.45");
        check(std::abs(Slic3r::Flow::extrusion_width(width_key, print_config, 0) - 0.45) < 1e-6,
            "a complete print config provides the selected advisory width");
#endif
#ifndef EDGE_FINISH_GEOMETRY_ONLY
        check_booleans();
#endif
        std::cout << "Mesh edge finishing checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
