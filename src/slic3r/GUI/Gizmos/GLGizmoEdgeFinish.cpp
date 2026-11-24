///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include <chrono>
#include "GLGizmoEdgeFinish.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <GL/glew.h>
#include <wx/glcanvas.h>
#include <wx/settings.h>
#include <algorithm>
#include <set>

namespace Slic3r::GUI {
namespace EF = Slic3r::MeshEdgeFinish;
namespace {

std::string error_text(EF::Error error)
{
    switch (error) {
    case EF::Error::InvalidMesh: return _u8L("Edge finishing requires one closed, connected mesh without self-intersections.");
    case EF::Error::InvalidSize: return _u8L("Enter a positive size that can be represented by this mesh.");
    case EF::Error::Adjacent: return _u8L("Selected edges meet. Remove one of the highlighted edges.");
    case EF::Error::TooLarge: return _u8L("The size exceeds the available space at the highlighted edge.");
    case EF::Error::Overlap: return _u8L("The cuts touch or overlap. Reduce the size or remove an edge.");
    case EF::Error::ProtectedFace: return _u8L("The cut reaches a neighboring surface. Reduce the size.");
    case EF::Error::BooleanFailed: return _u8L("The result could not be verified as a solid mesh. Try a smaller size or fewer edges.");
    default: return {};
    }
}

double min_z(const TriangleMesh &mesh, const Transform3d &world)
{
    double z = std::numeric_limits<double>::infinity();
    for (const auto &v : mesh.its.vertices)
        z = std::min(z, world.linear().row(2).dot(v.cast<double>()) + world.translation().z());
    return z;
}

} // namespace

GLGizmoEdgeFinish::GLGizmoEdgeFinish(GLCanvas3D &canvas)
    : GLGizmoBase(canvas, "edge_finish.svg", 16)
{}

GLGizmoEdgeFinish::~GLGizmoEdgeFinish()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_worker.valid())
        m_worker.wait();
}

bool GLGizmoEdgeFinish::on_is_selectable() const
{
#ifdef _WIN64
    return m_parent.current_printer_technology() == ptFFF;
#else
    return false;
#endif
}

bool GLGizmoEdgeFinish::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    return on_is_selectable() && !selection.is_empty() && !selection.is_wipe_tower() && !m_parent.is_layers_editing_enabled();
}

std::string GLGizmoEdgeFinish::on_get_name() const { return _u8L("Chamfer / Fillet"); }

std::optional<GLGizmoEdgeFinish::Target> GLGizmoEdgeFinish::selected_target() const
{
    const Selection &selection = m_parent.get_selection();
    if (!on_is_activable() || !selection.is_single_full_instance() || selection.get_mode() != Selection::Instance ||
        selection.get_volume_idxs().size() != 1)
        return std::nullopt;
    const GLVolume &scene = *selection.get_first_volume();
    if (scene.object_idx() < 0 || scene.volume_idx() != 0)
        return std::nullopt;
    const ModelObject *object = selection.get_model()->objects[scene.object_idx()];
    if (object->instances.size() != 1 || object->volumes.size() != 1 || object->is_cut())
        return std::nullopt;
    const ModelVolume &volume = *object->volumes.front();
    if (!volume.is_model_part() || volume.is_text() || volume.is_svg())
        return std::nullopt;
    return Target{scene.geometry_id, volume.mesh_ptr(), scene.world_matrix()};
}

void GLGizmoEdgeFinish::invalidate_preview()
{
    if (m_cancel)
        m_cancel->store(true);
    m_preview = {};
    m_preview_model.reset();
    m_preview_cut.reset();
    m_show_preview = false;
    m_error_edge.reset();
    m_related_edge.reset();
    m_status.clear();
    m_lines_dirty = true;
    set_dirty();
}

void GLGizmoEdgeFinish::sync_target()
{
    auto next = selected_target();
    if (next == m_target)
        return;
    invalidate_preview();
    m_target = std::move(next);
    m_analysis.reset();
    m_analysis_failed = false;
    m_down.reset();
    m_anchors.clear();
    m_selected.clear();
    m_hover_edge.reset();
}

