///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_MeshEdgeFinish_hpp_
#define slic3r_MeshEdgeFinish_hpp_

#include <admesh/stl.h>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

namespace Slic3r::MeshEdgeFinish {

using Vec3 = Eigen::Matrix<double, 3, 1, Eigen::DontAlign>;
using Cancelled = std::function<bool()>;

enum class Kind { Straight, Rim };
enum class Operation { Chamfer, Fillet };
enum class Error { None, Cancelled, InvalidMesh, NoEdges, InvalidSize, Adjacent, TooLarge, Overlap, ProtectedFace, BooleanFailed };

struct Segment {
    int from, to;
    int face, other_face;
};

struct Edge {
    Kind kind = Kind::Straight;
    std::vector<Segment> segments;
    std::vector<int> corners;
    std::vector<int> patches;
    Vec3 normal = Vec3::Zero();   // outward normal of the rim plane
    Vec3 center = Vec3::Zero();   // for spacing the rim picks
    Vec3 side_a = Vec3::Zero();
    Vec3 side_b = Vec3::Zero();
    std::array<int, 2> end_patches{{-1, -1}};
    double retreat = 0.;         // smallest available setback on either supporting face
    double depth = 0.;           // usable length of the original bore wall
    double angle = 0.;           // interior angle of a straight edge
};

struct Surface {
    std::vector<Vec3> vertices;
    std::vector<stl_triangle_vertex_indices> indices;
};

struct Analysis {
    Surface mesh; // centered in the instance's metric, with outward winding
    Vec3 local_origin = Vec3::Zero();
    Eigen::Matrix3d from_work = Eigen::Matrix3d::Identity();
    bool mirrored = false;
    double tolerance = 0.;
    int64_t euler = 0;
    std::vector<Vec3> patch_normals;
    std::vector<int> face_patch;
    std::vector<Edge> edges;
    // Candidate segments touch these facets, including neighboring coplanar facets.
    std::vector<std::vector<std::pair<size_t, size_t>>> facet_segments;
};

struct Limits {
    double maximum = std::numeric_limits<double>::infinity();
    size_t limiting_edge = 0;
    Error error = Error::None;
    std::optional<size_t> other_edge;
    std::vector<std::pair<size_t, size_t>> planar_joints;
};

struct Result {
    indexed_triangle_set mesh;
    Error error = Error::None;
    size_t edge = 0;
    bool thin_wall = false;
    std::optional<size_t> other_edge;
    std::vector<bool> cut_faces; // one tag per stored triangle
};

std::shared_ptr<Analysis> analyze(const indexed_triangle_set &mesh, const Eigen::Matrix3d &linear, const Cancelled &cancelled);
Limits limits(const Analysis &analysis, const std::vector<size_t> &edges, Operation operation);
bool points_span_edge(const Analysis &analysis, size_t edge, const std::vector<Vec3> &points);
Surface make_cutter(const Analysis &analysis, size_t edge, Operation operation, double size,
    std::pair<size_t, size_t> &profile_faces);
Surface make_rectangular_perimeter_cutter(const Analysis &analysis, const std::vector<size_t> &edges,
    Operation operation, double size, std::pair<size_t, size_t> &profile_faces);
Result finish(const Analysis &analysis, const std::vector<size_t> &edges, Operation operation,
    double size, double reference_wall, const Cancelled &cancelled);

} // namespace Slic3r::MeshEdgeFinish
#endif
