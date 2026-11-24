///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Slic3r;
namespace VT = ViewTransparency;

namespace {
void require(bool pass, const char *reason)
{
    if (!pass)
        throw std::runtime_error(reason);
}

indexed_triangle_set box(float x)
{
    indexed_triangle_set mesh;
    mesh.vertices = {{x, 0, 0}, {x+4, 0, 0}, {x+4, 4, 0}, {x, 4, 0},
                     {x, 0, 4}, {x+4, 0, 4}, {x+4, 4, 4}, {x, 4, 4}};
    mesh.indices = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                    {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                    {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    return mesh;
}

void append(indexed_triangle_set &mesh, indexed_triangle_set other)
{
    const int offset = int(mesh.vertices.size());
    for (const auto &f : other.indices)
        mesh.indices.emplace_back(f[0] + offset, f[1] + offset, f[2] + offset);
    mesh.vertices.insert(mesh.vertices.end(), other.vertices.begin(), other.vertices.end());
}
} // namespace

int main()
{
    try {
        auto mesh = box(0);
        append(mesh, box(8));
        const VT::Regions regions(mesh);
        require(regions.face(mesh, 0) == std::vector<size_t>{0, 1}, "select the whole plane up to its sharp edges");
        const auto solids = regions.solids(mesh);
        require(solids.size() == 2 && solids[0].size() == 12 && solids[1].size() == 12, "separate two closed solids without splitting the mesh");

        Model model;
        auto *part = model.add_object();
        auto *body = part->add_volume(TriangleMesh(mesh));
        auto *other_body = part->add_volume(TriangleMesh(box(16)));
        auto *instance = part->add_instance();
        instance->view_transparency.percent = 65;
        auto &data = instance->view_transparency.bodies[body->id().id];
        data.percent = 40;
        data.mesh_signature = VT::mesh_signature(body->mesh().its);
        data.shells.assign(mesh.indices.size(), VT::Inherit);
        data.faces.assign(mesh.indices.size(), VT::Inherit);
        for (size_t i : solids[0])
            data.shells[i] = 85;
        for (size_t i : regions.face(mesh, 0))
            data.faces[i] = 100;

        require(data.effective(65, 0, true) == 100 && data.effective(65, 4, true) == 85 &&
            data.effective(65, 14, true) == 40 && data.effective(65, 0, false) == 40,
            "face, shell, body and stale detail priority");
        auto *copy = part->add_instance(*instance);
        copy->set_offset(Vec3d(0., 12., 0.));
        copy->view_transparency.percent = 20;
        VT::Body second;
        second.percent = 35;
        copy->view_transparency.bodies.emplace(other_body->id().id, second);
        require(instance->view_transparency.percent == 65 &&
            instance->view_transparency.bodies.find(other_body->id().id) == instance->view_transparency.bodies.end(),
            "instance copies can be adjusted independently");

        const char *archive = "view-transparency-roundtrip.3mf";
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        require(store_3mf(archive, &model, &config, false), "write a project with view settings");
        Model restored;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
        boost::optional<Semver> version;
        require(load_3mf(archive, config, substitutions, &restored, true, version), "reopen the saved project");
        auto *reopened = restored.objects.front();
        require(reopened->volumes.size() == 2 && reopened->instances.size() == 2, "preserve two bodies and two instances");
        auto &view = reopened->instances[0]->view_transparency;
        auto &saved = view.bodies.at(reopened->volumes[0]->id().id);
        require(view.percent == 65 && saved.faces == data.faces && saved.shells == data.shells &&
            saved.mesh_signature == VT::mesh_signature(reopened->volumes[0]->mesh().its), "restore display values and original facet assignments");
        require(reopened->instances[1]->view_transparency.percent == 20 &&
            reopened->instances[1]->view_transparency.bodies.at(reopened->volumes[1]->id().id).percent == 35,
            "restore independent per-instance body settings");

        auto *replaced = reopened->volumes.front();
        replaced->set_mesh(TriangleMesh(box(0)));
        replaced->set_new_unique_id();
        auto &after = reopened->instances[0]->view_transparency.bodies.at(replaced->id().id);
        require(after.percent == 40 && !after.detailed(), "mesh replacement retains body transparency and drops old faces");
        std::filesystem::remove(archive);
        std::cout << "Transparency and 3MF checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
