///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "MeshEdgeFinish.hpp"

#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_tree.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/intersection.h>
#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Side_of_triangle_mesh.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/Euler_operations.h>
#include <CGAL/squared_distance_3.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace Slic3r::MeshEdgeFinish {
namespace {
using Kernel = CGAL::Exact_predicates_exact_constructions_kernel;
using Point = Kernel::Point_3;
using Triangle = Kernel::Triangle_3;
using ExactMesh = CGAL::Surface_mesh<Point>;
using Face = ExactMesh::Face_index;
using Vertex = ExactMesh::Vertex_index;
using Tree = CGAL::AABB_tree<CGAL::AABB_traits<Kernel, CGAL::AABB_face_graph_triangle_primitive<ExactMesh>>>;
namespace PMP = CGAL::Polygon_mesh_processing;
using CutFaces = ExactMesh::Property_map<Face, bool>;
constexpr char cut_property[] = "f:edge_finish_cut";

struct CutVisitor : PMP::Corefinement::Default_visitor<ExactMesh> {
    const ExactMesh &cutter;
    CutFaces input, tool, output;
    bool splitting_cut_face = false;

    CutVisitor(const ExactMesh &cutter, CutFaces input, CutFaces tool, CutFaces output)
        : cutter(cutter), input(input), tool(tool), output(output) {}

    CutFaces &tags(const ExactMesh &mesh) { return &mesh == &cutter ? tool : input; }

    void before_subface_creations(Face old_face, ExactMesh &mesh)
    {
        splitting_cut_face = tags(mesh)[old_face];
    }

    void after_subface_created(Face new_face, ExactMesh &mesh)
    {
        tags(mesh)[new_face] = splitting_cut_face;
    }

