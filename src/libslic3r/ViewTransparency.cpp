///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ViewTransparency.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Slic3r::ViewTransparency {
namespace {
constexpr uint64_t seed_hash = 14695981039346656037ull;
void mix(uint64_t &hash, uint32_t word)
{
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= (word >> (8 * byte)) & 0xff;
        hash *= 1099511628211ull;
    }
}

uint64_t reindexed_topology(const indexed_triangle_set &mesh, bool reverse)
{
    uint64_t hash = seed_hash;
    std::vector<uint32_t> labels(mesh.vertices.size(), UINT32_MAX);
    uint32_t next = 0;
    for (const auto &f : mesh.indices)
        for (int corner : {0, reverse ? 2 : 1, reverse ? 1 : 2}) {
            auto &label = labels[f[corner]];
            if (label == UINT32_MAX)
                label = next++;
            mix(hash, label);
        }
    return hash;
}

Percent override_at(const std::vector<Percent> &values, size_t facet)
{
    return facet < values.size() ? values[facet] : Inherit;
}
} // namespace

Percent Body::effective(Percent whole, size_t facet, bool current_mesh) const
{
    if (current_mesh) {
        const Percent face = override_at(faces, facet);
        if (face <= 100)
            return face;
        const Percent shell = override_at(shells, facet);
        if (shell <= 100)
            return shell;
    }
    return percent <= 100 ? percent : whole;
}

void Body::discard_stale_details(uint64_t signature)
{
    if (mesh_signature != signature) {
        shells.clear();
        faces.clear();
        mesh_signature = signature;
    }
}

uint64_t mesh_signature(const indexed_triangle_set &mesh)
{
    uint64_t hash = seed_hash;
    mix(hash, uint32_t(mesh.vertices.size()));
    mix(hash, uint32_t(mesh.indices.size()));
    for (const auto &f : mesh.indices)
        for (int i = 0; i < 3; ++i)
            mix(hash, uint32_t(f[i]));
    for (const auto &p : mesh.vertices)
        for (int i = 0; i < 3; ++i) {
            uint32_t bits;
            static_assert(sizeof(bits) == sizeof(p[i]));
            std::memcpy(&bits, &p[i], sizeof(bits));
            mix(hash, bits);
        }
    return hash;
}

uint64_t topology_signature(const indexed_triangle_set &mesh)
{
    return std::min(reindexed_topology(mesh, false), reindexed_topology(mesh, true));
}

Regions::Regions(const indexed_triangle_set &mesh)
{
    const auto neighbors = its_face_neighbors(mesh);
    m_neighbors.reserve(neighbors.size());
    for (const Vec3i &n : neighbors)
        m_neighbors.push_back({n[0], n[1], n[2]});
    if (!mesh.vertices.empty()) {
        Vec3f lo = mesh.vertices.front(), hi = lo;
        for (const auto &p : mesh.vertices) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        m_plane_tolerance = std::clamp(double((hi - lo).norm()) * 2e-6, 1e-6, 0.01);
    }
}

std::vector<std::vector<size_t>> Regions::solids(const indexed_triangle_set &mesh) const
{
    struct Component {
        std::vector<size_t> facets;
        Vec3d lo, hi;
        double signed_volume = 0.;
    };
    std::vector<Component> components;
    std::vector<bool> seen(m_neighbors.size());
    for (size_t seed = 0; seed < seen.size(); ++seed) {
        if (seen[seed])
            continue;
        const Vec3d origin = mesh.vertices[mesh.indices[seed][0]].cast<double>();
        Component &part = components.emplace_back(Component{{seed}, origin, origin});
        seen[seed] = true;
        for (size_t i = 0; i < part.facets.size(); ++i) {
            const size_t facet = part.facets[i];
            for (int n : m_neighbors[facet]) {
                if (n < 0)
                    return {}; // An open component does not identify a solid reliably.
                if (!seen[n]) {
                    seen[n] = true;
                    part.facets.push_back(size_t(n));
                }
            }
            const auto &f = mesh.indices[facet];
            const Vec3d p = mesh.vertices[f[0]].cast<double>(), q = mesh.vertices[f[1]].cast<double>(), r = mesh.vertices[f[2]].cast<double>();
            part.lo = part.lo.cwiseMin(p).cwiseMin(q).cwiseMin(r);
            part.hi = part.hi.cwiseMax(p).cwiseMax(q).cwiseMax(r);
            part.signed_volume += (p - origin).dot((q - origin).cross(r - origin));
        }
    }
    std::vector<std::vector<size_t>> solids;
    std::vector<size_t> parents;
    for (size_t i = 0; i < components.size(); ++i)
        if (components[i].signed_volume > 0.) {
            parents.push_back(i);
            solids.push_back(std::move(components[i].facets));
        }
    if (parents.size() < 2)
        return {};
    for (auto &part : components) {
        if (part.signed_volume > 0.)
            continue;
        size_t owner = parents.size();
        for (size_t i = 0; i < parents.size(); ++i) {
            const auto &potential = components[parents[i]];
            if ((potential.lo.array() <= part.lo.array()).all() && (potential.hi.array() >= part.hi.array()).all()) {
                if (owner != parents.size())
                    return {}; // Avoid assigning an ambiguous enclosed cavity to the wrong solid.
                owner = i;
            }
        }
        if (owner == parents.size())
            return {};
        solids[owner].insert(solids[owner].end(), part.facets.begin(), part.facets.end());
    }
    return solids;
}

std::vector<size_t> Regions::face(const indexed_triangle_set &mesh, size_t seed) const
{
    if (seed >= m_neighbors.size())
        return {};
    const auto &base = mesh.indices[seed];
    const Vec3d origin = mesh.vertices[base[0]].cast<double>();
    const Vec3d normal = (mesh.vertices[base[1]].cast<double>() - origin)
        .cross(mesh.vertices[base[2]].cast<double>() - origin).normalized();
    std::vector<size_t> region{seed};
    std::vector<bool> seen(m_neighbors.size());
    seen[seed] = true;
    for (size_t i = 0; i < region.size(); ++i)
        for (int neighbor : m_neighbors[region[i]]) {
            if (neighbor < 0 || seen[neighbor])
                continue;
            seen[neighbor] = true;
            const auto &f = mesh.indices[neighbor];
            const Vec3d p = mesh.vertices[f[0]].cast<double>(), q = mesh.vertices[f[1]].cast<double>(), r = mesh.vertices[f[2]].cast<double>();
            const Vec3d n = (q - p).cross(r - p).normalized();
            if (n.dot(normal) >= 0.99996 && std::max({std::abs((p - origin).dot(normal)),
                    std::abs((q - origin).dot(normal)), std::abs((r - origin).dot(normal))}) <= m_plane_tolerance)
                region.push_back(size_t(neighbor));
        }
    std::sort(region.begin(), region.end());
    return region;
}

} // namespace Slic3r::ViewTransparency
