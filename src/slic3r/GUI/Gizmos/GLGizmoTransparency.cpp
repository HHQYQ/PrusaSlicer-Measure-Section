///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "GLGizmoTransparency.hpp"

#include "libslic3r/MeshEdgeFinish.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TransparentFacePicking.hpp"

#include <GL/glew.h>
#include <wx/glcanvas.h>
#include <cstdio>

namespace Slic3r::GUI {
namespace VT = ViewTransparency;
namespace {

ModelVolume *part_by_id(const ModelObject &object, size_t id)
{
    auto it = std::find_if(object.volumes.begin(), object.volumes.end(), [id](auto *v) {
        return v->is_model_part() && v->id().id == id;
    });
    return it == object.volumes.end() ? nullptr : *it;
}

void add_segment(GLModel::Geometry &lines, const Vec3d &a, const Vec3d &b)
{
    const auto i = static_cast<unsigned int>(lines.vertices_count());
    lines.add_vertex(Vec3f(a.cast<float>()));
    lines.add_vertex(Vec3f(b.cast<float>()));
    lines.add_line(i, i + 1);
}

void add_outline(GLModel::Geometry &lines, const indexed_triangle_set &mesh, const std::vector<size_t> &facets,
    const Transform3d &world)
{
    std::map<std::pair<int, int>, unsigned> counts;
    for (size_t id : facets) {
        const auto &f = mesh.indices[id];
        for (int i = 0; i < 3; ++i)
            ++counts[std::minmax(f[i], f[(i + 1) % 3])];
    }
    for (const auto &[edge, count] : counts)
        if (count == 1)
            add_segment(lines, world * mesh.vertices[edge.first].cast<double>(), world * mesh.vertices[edge.second].cast<double>());
}

} // namespace

GLGizmoTransparency::GLGizmoTransparency(GLCanvas3D &canvas)
    : GLGizmoBase(canvas, "transparency.svg", 17)
{}

bool GLGizmoTransparency::on_is_selectable() const
{
#ifdef _WIN64
    return m_parent.current_printer_technology() == ptFFF;
#else
    return false;
#endif
}

std::string GLGizmoTransparency::on_get_name() const { return _u8L("Transparency"); }

std::optional<GLGizmoTransparency::Target> GLGizmoTransparency::target() const
{
    const Selection &selection = m_parent.get_selection();
    if (selection.is_empty() || selection.is_wipe_tower())
        return {};
    const auto *first = selection.get_first_volume();
    if (!first || first->object_idx() < 0 || first->instance_idx() < 0)
        return {};
    for (unsigned int index : selection.get_volume_idxs()) {
        const auto *volume = m_parent.get_volumes().volumes[index];
        if (volume->object_idx() != first->object_idx() || volume->instance_idx() != first->instance_idx())
            return {};
    }
    auto *object = selection.get_model()->objects[first->object_idx()];
    return Target{object, object->instances[first->instance_idx()], first->object_idx(), first->instance_idx()};
}

bool GLGizmoTransparency::on_is_activable() const
{
    const auto t = target();
    return on_is_selectable() && t && std::any_of(t->object->volumes.begin(), t->object->volumes.end(),
        [](const ModelVolume *part) { return part->is_model_part(); });
}

void GLGizmoTransparency::sync_selection()
{
    auto t = target();
    const auto key = t ? std::make_pair(t->object->id().id, t->instance->id().id) : std::pair<size_t, size_t>{};
    if (key != m_target_id) {
        m_target_id = key;
        m_native_bodies.clear();
        m_bodies.clear();
        m_faces.clear();
        m_hover.reset();
        m_down.reset();
        m_mesh_cache.clear();
        m_slider_snapshot = false;
        m_outline_dirty = true;
    }
    if (!t)
        return;
    m_bodies.erase(std::remove_if(m_bodies.begin(), m_bodies.end(), [&](const BodyPick &p) {
        const auto *part = part_by_id(*t->object, p.volume);
        const auto cache = m_mesh_cache.find(p.volume);
        return !part || (p.shell >= 0 && (cache == m_mesh_cache.end() || cache->second.mesh != part->mesh_ptr()));
    }), m_bodies.end());
    const auto present = [&](const FacePick &f) {
        const auto *v = part_by_id(*t->object, f.volume);
        return v && v->mesh_ptr() == f.mesh;
    };
    m_faces.erase(std::remove_if(m_faces.begin(), m_faces.end(), [&](const FacePick &f) { return !present(f); }), m_faces.end());
    if (m_hover && !present(*m_hover))
        m_hover.reset();

    std::vector<BodyPick> native;
    const Selection &selection = m_parent.get_selection();
    if (selection.get_mode() == Selection::Volume)
        for (unsigned int index : selection.get_volume_idxs()) {
            const int volume = m_parent.get_volumes().volumes[index]->volume_idx();
            if (volume >= 0 && t->object->volumes[volume]->is_model_part())
                native.push_back({t->object->volumes[volume]->id().id});
        }
    if (native != m_native_bodies) {
        if (!native.empty() && m_mode == Mode::Whole)
            m_mode = Mode::Body;
        m_native_bodies = native;
        m_bodies = std::move(native);
        m_outline_dirty = true;
    }
}

void GLGizmoTransparency::on_set_state()
{
    if (m_state == On)
        sync_selection();
    else {
        m_target_id = {};
        m_native_bodies.clear();
        m_bodies.clear();
        m_faces.clear();
        m_mesh_cache.clear();
        m_hover.reset();
        m_down.reset();
        m_selected_outline.reset();
        m_hover_outline.reset();
        m_outline_dirty = true;
        m_slider_snapshot = false;
    }
}

void GLGizmoTransparency::data_changed(bool)
{
    if (m_state == On) {
        sync_selection();
        m_outline_dirty = true;
    }
}

GLGizmoTransparency::MeshCache &GLGizmoTransparency::mesh_cache(const ModelVolume &volume)
{
    auto &cache = m_mesh_cache[volume.id().id];
    if (const auto mesh = volume.mesh_ptr(); mesh != cache.mesh) {
        cache = {};
        cache.mesh = mesh;
        cache.signature = VT::mesh_signature(mesh->its);
    }
    return cache;
}

void GLGizmoTransparency::pick_face(const Vec2d &mouse)
{
    auto t = target();
    if (!t)
        return;
    const auto planes = m_parent.section_planes();
    const auto *first_plane = planes[0].is_active() ? &planes[0] : nullptr;
    const auto *second_plane = planes[1].is_active() ? &planes[1] : nullptr;
    const Camera &camera = wxGetApp().plater()->get_camera();
    const GLVolume *best = nullptr;
    size_t seed = 0;
    double closest = -std::numeric_limits<double>::infinity();
    bool best_translucent = true;
    for (const GLVolume *scene : m_parent.get_volumes().volumes) {
        if (!scene->is_active || !scene->mesh_raycaster || scene->volume_idx() < 0 || scene->object_idx() < 0 || scene->instance_idx() < 0)
            continue;
        const ModelObject &object = *wxGetApp().model().objects[scene->object_idx()];
        const auto &volume = *object.volumes[scene->volume_idx()];
        if (!volume.is_model_part())
            continue;
        const auto &view = object.instances[scene->instance_idx()]->view_transparency;
        auto body = view.bodies.find(volume.id().id);
        const bool detailed = body != view.bodies.end() && body->second.detailed() &&
            mesh_cache(volume).signature == body->second.mesh_signature;
        const auto percent = [&](size_t facet) {
            return body == view.bodies.end() ? view.percent : body->second.effective(view.percent, facet, detailed);
        };
        Vec3f p, n;
        size_t facet;
        const auto world = scene->world_matrix();
        bool translucent = false;
        if (m_through) {
            const auto picked = pick_through_transparency(*scene->mesh_raycaster, mouse, world, camera,
                percent, first_plane, second_plane);
            if (!picked)
                continue;
            p = picked->position;
            facet = picked->facet;
            translucent = picked->translucent;
        } else if (!scene->mesh_raycaster->closest_hit(mouse, world, camera, p, n, first_plane, &facet,
                    second_plane, [&](size_t i) { return percent(i) < 100; }, true)) {
            continue;
        }
        const double depth = (camera.get_view_matrix() * (world * p.cast<double>())).z();
        if (!best || (translucent == best_translucent ? depth > closest : !translucent)) {
            closest = depth;
            best = scene;
            seed = facet;
            best_translucent = translucent;
        }
    }
    if (!best || best->object_idx() != t->object_index || best->instance_idx() != t->instance_index) {
        if (m_hover) {
            m_hover.reset();
            m_outline_dirty = true;
            set_dirty();
        }
        return;
    }
    const ModelVolume &volume = *t->object->volumes[best->volume_idx()];
    const size_t id = volume.id().id;
    if (m_hover && m_hover->volume == id && std::binary_search(m_hover->facets.begin(), m_hover->facets.end(), seed))
        return;
    auto &cache = mesh_cache(volume);
    std::vector<size_t> facets;
    if (!m_hole_wall) {
        if (!cache.regions)
            cache.regions = std::make_unique<VT::Regions>(cache.mesh->its);
        facets = cache.regions->face(cache.mesh->its, seed);
    } else {
        if (!cache.checked_holes) {
            cache.checked_holes = true;
            cache.holes = MeshEdgeFinish::analyze(cache.mesh->its, Eigen::Matrix3d::Identity(), [] { return false; });
        }
        if (cache.holes && seed < cache.holes->face_patch.size()) {
            const auto &patches = cache.holes->face_patch;
            for (const auto &edge : cache.holes->edges)
                if (edge.kind == MeshEdgeFinish::Kind::Rim && std::find(edge.patches.begin() + 1, edge.patches.end(), patches[seed]) != edge.patches.end()) {
                    for (size_t i = 0; i < patches.size(); ++i)
                        if (std::find(edge.patches.begin() + 1, edge.patches.end(), patches[i]) != edge.patches.end())
                            facets.push_back(i);
                    break;
                }
        }
    }
    m_hover = facets.empty() ? std::optional<FacePick>{} : FacePick{id, cache.mesh, std::move(facets)};
    m_outline_dirty = true;
    set_dirty();
}

bool GLGizmoTransparency::on_mouse(const wxMouseEvent &event)
{
    if (!picking_faces())
        return false;
    const Vec2d mouse(event.GetX(), event.GetY());
    if (event.Leaving() || event.Dragging()) {
        m_down.reset();
        if (m_hover) {
            m_hover.reset();
            m_outline_dirty = true;
            set_dirty();
        }
    }
    if (event.LeftDown())
        m_down = mouse;
    if (event.LeftUp() && m_down) {
        const Vec2d travel = mouse - *m_down;
        m_down.reset();
        if (travel.cwiseAbs().maxCoeff() < 4.) {
            pick_face(mouse);
            if (!event.ControlDown())
                m_faces.clear();
            if (m_hover) {
                const auto it = std::find_if(m_faces.begin(), m_faces.end(), [&](const FacePick &f) {
                    return f.volume == m_hover->volume && f.facets == m_hover->facets;
                });
                if (it == m_faces.end())
                    m_faces.push_back(*m_hover);
                else
                    m_faces.erase(it);
            }
            m_outline_dirty = true;
            set_dirty();
            return true;
        }
    }
    if (event.Moving())
        pick_face(mouse);
    return false;
}

void GLGizmoTransparency::set_percent(const Target &t, VT::Percent percent)
{
    auto &view = t.instance->view_transparency;
    if (m_mode == Mode::Whole) {
        view.percent = percent <= 100 ? percent : 0;
        return;
    }
    auto assign = [&](size_t id, int shell, const std::vector<size_t> *facets) {
        auto *volume = part_by_id(*t.object, id);
        if (!volume)
            return;
        auto &body = view.bodies[id];
        auto &cache = mesh_cache(*volume);
        body.discard_stale_details(cache.signature);
        if (shell < 0 && !facets) {
            body.percent = percent;
        } else {
            auto &values = facets ? body.faces : body.shells;
            const auto &group = facets ? *facets : (*cache.solids)[shell];
            if (values.empty())
                values.assign(cache.mesh->its.indices.size(), VT::Inherit);
            for (size_t facet : group)
                values[facet] = percent;
            if (percent == VT::Inherit && std::none_of(values.begin(), values.end(), [](auto p) { return p <= 100; }))
                values.clear();
        }
        if (body.empty())
            view.bodies.erase(id);
    };
    if (m_mode == Mode::Body) {
        for (const auto &p : m_bodies)
            assign(p.volume, p.shell, nullptr);
    } else {
        for (const auto &p : m_faces)
            assign(p.volume, -1, &p.facets);
    }
}

void GLGizmoTransparency::build_outlines(const Target &t)
{
    GLModel::Geometry selected, hovered;
    selected.format = hovered.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    for (const auto *scene : m_parent.get_volumes().volumes) {
        if (scene->object_idx() != t.object_index || scene->instance_idx() != t.instance_index || scene->volume_idx() < 0)
            continue;
        const ModelVolume &body = *t.object->volumes[scene->volume_idx()];
        const size_t id = body.id().id;
        const Transform3d world = scene->world_matrix();
        if (m_mode == Mode::Face) {
            for (const auto &p : m_faces)
                if (p.volume == id)
                    add_outline(selected, p.mesh->its, p.facets, world);
            if (m_hover && m_hover->volume == id)
                add_outline(hovered, m_hover->mesh->its, m_hover->facets, world);
        } else if (m_mode == Mode::Body) {
            for (const BodyPick &p : m_bodies) {
                if (p.volume != id)
                    continue;
                auto bounds = scene->bounding_box();
                if (p.shell >= 0) {
                    const auto &cache = mesh_cache(body);
                    if (!cache.solids || size_t(p.shell) >= cache.solids->size())
                        continue;
                    bounds = {};
                    for (size_t facet : (*cache.solids)[p.shell])
                        for (int c = 0; c < 3; ++c)
                            bounds.merge(Vec3d(cache.mesh->its.vertices[cache.mesh->its.indices[facet][c]].cast<double>()));
                }
                for (int corner = 0; corner < 8; ++corner) {
                    Vec3d p;
                    for (int axis = 0; axis < 3; ++axis)
                        p[axis] = (corner & (1 << axis)) ? bounds.max[axis] : bounds.min[axis];
                    for (int axis = 0; axis < 3; ++axis)
                        if (!(corner & (1 << axis))) {
                            Vec3d q = p;
                            q[axis] = bounds.max[axis];
                            add_segment(selected, world * p, world * q);
                        }
                }
            }
        }
    }
    m_selected_outline.reset();
    m_hover_outline.reset();
    if (!selected.is_empty())
        m_selected_outline.init_from(std::move(selected));
    if (!hovered.is_empty())
        m_hover_outline.init_from(std::move(hovered));
    m_outline_dirty = false;
}

void GLGizmoTransparency::on_render()
{
    auto t = target();
    if (!t)
        return;
    if (m_outline_dirty)
        build_outlines(*t);
    if (!m_selected_outline.is_initialized() && !m_hover_outline.is_initialized())
        return;
    auto *shader = wxGetApp().get_shader("dashed_thick_lines");
    const bool thick_lines = shader != nullptr;
    if (!shader)
        shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;
    const auto &camera = wxGetApp().plater()->get_camera();
    const auto &vp = camera.get_viewport();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    if (thick_lines) {
        shader->set_uniform("viewport_size", Vec2d(vp[2], vp[3]));
        shader->set_uniform("width", 3.f * m_imgui->get_style_scaling());
        shader->set_uniform("gap_size", 0.f);
        shader->set_uniform("dash_size", 1.f);
    }
    const bool depth = glIsEnabled(GL_DEPTH_TEST);
    glsafe(::glDisable(GL_DEPTH_TEST));
    m_selected_outline.set_color(ColorRGBA(0.05f, 0.9f, 1.f, 1.f));
    m_selected_outline.render();
    m_hover_outline.set_color(ColorRGBA(1.f, 0.8f, 0.1f, 1.f));
    m_hover_outline.render();
    if (depth)
        glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

void GLGizmoTransparency::on_render_input_window(float x, float y, float bottom_limit)
{
    sync_selection();
    const auto t = target();
    ImGuiPureWrap::begin(get_name(false), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowPos(ImVec2(x, std::max(0.f, std::min(y, bottom_limit - ImGui::GetWindowHeight()))), ImGuiCond_Always);
    if (t) {
        auto mode = [&](const std::string &label, Mode option) {
            if (ImGui::RadioButton(label.c_str(), m_mode == option)) {
                m_mode = option;
                m_hover.reset();
                m_outline_dirty = true;
                m_slider_snapshot = false;
            }
        };
        mode(_u8L("Whole part"), Mode::Whole); ImGui::SameLine();
        mode(_u8L("Bodies"), Mode::Body); ImGui::SameLine();
        mode(_u8L("Faces"), Mode::Face);
        auto &view = t->instance->view_transparency;
        int value = view.percent;
        bool can_edit = m_mode == Mode::Whole;
        auto body_percent = [&](size_t id, int shell, size_t facet, bool face) {
            const auto it = view.bodies.find(id);
            if (it == view.bodies.end())
                return view.percent;
            const auto *part = part_by_id(*t->object, id);
            const bool current = part && mesh_cache(*part).signature == it->second.mesh_signature;
            if (face)
                return it->second.effective(view.percent, facet, current);
            if (shell >= 0 && current && facet < it->second.shells.size() && it->second.shells[facet] <= 100)
                return it->second.shells[facet];
            return it->second.effective(view.percent, 0, false);
        };
        if (m_mode == Mode::Body) {
            auto choice = [&](const std::string &label, BodyPick p) {
                auto it = std::find(m_bodies.begin(), m_bodies.end(), p);
                bool selected = it != m_bodies.end();
                if (ImGui::Checkbox(label.c_str(), &selected)) {
                    if (selected)
                        m_bodies.push_back(p);
                    else
                        m_bodies.erase(it);
                    m_outline_dirty = true;
                }
            };
            for (const ModelVolume *part : t->object->volumes) {
                if (!part->is_model_part())
                    continue;
                const size_t id = part->id().id;
                ImGui::PushID(part);
                choice(part->name, {id});
                auto &cache = mesh_cache(*part);
                if (!cache.solids) {
                    if (!cache.regions)
                        cache.regions = std::make_unique<VT::Regions>(cache.mesh->its);
                    cache.solids = cache.regions->solids(cache.mesh->its);
                }
                if (!cache.solids->empty()) {
                    ImGui::Indent();
                    for (size_t i = 0; i < cache.solids->size(); ++i) {
                        char label[96];
                        std::snprintf(label, sizeof(label), _u8L("Solid %d").c_str(), int(i + 1));
                        choice(label, {id, int(i)});
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            can_edit = !m_bodies.empty();
            if (can_edit) {
                const auto &p = m_bodies.front();
                size_t facet = 0;
                if (p.shell >= 0)
                    facet = (*mesh_cache(*part_by_id(*t->object, p.volume)).solids)[p.shell].front();
                value = body_percent(p.volume, p.shell, facet, false);
            }
        } else if (m_mode == Mode::Face) {
            const bool through_changed = ImGui::Checkbox(_u8L("Pick through transparent surfaces").c_str(), &m_through);
            const bool hole_changed = ImGui::Checkbox(_u8L("Cylindrical hole wall").c_str(), &m_hole_wall);
            if (through_changed || hole_changed) {
                m_hover.reset();
                m_outline_dirty = true;
            }
            ImGui::Text(_u8L("Selected faces: %d").c_str(), int(m_faces.size()));
            can_edit = !m_faces.empty();
            if (can_edit)
                value = body_percent(m_faces.front().volume, -1, m_faces.front().facets.front(), true);
        }
        ImGui::SetNextItemWidth(175.f * m_imgui->get_style_scaling());
        m_imgui->disabled_begin(!can_edit);
        if (ImGui::SliderInt(_u8L("Transparency").c_str(), &value, 0, 100, "%d%%") && can_edit) {
            if (!m_slider_snapshot)
                wxGetApp().plater()->take_snapshot(_u8L("Transparency"));
            m_slider_snapshot = true;
            set_percent(*t, VT::Percent(value));
            set_dirty();
        }
        if (!ImGui::IsItemActive())
            m_slider_snapshot = false;
        if (ImGuiPureWrap::button(_u8L("Reset transparency")) && can_edit) {
            wxGetApp().plater()->take_snapshot(_u8L("Transparency"));
            set_percent(*t, VT::Inherit);
            set_dirty();
        }
        m_imgui->disabled_end();
        if (m_mode == Mode::Face && std::any_of(view.bodies.begin(), view.bodies.end(), [](const auto &item) { return !item.second.faces.empty(); }))
            if (ImGuiPureWrap::button(_u8L("Reset all face transparency"))) {
                wxGetApp().plater()->take_snapshot(_u8L("Transparency"));
                for (auto it = view.bodies.begin(); it != view.bodies.end();) {
                    it->second.faces.clear();
                    if (it->second.empty())
                        it = view.bodies.erase(it);
                    else
                        ++it;
                }
                set_dirty();
            }
    } else {
        ImGuiPureWrap::text(_u8L("Select one part instance."));
    }
    ImGuiPureWrap::end();
}

} // namespace Slic3r::GUI