void GLGizmoEdgeFinish::on_set_state()
{
    if (m_state == On) {
        sync_target();
    } else {
        invalidate_preview();
        m_target.reset();
        m_analysis.reset();
        m_analysis_failed = false;
        m_down.reset();
        m_anchors.clear();
        m_selected.clear();
        m_hover_edge.reset();
        m_selection_lines.reset();
        m_hover_lines.reset();
    }
}

void GLGizmoEdgeFinish::data_changed(bool)
{
    if (m_state == On)
        sync_target();
}

bool GLGizmoEdgeFinish::escape()
{
    if (!m_anchors.empty()) {
        m_anchors.clear();
        m_status.clear();
        set_dirty();
        return true;
    }
    return false;
}

void GLGizmoEdgeFinish::poll()
{
    using namespace std::chrono_literals;
    sync_target();
    if (m_worker.valid() && m_worker.wait_for(0ms) == std::future_status::ready) {
        Work work = m_worker.get();
        const bool cancelled = m_cancel->load();
        m_cancel.reset();
        if (!cancelled) {
            if (work.analysis)
                m_analysis = std::move(work.analysis);
            else if (work.mesh) {
                m_preview = std::move(work);
                m_show_preview = true;
            } else if (!m_analysis) {
                m_analysis_failed = true;
            } else {
                m_status = error_text(work.error);
                if (work.error == EF::Error::TooLarge || work.error == EF::Error::Adjacent ||
                    work.error == EF::Error::Overlap || work.error == EF::Error::ProtectedFace) {
                    m_error_edge = work.edge;
                    m_related_edge = work.other_edge;
                }
            }
            m_lines_dirty = true;
            m_parent.request_extra_frame();
        }
    }
    if (m_target && !m_analysis && !m_analysis_failed && !m_worker.valid()) {
        m_cancel = std::make_shared<std::atomic_bool>(false);
        m_worker = std::async(std::launch::async, [target = *m_target, stop = m_cancel] {
            Work work;
            try {
                work.analysis = EF::analyze(target.mesh->its, target.pose.linear(), [&] { return stop->load(); });
            } catch (const std::exception &) {
                work.error = EF::Error::InvalidMesh;
            }
            return work;
        });
    }
    if (m_worker.valid())
        m_parent.schedule_extra_frame(33);
}

void GLGizmoEdgeFinish::start_preview()
{
    invalidate_preview();
    const auto &selection = m_parent.get_selection();
    const ModelVolume *volume = selection.get_model()->objects[selection.get_object_idx()]->volumes.front();
    double width = 0.;
    if (const auto *presets = wxGetApp().preset_bundle)
        try {
            width = Flow::extrusion_width("external_perimeter_extrusion_width", presets->full_config(), std::max(1, volume->extruder_id()) - 1);
        } catch (const std::exception &) {
            // Geometry preview works without the optional reference extrusion width.
        }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_worker = std::async(std::launch::async,
        [analysis = m_analysis, edges = m_selected, operation = m_operation, size = m_size, width, target = *m_target, stop = m_cancel] {
        Work work;
        try {
            EF::Result result = EF::finish(*analysis, edges, operation, size, width, [&] { return stop->load(); });
            work.error = result.error;
            work.edge = result.edge;
            work.other_edge = result.other_edge;
            work.thin_wall = result.thin_wall;
            if (result.error == EF::Error::None && !stop->load()) {
                for (auto &surface : work.preview_surfaces)
                    surface.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
                for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
                    const auto &triangle = result.mesh.indices[i];
                    const Vec3f &a = result.mesh.vertices[triangle[0]], &b = result.mesh.vertices[triangle[1]], &c = result.mesh.vertices[triangle[2]];
                    const Vec3f normal = (b - a).cross(c - a).normalized();
                    auto &surface = work.preview_surfaces[result.cut_faces[i]];
                    const auto first = static_cast<unsigned int>(surface.vertices_count());
                    surface.add_vertex(a, normal);
                    surface.add_vertex(b, normal);
                    surface.add_vertex(c, normal);
                    surface.add_triangle(first, first + 1, first + 2);
                }
                auto mesh = std::make_shared<const TriangleMesh>(std::move(result.mesh));
                auto hull = std::make_shared<const TriangleMesh>(mesh->convex_hull_3d());
                if (hull->empty()) {
                    work.error = EF::Error::BooleanFailed;
                    return work;
                }
                const double before = min_z(*target.mesh, target.pose), after = min_z(*mesh, target.pose);
                if (std::abs(before) <= analysis->tolerance && after > analysis->tolerance)
                    work.dz = -after;
                work.mesh = std::move(mesh);
                work.hull = std::move(hull);
            }
        } catch (const std::exception &) {
            work.error = EF::Error::BooleanFailed;
        }
        return work;
    });
    m_parent.schedule_extra_frame(33);
}