    void after_face_copy(Face old_face, const ExactMesh &mesh, Face new_face, ExactMesh &)
    {
        output[new_face] = tags(mesh)[old_face];
    }
};

Point point(const Vec3 &v) { return {v.x(), v.y(), v.z()}; }

template<class Vertices> ExactMesh make_mesh(const Vertices &vertices, const std::vector<stl_triangle_vertex_indices> &indices)
{
    ExactMesh mesh;
    std::vector<Vertex> handles(vertices.size(), ExactMesh::null_vertex());
    for (const auto &f : indices) {
        for (int i = 0; i < 3; ++i)
            if (handles[f[i]] == ExactMesh::null_vertex())
                handles[f[i]] = mesh.add_vertex(point(vertices[f[i]].template cast<double>()));
        if (mesh.add_face(handles[f[0]], handles[f[1]], handles[f[2]]) == ExactMesh::null_face())
            return {};
    }
    return mesh;
}

Triangle triangle(const ExactMesh &mesh, Face face)
{
    const auto he = mesh.halfedge(face);
    return {mesh.point(mesh.source(he)), mesh.point(mesh.target(he)), mesh.point(mesh.target(mesh.next(he)))};
}

bool valid_body(ExactMesh &mesh, std::optional<int64_t> euler, Kernel::FT *volume = nullptr)
{
    if (mesh.number_of_faces() < 4 || !CGAL::is_closed(mesh) || !CGAL::is_triangle_mesh(mesh))
        return false;
    if (euler && int64_t(mesh.number_of_vertices()) - int64_t(mesh.number_of_edges()) +
            int64_t(mesh.number_of_faces()) != *euler)
        return false;
    auto components = mesh.add_property_map<Face, size_t>("f:edge_finish_component", 0).first;
    if (PMP::connected_components(mesh, components) != 1)
        return false;
    mesh.remove_property_map(components);
    std::vector<ExactMesh::Halfedge_index> non_manifold;
    PMP::non_manifold_vertices(mesh, std::back_inserter(non_manifold));
    if (!non_manifold.empty())
        return false;
    for (Face f : mesh.faces())
        if (triangle(mesh, f).is_degenerate())
            return false;
    const Kernel::FT signed_volume = PMP::volume(mesh);
    if (volume)
        *volume = signed_volume;
    return signed_volume > 0 && !PMP::does_self_intersect(mesh);
}

ExactMesh tagged_cutter(const Surface &surface, const std::pair<size_t, size_t> &profile_faces)
{
    ExactMesh cutter = make_mesh(surface.vertices, surface.indices);
    if (cutter.number_of_faces() != 0) {
        auto tags = cutter.add_property_map<Face, bool>(cut_property, false).first;
        for (size_t f = profile_faces.first; f < profile_faces.second; ++f)
            tags[Face(static_cast<ExactMesh::size_type>(f))] = true;
    }
    return cutter;
}

bool subtract(ExactMesh &body, ExactMesh &tool, ExactMesh &out)
{
    const auto body_tags = body.add_property_map<Face, bool>(cut_property, false).first;
    const auto tool_tags = tool.property_map<Face, bool>(cut_property).first;
    const auto out_tags = out.add_property_map<Face, bool>(cut_property, false).first;
    CutVisitor visitor(tool, body_tags, tool_tags, out_tags);
    return PMP::corefine_and_compute_difference(body, tool, out, CGAL::parameters::visitor(visitor));
}

Kernel::Iso_cuboid_3 expanded_box(const CGAL::Bbox_3 &b, double d)
{
    return {Point(b.xmin() - d, b.ymin() - d, b.zmin() - d), Point(b.xmax() + d, b.ymax() + d, b.zmax() + d)};
}

bool near_triangles(const Triangle &a, const Triangle &b, const Kernel::FT &distance_sq)
{
    if (CGAL::do_intersect(a, b))
        return true;
    for (int i = 0; i < 3; ++i) {
        if (CGAL::squared_distance(a[i], b) <= distance_sq || CGAL::squared_distance(b[i], a) <= distance_sq)
            return true;
        const Kernel::Segment_3 edge(a[i], a[(i + 1) % 3]);
        for (int j = 0; j < 3; ++j)
            if (CGAL::squared_distance(edge, Kernel::Segment_3(b[j], b[(j + 1) % 3])) <= distance_sq)
                return true;
    }
    return false;
}

bool near_mesh(const Tree &tree, const ExactMesh &mesh, const Triangle &tri, double clearance)
{
    std::vector<Face> nearby;
    tree.all_intersected_primitives(expanded_box(tri.bbox(), clearance), std::back_inserter(nearby));
    const Kernel::FT distance_sq = clearance * clearance;
    return std::any_of(nearby.begin(), nearby.end(), [&](Face f) {
        return near_triangles(tri, triangle(mesh, f), distance_sq);
    });
}

// Only the supporting faces and the ends of this feature may be cut. This
// checks all other surface triangles, even a wall completely inside a cutter.
bool check_protected_faces(const Analysis &a, const std::vector<int> &patches, const ExactMesh &cutter,
    double reference_wall, bool &thin, const Cancelled &cancelled)
{
    const Tree tree(cutter.faces().begin(), cutter.faces().end(), cutter);
    const CGAL::Side_of_triangle_mesh<ExactMesh, Kernel> side(tree);
    const auto region = expanded_box(tree.bbox(), std::max(a.tolerance, reference_wall)).bbox();
    const std::unordered_set<int> allowed(patches.begin(), patches.end());
    for (size_t i = 0; i < a.mesh.indices.size(); ++i) {
        if (i % 512 == 0 && cancelled())
            return false;
        if (allowed.count(a.face_patch[i]) > 0)
            continue;
        const auto &f = a.mesh.indices[i];
        const Vec3 &p = a.mesh.vertices[f[0]], &q = a.mesh.vertices[f[1]], &r = a.mesh.vertices[f[2]];
        const Vec3 lo = p.cwiseMin(q).cwiseMin(r), hi = p.cwiseMax(q).cwiseMax(r);
        if (!CGAL::do_overlap(region, CGAL::Bbox_3(lo.x(), lo.y(), lo.z(), hi.x(), hi.y(), hi.z())))
            continue;
        const Triangle tri(point(p), point(q), point(r));
        if (side(tri[0]) != CGAL::ON_UNBOUNDED_SIDE || near_mesh(tree, cutter, tri, a.tolerance))
            return false;
        if (!thin && reference_wall > a.tolerance && near_mesh(tree, cutter, tri, reference_wall))
            thin = true;
    }
    return true;
}

bool overlap(const ExactMesh &a, const ExactMesh &b, double tolerance)
{
    if (PMP::do_intersect(a, b, CGAL::parameters::do_overlap_test_of_bounded_sides(true)))
        return true;
    const Tree tree(a.faces().begin(), a.faces().end(), a);
    const auto bounds = expanded_box(tree.bbox(), tolerance).bbox();
    return std::any_of(b.faces().begin(), b.faces().end(), [&](Face f) {
        const Triangle tri = triangle(b, f);
        return CGAL::do_overlap(bounds, tri.bbox()) && near_mesh(tree, a, tri, tolerance);
    });
}

indexed_triangle_set store_mesh(const Analysis &a, const ExactMesh &mesh, CutFaces tags, std::vector<bool> &cut_faces)
{
    indexed_triangle_set result;
    std::map<std::array<float, 3>, int> rounded;
    std::map<Vertex, int> index;
    const Eigen::Matrix3d to_work = a.from_work.inverse();
    for (Vertex v : mesh.vertices()) {
        const Point &p = mesh.point(v);
        const Vec3 work(CGAL::to_double(p.x()), CGAL::to_double(p.y()), CGAL::to_double(p.z()));
        const stl_vertex local = (a.local_origin + a.from_work * work).cast<float>();
        if (!local.allFinite() || (to_work * (local.cast<double>() - a.local_origin) - work).norm() > a.tolerance)
            return {};
        const auto [it, added] = rounded.emplace(std::array<float, 3>{local.x(), local.y(), local.z()}, int(result.vertices.size()));
        if (added)
            result.vertices.push_back(local);
        index.emplace(v, it->second);
    }
    for (Face f : mesh.faces()) {
        const auto he = mesh.halfedge(f);
        const int i0 = index.at(mesh.source(he)), i1 = index.at(mesh.target(he)), i2 = index.at(mesh.target(mesh.next(he)));
        // Two exact intersections may round to the same stored coordinate.
        if (i0 != i1 && i1 != i2 && i0 != i2) {
            result.indices.emplace_back(i0, i1, i2);
            cut_faces.push_back(tags[f]);
        }
    }
    return result;
}

Result store_checked(const Analysis &a, const ExactMesh &mesh, bool thin, size_t edge)
{
    std::vector<bool> cut_faces;
    cut_faces.reserve(mesh.number_of_faces());
    indexed_triangle_set stored = store_mesh(a, mesh, mesh.property_map<Face, bool>(cut_property).first, cut_faces);
    ExactMesh rounded = make_mesh(stored.vertices, stored.indices);
    if (!valid_body(rounded, a.euler))
        return {{}, Error::BooleanFailed, edge};
    return {std::move(stored), Error::None, 0, thin, {}, std::move(cut_faces)};
}

Result store_result(const Analysis &a, const ExactMesh &mesh, bool thin, size_t edge, const Cancelled &cancelled)
{
    Result stored = store_checked(a, mesh, thin, edge);
    if (stored.error == Error::None)
        return stored;

    // Float rounding can fold a needle triangle at the end of a newly machined face.
    // Retry after collapsing only sub-tolerance edges on the cut, preserving every input vertex.
    ExactMesh repaired = mesh;
    const auto tags = repaired.property_map<Face, bool>(cut_property).first;
    using MeshEdge = ExactMesh::Edge_index;
    std::vector<MeshEdge> short_edges;
    const Kernel::FT max_length_sq = (a.tolerance / 8.) * (a.tolerance / 8.);
    for (MeshEdge e : repaired.edges()) {
        const auto h = repaired.halfedge(e);
        if ((tags[repaired.face(h)] || tags[repaired.face(repaired.opposite(h))]) &&
            CGAL::squared_distance(repaired.point(repaired.source(h)), repaired.point(repaired.target(h))) <= max_length_sq)
            short_edges.push_back(e);
    }
    if (short_edges.empty())
        return stored;
    std::set<std::array<double, 3>> original;
    for (const Vec3 &v : a.mesh.vertices)
        original.insert({v.x(), v.y(), v.z()});
    auto input_vertex = [&](Vertex v) {
        const Point &p = repaired.point(v);
        return original.count({CGAL::to_double(p.x()), CGAL::to_double(p.y()), CGAL::to_double(p.z())}) != 0;
    };
    std::set<Vertex> moved;
    for (MeshEdge e : short_edges) {
        if (cancelled())
            return {{}, Error::Cancelled};
        if (repaired.is_removed(e))
            continue;
        const auto h = repaired.halfedge(e);
        const Vertex u = repaired.source(h), v = repaired.target(h);
        if (moved.count(u) || moved.count(v) || input_vertex(u) || input_vertex(v) ||
            CGAL::squared_distance(repaired.point(u), repaired.point(v)) > max_length_sq ||
            !CGAL::Euler::does_satisfy_link_condition(e, repaired))
            continue;
        // Never move a survivor a second time, so each vertex displacement stays bounded.
        moved.insert(CGAL::Euler::collapse_edge(e, repaired));
    }
    if (moved.empty() || !valid_body(repaired, a.euler))
        return stored;
    return store_checked(a, repaired, thin, edge);
}
} // namespace

