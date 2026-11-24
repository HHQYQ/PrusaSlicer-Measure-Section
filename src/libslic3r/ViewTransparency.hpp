///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_ViewTransparency_hpp_
#define slic3r_ViewTransparency_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct indexed_triangle_set;

namespace Slic3r::ViewTransparency {

using Percent = uint8_t;
inline constexpr Percent Inherit = 255;

struct Body {
    Percent percent = Inherit;
    uint64_t mesh_signature = 0;
    std::vector<Percent> shells, faces;

    bool detailed() const { return !shells.empty() || !faces.empty(); }
    bool empty() const { return percent == Inherit && !detailed(); }
    Percent effective(Percent whole, size_t facet, bool current_mesh) const;
    void discard_stale_details(uint64_t signature);
    template<class Archive> void serialize(Archive &ar) { ar(percent, mesh_signature, shells, faces); }
};

struct Instance {
    Percent percent = 0;
    std::map<size_t, Body> bodies;

    template<class Archive> void serialize(Archive &ar) { ar(percent, bodies); }
};

uint64_t mesh_signature(const indexed_triangle_set &mesh);
uint64_t topology_signature(const indexed_triangle_set &mesh);

// The two collection levels share the same welded-triangle adjacency.
struct Regions {
    explicit Regions(const indexed_triangle_set &mesh);
    std::vector<std::vector<size_t>> solids(const indexed_triangle_set &mesh) const;
    std::vector<size_t> face(const indexed_triangle_set &mesh, size_t seed) const;

private:
    std::vector<std::array<int, 3>> m_neighbors;
    double m_plane_tolerance = 0.;
};

} // namespace Slic3r::ViewTransparency
#endif