void GLGizmoEdgeFinish::close()
{
    m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::EdgeFinish);
    m_parent.request_extra_frame();
}

void GLGizmoEdgeFinish::apply()
{
    const auto target = selected_target();
    if (!m_preview.mesh || !m_target || !(target == m_target))
        return;
    Plater *plater = wxGetApp().plater();
    const int idx = m_parent.get_selection().get_object_idx();
    ModelObject *object = plater->model().objects[idx];
    ModelVolume *volume = object->volumes.front();
    if (volume->is_fdm_support_painted() || volume->is_seam_painted() || volume->is_mm_painted() || volume->is_fuzzy_skin_painted()) {
        MessageDialog dialog(plater,
            _L("Applying this cut clears painted supports, seams, materials and fuzzy skin on this part. Continue?"),
            _L("Chamfer / Fillet"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        if (dialog.ShowModal() != wxID_YES || !(selected_target() == target) || !m_preview.mesh)
            return;
    }
    Work result = std::move(m_preview);
    const std::string action = m_operation == EF::Operation::Chamfer ? _u8L("Chamfer") : _u8L("Fillet");
    close();
    Plater::TakeSnapshot snapshot(plater, action);
    object = plater->model().objects[idx];
    volume = object->volumes.front();
    plater->clear_before_change_mesh(idx, _u8L("Surface painting was removed after edge finishing."));
    volume->set_mesh_and_convex_hull(std::move(result.mesh), std::move(result.hull));
    volume->discard_splittable();
    volume->set_new_unique_id();
    volume->source = ModelVolume::Source();
    if (result.dz != 0.)
        object->translate_instance(0, Vec3d(0., 0., result.dz));
    object->invalidate_bounding_box();
    plater->changed_mesh(idx);
    wxGetApp().obj_list()->update_info_items(idx);
    wxGetApp().obj_list()->update_item_error_icon(idx, -1);
}

std::vector<GLGizmoEdgeFinish::Pick> GLGizmoEdgeFinish::pick(const Vec2d &mouse) const
{
    std::vector<Pick> picks;
    if (!m_analysis || !m_target || m_show_preview || !(selected_target() == m_target))
        return picks;
    const Selection &selection = m_parent.get_selection();
    const GLVolume &volume = *selection.get_first_volume();
    if (!volume.mesh_raycaster)
        return picks;
    const Camera &camera = wxGetApp().plater()->get_camera();
    const auto &viewport = camera.get_viewport();
    const Matrix4d projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const Vec3d origin = m_target->pose * m_analysis->local_origin;
    const Matrix3d to_world = m_target->pose.linear() * m_analysis->from_work;
    const double radius = 8. * m_imgui->get_style_scaling();
    std::set<std::pair<size_t, size_t>> candidates;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            Vec3f hit, normal;
            size_t facet = 0;
            const Vec2d probe = mouse + Vec2d(x, y) * (radius * 0.75);
            if (volume.mesh_raycaster->closest_hit(probe, m_target->pose, camera, hit, normal, nullptr, &facet) &&
                facet < m_analysis->facet_segments.size()) {
                const auto &found = m_analysis->facet_segments[facet];
                candidates.insert(found.begin(), found.end());
            }
        }
    auto screen = [&](const Vec4d &p) -> Vec2d {
        return {viewport[0] + 0.5 * viewport[2] * (1. + p.x() / p.w()),
            0.5 * viewport[3] * (1. - p.y() / p.w())};
    };
    for (const auto &[edge_id, segment_id] : candidates) {
        const EF::Edge &edge = m_analysis->edges[edge_id];
        if ((m_mode == PickMode::Straight && edge.kind != EF::Kind::Straight) ||
            (m_mode == PickMode::Rim && edge.kind != EF::Kind::Rim))
            continue;
        const auto &segment = edge.segments[segment_id];
        const Vec3d u = m_analysis->mesh.vertices[segment.from], v = m_analysis->mesh.vertices[segment.to];
        const Vec3d ua = origin + to_world * u, vb = origin + to_world * v;
        const Vec4d a = projection * Vec4d(ua.x(), ua.y(), ua.z(), 1.);
        const Vec4d b = projection * Vec4d(vb.x(), vb.y(), vb.z(), 1.);
        if (a.w() <= 0. || b.w() <= 0.)
            continue;
        const Vec2d p = screen(a), q = screen(b), d = q - p;
        if (d.squaredNorm() < 1e-8)
            continue;
        const double t = std::clamp((mouse - p).dot(d) / d.squaredNorm(), 0., 1.);
        const Vec2d nearest = p + t * d;
        const double distance = (nearest - mouse).norm();
        if (distance > radius)
            continue;
        const double w = (t / b.w()) / ((1. - t) / a.w() + t / b.w());
        const Vec4d clip = (1. - w) * a + w * b;
        if (std::abs(clip.z()) > clip.w())
            continue;
        const Vec3d at = u + w * (v - u);
        const auto hit = m_parent.get_scene_raycaster().hit(nearest, camera);
        const double epsilon = std::max(m_analysis->tolerance, 0.20 * camera.get_inv_zoom());
        if (hit.is_valid() && camera.get_dir_forward().dot(hit.position.cast<double>() - (origin + to_world * at)) < -epsilon)
            continue;
        auto existing = std::find_if(picks.begin(), picks.end(), [edge_id](const Pick &p) { return p.edge == edge_id; });
        if (existing == picks.end())
            picks.push_back({edge_id, at, distance});
        else if (distance < existing->pixels)
            *existing = {edge_id, at, distance};
    }
    std::sort(picks.begin(), picks.end(), [](const Pick &a, const Pick &b) { return a.pixels < b.pixels; });
    return picks;
}

void GLGizmoEdgeFinish::accept_pick(std::vector<Pick> picks)
{
    if (picks.empty()) {
        m_status = _u8L("No eligible edge here. Move closer to a visible edge.");
        return;
    }
    const double ambiguity = 2. * m_imgui->get_style_scaling();
    if (m_mode == PickMode::Direct && picks.size() > 1 && picks[1].pixels - picks[0].pixels < ambiguity) {
        m_status = _u8L("More than one edge is nearby. Rotate the view or use point selection.");
        return;
    }
    size_t edge = picks.front().edge;
    invalidate_preview();
    if (m_mode != PickMode::Direct) {
        std::vector<Pick> matches;
        for (const Pick &candidate : picks)
            if (std::all_of(m_anchors.begin(), m_anchors.end(), [&](const auto &anchor) {
                return std::any_of(anchor.begin(), anchor.end(), [&](const Pick &p) { return p.edge == candidate.edge; });
            }))
                matches.push_back(candidate);
        if (matches.empty()) {
            m_status = _u8L("Pick points on the same straight edge or hole rim.");
            return;
        }
        const size_t needed = m_mode == PickMode::Straight ? 2 : 3;
        if (m_anchors.size() + 1 < needed) {
            m_anchors.push_back(std::move(matches));
            return;
        }
        if (matches.size() != 1) {
            m_status = _u8L("More than one edge is nearby. Rotate the view or use point selection.");
            return;
        }
        edge = matches.front().edge;
        std::vector<EF::Vec3> points;
        for (const auto &anchor : m_anchors)
            points.push_back(std::find_if(anchor.begin(), anchor.end(), [&](const Pick &p) { return p.edge == edge; })->point);
        points.push_back(matches.front().point);
        if (!EF::points_span_edge(*m_analysis, edge, points)) {
            m_status = _u8L("Spread the points further apart along the edge.");
            return;
        }
        m_anchors.clear();
    }
    const auto it = std::find(m_selected.begin(), m_selected.end(), edge);
    if (it == m_selected.end())
        m_selected.push_back(edge);
    else
        m_selected.erase(it);
    m_lines_dirty = true;
}

bool GLGizmoEdgeFinish::on_mouse(const wxMouseEvent &event)
{
    const Vec2d pos(event.GetX(), event.GetY());
    if (event.Leaving() || event.Dragging()) {
        if (m_hover_edge) {
            m_hover_edge.reset();
            m_lines_dirty = true;
            set_dirty();
        }
    }
    if (event.Leaving())
        m_down.reset();
    if (event.LeftDown())
        m_down = pos;
    if (m_down && (event.Dragging() || event.LeftUp())) {
        const int dx = std::max(3, wxSystemSettings::GetMetric(wxSYS_DRAG_X, m_parent.get_wxglcanvas()));
        const int dy = std::max(3, wxSystemSettings::GetMetric(wxSYS_DRAG_Y, m_parent.get_wxglcanvas()));
        if (std::abs(pos.x() - m_down->x()) >= dx || std::abs(pos.y() - m_down->y()) >= dy)
            m_down.reset();
        if (event.LeftUp() && m_down) {
            m_down.reset();
            if (!m_show_preview)
                accept_pick(pick(pos));
            set_dirty();
            return true;
        }
    }
    if (event.Moving()) {
        const auto found = pick(pos);
        std::optional<size_t> hover;
        if (!found.empty() && (found.size() == 1 || found[1].pixels - found.front().pixels >= 2. * m_imgui->get_style_scaling()))
            hover = found.front().edge;
        if (m_hover_edge != hover) {
            m_hover_edge = hover;
            m_lines_dirty = true;
            set_dirty();
        }
    }
    return false;
}

bool GLGizmoEdgeFinish::replaces_volume(const GLVolume &volume) const
{
    return m_show_preview && m_preview.mesh && m_target && m_target->id == volume.geometry_id &&
        m_target->pose.matrix() == volume.world_matrix().matrix();
}

void GLGizmoEdgeFinish::update_lines(GLModel &model, const std::vector<size_t> &ids)
{
    model.reset();
    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    unsigned int index = 0;
    for (size_t id : ids)
        for (const auto &segment : m_analysis->edges[id].segments) {
            data.add_vertex(Vec3f(m_analysis->mesh.vertices[segment.from].cast<float>()));
            data.add_vertex(Vec3f(m_analysis->mesh.vertices[segment.to].cast<float>()));
            data.add_line(index, index + 1);
            index += 2;
        }
    if (index > 0)
        model.init_from(std::move(data));
}

void GLGizmoEdgeFinish::on_render()
{
    if (!m_analysis || !(selected_target() == m_target))
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    if (m_show_preview && m_preview.mesh) {
        GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
        if (!shader)
            return;
        if (!m_preview_model.is_initialized()) {
            m_preview_model.init_from(std::move(m_preview.preview_surfaces[0]));
            m_preview_model.set_color(ColorRGBA(0.56f, 0.61f, 0.67f, 1.f));
            m_preview_cut.init_from(std::move(m_preview.preview_surfaces[1]));
            m_preview_cut.set_color(ColorRGBA(1.f, 0.44f, 0.04f, 1.f));
        }
        Transform3d pose = m_target->pose;
        pose.translation().z() += m_preview.dz;
        const Transform3d view = camera.get_view_matrix() * pose;
        const Matrix3d normal = view.linear().inverse().transpose();
        const bool depth = ::glIsEnabled(GL_DEPTH_TEST);
        GLint front = GL_CCW;
        glsafe(::glGetIntegerv(GL_FRONT_FACE, &front));
        glsafe(::glEnable(GL_DEPTH_TEST));
        if (m_analysis->mirrored)
            glsafe(::glFrontFace(GL_CW));
        shader->start_using();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", view);
        shader->set_uniform("view_normal_matrix", normal);
        m_preview_model.render();
        m_preview_cut.render();
        shader->stop_using();
        glsafe(::glFrontFace(front));
        if (!depth)
            glsafe(::glDisable(GL_DEPTH_TEST));
        return;
    }
    const auto highlight = m_error_edge ? m_error_edge : m_hover_edge;
    if (m_lines_dirty) {
        update_lines(m_selection_lines, m_selected);
        std::vector<size_t> highlights;
        if (highlight && (m_error_edge || std::find(m_selected.begin(), m_selected.end(), *highlight) == m_selected.end()))
            highlights.push_back(*highlight);
        if (m_related_edge && m_related_edge != highlight)
            highlights.push_back(*m_related_edge);
        update_lines(m_hover_lines, highlights);
        m_lines_dirty = false;
    }
    GLShaderProgram *shader = wxGetApp().get_shader("dashed_thick_lines");
    const bool thick_lines = shader != nullptr;
    if (!shader)
        shader = wxGetApp().get_shader("flat");
    if (!shader || (!m_selection_lines.is_initialized() && !m_hover_lines.is_initialized()))
        return;
    Transform3d pose = Transform3d::Identity();
    pose.linear() = m_target->pose.linear() * m_analysis->from_work;
    pose.translation() = m_target->pose * m_analysis->local_origin - 0.05 * camera.get_inv_zoom() * camera.get_dir_forward();
    const Transform3d view = camera.get_view_matrix() * pose;
    const bool depth = ::glIsEnabled(GL_DEPTH_TEST);
    GLint depth_func;
    GLfloat line_width, range[2];
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
    glsafe(::glGetFloatv(GL_LINE_WIDTH, &line_width));
    glsafe(::glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LEQUAL));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", view);
    if (thick_lines) {
        const auto &viewport = camera.get_viewport();
        shader->set_uniform("viewport_size", Vec2d(viewport[2], viewport[3]));
        shader->set_uniform("dash_size", 1.f);
        shader->set_uniform("gap_size", 0.f);
    }
    auto draw = [&](GLModel &model, float width, const ColorRGBA &color) {
        width *= m_imgui->get_style_scaling();
        if (thick_lines)
            shader->set_uniform("width", width);
        else
            glsafe(::glLineWidth(std::clamp(width, range[0], range[1])));
        model.set_color(color);
        model.render();
    };
    const ColorRGBA outline(0.06f, 0.09f, 0.14f, 1.f);
    draw(m_selection_lines, 7.f, outline);
    draw(m_selection_lines, 4.f, ColorRGBA(0.08f, 0.95f, 1.f, 1.f));
    draw(m_hover_lines, 8.f, outline);
    draw(m_hover_lines, 4.f, m_error_edge ? ColorRGBA(1.f, 0.31f, 0.15f, 1.f) : ColorRGBA(1.f, 0.81f, 0.12f, 1.f));
    shader->stop_using();
    glsafe(::glLineWidth(line_width));
    glsafe(::glDepthFunc(depth_func));
    if (!depth)
        glsafe(::glDisable(GL_DEPTH_TEST));
}