Result finish(const Analysis &a, const std::vector<size_t> &edges, Operation operation,
    double size, double reference_wall, const Cancelled &cancelled)
{
    const Limits limit = limits(a, edges, operation);
    if (limit.error != Error::None)
        return {{}, limit.error, limit.limiting_edge, false, limit.other_edge};
    if (!std::isfinite(size) || size <= a.tolerance)
        return {{}, Error::InvalidSize};
    if (size > limit.maximum)
        return {{}, Error::TooLarge, limit.limiting_edge, false, limit.other_edge};
    size_t active = edges.front();
    try {
        if (cancelled())
            return {{}, Error::Cancelled};
        ExactMesh original = make_mesh(a.mesh.vertices, a.mesh.indices);
        Kernel::FT volume;
        if (!valid_body(original, a.euler, &volume))
            return {{}, Error::InvalidMesh};
        std::vector<ExactMesh> cutters;
        std::vector<bool> possible_thin;
        for (size_t id : edges) {
            active = id;
            if (cancelled())
                return {{}, Error::Cancelled};
            std::pair<size_t, size_t> profile_faces;
            const Surface surface = make_cutter(a, id, operation, size, profile_faces);
            ExactMesh cutter = tagged_cutter(surface, profile_faces);
            if (!valid_body(cutter, 2))
                return {{}, Error::TooLarge, id};
            bool near_protected = false;
            if (!check_protected_faces(a, a.edges[id].patches, cutter, reference_wall, near_protected, cancelled))
                return {{}, cancelled() ? Error::Cancelled : Error::ProtectedFace, id};
            possible_thin.push_back(near_protected);
            cutters.push_back(std::move(cutter));
        }
        std::vector<std::optional<ExactMesh>> removed(cutters.size());
        auto footprint = [&](size_t i) -> const ExactMesh & {
            if (!removed[i]) {
                ExactMesh body = original, knife = cutters[i];
                removed[i].emplace();
                if (!PMP::corefine_and_compute_intersection(body, knife, *removed[i]) ||
                    removed[i]->number_of_faces() < 4 || !CGAL::is_closed(*removed[i]))
                    throw std::runtime_error("Invalid edge finishing footprint");
            }
            return *removed[i];
        };
        for (size_t i = 0; i < cutters.size(); ++i)
            for (size_t j = 0; j < i; ++j) {
                if (cancelled())
                    return {{}, Error::Cancelled};
                // A coplanar joint intentionally shares material at the corner.
                if (std::find(limit.planar_joints.begin(), limit.planar_joints.end(), std::make_pair(edges[j], edges[i])) != limit.planar_joints.end())
                    continue;
                if (overlap(cutters[j], cutters[i], a.tolerance) &&
                    overlap(footprint(j), footprint(i), a.tolerance))
                    return {{}, Error::Overlap, edges[i], false, edges[j]};
            }
        bool thin = false;
        if (reference_wall > a.tolerance)
            for (size_t i = 0; i < cutters.size() && !thin; ++i) {
                if (possible_thin[i] && !check_protected_faces(a, a.edges[edges[i]].patches, footprint(i), reference_wall, thin, cancelled))
                    return {{}, cancelled() ? Error::Cancelled : Error::ProtectedFace, edges[i]};
            }
        const double min_removed = a.tolerance * a.tolerance * a.tolerance;
        // An exact rectangular perimeter can use one ring cutter and one body corefinement.
        if (limit.planar_joints.size() == 4 && cutters.size() == 4) {
            try {
                std::pair<size_t, size_t> profiles;
                const Surface surface = make_rectangular_perimeter_cutter(a, edges, operation, size, profiles);
                ExactMesh ring = tagged_cutter(surface, profiles);
                std::vector<int> patches;
                for (size_t id : edges)
                    patches.insert(patches.end(), a.edges[id].patches.begin(), a.edges[id].patches.end());
                bool unused = false;
                if (valid_body(ring, 0) && check_protected_faces(a, patches, ring, 0., unused, cancelled)) {
                    if (cancelled())
                        return {{}, Error::Cancelled};
                    ExactMesh body = original, next;
                    Kernel::FT next_volume;
                    if (subtract(body, ring, next) && valid_body(next, a.euler, &next_volume) && volume - next_volume > min_removed) {
                        if (cancelled())
                            return {{}, Error::Cancelled};
                        Result batch = store_result(a, next, thin, active, cancelled);
                        if (batch.error == Error::None)
                            return batch;
                    }
                }
            } catch (const std::exception &) {
                // Use the individual cutters if the ring cannot be applied safely.
            }
        }
        ExactMesh result = std::move(original);
        for (size_t i = 0; i < cutters.size(); ++i) {
            active = edges[i];
            if (cancelled())
                return {{}, Error::Cancelled};
            ExactMesh next;
            Kernel::FT next_volume;
            if (!subtract(result, cutters[i], next) || !valid_body(next, a.euler, &next_volume))
                return {{}, Error::BooleanFailed, active};
            if (volume - next_volume <= min_removed)
                return {{}, Error::InvalidSize, active};
            volume = next_volume;
            result = std::move(next);
        }
        if (cancelled())
            return {{}, Error::Cancelled};
        return store_result(a, result, thin, active, cancelled);
    } catch (const std::exception &) {
        return {{}, cancelled() ? Error::Cancelled : Error::BooleanFailed, active};
    }
}
} // namespace Slic3r::MeshEdgeFinish