void GLGizmoEdgeFinish::on_render_input_window(float x, float y, float bottom_limit)
{
    poll();
    ImGuiPureWrap::begin(get_name(false), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowPos(ImVec2(x, std::max(0.f, std::min(y, bottom_limit - ImGui::GetWindowHeight()))), ImGuiCond_Always);
    if (!m_target) {
        ImGuiPureWrap::text(_u8L("Select one independent model part. Use Set as a Separated Object for shared copies."));
    } else if (m_analysis_failed) {
        ImGuiPureWrap::text(error_text(EF::Error::InvalidMesh));
    } else if (!m_analysis) {
        ImGuiPureWrap::text(_u8L("Finding eligible edges..."));
    } else if (m_analysis->edges.empty()) {
        ImGuiPureWrap::text(_u8L("No supported straight edges or cylindrical hole rims were found."));
    } else {
        auto mode = [&](const std::string &label, PickMode value) {
            if (ImGui::RadioButton(label.c_str(), m_mode == value)) {
                m_mode = value;
                m_anchors.clear();
                m_hover_edge.reset();
                m_status.clear();
                m_lines_dirty = true;
            }
        };
        mode(_u8L("Direct"), PickMode::Direct); ImGui::SameLine();
        mode(_u8L("Two points"), PickMode::Straight); ImGui::SameLine();
        mode(_u8L("Three points"), PickMode::Rim);
        if (m_mode != PickMode::Direct)
            ImGui::Text(_u8L("Points: %d / %d").c_str(), int(m_anchors.size()), m_mode == PickMode::Straight ? 2 : 3);
        ImGui::Text(_u8L("Selected edges: %d").c_str(), int(m_selected.size()));
        if (!m_selected.empty() || !m_anchors.empty()) {
            ImGui::SameLine();
            if (ImGuiPureWrap::button(_u8L("Clear"))) {
                m_selected.clear();
                m_anchors.clear();
                invalidate_preview();
            }
        }
        if (ImGui::RadioButton(_u8L("Chamfer").c_str(), m_operation == EF::Operation::Chamfer)) {
            m_operation = EF::Operation::Chamfer;
            invalidate_preview();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Fillet").c_str(), m_operation == EF::Operation::Fillet)) {
            m_operation = EF::Operation::Fillet;
            invalidate_preview();
        }
        ImGui::TextUnformatted((m_operation == EF::Operation::Chamfer ? _u8L("Setback (mm)") : _u8L("Radius (mm)")).c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f * m_imgui->get_style_scaling());
        if (ImGui::InputDouble("##edge_finish_size", &m_size, 0., 0., "%.4f"))
            invalidate_preview();

        const EF::Limits limit = EF::limits(*m_analysis, m_selected, m_operation);
        const bool valid_size = std::isfinite(m_size) && m_size > m_analysis->tolerance;
        const EF::Error check = limit.error != EF::Error::None ? limit.error :
            !valid_size ? EF::Error::InvalidSize : m_size > limit.maximum ? EF::Error::TooLarge : EF::Error::None;
        if (limit.error == EF::Error::None && std::isfinite(limit.maximum)) {
            const auto text = _u8L("Preliminary limit: %.4f mm");
            ImGui::Text(text.c_str(), std::floor(std::max(0., limit.maximum) * 10000.) / 10000.);
        }
        if (check == EF::Error::TooLarge || check == EF::Error::Adjacent) {
            if (m_error_edge != limit.limiting_edge || m_related_edge != limit.other_edge) {
                m_error_edge = limit.limiting_edge;
                m_related_edge = limit.other_edge;
                m_lines_dirty = true;
            }
        }
        if (check != EF::Error::None && check != EF::Error::NoEdges)
            ImGuiPureWrap::text(error_text(check == EF::Error::TooLarge && limit.other_edge ? EF::Error::Overlap : check));
        if (!m_status.empty())
            ImGuiPureWrap::text(m_status);
        if (m_worker.valid())
            ImGuiPureWrap::text(m_cancel->load() ? _u8L("Discarding previous preview...") : _u8L("Calculating preview..."));
        if (m_preview.mesh) {
            ImGui::Checkbox(_u8L("Show result").c_str(), &m_show_preview);
            if (m_preview.thin_wall)
                ImGuiPureWrap::text(_u8L("A wall near the cut is thinner than the reference extrusion width. Check the sliced toolpaths."));
            if (m_preview.dz != 0.)
                ImGui::Text(_u8L("Z change to keep the part on the bed: %.4f mm").c_str(), m_preview.dz);
        }
        m_imgui->disabled_begin(m_worker.valid() || check != EF::Error::None || !m_anchors.empty());
        const bool preview = ImGuiPureWrap::button(_u8L("Preview"));
        m_imgui->disabled_end();
        ImGui::SameLine();
        m_imgui->disabled_begin(!m_preview.mesh);
        const bool apply_now = ImGuiPureWrap::button(_u8L("Apply"));
        m_imgui->disabled_end();
        ImGui::SameLine();
        const bool cancel = ImGuiPureWrap::button(_u8L("Cancel"));
        ImGuiPureWrap::end();
        if (cancel)
            close();
        else if (apply_now)
            apply();
        else if (preview) {
            auto *app = &wxGetApp();
            app->CallAfter([gizmo = this, app] {
                const auto *plater = app->plater();
                const auto *canvas = plater ? plater->canvas3D() : nullptr;
                if (canvas && canvas->get_gizmos_manager().get_current() == gizmo && gizmo->m_analysis &&
                    !gizmo->m_worker.valid() && gizmo->m_target && gizmo->selected_target() == gizmo->m_target)
                    gizmo->start_preview();
            });
        }
        return;
    }
    const bool cancel = ImGuiPureWrap::button(_u8L("Cancel"));
    ImGuiPureWrap::end();
    if (cancel)
        close();
}

} // namespace Slic3r::GUI
