///|/ Copyright (c) Prusa Research 2019 - 2023 Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Filip Sykala @Jony01
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GLGizmoMeasure.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectManipulation.hpp"
#include "slic3r/GUI/TransparentFacePicking.hpp"


#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeasureUtils.hpp"

#include <imgui/imgui.h>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <numeric>

#include <GL/glew.h>

#include <tbb/parallel_for.h>

#include <wx/clipbrd.h>
#include <wx/glcanvas.h>
#include <wx/settings.h>

namespace Slic3r {
namespace GUI {

static const Slic3r::ColorRGBA SELECTED_1ST_COLOR = { 0.25f, 0.75f, 0.75f, 1.0f };
static const Slic3r::ColorRGBA SELECTED_2ND_COLOR = { 0.75f, 0.25f, 0.75f, 1.0f };
static const Slic3r::ColorRGBA NEUTRAL_COLOR      = { 0.5f, 0.5f, 0.5f, 1.0f };
static const Slic3r::ColorRGBA REFERENCE_COLOR    = { 0.923f, 0.504f, 0.264f, 1.0f };

static const int POINT_ID         = 100;
static const int EDGE_ID          = 200;
static const int CIRCLE_ID        = 300;
static const int PLANE_ID         = 400;
static const int SEL_SPHERE_1_ID  = 501;
static const int SEL_SPHERE_2_ID  = 502;

static const float TRIANGLE_BASE = 10.0f;
static const float TRIANGLE_HEIGHT = TRIANGLE_BASE * 1.618033f;

static const std::string CTRL_STR =
#ifdef __APPLE__
"⌘"
#else
"Ctrl"
#endif //__APPLE__
;

static std::string format_double(double value)
{
    char buf[1024];
    sprintf(buf, "%.3f", value);
    return std::string(buf);
}

static std::string format_vec3(const Vec3d& v)
{
    char buf[1024];
    sprintf(buf, "X: %.3f, Y: %.3f, Z: %.3f", v.x(), v.y(), v.z());
    return std::string(buf);
}

static std::string measurement_length(double length)
{
    const bool inches = wxGetApp().app_config->get_bool("use_inches");
    return format_double(inches ? length * ObjectManipulation::mm_to_in : length) + (inches ? " in" : " mm");
}

static std::string surface_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    switch (type)
    {
    default:
    case Measure::SurfaceFeatureType::Undef:  { return ("No feature"); }
    case Measure::SurfaceFeatureType::Point:  { return _u8L("Vertex"); }
    case Measure::SurfaceFeatureType::Edge:   { return _u8L("Edge"); }
    case Measure::SurfaceFeatureType::Circle: { return _u8L("Circle"); }
    case Measure::SurfaceFeatureType::Plane:  { return _u8L("Plane"); }
    }
}

static std::string point_on_feature_type_as_string(Measure::SurfaceFeatureType type, int hover_id)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Point:  { ret = _u8L("Vertex"); break; }
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Point on edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Point on circle"); break; }
    case Measure::SurfaceFeatureType::Plane:  { ret = _u8L("Point on plane"); break; }
    default:                                  { assert(false); break; }
    }
    return ret;
}

static std::string center_on_feature_type_as_string(Measure::SurfaceFeatureType type)
{
    std::string ret;
    switch (type) {
    case Measure::SurfaceFeatureType::Edge:   { ret = _u8L("Center of edge"); break; }
    case Measure::SurfaceFeatureType::Circle: { ret = _u8L("Center of circle"); break; }
    default: { assert(false); break; }
    }
    return ret;
}

static GLModel::Geometry init_plane_data(const indexed_triangle_set& its, const std::vector<int>& triangle_indices)
{
    GLModel::Geometry init_data;
    init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    init_data.reserve_indices(3 * triangle_indices.size());
    init_data.reserve_vertices(3 * triangle_indices.size());
    unsigned int i = 0;
    for (int idx : triangle_indices) {
        const Vec3f& v0 = its.vertices[its.indices[idx][0]];
        const Vec3f& v1 = its.vertices[its.indices[idx][1]];
        const Vec3f& v2 = its.vertices[its.indices[idx][2]];

        const Vec3f n = (v1 - v0).cross(v2 - v0).normalized();
        init_data.add_vertex(v0, n);
        init_data.add_vertex(v1, n);
        init_data.add_vertex(v2, n);
        init_data.add_triangle(i, i + 1, i + 2);
        i += 3;
    }

    return init_data;
}

static GLModel::Geometry init_torus_data(unsigned int primary_resolution, unsigned int secondary_resolution, const Vec3f& center,
    float radius, float thickness, const Vec3f& model_axis, const Transform3f& world_trafo)
{
    const unsigned int torus_sector_count = std::max<unsigned int>(4, primary_resolution);
    const unsigned int section_sector_count = std::max<unsigned int>(4, secondary_resolution);
    const float torus_sector_step = 2.0f * float(M_PI) / float(torus_sector_count);
    const float section_sector_step = 2.0f * float(M_PI) / float(section_sector_count);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(torus_sector_count * section_sector_count);
    data.reserve_indices(torus_sector_count * section_sector_count * 2 * 3);

    // vertices
    const Transform3f local_to_world_matrix = world_trafo * Geometry::translation_transform(center.cast<double>()).cast<float>() *
        Eigen::Quaternion<float>::FromTwoVectors(Vec3f::UnitZ(), model_axis);
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const float section_angle = torus_sector_step * i;
        const Vec3f radius_dir(std::cos(section_angle), std::sin(section_angle), 0.0f);
        const Vec3f local_section_center = radius * radius_dir;
        const Vec3f world_section_center = local_to_world_matrix * local_section_center;
        const Vec3f local_section_normal = local_section_center.normalized().cross(Vec3f::UnitZ()).normalized();
        const Vec3f world_section_normal = (Vec3f)(local_to_world_matrix.matrix().block(0, 0, 3, 3) * local_section_normal).normalized();
        const Vec3f base_v = thickness * radius_dir;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const Vec3f v = Eigen::AngleAxisf(section_sector_step * j, world_section_normal) * base_v;
            data.add_vertex(world_section_center + v, (Vec3f)v.normalized());
        }
    }

    // triangles
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const unsigned int ii = i * section_sector_count;
        const unsigned int ii_next = ((i + 1) % torus_sector_count) * section_sector_count;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const unsigned int j_next = (j + 1) % section_sector_count;
            const unsigned int i0 = ii + j;
            const unsigned int i1 = ii_next + j;
            const unsigned int i2 = ii_next + j_next;
            const unsigned int i3 = ii + j_next;
            data.add_triangle(i0, i1, i2);
            data.add_triangle(i0, i2, i3);
        }
    }

    return data;
}

static bool is_feature_with_center(const Measure::SurfaceFeature& feature)
{
    const Measure::SurfaceFeatureType type = feature.get_type();
    return (type == Measure::SurfaceFeatureType::Circle || (type == Measure::SurfaceFeatureType::Edge && feature.get_extra_point().has_value()));
}

static Vec3d get_feature_offset(const Measure::SurfaceFeature& feature)
{
    Vec3d ret;
    switch (feature.get_type())
    {
    case Measure::SurfaceFeatureType::Circle:
    {
        const auto [center, radius, normal] = feature.get_circle();
        ret = center;
        break;
    }
    case Measure::SurfaceFeatureType::Edge:
    {
        std::optional<Vec3d> p = feature.get_extra_point();
        assert(p.has_value());
        ret = *p;
        break;
    }
    case Measure::SurfaceFeatureType::Point:
    {
        ret = feature.get_point();
        break;
    }
    default: { assert(false); }
    }
     
    return ret;
}

class TransformHelper
{
    struct Cache
    {
        std::array<int, 4> viewport;
        Matrix4d ndc_to_ss_matrix;
        Transform3d ndc_to_ss_matrix_inverse;
    };

    static Cache s_cache;

public:
    static Vec3d model_to_world(const Vec3d& model, const Transform3d& world_matrix) {
        return world_matrix * model;
    }

    static Vec4d world_to_clip(const Vec3d& world, const Matrix4d& projection_view_matrix) {
        return projection_view_matrix * Vec4d(world.x(), world.y(), world.z(), 1.0);
    }

    static Vec3d clip_to_ndc(const Vec4d& clip) {
        return Vec3d(clip.x(), clip.y(), clip.z()) / clip.w();
    }

    static Vec2d ndc_to_ss(const Vec3d& ndc, const std::array<int, 4>& viewport) {
        const double half_w = 0.5 * double(viewport[2]);
        const double half_h = 0.5 * double(viewport[3]);
        return { half_w * ndc.x() + double(viewport[0]) + half_w, half_h * ndc.y() + double(viewport[1]) + half_h };
    };

    static Vec4d model_to_clip(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix) {
        return world_to_clip(model_to_world(model, world_matrix), projection_view_matrix);
    }

    static Vec3d model_to_ndc(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix) {
        return clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix));
    }

    static Vec2d model_to_ss(const Vec3d& model, const Transform3d& world_matrix, const Matrix4d& projection_view_matrix, const std::array<int, 4>& viewport) {
        return ndc_to_ss(clip_to_ndc(world_to_clip(model_to_world(model, world_matrix), projection_view_matrix)), viewport);
    }

    static Vec2d world_to_ss(const Vec3d& world, const Matrix4d& projection_view_matrix, const std::array<int, 4>& viewport) {
        return ndc_to_ss(clip_to_ndc(world_to_clip(world, projection_view_matrix)), viewport);
    }

    static const Matrix4d& ndc_to_ss_matrix(const std::array<int, 4>& viewport) {
        update(viewport);
        return s_cache.ndc_to_ss_matrix;
    }

    static const Transform3d ndc_to_ss_matrix_inverse(const std::array<int, 4>& viewport) {
        update(viewport);
        return s_cache.ndc_to_ss_matrix_inverse;
    }

private:
    static void update(const std::array<int, 4>& viewport) {
        if (s_cache.viewport == viewport)
            return;

        const double half_w = 0.5 * double(viewport[2]);
        const double half_h = 0.5 * double(viewport[3]);
        s_cache.ndc_to_ss_matrix << half_w, 0.0, 0.0, double(viewport[0]) + half_w,
            0.0, half_h, 0.0, double(viewport[1]) + half_h,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0;

        s_cache.ndc_to_ss_matrix_inverse = s_cache.ndc_to_ss_matrix.inverse();
        s_cache.viewport = viewport;
    }
};

// Draw on the canvas background list so the geometry stays legible without covering tool panels.
static constexpr ImU32 MEASUREMENT_OUTLINE = IM_COL32(24, 28, 35, 255);
static constexpr ImU32 MEASUREMENT_HOVER = IM_COL32(255, 215, 48, 255);
static constexpr ImU32 MEASUREMENT_PICKED = IM_COL32(48, 227, 239, 255);

static std::optional<ImVec2> measurement_screen_point(const Vec3d& point, const Matrix4d& projection_view,
    const std::array<int, 4>& viewport)
{
    const Vec4d clip = TransformHelper::world_to_clip(point, projection_view);
    if (!clip.allFinite() || clip.w() <= 0.0 || clip.z() < -clip.w() || clip.z() > clip.w())
        return std::nullopt;
    const Vec2d screen = TransformHelper::ndc_to_ss(TransformHelper::clip_to_ndc(clip), viewport);
    return ImVec2(float(screen.x()), float(viewport[1] + viewport[3] - screen.y()));
}

static std::string slope_angle_text(double slope)
{
    return format_double(Geometry::rad2deg(slope)) + "°";
}

static void draw_hover_slope(const Vec2d& mouse, double slope, const std::array<int, 4>& viewport, float scale)
{
    const ImVec2 cursor(float(mouse.x()), float(mouse.y()));
    const ImVec2 corner{float(viewport[0]), float(viewport[1])};
    const ImVec2 limit = corner + ImVec2(float(viewport[2]), float(viewport[3]));
    if (!ImRect(corner, limit).Contains(cursor))
        return;

    const std::string text = slope_angle_text(slope);
    const ImVec2 padding = ImVec2(6.0f, 4.0f) * scale;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str()) + padding * 2.0f;
    const float gap = 16.0f * scale;
    const ImVec2 pos(
        std::clamp(cursor.x + gap + size.x <= limit.x ? cursor.x + gap : cursor.x - gap - size.x,
            corner.x, std::max(corner.x, limit.x - size.x)),
        std::clamp(cursor.y + gap + size.y <= limit.y ? cursor.y + gap : cursor.y - gap - size.y,
            corner.y, std::max(corner.y, limit.y - size.y)));
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(corner, limit, true);
    draw->AddRectFilled(pos, pos + size, IM_COL32(24, 28, 35, 236), 3.0f * scale);
    draw->AddText(pos + padding, IM_COL32(255, 255, 255, 255), text.c_str());
    draw->PopClipRect();
}

static void draw_circle_dimensions(const Measure::SurfaceFeature& first, const Measure::SurfaceFeature& second,
    const Camera& camera)
{
    const auto [center1, radius1, normal1] = first.get_circle();
    const auto [center2, radius2, normal2] = second.get_circle();
    const auto result = Measure::compare_circles(first, second);
    const auto& viewport = camera.get_viewport();
    const Matrix4d projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto project = [&](const Vec3d& point) { return measurement_screen_point(point, projection, viewport); };
    const auto start = project(center1);
    const auto end = project(center2);
    if (!start && !end)
        return;

    const ImVec2 corner{float(viewport[0]), float(viewport[1])};
    const ImVec2 limit = corner + ImVec2(float(viewport[2]), float(viewport[3]));
    const auto visible = [&](const ImVec2& p) {
        return p.x >= corner.x && p.x <= limit.x && p.y >= corner.y && p.y <= limit.y;
    };
    const auto middle = project(0.5 * (center1 + center2));
    const auto fallback = middle && visible(*middle) ? middle : start && visible(*start) ? start : end;
    if (!fallback || !visible(*fallback))
        return;

    constexpr ImU32 axis_colors[] = {IM_COL32(255, 132, 126, 255),
        IM_COL32(139, 225, 129, 255), IM_COL32(127, 187, 255, 255)};
    constexpr ImU32 angle_color = IM_COL32(221, 172, 255, 255);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(corner, limit, true);
    const auto line = [&](const ImVec2& a, const ImVec2& b, ImU32 color) {
        draw->AddLine(a, b, MEASUREMENT_OUTLINE, 4.0f);
        draw->AddLine(a, b, color, 2.0f);
    };
    std::vector<ImRect> labels;
    const auto label = [&](const std::optional<ImVec2>& anchor, const std::string& value, ImU32 color) {
        const ImVec2 origin = anchor && visible(*anchor) ? *anchor : *fallback;
        const ImVec2 padding(5.0f, 3.0f);
        const ImVec2 size = ImGui::CalcTextSize(value.c_str()) + padding * 2.0f;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const int row = attempt / 4 + 1;
            const float y = row * (size.y + 4.0f);
            const ImVec2 offsets[] = {{8.0f, -y}, {8.0f, y - size.y}, {-8.0f - size.x, -y}, {-8.0f - size.x, y - size.y}};
            const ImVec2 offset = offsets[attempt % 4];
            const ImVec2 pos(std::clamp(origin.x + offset.x, corner.x, std::max(corner.x, limit.x - size.x)),
                std::clamp(origin.y + offset.y, corner.y, std::max(corner.y, limit.y - size.y)));
            const ImRect box(pos, pos + size);
            if (std::any_of(labels.begin(), labels.end(), [&](const ImRect& used) { return used.Overlaps(box); }))
                continue;
            draw->AddRectFilled(box.Min, box.Max, IM_COL32(24, 28, 35, 236), 3.0f);
            draw->AddText(box.Min + padding, color, value.c_str());
            labels.push_back(box);
            return;
        }
    };
    if (start && end)
        line(*start, *end, MEASUREMENT_HOVER);
    label(middle, _u8L("3D center distance") + " " + measurement_length(result.center_distance), MEASUREMENT_HOVER);

    Vec3d segment_start = center1;
    constexpr const char* axis_names[] = {"ΔX ", "ΔY ", "ΔZ "};
    for (int axis = 0; axis < 3; ++axis) {
        Vec3d segment_end = segment_start;
        segment_end[axis] = center2[axis];
        const auto a = project(segment_start);
        const auto b = project(segment_end);
        const bool has_line = a && b && ImLengthSqr(*b - *a) >= 36.0f;
        if (has_line)
            line(*a, *b, axis_colors[axis]);
        label(has_line ? project(0.5 * (segment_start + segment_end)) : fallback,
            axis_names[axis] + measurement_length(result.delta[axis]), axis_colors[axis]);
        segment_start = segment_end;
    }

    std::optional<ImVec2> angle_label = start;
    if (start && result.normal_angle > EPSILON) {
        const Vec3d aligned_normal = normal1.dot(normal2) < 0.0 ? -normal2 : normal2;
        const Vec3d perpendicular = (aligned_normal - normal1 * normal1.dot(aligned_normal)).normalized();
        const double radius = 38.0 * camera.get_inv_zoom();
        for (const Vec3d& direction : {normal1, aligned_normal})
            if (const auto tip = project(center1 + 1.2 * radius * direction))
                line(*start, *tip, angle_color);
        const auto arc = [&](double angle) {
            return project(center1 + radius * (std::cos(angle) * normal1 + std::sin(angle) * perpendicular));
        };
        constexpr int segments = 24;
        for (int i = 0; i < segments; ++i) {
            const auto a = arc(result.normal_angle * i / segments);
            const auto b = arc(result.normal_angle * (i + 1) / segments);
            if (a && b)
                line(*a, *b, angle_color);
        }
        angle_label = arc(0.5 * result.normal_angle);
    }
    label(angle_label, "α " + format_double(Geometry::rad2deg(result.normal_angle)) + "°", angle_color);
    draw->PopClipRect();
}

static void draw_measurement_feature_outline(const Measure::SurfaceFeature& feature, const Camera& camera)
{
    const Matrix4d projection_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto& viewport = camera.get_viewport();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    if (feature.get_type() == Measure::SurfaceFeatureType::Edge) {
        const auto [start, end] = feature.get_edge();
        const auto from = measurement_screen_point(start, projection_view, viewport);
        const auto to = measurement_screen_point(end, projection_view, viewport);
        if (from && to) {
            draw_list->AddLine(*from, *to, MEASUREMENT_OUTLINE, 9.0f);
            draw_list->AddLine(*from, *to, MEASUREMENT_HOVER, 5.0f);
        }
    } else if (feature.get_type() == Measure::SurfaceFeatureType::Circle) {
        constexpr size_t segments = 64;
        std::array<ImVec2, segments> points;
        const auto [center, radius, normal] = feature.get_circle();
        const Vec3d axis_x = Measure::get_orthogonal(normal, true);
        const Vec3d axis_y = normal.normalized().cross(axis_x);
        for (size_t i = 0; i < segments; ++i) {
            const double angle = 2.0 * M_PI * i / segments;
            const Vec3d point = center + radius * (std::cos(angle) * axis_x + std::sin(angle) * axis_y);
            const auto screen = measurement_screen_point(point, projection_view, viewport);
            if (!screen)
                return;
            points[i] = *screen;
        }
        draw_list->AddPolyline(points.data(), int(points.size()), MEASUREMENT_OUTLINE, true, 9.0f);
        draw_list->AddPolyline(points.data(), int(points.size()), MEASUREMENT_HOVER, true, 5.0f);
    }
}

static void draw_circle_hover_marker(ImDrawList& draw_list, const ImVec2& point, bool snapped)
{
    // A diamond marks vertex snapping. A circle marks a point on the mesh surface.
    constexpr float radius = 11.0f;
    const auto draw_outline = [&](ImU32 color, float width) {
        if (snapped)
            draw_list.AddQuad({point.x, point.y - radius}, {point.x + radius, point.y},
                {point.x, point.y + radius}, {point.x - radius, point.y}, color, width);
        else
            draw_list.AddCircle(point, radius, color, 32, width);
        draw_list.AddLine({point.x - 5.0f, point.y}, {point.x + 5.0f, point.y}, color, width);
        draw_list.AddLine({point.x, point.y - 5.0f}, {point.x, point.y + 5.0f}, color, width);
    };
    draw_outline(MEASUREMENT_OUTLINE, 5.5f);
    draw_outline(MEASUREMENT_HOVER, 2.5f);
}

TransformHelper::Cache TransformHelper::s_cache = { { 0, 0, 0, 0 }, Matrix4d::Identity(), Transform3d::Identity() };

GLGizmoMeasure::GLGizmoMeasure(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
: GLGizmoBase(parent, icon_filename, sprite_id)
{
    GLModel::Geometry sphere_geometry = smooth_sphere(16, 7.5f);
    m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(sphere_geometry.get_as_indexed_triangle_set()));
    m_sphere.model.init_from(std::move(sphere_geometry));

    GLModel::Geometry cylinder_geometry = smooth_cylinder(16, 5.0f, 1.0f);
    m_cylinder.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(cylinder_geometry.get_as_indexed_triangle_set()));
    m_cylinder.model.init_from(std::move(cylinder_geometry));
}

bool GLGizmoMeasure::on_mouse(const wxMouseEvent &mouse_event)
{
    m_mouse_pos = { double(mouse_event.GetX()), double(mouse_event.GetY()) };
    m_mouse_in_viewport = !mouse_event.Leaving();
    const bool click_to_pick = uses_click_to_pick();
    if ((mouse_event.LeftDown() || (click_to_pick && mouse_event.LeftUp() && has_pending_pick())) &&
        !m_parent.is_section_offset_adjusting())
        update_if_needed();
    if (m_parent.is_section_offset_adjusting() || !m_raycaster) {
        cancel_pending_pick();
        return false;
    }
    if (click_to_pick) {
        if (mouse_event.LeftDown()) {
            cancel_pending_pick();
            if (!mouse_event.CmdDown())
                m_pick_press = m_mouse_pos;
        } else if (mouse_event.Leaving() || mouse_event.Moving() || mouse_event.RightDown() || mouse_event.MiddleDown()) {
            cancel_pending_pick();
        } else if (m_pick_press && (mouse_event.Dragging() || mouse_event.LeftUp())) {
            const int dx = std::max(3, wxSystemSettings::GetMetric(wxSYS_DRAG_X, m_parent.get_wxglcanvas()));
            const int dy = std::max(3, wxSystemSettings::GetMetric(wxSYS_DRAG_Y, m_parent.get_wxglcanvas()));
            const Vec2d travel = (m_mouse_pos - *m_pick_press).cwiseAbs();
            if (travel.x() >= dx || travel.y() >= dy || mouse_event.CmdDown())
                cancel_pending_pick();
            else if (mouse_event.LeftUp()) {
                cancel_pending_pick();
                if (measures_circles())
                    commit_circle_pick();
                else
                    commit_perpendicular_pick(mouse_event.ShiftDown());
                m_parent.request_extra_frame();
                return true;
            }
        }
        return false;
    }

    if (mouse_event.Moving()) {
        // only for sure 
        m_mouse_left_down = false;
        return false;
    }
    else if (mouse_event.Dragging()) {
        // Enable/Disable panning/rotating the 3D scene 
        // Ctrl is pressed or the mouse is not hovering a selected volume
        bool unlock_dragging = mouse_event.CmdDown() || (m_hover_id == -1 && !m_parent.get_selection().contains_volume(m_parent.get_first_hover_volume_idx()));
        // mode is not center selection or mouse is not hovering a center
        unlock_dragging &= !mouse_event.ShiftDown() || (m_hover_id != SEL_SPHERE_1_ID && m_hover_id != SEL_SPHERE_2_ID && m_hover_id != POINT_ID);
        return !unlock_dragging;
    }
    else if (mouse_event.LeftDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if (mouse_event.CmdDown())
            return false;

        if (m_hover_id != -1) {
            m_mouse_left_down = true;
            
            auto detect_current_item = [this]() {
                SelectedFeatures::Item item;
                if (m_hover_id == SEL_SPHERE_1_ID) {
                    if (m_selected_features.first.is_center)
                        // mouse is hovering over a selected center
                        item = { true, m_selected_features.first.source, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.first.source)) } };
                    else if (is_feature_with_center(*m_selected_features.first.feature))
                        // mouse is hovering over a unselected center
                        item = { true, m_selected_features.first.feature, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.first.feature)) } };
                    else
                        // mouse is hovering over a point
                        item = m_selected_features.first;
                }
                else if (m_hover_id == SEL_SPHERE_2_ID) {
                    if (m_selected_features.second.is_center)
                        // mouse is hovering over a selected center
                        item = { true, m_selected_features.second.source, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.second.source)) } };
                    else if (is_feature_with_center(*m_selected_features.second.feature))
                        // mouse is hovering over a center
                        item = { true, m_selected_features.second.feature, { Measure::SurfaceFeature(get_feature_offset(*m_selected_features.second.feature)) } };
                    else
                        // mouse is hovering over a point
                        item = m_selected_features.second;
                }
                else {
                    switch (m_mode)
                    {
                    case EMode::FeatureSelection: { item = { false, m_curr_feature, m_curr_feature }; break; }
                    case EMode::PointSelection:   { item = { false, m_curr_feature, Measure::SurfaceFeature(*m_curr_point_on_feature_position) }; break; }
                    }
                }
                return item;
            };

            auto requires_sphere_raycaster_for_picking = [this](const SelectedFeatures::Item& item) {
                if (m_mode == EMode::PointSelection || item.feature->get_type() == Measure::SurfaceFeatureType::Point)
                    return true;
                else if (m_mode == EMode::FeatureSelection) {
                    if (is_feature_with_center(*item.feature))
                        return true;
                }
                return false;
            };

            if (m_selected_features.first.feature.has_value()) {
                const SelectedFeatures::Item item = detect_current_item();
                if (m_selected_features.first != item) {
                    bool processed = false;
                    if (item.is_center) {
                        if (item.source == m_selected_features.first.feature) {
                            // switch 1st selection from feature to its center
                            m_selected_features.first = item;
                            processed = true;
                        }
                        else if (item.source == m_selected_features.second.feature) {
                            // switch 2nd selection from feature to its center
                            m_selected_features.second = item;
                            processed = true;
                        }
                    }
                    else if (is_feature_with_center(*item.feature)) {
                      if (m_selected_features.first.is_center && m_selected_features.first.source == item.feature) {
                          // switch 1st selection from center to its feature
                          m_selected_features.first = item;
                          processed = true;
                      }
                      else if (m_selected_features.second.is_center && m_selected_features.second.source == item.feature) {
                          // switch 2nd selection from center to its feature
                          m_selected_features.second = item;
                          processed = true;
                      }
                    }

                    if (!processed) {
                        remove_selected_sphere_raycaster(SEL_SPHERE_2_ID);
                        if (m_selected_features.second == item)
                            // 2nd feature deselection
                            m_selected_features.second.reset();
                        else {
                            // 2nd feature selection
                            m_selected_features.second = item;
                            if (requires_sphere_raycaster_for_picking(item))
                                m_selected_sphere_raycasters.push_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, SEL_SPHERE_2_ID, *m_sphere.mesh_raycaster));
                        }
                    }
                }
                else {
                    remove_selected_sphere_raycaster(SEL_SPHERE_1_ID);
                    if (m_selected_features.second.feature.has_value()) {
                        // promote 2nd feature to 1st feature
                        remove_selected_sphere_raycaster(SEL_SPHERE_2_ID);
                        m_selected_features.first = m_selected_features.second;
                        if (requires_sphere_raycaster_for_picking(m_selected_features.first))
                            m_selected_sphere_raycasters.push_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, SEL_SPHERE_1_ID, *m_sphere.mesh_raycaster));
                        m_selected_features.second.reset();
                    }
                    else
                        // 1st feature deselection
                        m_selected_features.first.reset();
                }
            }
            else {
                // 1st feature selection
                const SelectedFeatures::Item item = detect_current_item();
                m_selected_features.first = item;
                if (requires_sphere_raycaster_for_picking(item))
                    m_selected_sphere_raycasters.push_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, SEL_SPHERE_1_ID, *m_sphere.mesh_raycaster));
            }

            update_measurement_result();

            m_imgui->set_requires_extra_frame();

            return true;
        }
        else
            // if the mouse pointer is on any volume, filter out the event to prevent the user to move it
            // equivalent tp: return (m_parent.get_first_hover_volume_idx() != -1);
            return m_curr_feature.has_value();

        // fix: prevent restart gizmo when reselect object
        // take responsibility for left up
        if (m_parent.get_first_hover_volume_idx() >= 0)
            m_mouse_left_down = true;
    }
    else if (mouse_event.LeftUp()) {
        if (m_mouse_left_down) {
            // responsible for mouse left up after selecting plane
            m_mouse_left_down = false;
            return true;
        }
        if (m_hover_id == -1 && !m_parent.is_mouse_dragging())
            // avoid closing the gizmo if the user clicks outside of any volume
            return true;
    }
    else if (mouse_event.RightDown()) {
        // let the event pass through to allow panning/rotating the 3D scene
        if (mouse_event.CmdDown())
            return false;
    }
    else if (mouse_event.Leaving())
        m_mouse_left_down = false;

    return false;
}

void GLGizmoMeasure::data_changed(bool is_serializing)
{
    m_refresh_mesh = true;
    m_parent.toggle_sla_auxiliaries_visibility(false, nullptr, -1);

    // Section caps are prepared during rendering, before this gizmo renders.
    if (!m_parent.is_section_view_enabled() && !m_section_planes[0].is_active())
        update_if_needed();

    m_last_inv_zoom = 0.0f;
    m_last_plane_idx = -1;
    if (m_pending_scale) {
        update_measurement_result();
        m_pending_scale = false;
    }
    else
        m_selected_features.reset();
    m_selected_sphere_raycasters.clear();
    m_editing_distance = false;
    m_is_editing_distance_first_frame = true;
}

bool GLGizmoMeasure::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (m_input_mode == InputMode::Perpendicular) {
        if (action != SLAGizmoEventType::Delete && action != SLAGizmoEventType::Escape)
            return false;
        if (m_target)
            m_target.reset();
        else if (m_reference_plane)
            clear_reference_plane();
        else if (action == SLAGizmoEventType::Escape)
            return false;
        m_parent.request_extra_frame();
        return true;
    }
    if (measures_circles()) {
        if (action == SLAGizmoEventType::Delete) {
            clear_circle_points();
            m_first_circle = {};
        } else if (action == SLAGizmoEventType::Escape) {
            if (m_current_circle.points.empty() && m_first_circle.points.empty())
                set_input_mode(InputMode::Standard);
            else
                undo_circle_point();
        } else {
            return false;
        }
        m_parent.request_extra_frame();
        return true;
    }

    if (action == SLAGizmoEventType::ShiftDown) {
        if (m_shift_kar_filter.is_first()) {
            m_mode = EMode::PointSelection;
            disable_scene_raycasters();
        }
        m_shift_kar_filter.increase_count();
    }
    else if (action == SLAGizmoEventType::ShiftUp) {
        m_shift_kar_filter.reset_count();
        m_mode = EMode::FeatureSelection;
        restore_scene_raycasters_state();
    }
    else if (action == SLAGizmoEventType::Delete) {
        m_selected_features.reset();
        m_selected_sphere_raycasters.clear();
        m_parent.request_extra_frame();
    }
    else if (action == SLAGizmoEventType::Escape) {
        if (!m_selected_features.first.feature.has_value()) {
            update_measurement_result();
            return false;
        }
        else {
            if (m_selected_features.second.feature.has_value()) {
                remove_selected_sphere_raycaster(SEL_SPHERE_2_ID);
                m_selected_features.second.feature.reset();
            }
            else {
                remove_selected_sphere_raycaster(SEL_SPHERE_1_ID);
                m_selected_features.first.feature.reset();
            }

            update_measurement_result();
        }
    }

    return true;
}

bool GLGizmoMeasure::on_init()
{
    m_shortcut_key = WXK_CONTROL_U;
    return true;
}

void GLGizmoMeasure::on_set_state()
{
    if (m_state == Off) {
        m_input_mode = InputMode::Standard;
        clear_circle_points();
        m_first_circle = {};
        cancel_pending_pick();
        clear_reference_plane();
        m_hover_slope.reset();
        m_mouse_in_viewport = false;
        m_mouse_left_down = false;
        m_parent.toggle_sla_auxiliaries_visibility(true, nullptr, -1);
        m_shift_kar_filter.reset_count();
        m_curr_feature.reset();
        m_curr_point_on_feature_position.reset();
        restore_scene_raycasters_state();
        m_editing_distance = false;
        m_is_editing_distance_first_frame = true;
        m_measuring.reset();
        m_raycaster.reset();
        m_volumes_cache.clear();
        m_facet_sources.clear();
    }
    else {
        m_mode = EMode::FeatureSelection;
        // store current state of scene raycaster for later use
        m_scene_raycasters.clear();
        auto scene_raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
        if (scene_raycasters != nullptr) {
            m_scene_raycasters.reserve(scene_raycasters->size());
            for (auto r : *scene_raycasters) {
                SceneRaycasterState state = { r, r->is_active() };
                m_scene_raycasters.emplace_back(state);
            }
        }
    }
}

std::string GLGizmoMeasure::on_get_name() const
{
    return _u8L("Measure");
}

bool GLGizmoMeasure::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    bool res = (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) ?
        selection.is_single_full_instance() :
        selection.is_single_full_instance() || selection.is_single_volume() || selection.is_single_modifier();
    if (res)
        res &= !selection.contains_sinking_volumes();

    return res;
}

bool GLGizmoMeasure::advanced_measurement_available() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

bool GLGizmoMeasure::can_hover() const
{
    return m_mouse_in_viewport && !ImGui::GetIO().WantCaptureMouse && !wxGetMouseState().LeftIsDown();
}

void GLGizmoMeasure::on_render()
{
#if ENABLE_MEASURE_GIZMO_DEBUG
    render_debug_dialog();
#endif // ENABLE_MEASURE_GIZMO_DEBUG

//    // do not render if the user is panning/rotating the 3d scene
//    if (m_parent.is_mouse_dragging())
//        return;

    m_hover_slope.reset();
    m_hover_plane.reset();
    m_hover_target.reset();
    m_hover_circle.reset();
    if (m_parent.is_section_offset_adjusting())
        return;
    update_if_needed();
    if (!m_raycaster)
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const bool fff = advanced_measurement_available();
    if (!fff && (m_input_mode == InputMode::Perpendicular || m_input_mode == InputMode::CircleDistance))
        set_input_mode(InputMode::Standard);
    const bool detect_circle = fff && m_pick_detected_circle;
    const bool hover_slope = fff && m_show_overhang && can_hover() && !m_parent.is_mouse_dragging();
    Vec3f position_on_model;
    Vec3f normal_on_model;
    size_t model_facet_idx = 0;
    const bool check_hit = m_input_mode == InputMode::Standard ||
        (can_hover() && (hover_slope ||
            (m_input_mode == InputMode::Perpendicular && !m_reference_plane) ||
            (measures_circles() && detect_circle && !circle_to_pick().circle)));
    const bool mouse_on_object = check_hit &&
        unproject_measurement(m_mouse_pos, camera, position_on_model, normal_on_model, &model_facet_idx);
    if (hover_slope && mouse_on_object) {
        const auto& source = m_facet_sources[model_facet_idx];
        if (!source.is_cap() && m_volumes_cache[source.volume].volume->is_model_part())
            m_hover_slope = Measure::overhang_slope(normal_on_model.cast<double>());
        if (m_hover_slope)
            draw_hover_slope(m_mouse_pos, *m_hover_slope, camera.get_viewport(), m_imgui->get_style_scaling());
    }
    if (measures_circles()) {
        if (detect_circle && !circle_to_pick().circle && mouse_on_object)
            m_hover_circle = detected_circle(model_facet_idx, position_on_model.cast<double>());
        render_constructed_circle();
        return;
    }
    if (m_input_mode == InputMode::Perpendicular) {
        if (can_hover()) {
            if (!m_reference_plane && mouse_on_object)
                m_hover_plane = m_measuring->get_plane(model_facet_idx);
            if (m_reference_plane)
                m_hover_target = pick_point(m_mouse_pos, wxGetMouseState().ShiftDown());
        }
        render_perpendicular();
        return;
    }

    const float inv_zoom = (float)camera.get_inv_zoom();
    const bool is_hovering_on_feature = m_mode == EMode::PointSelection && m_hover_id != -1;

    auto update_circle = [this, inv_zoom]() {
        if (m_last_inv_zoom != inv_zoom || m_last_circle != m_curr_feature) {
            m_last_inv_zoom = inv_zoom;
            m_last_circle = m_curr_feature;
            m_circle.reset();
            const auto [center, radius, normal] = m_curr_feature->get_circle();
            GLModel::Geometry circle_geometry = init_torus_data(64, 16, center.cast<float>(), float(radius), 5.0f * inv_zoom, normal.cast<float>(), Transform3f::Identity());
            m_circle.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(circle_geometry.get_as_indexed_triangle_set()));
            m_circle.model.init_from(std::move(circle_geometry));
            return true;
        }
        return false;
    };

    if (m_mode == EMode::FeatureSelection || m_mode == EMode::PointSelection) {
        if (m_hover_id == SEL_SPHERE_1_ID || m_hover_id == SEL_SPHERE_2_ID) {
            // Skip feature detection if hovering on a selected point/center
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID);
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
            m_curr_feature.reset();
            m_curr_point_on_feature_position.reset();
        }
        else {
            std::optional<Measure::SurfaceFeature> curr_feature = wxGetMouseState().LeftIsDown() ? m_curr_feature :
                mouse_on_object ? m_measuring->get_feature(model_facet_idx, position_on_model.cast<double>()) : std::nullopt;

            if (m_curr_feature != curr_feature ||
                (curr_feature.has_value() && curr_feature->get_type() == Measure::SurfaceFeatureType::Circle && (m_curr_feature != curr_feature || m_last_inv_zoom != inv_zoom))) {
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID);
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
                m_raycasters.clear();
                m_curr_feature = curr_feature;
                if (!m_curr_feature.has_value())
                    return;

                switch (m_curr_feature->get_type()) {
                default: { assert(false); break; }
                case Measure::SurfaceFeatureType::Point:
                {
                    m_raycasters.insert({ POINT_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, POINT_ID, *m_sphere.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Edge:
                {
                    m_raycasters.insert({ EDGE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, EDGE_ID, *m_cylinder.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Circle:
                {
                    update_circle();
                    m_raycasters.insert({ CIRCLE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID, *m_circle.mesh_raycaster) });
                    break;
                }
                case Measure::SurfaceFeatureType::Plane:
                {
                    const auto [idx, normal, point] = m_curr_feature->get_plane();
                    if (m_last_plane_idx != idx) {
                        m_last_plane_idx = idx;
                        const indexed_triangle_set& its = m_measuring->get_its();
                        const std::vector<int>& plane_triangles = m_measuring->get_plane_triangle_indices(idx);
                        GLModel::Geometry init_data = init_plane_data(its, plane_triangles);
                        m_plane.reset();
                        m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(init_data.get_as_indexed_triangle_set()));
                    }

                    m_raycasters.insert({ PLANE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, PLANE_ID, *m_plane.mesh_raycaster) });
                    break;
                }
                }
            }
        }
    }

    if (m_mode != EMode::PointSelection)
        m_curr_point_on_feature_position.reset();
    else if (is_hovering_on_feature) {
        auto position_on_feature = [this](int feature_type_id, const Camera& camera, std::function<Vec3f(const Vec3f&)> callback = nullptr) -> Vec3d {
            auto it = m_raycasters.find(feature_type_id);
            if (it != m_raycasters.end() && it->second != nullptr) {
                Vec3f p;
                Vec3f n;
                const Transform3d& trafo = it->second->get_transform();
                bool res = it->second->get_raycaster()->closest_hit(m_mouse_pos, trafo, camera, p, n);
                if (res) {
                    if (callback)
                        p = callback(p);
                    return trafo * p.cast<double>();
                }
            }
            return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
        };

        if (m_curr_feature.has_value()) {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                m_curr_point_on_feature_position = m_curr_feature->get_point();
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const std::optional<Vec3d> extra = m_curr_feature->get_extra_point();
                if (extra.has_value() && m_hover_id == POINT_ID)
                    m_curr_point_on_feature_position = *extra;
                else {
                    const Vec3d pos = position_on_feature(EDGE_ID, camera, [](const Vec3f& v) { return Vec3f(0.0f, 0.0f, v.z()); });
                    if (!pos.isApprox(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX)))
                        m_curr_point_on_feature_position = pos;
                }
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                m_curr_point_on_feature_position = position_on_feature(PLANE_ID, camera);
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto [center, radius, normal] = m_curr_feature->get_circle();
                if (m_hover_id == POINT_ID)
                    m_curr_point_on_feature_position = center;
                else {
                    const Vec3d world_pof = position_on_feature(CIRCLE_ID, camera, [](const Vec3f& v) { return v; });
                    const Eigen::Hyperplane<double, 3> plane(normal, center);
                    const Transform3d local_to_model_matrix = Geometry::translation_transform(center) * Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), normal);
                    const Vec3d local_proj = local_to_model_matrix.inverse() * plane.projection(world_pof);
                    double angle = std::atan2(local_proj.y(), local_proj.x());
                    if (angle < 0.0)
                        angle += 2.0 * double(M_PI);

                    const Vec3d local_pos = radius * Vec3d(std::cos(angle), std::sin(angle), 0.0);
                    m_curr_point_on_feature_position = local_to_model_matrix * local_pos;
                }
                break;
            }
            }
        }
    }
    else {
        m_curr_point_on_feature_position.reset();
        if (m_curr_feature.has_value() && m_curr_feature->get_type() == Measure::SurfaceFeatureType::Circle) {
            if (update_circle()) {
                m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID);
                auto it = m_raycasters.find(CIRCLE_ID);
                if (it != m_raycasters.end())
                    m_raycasters.erase(it);
                m_raycasters.insert({ CIRCLE_ID, m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CIRCLE_ID, *m_circle.mesh_raycaster) });
            }
        }
    }

    if (!m_curr_feature.has_value() && !m_selected_features.first.feature.has_value())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    const bool old_cullface = ::glIsEnabled(GL_CULL_FACE);
    glsafe(::glDisable(GL_CULL_FACE));

    const Transform3d& view_matrix = camera.get_view_matrix();

    auto set_matrix_uniforms = [shader, &view_matrix](const Transform3d& model_matrix) {
        const Transform3d view_model_matrix = view_matrix * model_matrix;
        shader->set_uniform("view_model_matrix", view_model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
    };

    auto set_emission_uniform = [shader](const ColorRGBA& color, bool hover) {
        shader->set_uniform("emission_factor", (color == GLVolume::SELECTED_COLOR) ? 0.0f :
            hover ? 0.5f : 0.25f);
    };

    auto render_feature = [this, set_matrix_uniforms, set_emission_uniform](const Measure::SurfaceFeature& feature, const std::vector<ColorRGBA>& colors,
        float inv_zoom, bool hover, bool update_raycasters_transform) {
            switch (feature.get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                const Transform3d feature_matrix = Geometry::translation_transform(feature.get_point()) * Geometry::scale_transform(inv_zoom);
                set_matrix_uniforms(feature_matrix);
                set_emission_uniform(colors.front(), hover);
                m_sphere.model.set_color(colors.front());
                m_sphere.model.render();
                if (update_raycasters_transform) {
                    auto it = m_raycasters.find(POINT_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(feature_matrix);
                }
                break;
            }
            case Measure::SurfaceFeatureType::Circle:
            {
                const auto& [center, radius, normal] = feature.get_circle();
                // render circle
                const Transform3d circle_matrix = Transform3d::Identity();
                set_matrix_uniforms(circle_matrix);
                if (update_raycasters_transform) {
                    set_emission_uniform(colors.front(), hover);
                    m_circle.model.set_color(colors.front());
                    m_circle.model.render();
                    auto it = m_raycasters.find(CIRCLE_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(circle_matrix);
                }
                else {
                    GLModel circle;
                    GLModel::Geometry circle_geometry = init_torus_data(64, 16, center.cast<float>(), float(radius), 5.0f * inv_zoom, normal.cast<float>(), Transform3f::Identity());
                    circle.init_from(std::move(circle_geometry));
                    set_emission_uniform(colors.front(), hover);
                    circle.set_color(colors.front());
                    circle.render();
                }
                // render center
                if (colors.size() > 1) {
                    const Transform3d center_matrix = Geometry::translation_transform(center) * Geometry::scale_transform(inv_zoom);
                    set_matrix_uniforms(center_matrix);
                    set_emission_uniform(colors.back(), hover);
                    m_sphere.model.set_color(colors.back());
                    m_sphere.model.render();
                    auto it = m_raycasters.find(POINT_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(center_matrix);
                }
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            {
                const auto& [from, to] = feature.get_edge();
                // render edge
                const Transform3d edge_matrix = Geometry::translation_transform(from) *
                    Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), to - from) *
                    Geometry::scale_transform({ (double)inv_zoom, (double)inv_zoom, (to - from).norm() });
                set_matrix_uniforms(edge_matrix);
                set_emission_uniform(colors.front(), hover);
                m_cylinder.model.set_color(colors.front());
                m_cylinder.model.render();
                if (update_raycasters_transform) {
                    auto it = m_raycasters.find(EDGE_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(edge_matrix);
                }

                // render extra point
                if (colors.size() > 1) {
                    const std::optional<Vec3d> extra = feature.get_extra_point();
                    if (extra.has_value()) {
                        const Transform3d point_matrix = Geometry::translation_transform(*extra) * Geometry::scale_transform(inv_zoom);
                        set_matrix_uniforms(point_matrix);
                        set_emission_uniform(colors.back(), hover);
                        m_sphere.model.set_color(colors.back());
                        m_sphere.model.render();
                        auto it = m_raycasters.find(POINT_ID);
                        if (it != m_raycasters.end() && it->second != nullptr)
                            it->second->set_transform(point_matrix);
                    }
                }
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                const auto& [idx, normal, pt] = feature.get_plane();
                assert(idx < m_plane_models_cache.size());
                set_matrix_uniforms(Transform3d::Identity());
                set_emission_uniform(colors.front(), hover);
                m_plane_models_cache[idx].set_color(colors.front());
                m_plane_models_cache[idx].render();
                if (update_raycasters_transform) {
                    auto it = m_raycasters.find(PLANE_ID);
                    if (it != m_raycasters.end() && it->second != nullptr)
                        it->second->set_transform(Transform3d::Identity());
                }
                break;
            }
            }
    };

    auto hover_selection_color = [this]() {
      return ((m_mode == EMode::PointSelection && !m_selected_features.first.feature.has_value()) ||
              (m_mode != EMode::PointSelection && (!m_selected_features.first.feature.has_value() || *m_curr_feature == *m_selected_features.first.feature))) ?
          SELECTED_1ST_COLOR : SELECTED_2ND_COLOR;
    };

    auto hovering_color = [this, hover_selection_color]() {
        return (m_mode == EMode::PointSelection) ? GLVolume::SELECTED_COLOR : hover_selection_color();
    };

    if (m_curr_feature.has_value()) {
        // render hovered feature

        std::vector<ColorRGBA> colors;
        if (m_selected_features.first.feature.has_value() && *m_curr_feature == *m_selected_features.first.feature) {
            // hovering over the 1st selected feature
            if (m_selected_features.first.is_center)
                // hovering over a center
                colors = { NEUTRAL_COLOR, hovering_color() };
            else if (is_feature_with_center(*m_selected_features.first.feature))
                // hovering over a feature with center
                colors = { hovering_color(), NEUTRAL_COLOR };
            else
                colors = { hovering_color() };
        }
        else if (m_selected_features.second.feature.has_value() && *m_curr_feature == *m_selected_features.second.feature) {
            // hovering over the 2nd selected feature
            if (m_selected_features.second.is_center)
                // hovering over a center
                colors = { NEUTRAL_COLOR, hovering_color() };
            else if (is_feature_with_center(*m_selected_features.second.feature))
                // hovering over a feature with center
                colors = { hovering_color(), NEUTRAL_COLOR };
            else
                colors = { hovering_color() };
        }
        else {
            switch (m_curr_feature->get_type())
            {
            default: { assert(false); break; }
            case Measure::SurfaceFeatureType::Point:
            {
                colors.emplace_back(hover_selection_color());
                break;
            }
            case Measure::SurfaceFeatureType::Edge:
            case Measure::SurfaceFeatureType::Circle:
            {
                if (m_selected_features.first.is_center && m_curr_feature == m_selected_features.first.source)
                    colors = { SELECTED_1ST_COLOR, NEUTRAL_COLOR };
                else if (m_selected_features.second.is_center && m_curr_feature == m_selected_features.second.source)
                    colors = { SELECTED_2ND_COLOR, NEUTRAL_COLOR };
                else
                    colors = { hovering_color(), hovering_color() };
                break;
            }
            case Measure::SurfaceFeatureType::Plane:
            {
                colors.emplace_back(hovering_color());
                break;
            }
            }
        }

        render_feature(*m_curr_feature, colors, inv_zoom, true, true);
        if (!ImGui::GetIO().WantCaptureMouse &&
            ((m_hover_id == EDGE_ID && m_curr_feature->get_type() == Measure::SurfaceFeatureType::Edge) ||
             (m_hover_id == CIRCLE_ID && m_curr_feature->get_type() == Measure::SurfaceFeatureType::Circle)))
            draw_measurement_feature_outline(*m_curr_feature, camera);
    }

    if (m_selected_features.first.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.first.feature)) {
        // render 1st selected feature

        std::optional<Measure::SurfaceFeature> feature_to_render;
        std::vector<ColorRGBA> colors;
        bool requires_raycaster_update = false;
        if (m_hover_id == SEL_SPHERE_1_ID && (m_selected_features.first.is_center || is_feature_with_center(*m_selected_features.first.feature))) {
            // hovering over a center
            feature_to_render = m_selected_features.first.source;
            colors = { NEUTRAL_COLOR, SELECTED_1ST_COLOR };
            requires_raycaster_update = true;
        }
        else if (is_feature_with_center(*m_selected_features.first.feature)) {
            // hovering over a feature with center
            feature_to_render = m_selected_features.first.feature;
            colors = { SELECTED_1ST_COLOR, NEUTRAL_COLOR };
            requires_raycaster_update = true;
        }
        else {
            feature_to_render = m_selected_features.first.feature;
            colors = { SELECTED_1ST_COLOR };
            requires_raycaster_update = m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Point;
        }

        render_feature(*feature_to_render, colors, inv_zoom, m_hover_id == SEL_SPHERE_1_ID, false);

        if (requires_raycaster_update) {
            auto it = std::find_if(m_selected_sphere_raycasters.begin(), m_selected_sphere_raycasters.end(),
                [](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == SEL_SPHERE_1_ID; });
            if (it != m_selected_sphere_raycasters.end())
                (*it)->set_transform(Geometry::translation_transform(get_feature_offset(*m_selected_features.first.feature)) * Geometry::scale_transform(inv_zoom));
        }
    }

    if (m_selected_features.second.feature.has_value() && (!m_curr_feature.has_value() || *m_curr_feature != *m_selected_features.second.feature)) {
        // render 2nd selected feature

        std::optional<Measure::SurfaceFeature> feature_to_render;
        std::vector<ColorRGBA> colors;
        bool requires_raycaster_update = false;
        if (m_hover_id == SEL_SPHERE_2_ID && (m_selected_features.second.is_center || is_feature_with_center(*m_selected_features.second.feature))) {
            // hovering over a center
            feature_to_render = m_selected_features.second.source;
            colors = { NEUTRAL_COLOR, SELECTED_2ND_COLOR };
            requires_raycaster_update = true;
        }
        else if (is_feature_with_center(*m_selected_features.second.feature)) {
            // hovering over a feature with center
            feature_to_render = m_selected_features.second.feature;
            colors = { SELECTED_2ND_COLOR, NEUTRAL_COLOR };
            requires_raycaster_update = true;
        }
        else {
            feature_to_render = m_selected_features.second.feature;
            colors = { SELECTED_2ND_COLOR };
            requires_raycaster_update = m_selected_features.second.feature->get_type() == Measure::SurfaceFeatureType::Point;
        }

        render_feature(*feature_to_render, colors, inv_zoom, m_hover_id == SEL_SPHERE_2_ID, false);

        if (requires_raycaster_update) {
            auto it = std::find_if(m_selected_sphere_raycasters.begin(), m_selected_sphere_raycasters.end(),
                [](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == SEL_SPHERE_2_ID; });
            if (it != m_selected_sphere_raycasters.end())
                (*it)->set_transform(Geometry::translation_transform(get_feature_offset(*m_selected_features.second.feature)) * Geometry::scale_transform(inv_zoom));
        }
    }

    if (is_hovering_on_feature && m_curr_point_on_feature_position.has_value()) {
        if (m_hover_id != POINT_ID) {
            // render point on feature while SHIFT is pressed
            const Transform3d matrix = Geometry::translation_transform(*m_curr_point_on_feature_position) * Geometry::scale_transform(inv_zoom);
            set_matrix_uniforms(matrix);
            const ColorRGBA color = hover_selection_color();
            set_emission_uniform(color, true);
            m_sphere.model.set_color(color);
            m_sphere.model.render();
        }
    }

    shader->stop_using();

    if (old_cullface)
        glsafe(::glEnable(GL_CULL_FACE));

    render_dimensioning();
}

bool GLGizmoMeasure::unproject_measurement(const Vec2d& mouse_pos, const Camera& camera,
    Vec3f& hit, Vec3f& normal, size_t* facet, bool require_even_hits) const
{
    // Section surfaces may be open, particularly when the section fill is hidden.
    const bool section_active = m_section_planes[0].is_active();
    if (m_has_transparent_facets && m_pick_through) {
        const auto picked = pick_through_transparency(*m_raycaster, mouse_pos, Transform3d::Identity(), camera,
            [this](size_t face) { return m_facet_sources[face].transparency; }, nullptr, nullptr, true);
        if (!picked)
            return false;
        hit = picked->position;
        normal = picked->normal;
        if (facet)
            *facet = picked->facet;
        return true;
    }
    if (m_has_transparent_facets)
        return m_raycaster->unproject_on_mesh(mouse_pos, Transform3d::Identity(), camera, hit, normal,
            nullptr, facet, false, true, [this](size_t face) {
                return m_facet_sources[face].transparency < 100;
            });
    return m_raycaster->unproject_on_mesh(mouse_pos, Transform3d::Identity(), camera, hit, normal, nullptr,
        facet, require_even_hits && !section_active, section_active);
}

bool GLGizmoMeasure::section_hides(const Vec3d& position, bool from_cap) const
{
    return (from_cap && !m_section_caps_visible) ||
        (m_section_planes[0].distance(position) < -EPSILON &&
            (!m_section_planes[1].is_active() || m_section_planes[1].distance(position) < -EPSILON));
}

std::optional<GLGizmoMeasure::PointPick> GLGizmoMeasure::pick_point(const Vec2d& mouse_pos, bool snap_to_vertex) const
{
    if (!m_raycaster || !m_measuring)
        return std::nullopt;

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f hit, normal;
    size_t facet = 0;
    if (!snap_to_vertex) {
        if (unproject_measurement(mouse_pos, camera, hit, normal, &facet)) {
            const bool cap = m_facet_sources[facet].is_cap();
            return PointPick{hit.cast<double>(), false, cap, cap, normal.cast<double>()};
        }
        return std::nullopt;
    }

    // Probe nearby visible facets as well as the one directly under the cursor.
    // This also works just outside a silhouette, without searching the entire mesh.
    const double radius = 12.0 * m_imgui->get_style_scaling();
    const Matrix4d projection_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto& viewport = camera.get_viewport();
    const indexed_triangle_set& mesh = m_measuring->get_its();
    std::vector<int> candidates;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const Vec2d probe = mouse_pos + Vec2d(x, y) * (radius * 0.5);
            if (unproject_measurement(probe, camera, hit, normal, &facet, false)
                && facet < mesh.indices.size()) {
                const auto& triangle = mesh.indices[facet];
                for (int i = 0; i < 3; ++i)
                    candidates.push_back(triangle[i]);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::optional<PointPick> pick;
    double closest_distance_sq = radius * radius;
    for (int index : candidates) {
        const Vec3d vertex = mesh.vertices[index].cast<double>();
        const auto screen = measurement_screen_point(vertex, projection_view, viewport);
        if (!screen)
            continue;
        const Vec2d screen_pos(screen->x, screen->y);
        const double distance_sq = (screen_pos - mouse_pos).squaredNorm();
        if (distance_sq >= closest_distance_sq)
            continue;
        // Reject vertices hidden behind another surface, including the far rim of a hole.
        // A small pixel tolerance accounts for float ray intersections at triangle edges.
        if (!unproject_measurement(screen_pos, camera, hit, normal, &facet, false)
            || (hit.cast<double>() - vertex).norm() > 0.25 * camera.get_inv_zoom())
            continue;
        const bool cap = m_facet_sources[facet].is_cap();
        const bool cut_vertex = std::any_of(m_section_planes.begin(), m_section_planes.end(),
            [&vertex](const ClippingPlane& plane) { return std::abs(plane.distance(vertex)) < EPSILON; });
        closest_distance_sq = distance_sq;
        pick = PointPick{vertex, true, cap, cap || cut_vertex, std::nullopt};
    }
    // In vertex mode an empty candidate must never silently become a point inside a facet.
    return pick;
}

std::optional<Measure::SurfaceFeature> GLGizmoMeasure::detected_circle(size_t face, const Vec3d& position) const
{
    auto feature = m_measuring->get_feature(face, position);
    if (feature && feature->get_type() == Measure::SurfaceFeatureType::Circle)
        return feature;
    return std::nullopt;
}

bool GLGizmoMeasure::circle_hidden(const CircleSelection& selection) const
{
    return std::any_of(selection.points.begin(), selection.points.end(),
        [this](const PointPick& point) { return section_hides(point.position, point.from_cap); });
}

bool GLGizmoMeasure::measures_circles() const
{
    return m_input_mode == InputMode::Circles || m_input_mode == InputMode::CircleDistance;
}

GLGizmoMeasure::CircleSelection& GLGizmoMeasure::circle_to_pick()
{
    return m_input_mode == InputMode::CircleDistance && !m_first_circle.circle ? m_first_circle : m_current_circle;
}

void GLGizmoMeasure::commit_circle_pick()
{
    CircleSelection& selection = circle_to_pick();
    if (selection.circle)
        return;
    if (m_pick_detected_circle && advanced_measurement_available()) {
        const Camera& camera = wxGetApp().plater()->get_camera();
        Vec3f position, normal;
        size_t face = 0;
        if (unproject_measurement(m_mouse_pos, camera, position, normal, &face))
            selection.circle = detected_circle(face, position.cast<double>());
        if (selection.circle) {
            const auto [center, radius, axis] = selection.circle->get_circle();
            const bool cap = m_facet_sources[face].is_cap();
            const bool touches_cut = std::any_of(m_section_planes.begin(), m_section_planes.end(),
                [&](const ClippingPlane& plane) {
                    const double extent = radius * axis.cross(plane.get_normal()).norm();
                    return std::abs(plane.distance(center)) <= extent + EPSILON;
                });
            selection.points = {{position.cast<double>(), false, cap, cap || touches_cut, std::nullopt}};
        }
        m_invalid_circle_pick = !selection.circle;
    } else {
        const auto pick = pick_point(m_mouse_pos, m_snap_circle_vertices);
        auto& points = selection.points;
        m_invalid_circle_pick = !pick || std::any_of(points.begin(), points.end(),
            [&](const PointPick& point) { return (point.position - pick->position).norm() <= EPSILON; });
        if (!m_invalid_circle_pick && points.size() == 2) {
            selection.circle = Measure::circle_from_three_points(points[0].position, points[1].position, pick->position);
            m_invalid_circle_pick = !selection.circle;
        }
        if (!m_invalid_circle_pick)
            points.push_back(*pick);
    }
}

void GLGizmoMeasure::commit_perpendicular_pick(bool snap_to_vertex)
{
    if (m_reference_plane) {
        if (const auto pick = pick_point(m_mouse_pos, snap_to_vertex))
            m_target = pick;
        return;
    }
    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f position, normal;
    size_t face = 0;
    if (unproject_measurement(m_mouse_pos, camera, position, normal, &face))
        if (const auto plane = m_measuring->get_plane(face)) {
            m_reference_plane = PlaneReference{*plane, position.cast<double>(), m_facet_sources[face]};
            m_reference_plane_index = std::get<0>(plane->get_plane());
        }
}

void GLGizmoMeasure::render_constructed_circle()
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& view = camera.get_view_matrix();
    const auto render_point = [&](const Vec3d& point, const ColorRGBA& color) {
        const Transform3d marker = Geometry::translation_transform(point) * Geometry::scale_transform(camera.get_inv_zoom());
        shader->set_uniform("view_model_matrix", view * marker);
        m_sphere.model.set_color(color);
        m_sphere.model.render();
    };

    const bool restore_depth_test = ::glIsEnabled(GL_DEPTH_TEST);
    glsafe(::glDisable(GL_DEPTH_TEST));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const auto render_circle = [&](const Measure::SurfaceFeature& feature, const ColorRGBA& color) {
        if (!m_circle_outline.is_initialized()) {
            GLModel::Geometry outline;
            outline.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P3 };
            constexpr unsigned int segments = 96;
            outline.reserve_vertices(segments);
            outline.reserve_indices(segments);
            for (unsigned int i = 0; i < segments; ++i) {
                const double angle = 2.0 * M_PI * i / segments;
                outline.add_vertex(Vec3f(float(std::cos(angle)), float(std::sin(angle)), 0.f));
                outline.add_index(i);
            }
            m_circle_outline.init_from(std::move(outline));
        }

        const auto [center, radius, normal] = feature.get_circle();
        const Transform3d circle = Geometry::translation_transform(center) *
            Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal) * Geometry::scale_transform(radius);
        shader->set_uniform("view_model_matrix", view * circle);
        m_circle_outline.set_color(color);
        m_circle_outline.render();
        render_point(center, color);
    };
    const bool show_current = m_current_circle.circle && !circle_hidden(m_current_circle);
    const bool show_first = m_first_circle.circle && !circle_hidden(m_first_circle);
    if (show_current)
        render_circle(*m_current_circle.circle, SELECTED_1ST_COLOR);
    if (show_first)
        render_circle(*m_first_circle.circle, REFERENCE_COLOR);
    if (m_hover_circle)
        render_circle(*m_hover_circle, REFERENCE_COLOR);
    shader->stop_using();
    if (restore_depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));

    if (show_first && show_current)
        draw_circle_dimensions(*m_first_circle.circle, *m_current_circle.circle, camera);

    const Matrix4d projection_view = camera.get_projection_matrix().matrix() * view.matrix();
    const auto& viewport = camera.get_viewport();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const auto& selection = circle_to_pick();
    const auto& points = selection.points;
    const bool current_is_detected = points.size() == 1 && selection.circle;
    for (size_t i = 0; i < points.size() && !current_is_detected; ++i) {
        if (section_hides(points[i].position, points[i].from_cap))
            continue;
        if (const auto screen = measurement_screen_point(points[i].position, projection_view, viewport)) {
            draw_list->AddCircleFilled(*screen, 8.0f, MEASUREMENT_OUTLINE, 24);
            draw_list->AddCircleFilled(*screen, 5.5f, MEASUREMENT_PICKED, 24);
            const std::string label = std::to_string(i + 1);
            draw_list->AddText(ImVec2(screen->x + 10.0f, screen->y - 10.0f), MEASUREMENT_OUTLINE, label.c_str());
            draw_list->AddText(ImVec2(screen->x + 9.0f, screen->y - 11.0f), MEASUREMENT_PICKED, label.c_str());
        }
    }

    if (!selection.circle && !(m_pick_detected_circle && advanced_measurement_available()) && can_hover()) {
        if (const auto pick = pick_point(m_mouse_pos, m_snap_circle_vertices)) {
            if (const auto screen = measurement_screen_point(pick->position, projection_view, viewport))
                draw_circle_hover_marker(*draw_list, *screen, pick->snapped_to_vertex);
        }
    }
}

void GLGizmoMeasure::render_perpendicular()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const bool reference_visible = m_reference_plane &&
        !section_hides(m_reference_plane->anchor, m_reference_plane->source.is_cap());
    const int plane_idx = m_hover_plane ? std::get<0>(m_hover_plane->get_plane())
        : reference_visible ? m_reference_plane_index : -1;
    if (plane_idx >= 0) {
        if (GLShaderProgram* shader = wxGetApp().get_shader("flat")) {
            const bool blend_enabled = ::glIsEnabled(GL_BLEND);
            const bool offset_enabled = ::glIsEnabled(GL_POLYGON_OFFSET_FILL);
            GLfloat factor, units;
            glsafe(::glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor));
            glsafe(::glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glPolygonOffset(-1.0f, -1.0f));
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            GLModel& model = m_plane_models_cache[plane_idx];
            model.set_color({0.25f, 0.75f, 0.75f, 0.45f});
            model.render();
            shader->stop_using();
            glsafe(::glPolygonOffset(factor, units));
            if (!offset_enabled)
                glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
            if (!blend_enabled)
                glsafe(::glDisable(GL_BLEND));
        }
    }
    if (!reference_visible)
        return;

    const Matrix4d projection_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto draw_distance = [&](const PointPick& point, ImU32 color) {
        if (section_hides(point.position, point.from_cap))
            return;
        const auto result = Measure::get_measurement(Measure::SurfaceFeature(point.position), m_reference_plane->plane);
        const auto start = measurement_screen_point(result.distance_infinite->from, projection_view, camera.get_viewport());
        const auto end = measurement_screen_point(result.distance_infinite->to, projection_view, camera.get_viewport());
        if (start && end) {
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            draw->AddLine(*start, *end, MEASUREMENT_OUTLINE, 5.0f);
            draw->AddLine(*start, *end, color, 3.0f);
            draw->AddCircleFilled(*end, 3.0f, color);
            draw_circle_hover_marker(*draw, *start, point.snapped_to_vertex);
        }
    };
    if (m_target)
        draw_distance(*m_target, MEASUREMENT_PICKED);
    if (m_hover_target)
        draw_distance(*m_hover_target, MEASUREMENT_HOVER);
}

void GLGizmoMeasure::clear_reference_plane()
{
    m_reference_plane.reset();
    m_reference_plane_index = -1;
    m_target.reset();
    m_hover_target.reset();
    m_hover_plane.reset();
}

void GLGizmoMeasure::clear_circle_points()
{
    m_current_circle = {};
    m_hover_circle.reset();
    m_invalid_circle_pick = false;
}

void GLGizmoMeasure::undo_circle_point()
{
    auto& active = circle_to_pick();
    auto& selection = !active.points.empty() ? active :
        !m_current_circle.points.empty() ? m_current_circle : m_first_circle;
    if (!selection.points.empty()) {
        selection.points.pop_back();
        selection.circle.reset();
    }
    m_invalid_circle_pick = false;
}

void GLGizmoMeasure::set_input_mode(InputMode mode)
{
    m_input_mode = mode;
    clear_circle_points();
    m_first_circle = {};
    cancel_pending_pick();
    if (measures_circles()) {
        m_snap_circle_vertices = true;
        m_pick_detected_circle = false;
    }
    clear_reference_plane();
    m_mouse_left_down = false;
    m_selected_features.reset();
    m_selected_sphere_raycasters.clear();
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_raycasters.clear();
    m_hover_id = -1;
    m_curr_feature.reset();
    m_curr_point_on_feature_position.reset();
    m_last_circle.reset();
    m_last_plane_idx = -1;
    m_mode = EMode::FeatureSelection;
    m_shift_kar_filter.reset_count();
    restore_scene_raycasters_state();
    update_measurement_result();
    m_parent.request_extra_frame();
}

void GLGizmoMeasure::update_if_needed()
{
    const auto planes = m_parent.section_planes();
    const bool section_active = planes[0].is_active();
    const bool caps_visible = m_parent.section_caps_visible();
    const bool planes_changed = planes != m_section_planes;
    const bool section_changed = planes_changed || caps_visible != m_section_caps_visible;

    auto update_plane_models_cache = [this](const indexed_triangle_set& its) {
        m_plane_models_cache.clear();
        m_plane_models_cache.resize(m_measuring->get_num_of_planes(), GLModel());

        auto& plane_models_cache = m_plane_models_cache;
        const auto& measuring = m_measuring;

        //for (int idx = 0; idx < m_measuring->get_num_of_planes(); ++idx) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_measuring->get_num_of_planes()),
        [&plane_models_cache, &measuring, &its](const tbb::blocked_range<size_t>& range) {
            for (size_t idx = range.begin(); idx != range.end(); ++idx) {
                GLModel::Geometry init_data = init_plane_data(its, measuring->get_plane_triangle_indices(idx));
                plane_models_cache[idx].init_from(std::move(init_data));
            }
        });
    };

    auto do_update = [&](const std::vector<VolumeCacheItem>& volumes_cache) {
        const bool geometry_changed = m_volumes_cache != volumes_cache;
        if (geometry_changed || section_changed)
            cancel_pending_pick();
        if (geometry_changed) {
            clear_circle_points();
            m_first_circle = {};
            clear_reference_plane();
        } else if (planes_changed) {
            const auto uses_section = [](const CircleSelection& selection) {
                return std::any_of(selection.points.begin(), selection.points.end(),
                    [](const PointPick& point) { return point.section_dependent; });
            };
            if (!m_current_circle.circle || uses_section(m_current_circle))
                clear_circle_points();
            if (!m_first_circle.circle || uses_section(m_first_circle))
                m_first_circle = {};
            m_invalid_circle_pick = false;
            if (m_reference_plane && m_reference_plane->source.is_cap())
                clear_reference_plane();
            if (m_target && m_target->section_dependent)
                m_target.reset();
        }

        indexed_triangle_set composite_mesh;
        m_facet_sources.clear();
        m_has_transparent_facets = false;
        const bool fff = advanced_measurement_available();
        for (size_t i = 0; i < volumes_cache.size(); ++i) {
            const VolumeCacheItem& vol = volumes_cache[i];
            TriangleMesh volume_mesh = *vol.mesh;
            volume_mesh.transform(vol.world_trafo, true);

            const bool use_transparency = fff && vol.volume->is_model_part();
            const auto& view = vol.instance->view_transparency;
            const auto body = view.bodies.find(vol.volume->id().id);
            const bool current_mask = use_transparency && body != view.bodies.end() && body->second.detailed() &&
                body->second.mesh_signature == ViewTransparency::mesh_signature(vol.mesh->its);
            const auto add_source = [&](size_t original_facet) {
                const auto percent = !use_transparency || original_facet == SECTION_CAP ? ViewTransparency::Percent{0}
                    : body != view.bodies.end() ? body->second.effective(view.percent, original_facet, current_mask)
                    : view.percent;
                m_facet_sources.push_back({i, original_facet, percent});
                m_has_transparent_facets |= percent != 0;
            };

            if (section_active) {
                std::vector<size_t> sources;
                auto visible_mesh = clip_section_surface(volume_mesh.its, planes[0], planes[1], &sources);
                if (caps_visible)
                    m_parent.append_section_caps(vol.canvas_volume_idx, visible_mesh);
                its_merge_vertices(visible_mesh);
                // Compact triangles and provenance together after welding.
                size_t kept = 0;
                for (size_t face = 0; face < visible_mesh.indices.size(); ++face) {
                    const Vec3i triangle = visible_mesh.indices[face];
                    if (triangle[0] == triangle[1] || triangle[0] == triangle[2] || triangle[1] == triangle[2])
                        continue;
                    visible_mesh.indices[kept++] = triangle;
                    add_source(face < sources.size() ? sources[face] : SECTION_CAP);
                }
                visible_mesh.indices.resize(kept);
                its_merge(composite_mesh, std::move(visible_mesh));
            } else {
                for (size_t face = 0; face < volume_mesh.its.indices.size(); ++face)
                    add_source(face);
                its_merge(composite_mesh, std::move(volume_mesh.its));
            }
        }

        m_measuring = std::make_unique<Measure::Measuring>(composite_mesh, section_active);
        update_plane_models_cache(m_measuring->get_its());
        m_raycaster.reset();
        if (!composite_mesh.indices.empty())
            m_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(composite_mesh)));
        m_volumes_cache = volumes_cache;
        m_section_planes = planes;
        m_section_caps_visible = caps_visible;
        m_refresh_mesh = false;

        m_reference_plane_index = -1;
        if (m_reference_plane && !section_hides(m_reference_plane->anchor, m_reference_plane->source.is_cap())) {
            const auto [index, normal, anchor] = m_reference_plane->plane.get_plane();
            const auto& source = m_reference_plane->source;
            for (size_t face = 0; face < m_facet_sources.size(); ++face) {
                const auto& candidate = m_facet_sources[face];
                if (candidate.volume != source.volume || candidate.original_facet != source.original_facet)
                    continue;
                if (const auto plane = m_measuring->get_plane(face)) {
                    const auto [new_index, new_normal, new_anchor] = plane->get_plane();
                    if (std::abs((new_anchor - anchor).dot(normal)) < EPSILON && new_normal.dot(normal) > 0.9999) {
                        m_reference_plane_index = new_index;
                        break;
                    }
                }
            }
        }

        if (section_changed || section_active || (geometry_changed && !m_pending_scale)) {
            // Feature IDs and coordinates refer to the previous visible mesh.
            m_selected_features.reset();
            m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
            m_raycasters.clear();
            m_selected_sphere_raycasters.clear();
            m_curr_feature.reset();
            m_curr_point_on_feature_position.reset();
            m_last_plane_idx = -1;
            m_last_circle.reset();
            m_hover_id = -1;
            m_mouse_left_down = false;
            update_measurement_result();
            m_parent.request_extra_frame();
        }
    };

    const Selection& selection = m_parent.get_selection();
    if (m_state != On)
        return;

    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    std::vector<VolumeCacheItem> volumes_cache;
    volumes_cache.reserve(idxs.size());
    for (unsigned int idx : idxs) {
        const GLVolume* v = selection.get_volume(idx);
        const int volume_idx = v->volume_idx();
        if (volume_idx < 0)
            continue;

        const ModelObject* obj = selection.get_model()->objects[v->object_idx()];
        const ModelInstance* inst = obj->instances[v->instance_idx()];
        const ModelVolume* vol = obj->volumes[volume_idx];
        const VolumeCacheItem item = {
            obj, inst, vol, vol->mesh_ptr(),
            section_active ? v->world_matrix() :
                Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) * inst->get_matrix() * vol->get_matrix(),
            idx
        };
        volumes_cache.emplace_back(item);
    }

    if (volumes_cache.empty()) {
        clear_circle_points();
        m_first_circle = {};
        cancel_pending_pick();
        clear_reference_plane();
        m_volumes_cache.clear();
        m_facet_sources.clear();
        m_measuring.reset();
        m_raycaster.reset();
        return;
    }

    if (m_measuring == nullptr || m_volumes_cache != volumes_cache || section_changed || m_refresh_mesh)
        do_update(volumes_cache);
}

void GLGizmoMeasure::disable_scene_raycasters()
{
    for (auto r : m_scene_raycasters) {
        r.raycaster->set_active(false);
    }
}

void GLGizmoMeasure::restore_scene_raycasters_state()
{
    for (auto r : m_scene_raycasters) {
        r.raycaster->set_active(r.state);
    }
}

void GLGizmoMeasure::render_dimensioning()
{
    static SelectedFeatures last_selected_features;

    if (!m_selected_features.first.feature.has_value())
        return;

    if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() != Measure::SurfaceFeatureType::Circle)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    auto point_point = [this, &shader](const Vec3d& v1, const Vec3d& v2, float distance) {
        if ((v2 - v1).squaredNorm() < 0.000001 || distance < 0.001f)
            return;

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        const std::array<int, 4>& viewport = camera.get_viewport();

        // screen coordinates
        const Vec2d v1ss = TransformHelper::world_to_ss(v1, projection_view_matrix, viewport);
        const Vec2d v2ss = TransformHelper::world_to_ss(v2, projection_view_matrix, viewport);

        if (v1ss.isApprox(v2ss))
            return;

        const Vec2d v12ss = v2ss - v1ss;
        const double v12ss_len = v12ss.norm();

        const bool overlap = v12ss_len - 2.0 * TRIANGLE_HEIGHT < 0.0;

        const auto q12ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(v12ss.x(), v12ss.y(), 0.0));
        const auto q21ss = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(-v12ss.x(), -v12ss.y(), 0.0));

        shader->set_uniform("projection_matrix", Transform3d::Identity());

        const Vec3d v1ss_3 = { v1ss.x(), v1ss.y(), 0.0 };
        const Vec3d v2ss_3 = { v2ss.x(), v2ss.y(), 0.0 };

        const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            shader->stop_using();

#if SLIC3R_OPENGL_ES
            shader = wxGetApp().get_shader("dashed_lines");
#else
            shader = wxGetApp().get_shader("dashed_thick_lines");
#endif // SLIC3R_OPENGL_ES
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
        }
        else
            glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

        // stem
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::translation_transform(-2.0 * TRIANGLE_HEIGHT * Vec3d::UnitX()) * Geometry::scale_transform({ v12ss_len + 4.0 * TRIANGLE_HEIGHT, 1.0f, 1.0f }) :
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss * Geometry::scale_transform({ v12ss_len, 1.0f, 1.0f }));
        m_dimensioning.line.set_color(ColorRGBA::WHITE());
        m_dimensioning.line.render();

#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
#if !SLIC3R_OPENGL_ES
        }
        else
            glsafe(::glLineWidth(1.0f));
#endif // !SLIC3R_OPENGL_ES

        // arrow 1
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q12ss :
            ss_to_ndc_matrix * Geometry::translation_transform(v1ss_3) * q21ss);
        m_dimensioning.triangle.render();

        // arrow 2
        shader->set_uniform("view_model_matrix", overlap ?
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q21ss :
            ss_to_ndc_matrix * Geometry::translation_transform(v2ss_3) * q12ss);
        m_dimensioning.triangle.render();

        const bool use_inches = wxGetApp().app_config->get_bool("use_inches");
        const double curr_value = use_inches ? ObjectManipulation::mm_to_in * distance : distance;
        const std::string curr_value_str = format_double(curr_value);
        const std::string units = use_inches ? "in" : "mm";
        const float value_str_width = 20.0f + ImGui::CalcTextSize(curr_value_str.c_str()).x;
        static double edit_value = 0.0;

        const Vec2d label_position = 0.5 * (v1ss + v2ss);
        ImGuiPureWrap::set_next_window_pos(label_position.x(), viewport[3] - label_position.y(), ImGuiCond_Always, 0.0f, 1.0f);
        ImGuiPureWrap::set_next_window_bg_alpha(0.0f);

        if (!m_editing_distance) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
            ImGuiPureWrap::begin(std::string("distance"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ImGui::AlignTextToFramePadding();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const std::string txt = curr_value_str + " " + units;
            ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
            const ImGuiStyle& style = ImGui::GetStyle();
            draw_list->AddRectFilled({ pos.x - style.FramePadding.x, pos.y + style.FramePadding.y }, { pos.x + txt_size.x + 2.0f * style.FramePadding.x , pos.y + txt_size.y + 2.0f * style.FramePadding.y },
              ImGuiPSWrap::to_ImU32(ColorRGBA(0.5f, 0.5f, 0.5f, 0.5f)));
            ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
            ImGuiPureWrap::text(txt);
            if (!m_section_planes[0].is_active()) {
                ImGui::SameLine();
                if (m_imgui->image_button(ImGui::SliderFloatEditBtnIcon, _u8L("Edit to scale"))) {
                    m_editing_distance = true;
                    edit_value = curr_value;
                    m_imgui->requires_extra_frame();
                }
            }
            ImGuiPureWrap::end();
            ImGui::PopStyleVar(3);
        }

        if (m_editing_distance && !ImGui::IsPopupOpen("distance_popup"))
            ImGui::OpenPopup("distance_popup");

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 1.0f, 1.0f });
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 0.0f });
        if (ImGui::BeginPopupModal("distance_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration)) {
            auto perform_scale = [this](double new_value, double old_value) {
                if (new_value == old_value || new_value <= 0.0)
                    return;

                const double ratio = new_value / old_value;
                wxGetApp().plater()->take_snapshot(_L("Scale"));

                struct TrafoData
                {
                    double ratio;
                    Vec3d old_pivot;
                    Vec3d new_pivot;
                    Transform3d scale_matrix;

                    TrafoData(double ratio, const Vec3d& old_pivot, const Vec3d& new_pivot) {
                        this->ratio = ratio;
                        this->scale_matrix = Geometry::scale_transform(ratio);
                        this->old_pivot = old_pivot;
                        this->new_pivot = new_pivot;
                    }

                    Vec3d transform(const Vec3d& point) const { return this->scale_matrix * (point - this->old_pivot) + this->new_pivot; }
                };

                auto scale_feature = [](Measure::SurfaceFeature& feature, const TrafoData& trafo_data) {
                    switch (feature.get_type())
                    {
                    case Measure::SurfaceFeatureType::Point:
                    {
                        feature = Measure::SurfaceFeature(trafo_data.transform(feature.get_point()));
                        break;
                    }
                    case Measure::SurfaceFeatureType::Edge:
                    {
                        const auto [from, to] = feature.get_edge();
                        const std::optional<Vec3d> extra = feature.get_extra_point();
                        const std::optional<Vec3d> new_extra = extra.has_value() ? trafo_data.transform(*extra) : extra;
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, trafo_data.transform(from), trafo_data.transform(to), new_extra);
                        break;
                    }
                    case Measure::SurfaceFeatureType::Circle:
                    {
                        const auto [center, radius, normal] = feature.get_circle();
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Circle, trafo_data.transform(center), normal, std::nullopt, trafo_data.ratio * radius);
                        break;
                    }
                    case Measure::SurfaceFeatureType::Plane:
                    {
                        const auto [idx, normal, origin] = feature.get_plane();
                        feature = Measure::SurfaceFeature(Measure::SurfaceFeatureType::Plane, normal, trafo_data.transform(origin), std::nullopt, idx);
                        break;
                    }
                    default: { break; }
                    }
                  };

                // apply scale
                TransformationType type;
                type.set_world();
                type.set_relative();
                type.set_joint();

                // scale selection
                Selection& selection = m_parent.get_selection();
                const Vec3d old_center = selection.get_bounding_box().center();
                selection.setup_cache();
                selection.scale(ratio * Vec3d::Ones(), type);
                wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot
                wxGetApp().obj_manipul()->set_dirty();

                // scale dimensioning
                const Vec3d new_center = selection.get_bounding_box().center();
                const TrafoData trafo_data(ratio, old_center, new_center);
                scale_feature(*m_selected_features.first.feature, trafo_data);
                if (m_selected_features.second.feature.has_value())
                    scale_feature(*m_selected_features.second.feature, trafo_data);

                // update measure on next call to data_changed()
                m_pending_scale = true;
            };
            auto action_exit = [this]() {
                m_editing_distance = false;
                m_is_editing_distance_first_frame = true;
                ImGui::CloseCurrentPopup();
            };
            auto action_scale = [perform_scale, action_exit](double new_value, double old_value) {
                perform_scale(new_value, old_value);
                action_exit();
            };

            ImGuiPureWrap::disable_background_fadeout_animation();
            ImGui::PushItemWidth(value_str_width);
            if (ImGui::InputDouble("##distance", &edit_value, 0.0f, 0.0f, "%.3f")) {
            }

            // trick to auto-select text in the input widgets on 1st frame
            if (m_is_editing_distance_first_frame) {
                ImGui::SetKeyboardFocusHere(0);
                m_is_editing_distance_first_frame = false;
                m_imgui->set_requires_extra_frame();
            }

            // handle keys input
            if (ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter))
                action_scale(edit_value, curr_value);
            else if (ImGui::IsKeyPressedMap(ImGuiKey_Escape))
                action_exit();

            ImGui::SameLine();
            if (ImGuiPureWrap::button(_CTX_utf8(L_CONTEXT("Scale", "Verb"), "Verb")))
                action_scale(edit_value, curr_value);
            ImGui::SameLine();
            if (ImGuiPureWrap::button(_u8L("Cancel")))
                action_exit();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
    };

    auto point_edge = [this, shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Point && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        std::pair<Vec3d, Vec3d> e = f2.get_edge();
        const Vec3d v_proj = m_measurement_result.distance_infinite->to;
        const Vec3d e1e2 = e.second - e.first;
        const Vec3d v_proje1 = v_proj - e.first;
        const bool on_e1_side = v_proje1.dot(e1e2) < -EPSILON;
        const bool on_e2_side = !on_e1_side && v_proje1.norm() > e1e2.norm();
        if (on_e1_side || on_e2_side) {
            const Camera& camera = wxGetApp().plater()->get_camera();
            const Matrix4d projection_view_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
            const std::array<int, 4>& viewport = camera.get_viewport();
            const Transform3d ss_to_ndc_matrix = TransformHelper::ndc_to_ss_matrix_inverse(viewport);

            const Vec2d v_projss = TransformHelper::world_to_ss(v_proj, projection_view_matrix, viewport);
            auto render_extension = [this, &v_projss, &projection_view_matrix, &viewport, &ss_to_ndc_matrix, shader](const Vec3d& p) {
                const Vec2d pss = TransformHelper::world_to_ss(p, projection_view_matrix, viewport);
                if (!pss.isApprox(v_projss)) {
                    const Vec2d pv_projss = v_projss - pss;
                    const double pv_projss_len = pv_projss.norm();

                    const auto q = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Vec3d(pv_projss.x(), pv_projss.y(), 0.0));

                    shader->set_uniform("projection_matrix", Transform3d::Identity());
                    shader->set_uniform("view_model_matrix", ss_to_ndc_matrix * Geometry::translation_transform({ pss.x(), pss.y(), 0.0 }) * q *
                        Geometry::scale_transform({ pv_projss_len, 1.0f, 1.0f }));
                    m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
                    m_dimensioning.line.render();
                }
            };

            render_extension(on_e1_side ? e.first : e.second);
        }
    };

    auto arc_edge_edge = [this, &shader](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2, double radius = 0.0) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Edge);
        if (!m_measurement_result.angle.has_value())
            return;

        const double angle = m_measurement_result.angle->angle;
        const Vec3d  center = m_measurement_result.angle->center;
        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;
        const bool   coplanar = m_measurement_result.angle->coplanar;

        if (std::abs(angle) < EPSILON || std::abs(calc_radius) < EPSILON)
            return;

        const double draw_radius = (radius > 0.0) ? radius : calc_radius;

        const Vec3d e1_unit = Measure::edge_direction(e1);
        const Vec3d e2_unit = Measure::edge_direction(e2);

        const unsigned int resolution = std::max<unsigned int>(2, 64 * angle / double(PI));
        const double step = angle / double(resolution);
        const Vec3d normal = e1_unit.cross(e2_unit).normalized();

        if (!m_dimensioning.arc.is_initialized()) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
            init_data.color = ColorRGBA::WHITE();
            init_data.reserve_vertices(resolution + 1);
            init_data.reserve_indices(resolution + 1);

            // vertices + indices
            for (unsigned int i = 0; i <= resolution; ++i) {
                const double a = step * double(i);
                const Vec3d v = draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(a, normal)) * e1_unit);
                init_data.add_vertex((Vec3f)v.cast<float>());
                init_data.add_index(i);
            }

            m_dimensioning.arc.init_from(std::move(init_data));
        }

        const Camera& camera = wxGetApp().plater()->get_camera();
#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            shader->stop_using();

#if SLIC3R_OPENGL_ES
            shader = wxGetApp().get_shader("dashed_lines");
#else
            shader = wxGetApp().get_shader("dashed_thick_lines");
#endif // SLIC3R_OPENGL_ES
            if (shader == nullptr)
                return;

            shader->start_using();
            shader->set_uniform("projection_matrix", Transform3d::Identity());
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 1.0f);
            shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
        }
        else
            glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

        // arc
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center));
        m_dimensioning.arc.render();

#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            shader->stop_using();

            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;

            shader->start_using();
#if !SLIC3R_OPENGL_ES
        }
        else
            glsafe(::glLineWidth(1.0f));
#endif // !SLIC3R_OPENGL_ES

        // arrows
        auto render_arrow = [this, shader, &camera, &normal, &center, &e1_unit, draw_radius, step, resolution](unsigned int endpoint_id) {
            const double angle = (endpoint_id == 1) ? 0.0 : step * double(resolution);
            const Vec3d position_model = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(angle, normal)) * e1_unit));
            const Vec3d direction_model = (endpoint_id == 1) ? -normal.cross(position_model - center).normalized() : normal.cross(position_model - center).normalized();
            const auto qz = Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), (endpoint_id == 1) ? normal : -normal);
            const auto qx = Eigen::Quaternion<double>::FromTwoVectors(qz * Vec3d::UnitX(), direction_model);
            const Transform3d view_model_matrix = camera.get_view_matrix() * Geometry::translation_transform(position_model) *
                qx * qz * Geometry::scale_transform(camera.get_inv_zoom());
            shader->set_uniform("view_model_matrix", view_model_matrix);
            m_dimensioning.triangle.render();
        };

        glsafe(::glDisable(GL_CULL_FACE));
        render_arrow(1);
        render_arrow(2);
        glsafe(::glEnable(GL_CULL_FACE));

        // edge 1 extension
        const Vec3d e11e12 = e1.second - e1.first;
        const Vec3d e11center = center - e1.first;
        const double e11center_len = e11center.norm();
        if (e11center_len > EPSILON && e11center.dot(e11e12) < 0.0) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e1.first, e1.second)) *
                Geometry::scale_transform({ e11center_len, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // edge 2 extension
        const Vec3d e21center = center - e2.first;
        const double e21center_len = e21center.norm();
        if (e21center_len > EPSILON) {
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(center) *
                Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitX(), Measure::edge_direction(e2.first, e2.second)) *
                Geometry::scale_transform({ (coplanar && radius > 0.0) ? e21center_len : draw_radius, 1.0f, 1.0f }));
            m_dimensioning.line.set_color(ColorRGBA::LIGHT_GRAY());
            m_dimensioning.line.render();
        }

        // label
        // label world coordinates
        const Vec3d label_position_world = Geometry::translation_transform(center) * (draw_radius * (Eigen::Quaternion<double>(Eigen::AngleAxisd(step * 0.5 * double(resolution), normal)) * e1_unit));

        // label screen coordinates
        const std::array<int, 4>& viewport = camera.get_viewport();
        const Vec2d label_position_ss = TransformHelper::world_to_ss(label_position_world,
            camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix(), viewport);

        ImGuiPureWrap::set_next_window_pos(label_position_ss.x(), viewport[3] - label_position_ss.y(), ImGuiCond_Always, 0.0f, 1.0f);
        ImGuiPureWrap::set_next_window_bg_alpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiPureWrap::begin("##angle", ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::AlignTextToFramePadding();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const std::string txt = format_double(Geometry::rad2deg(angle)) + "°";
        ImVec2 txt_size = ImGui::CalcTextSize(txt.c_str());
        const ImGuiStyle& style = ImGui::GetStyle();
        draw_list->AddRectFilled({ pos.x - style.FramePadding.x, pos.y + style.FramePadding.y }, { pos.x + txt_size.x + 2.0f * style.FramePadding.x , pos.y + txt_size.y + 2.0f * style.FramePadding.y },
          ImGuiPSWrap::to_ImU32(ColorRGBA(0.5f, 0.5f, 0.5f, 0.5f)));
        ImGui::SetCursorScreenPos({ pos.x + style.FramePadding.x, pos.y });
        ImGuiPureWrap::text(txt);
        ImGuiPureWrap::end();
        ImGui::PopStyleVar();
    };

    auto arc_edge_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Edge && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    auto arc_plane_plane = [this, arc_edge_edge](const Measure::SurfaceFeature& f1, const Measure::SurfaceFeature& f2) {
        assert(f1.get_type() == Measure::SurfaceFeatureType::Plane && f2.get_type() == Measure::SurfaceFeatureType::Plane);
        if (!m_measurement_result.angle.has_value())
            return;

        const std::pair<Vec3d, Vec3d> e1 = m_measurement_result.angle->e1;
        const std::pair<Vec3d, Vec3d> e2 = m_measurement_result.angle->e2;
        const double calc_radius = m_measurement_result.angle->radius;

        if (calc_radius == 0.0)
            return;

        arc_edge_edge(Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e1.first, e1.second),
            Measure::SurfaceFeature(Measure::SurfaceFeatureType::Edge, e2.first, e2.second), calc_radius);
    };

    shader->start_using();

    if (!m_dimensioning.line.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color = ColorRGBA::WHITE();
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(1.0f, 0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_dimensioning.line.init_from(std::move(init_data));
    }

    if (!m_dimensioning.triangle.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.color = ColorRGBA::WHITE();
        init_data.reserve_vertices(3);
        init_data.reserve_indices(3);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, 0.5f * TRIANGLE_BASE, 0.0f));
        init_data.add_vertex(Vec3f(-TRIANGLE_HEIGHT, -0.5f * TRIANGLE_BASE, 0.0f));

        // indices
        init_data.add_triangle(0, 1, 2);

        m_dimensioning.triangle.init_from(std::move(init_data));
    }

    if (last_selected_features != m_selected_features)
        m_dimensioning.arc.reset();

    glsafe(::glDisable(GL_DEPTH_TEST));

    const bool has_distance = m_measurement_result.has_distance_data();

    const Measure::SurfaceFeature* f1 = &(*m_selected_features.first.feature);
    const Measure::SurfaceFeature* f2 = nullptr;
    std::unique_ptr<Measure::SurfaceFeature> temp_feature;
    if (m_selected_features.second.feature.has_value())
        f2 = &(*m_selected_features.second.feature);
    else {
        assert(m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Circle);
        temp_feature = std::make_unique<Measure::SurfaceFeature>(std::get<0>(m_selected_features.first.feature->get_circle()));
        f2 = temp_feature.get();
    }

    if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() != Measure::SurfaceFeatureType::Circle)
        return;

    Measure::SurfaceFeatureType ft1 = f1->get_type();
    Measure::SurfaceFeatureType ft2 = f2->get_type();

    // Order features by type so following conditions are simple.
    if (ft1 > ft2) {
        std::swap(ft1, ft2);
        std::swap(f1, f2);
    }

    // If there is an angle to show, draw the arc:
    if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Edge)
        arc_edge_edge(*f1, *f2);
    else if (ft1 == Measure::SurfaceFeatureType::Edge && ft2 == Measure::SurfaceFeatureType::Plane)
        arc_edge_plane(*f1, *f2);
    else if (ft1 == Measure::SurfaceFeatureType::Plane && ft2 == Measure::SurfaceFeatureType::Plane)
        arc_plane_plane(*f1, *f2);

    if (has_distance){
        // Where needed, draw the extension of the edge to where the dist is measured:
        if (ft1 == Measure::SurfaceFeatureType::Point && ft2 == Measure::SurfaceFeatureType::Edge)
            point_edge(*f1, *f2);

        // Render the arrow between the points that the backend passed:
        const Measure::DistAndPoints& dap = m_measurement_result.distance_infinite.has_value()
            ? *m_measurement_result.distance_infinite
            : *m_measurement_result.distance_strict;
        point_point(dap.from, dap.to, dap.dist);
    }

    glsafe(::glEnable(GL_DEPTH_TEST));

    shader->stop_using();
}

static void add_row_to_table(std::function<void(void)> col_1 = nullptr, std::function<void(void)> col_2 = nullptr)
{
    assert(col_1 != nullptr && col_2 != nullptr);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    col_1();
    ImGui::TableSetColumnIndex(1);
    col_2();
}

static void add_strings_row_to_table(const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color)
{
    add_row_to_table([&]() { ImGuiPureWrap::text_colored(col_1_color, col_1); }, [&]() { ImGuiPureWrap::text_colored(col_2_color, col_2); });
};

#if ENABLE_MEASURE_GIZMO_DEBUG
void GLGizmoMeasure::render_debug_dialog()
{
    auto add_feature_data = [this](const SelectedFeatures::Item& item) {
        const std::string text = (item.source == item.feature) ? surface_feature_type_as_string(item.feature->get_type()) : point_on_feature_type_as_string(item.source->get_type(), m_hover_id);
        add_strings_row_to_table(*m_imgui, "Type", ImGuiPureWrap::COL_ORANGE_LIGHT, text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        switch (item.feature->get_type())
        {
        case Measure::SurfaceFeatureType::Point:
        {
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(item.feature->get_point()), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Edge:
        {
            auto [from, to] = item.feature->get_edge();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(from), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(to), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Plane:
        {
            auto [idx, normal, origin] = item.feature->get_plane();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(origin), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiPureWrap::COL_ORANGE_LIGHT, format_double(idx), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        case Measure::SurfaceFeatureType::Circle:
        {
            auto [center, radius, normal] = item.feature->get_circle();
            const Vec3d on_circle = center + radius * Measure::get_orthogonal(normal, true);
            radius = (on_circle - center).norm();
            add_strings_row_to_table(*m_imgui, "m_pt1", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(center), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_pt2", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(normal), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table(*m_imgui, "m_value", ImGuiPureWrap::COL_ORANGE_LIGHT, format_double(radius), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            break;
        }
        }
        std::optional<Vec3d> extra_point = item.feature->get_extra_point();
        if (extra_point.has_value())
            add_strings_row_to_table(*m_imgui, "m_pt3", ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(*extra_point), ImGui::GetStyleColorVec4(ImGuiCol_Text));
    };

    ImGuiPureWrap::begin("Measure tool debug", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTable("Mode", 2)) {
        std::string txt;
        switch (m_mode)
        {
        case EMode::FeatureSelection: { txt = "Feature selection"; break; }
        case EMode::PointSelection:   { txt = "Point selection"; break; }
        default:                      { assert(false); break; }
        }
        add_strings_row_to_table(*m_imgui, "Mode", ImGuiPureWrap::COL_ORANGE_LIGHT, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::BeginTable("Hover", 2)) {
        add_strings_row_to_table(*m_imgui, "Hover id", ImGuiPureWrap::COL_ORANGE_LIGHT, std::to_string(m_hover_id), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const std::string txt = m_curr_feature.has_value() ? surface_feature_type_as_string(m_curr_feature->get_type()) : "None";
        add_strings_row_to_table(*m_imgui, "Current feature", ImGuiPureWrap::COL_ORANGE_LIGHT, txt, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (!m_selected_features.first.feature.has_value() && !m_selected_features.second.feature.has_value())
        ImGuiPureWrap::text("Empty selection");
    else {
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersH;
        if (m_selected_features.first.feature.has_value()) {
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Selection 1");
            if (ImGui::BeginTable("Selection 1", 2, flags)) {
                add_feature_data(m_selected_features.first);
                ImGui::EndTable();
            }
        }
        if (m_selected_features.second.feature.has_value()) {
            ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Selection 2");
            if (ImGui::BeginTable("Selection 2", 2, flags)) {
                add_feature_data(m_selected_features.second);
                ImGui::EndTable();
            }
        }
    }
    ImGuiPureWrap::end();
}
#endif // ENABLE_MEASURE_GIZMO_DEBUG

void GLGizmoMeasure::render_circle_options()
{
    const bool pair = m_input_mode == InputMode::CircleDistance;
    const bool detection_available = advanced_measurement_available();
    auto& selection = circle_to_pick();
    if (detection_available && ImGui::Checkbox(_u8L("Pick detected circular edges").c_str(), &m_pick_detected_circle)) {
        if (!pair || !selection.circle)
            selection = {};
        m_hover_circle.reset();
        m_invalid_circle_pick = false;
        m_parent.request_extra_frame();
    }
    if (pair && !selection.circle)
        ImGuiPureWrap::text(m_first_circle.circle ? _u8L("Select the second circle.") : _u8L("Select the first circle."));
    if (!selection.circle && detection_available && m_pick_detected_circle) {
        ImGuiPureWrap::text(_u8L("Click a highlighted circular edge."));
        if (m_invalid_circle_pick)
            ImGuiPureWrap::text(_u8L("No circular edge was detected here."));
    } else if (!m_pick_detected_circle || !detection_available) {
        if (ImGui::Checkbox(_u8L("Snap to mesh vertices").c_str(), &m_snap_circle_vertices))
            m_parent.request_extra_frame();
        if (!selection.circle) {
            if (m_snap_circle_vertices)
                ImGuiPureWrap::text_wrapped(_u8L("Only visible mesh vertices are accepted. Move closer until the yellow diamond appears."), 360.0f * m_imgui->get_style_scaling());
            ImGuiPureWrap::text(_u8L("Select three points on the same circular edge.") +
                " (" + std::to_string(selection.points.size()) + "/3)");
            if (detection_available)
                ImGuiPureWrap::text_wrapped(_u8L("Three points construct a circle even on an oval or a chamfer edge."), 360.0f * m_imgui->get_style_scaling());
            if (m_invalid_circle_pick)
                ImGuiPureWrap::text_wrapped(_u8L("No valid point selected. Choose a visible vertex; the three points must be distinct and not nearly collinear."), 360.0f * m_imgui->get_style_scaling());
        }
    }

    if (!pair && m_current_circle.circle) {
        const auto [center, radius, normal] = m_current_circle.circle->get_circle();
        const bool use_inches = wxGetApp().app_config->get_bool("use_inches");
        const double scale = use_inches ? ObjectManipulation::mm_to_in : 1.0;
        ImGuiPureWrap::text(_u8L("Diameter") + ": " + measurement_length(2.0 * radius));
        ImGuiPureWrap::text(_u8L("Radius") + ": " + measurement_length(radius));
        ImGuiPureWrap::text(_u8L("Circle center") + ": " + format_vec3(center * scale) + (use_inches ? " in" : " mm"));
        if (circle_hidden(m_current_circle))
            ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), _u8L("Current circle hidden by section."));
    }

    if ((!selection.points.empty() || (pair && !m_first_circle.points.empty())) &&
        ImGuiPureWrap::button(_u8L("Undo last point"))) {
        undo_circle_point();
        m_parent.request_extra_frame();
    }
    if (!pair) {
        if (ImGuiPureWrap::button(_u8L("Restart selection"))) {
            clear_circle_points();
            m_parent.request_extra_frame();
        }
        return;
    }
    if (ImGuiPureWrap::button(_u8L("Restart measurement"))) {
        clear_circle_points();
        m_first_circle = {};
        m_parent.request_extra_frame();
    }
    if (m_first_circle.circle && !m_current_circle.points.empty()) {
        ImGui::SameLine();
        if (ImGuiPureWrap::button(_u8L("Reselect second circle"))) {
            clear_circle_points();
            m_parent.request_extra_frame();
        }
    }
    ImGui::Separator();
    for (const bool first : {true, false}) {
        const auto& circle = first ? m_first_circle : m_current_circle;
        if (circle.circle) {
            ImGuiPureWrap::text((first ? _u8L("First circle") : _u8L("Second circle")) + "  Ø " +
                measurement_length(2.0 * std::get<1>(circle.circle->get_circle())));
            if (circle_hidden(circle))
                ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                    first ? _u8L("First circle hidden by section.") : _u8L("Second circle hidden by section."));
        }
    }
    if (m_first_circle.circle && m_current_circle.circle) {
        const auto result = Measure::compare_circles(*m_first_circle.circle, *m_current_circle.circle);
        const bool inches = wxGetApp().app_config->get_bool("use_inches");
        ImGuiPureWrap::text(_u8L("3D center distance") + ": " + measurement_length(result.center_distance));
        ImGuiPureWrap::text("Δ " + format_vec3(result.delta * (inches ? ObjectManipulation::mm_to_in : 1.0)) + (inches ? " in" : " mm"));
        ImGuiPureWrap::text(_u8L("Center offset in reference plane") + ": " + measurement_length(result.radial_offset));
        ImGuiPureWrap::text(_u8L("Center offset along reference axis") + ": " + measurement_length(result.axial_offset));
        ImGuiPureWrap::text(_u8L("Diameter difference") + ": " + measurement_length(result.diameter_difference));
        ImGuiPureWrap::text(_u8L("Angle between circle normals") + ": " + format_double(Geometry::rad2deg(result.normal_angle)) + "°");
        if (ImGuiPureWrap::button(_u8L("Swap circles"))) {
            std::swap(m_first_circle, m_current_circle);
            m_parent.request_extra_frame();
        }
    }
}

void GLGizmoMeasure::render_perpendicular_options()
{
    if (!m_reference_plane) {
        ImGuiPureWrap::text(_u8L("Click a surface to lock the reference plane."));
        return;
    }
    if (section_hides(m_reference_plane->anchor, m_reference_plane->source.is_cap()))
        ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), _u8L("Reference plane hidden by section."));
    ImGuiPureWrap::text(_u8L("Click to set or replace the target point."));
    ImGuiPureWrap::text(_u8L("Hold Shift to snap to a visible vertex."));
    const auto distance = [this](const PointPick& point) {
        return Measure::get_measurement(Measure::SurfaceFeature(point.position), m_reference_plane->plane).distance_infinite->dist;
    };
    if (m_target) {
        ImGuiPureWrap::text(_u8L("Point-to-plane distance") + ": " + measurement_length(distance(*m_target)));
        if (section_hides(m_target->position, m_target->from_cap))
            ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), _u8L("Target point hidden by section."));
        if (m_target->normal) {
            const auto normal = std::get<1>(m_reference_plane->plane.get_plane());
            const double angle = std::atan2(normal.cross(*m_target->normal).norm(), std::abs(normal.dot(*m_target->normal)));
            ImGuiPureWrap::text(_u8L("Angle between surfaces at the picks") + ": " + format_double(Geometry::rad2deg(angle)) + "°");
        }
    }
    if (m_hover_target)
        ImGuiPureWrap::text(_u8L("Distance at cursor") + ": " + measurement_length(distance(*m_hover_target)));
    if (ImGuiPureWrap::button(_u8L("Reset reference plane"))) {
        clear_reference_plane();
        m_parent.request_extra_frame();
    }
}

void GLGizmoMeasure::on_render_input_window(float x, float y, float bottom_limit)
{
    static std::optional<Measure::SurfaceFeature> last_feature;
    static EMode last_mode = EMode::FeatureSelection;
    static SelectedFeatures last_selected_features;

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    if (m_editing_distance)
        return;

    ImGuiPureWrap::begin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    ImGui::SetWindowPos(ImVec2(x, y), ImGuiCond_Always);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }

    if (m_parent.is_section_offset_adjusting()) {
        ImGuiPureWrap::text(_u8L("Release the section slider to measure."));
        ImGuiPureWrap::end();
        return;
    }

    if (advanced_measurement_available()) {
        int mode = int(m_input_mode);
        if (ImGuiPureWrap::combo(_u8L("Measurement mode"), {
                _u8L("Standard measurement"), _u8L("Circle measurement"), _u8L("Circle center distance"),
                _u8L("Perpendicular distance")}, mode))
            set_input_mode(InputMode(mode));
        if (ImGui::Checkbox(_u8L("Pick through transparent surfaces").c_str(), &m_pick_through))
            m_parent.request_extra_frame();
        if (ImGui::Checkbox(_u8L("Show overhang slope").c_str(), &m_show_overhang))
            m_parent.request_extra_frame();
        ImGuiPureWrap::text_wrapped(_u8L("Turn this on, rotate the view to expose the underside, and hover over a downward-facing model surface to see the slope beside the pointer. The slope is measured from the horizontal build plate: a horizontal overhang is 0°, and the angle approaches 90° near vertical. Upward faces, vertical faces, and section fill have no reading. Check the sliced preview for actual supports."), 360.0f * m_imgui->get_style_scaling());
        if (m_show_overhang && m_hover_slope)
            ImGuiPureWrap::text(_u8L("Overhang slope from horizontal") + ": " +
                slope_angle_text(*m_hover_slope));
    } else {
        bool three_point_mode = m_input_mode == InputMode::Circles;
        if (ImGui::Checkbox(_u8L("Three-point circle").c_str(), &three_point_mode))
            set_input_mode(three_point_mode ? InputMode::Circles : InputMode::Standard);
    }
    if (uses_click_to_pick())
        ImGuiPureWrap::text_wrapped(_u8L("Left-click to pick. Drag with the left button to rotate, or with the right or middle button to pan. Scroll to zoom."), 360.0f * m_imgui->get_style_scaling());
    if (measures_circles()) {
        render_circle_options();
        ImGuiPureWrap::end();
        return;
    }
    if (m_input_mode == InputMode::Perpendicular) {
        render_perpendicular_options();
        ImGuiPureWrap::end();
        return;
    }

    if (ImGui::BeginTable("Commands", 2)) {
        unsigned int row_count = 1;
        add_row_to_table(
            []() {
                ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, _u8L("Left mouse button"));
            },
            [this]() {
                std::string text;
                ColorRGBA color;
                if (m_selected_features.second.feature.has_value()) {
                    if (m_selected_features.first.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                        // hovering over 1st selected feature
                        text = _u8L("Unselect feature");
                        color = SELECTED_1ST_COLOR;
                    }
                    else if (m_hover_id == SEL_SPHERE_1_ID) {
                        if (m_selected_features.first.is_center) {
                            // hovering over center selected as 1st feature
                            text = _u8L("Unselect center");
                            color = SELECTED_1ST_COLOR;
                        }
                        else if (is_feature_with_center(*m_selected_features.first.feature)) {
                            // hovering over center of 1st selected feature
                            text = _u8L("Select center");
                            color = SELECTED_1ST_COLOR;
                        }
                        else {
                            // hovering over point selected as 1st feature
                            text = _u8L("Unselect point");
                            color = SELECTED_1ST_COLOR;
                        }
                    }
                    else if (m_selected_features.first.is_center && m_selected_features.first.source == m_curr_feature) {
                        // hovering over feature whose center is selected as 1st feature
                        text = _u8L("Select feature");
                        color = SELECTED_1ST_COLOR;
                    }
                    else if (m_selected_features.second.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                        // hovering over 2nd selected feature
                        text = _u8L("Unselect feature");
                        color = SELECTED_2ND_COLOR;
                    }
                    else if (m_hover_id == SEL_SPHERE_2_ID) {
                        if (m_selected_features.second.is_center) {
                            // hovering over center selected as 2nd feature
                            text = _u8L("Unselect feature");
                            color = SELECTED_2ND_COLOR;
                        }
                        else if (is_feature_with_center(*m_selected_features.second.feature)) {
                            // hovering over center of 2nd selected feature
                            text = _u8L("Select center");
                            color = SELECTED_2ND_COLOR;
                        }
                        else {
                            // hovering over point selected as 2nd feature
                            text = _u8L("Unselect point");
                            color = SELECTED_2ND_COLOR;
                        }
                    }
                    else if (m_selected_features.second.is_center && m_selected_features.second.source == m_curr_feature) {
                        // hovering over feature whose center is selected as 2nd feature
                        text = _u8L("Select feature");
                        color = SELECTED_2ND_COLOR;
                    }
                    else {
                        // 1st feature selected
                        text = (m_mode == EMode::PointSelection) ? _u8L("Select point") : _u8L("Select feature");
                        color = SELECTED_2ND_COLOR;
                    }
                }
                else {
                    if (m_selected_features.first.feature.has_value()) {
                        if (m_selected_features.first.feature == m_curr_feature && m_mode == EMode::FeatureSelection) {
                            // hovering over 1st selected feature
                            text = _u8L("Unselect feature");
                            color = SELECTED_1ST_COLOR;
                        }
                        else {
                            if (m_hover_id == SEL_SPHERE_1_ID) {
                                if (m_selected_features.first.is_center) {
                                    // hovering over center selected as 1st feature
                                    text = _u8L("Unselect feature");
                                    color = SELECTED_1ST_COLOR;
                                }
                                else if (is_feature_with_center(*m_selected_features.first.feature)) {
                                    // hovering over center of 1st selected feature
                                    text = _u8L("Select center");
                                    color = SELECTED_1ST_COLOR;
                                }
                                else {
                                    // hovering over point selected as 1st feature
                                    text = _u8L("Unselect point");
                                    color = SELECTED_1ST_COLOR;
                                }
                            }
                            else {
                                if (m_selected_features.first.is_center && m_selected_features.first.source == m_curr_feature) {
                                    // hovering over feature whose center is selected as 1st feature
                                    text = _u8L("Select feature");
                                    color = SELECTED_1ST_COLOR;
                                }
                                else {
                                    // 1st feature selected
                                    text = (m_mode == EMode::PointSelection) ? _u8L("Select point") : _u8L("Select feature");
                                    color = SELECTED_2ND_COLOR;
                                }
                            }
                        }
                    }
                    else {
                        // nothing is selected
                        text = (m_mode == EMode::PointSelection) ? _u8L("Select point") : _u8L("Select feature");
                        color = SELECTED_1ST_COLOR;
                    }
                }

                assert(!text.empty());

                ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_Text), text);
                ImGui::SameLine();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float rect_size = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + rect_size, pos.y + rect_size), ImGuiPSWrap::to_ImU32(color));
                ImGui::Dummy(ImVec2(rect_size, rect_size));
            }
            );

        if (m_mode == EMode::FeatureSelection && m_hover_id != -1) {
            add_strings_row_to_table("Shift", ImGuiPureWrap::COL_ORANGE_LIGHT, _u8L("Enable point selection"), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ++row_count;
        }

        if (m_selected_features.first.feature.has_value()) {
            add_strings_row_to_table("Delete", ImGuiPureWrap::COL_ORANGE_LIGHT, _u8L("Restart selection"), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ++row_count;
        }

        if (m_selected_features.first.feature.has_value() || m_selected_features.second.feature.has_value()) {
          add_row_to_table(
            []() {
                ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, "Esc");
            },
            [this]() {
                ImGuiPureWrap::text_colored(ImGui::GetStyleColorVec4(ImGuiCol_Text), _u8L("Unselect"));
                ImGui::SameLine();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float rect_size = ImGui::GetTextLineHeight();
                const ColorRGBA color = m_selected_features.second.feature.has_value() ? SELECTED_2ND_COLOR : SELECTED_1ST_COLOR;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + rect_size, pos.y + rect_size), ImGuiPSWrap::to_ImU32(color));
                ImGui::Dummy(ImVec2(rect_size, rect_size));
            }
            );
          
          ++row_count;
        }

        // add dummy rows to keep dialog size fixed
        for (unsigned int i = row_count; i < 4; ++i) {
            add_strings_row_to_table(" ", ImGuiPureWrap::COL_ORANGE_LIGHT, " ", ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }

        ImGui::EndTable();
    }

    const bool use_inches = wxGetApp().app_config->get_bool("use_inches");
    const std::string units = use_inches ? " in" : " mm";

    ImGui::Separator();
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersH;
    if (ImGui::BeginTable("Selection", 2, flags)) {
        auto format_item_text = [this, use_inches, &units](const SelectedFeatures::Item& item) {
            if (!item.feature.has_value())
                return _u8L("None");

            std::string text = (item.source == item.feature) ? surface_feature_type_as_string(item.feature->get_type()) :
                item.is_center ? center_on_feature_type_as_string(item.source->get_type()) : point_on_feature_type_as_string(item.source->get_type(), m_hover_id);
            if (item.feature.has_value() && item.feature->get_type() == Measure::SurfaceFeatureType::Circle) {
                auto [center, radius, normal] = item.feature->get_circle();
                const Vec3d on_circle = center + radius * Measure::get_orthogonal(normal, true);
                radius = (on_circle - center).norm();
                if (use_inches)
                    radius = ObjectManipulation::mm_to_in * radius;
                text += " (" + _u8L("Diameter") + ": " + format_double(2.0 * radius) + units + ")";
            }
            else if (item.feature.has_value() && item.feature->get_type() == Measure::SurfaceFeatureType::Edge) {
                auto [start, end] = item.feature->get_edge();
                double length = (end - start).norm();
                if (use_inches)
                    length = ObjectManipulation::mm_to_in * length;
                text += " (" + _u8L("Length") + ": " + format_double(length) + units + ")";
            }
            return text;
        };

        add_strings_row_to_table(_u8L("Selection") + " 1:", ImGuiPSWrap::to_ImVec4(SELECTED_1ST_COLOR), format_item_text(m_selected_features.first),
            ImGuiPSWrap::to_ImVec4(SELECTED_1ST_COLOR));
        add_strings_row_to_table(_u8L("Selection") + " 2:", ImGuiPSWrap::to_ImVec4(SELECTED_2ND_COLOR), format_item_text(m_selected_features.second),
            ImGuiPSWrap::to_ImVec4(SELECTED_2ND_COLOR));
        ImGui::EndTable();
    }

    m_imgui->disabled_begin(!m_selected_features.first.feature.has_value());
        if (ImGuiPureWrap::button(_u8L("Restart selection"))) {
            m_selected_features.reset();
            m_selected_sphere_raycasters.clear();
            m_imgui->set_requires_extra_frame();
        }
    m_imgui->disabled_end();

    auto add_measure_row_to_table = [this](const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGuiPureWrap::text_colored(col_1_color, col_1);
        ImGui::TableSetColumnIndex(1);
        ImGuiPureWrap::text_colored(col_2_color, col_2);
        ImGui::TableSetColumnIndex(2);
        if (m_imgui->image_button(ImGui::ClipboardBtnIcon, _u8L("Copy to clipboard"))) {
            wxTheClipboard->Open();
            wxTheClipboard->SetData(new wxTextDataObject(col_1 + ": " + col_2));
            wxTheClipboard->Close();
        }
    };

    ImGui::Separator();
    ImGuiPureWrap::text(_u8L("Measure"));

    const unsigned int max_measure_row_count = 2;
    unsigned int measure_row_count = 0;
    if (ImGui::BeginTable("Measure", 4)) {
        if (m_selected_features.second.feature.has_value()) {
            const Measure::MeasurementResult& measure = m_measurement_result;
            if (measure.angle.has_value()) {
                ImGui::PushID("ClipboardAngle");
                add_measure_row_to_table(_u8L("Angle"), ImGuiPureWrap::COL_ORANGE_LIGHT, format_double(Geometry::rad2deg(measure.angle->angle)) + "°",
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }

            const bool show_strict = measure.distance_strict.has_value() &&
                (!measure.distance_infinite.has_value() || std::abs(measure.distance_strict->dist - measure.distance_infinite->dist) > EPSILON);

            if (measure.distance_infinite.has_value()) {
                double distance = measure.distance_infinite->dist;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceInfinite");
                add_measure_row_to_table(show_strict ? _u8L("Perpendicular distance") : _u8L("Distance"), ImGuiPureWrap::COL_ORANGE_LIGHT, format_double(distance) + units,
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (show_strict) {
                double distance = measure.distance_strict->dist;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceStrict");
                add_measure_row_to_table(_u8L("Direct distance"), ImGuiPureWrap::COL_ORANGE_LIGHT, format_double(distance) + units,
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
            if (measure.distance_xyz.has_value() && measure.distance_xyz->norm() > EPSILON) {
                Vec3d distance = *measure.distance_xyz;
                if (use_inches)
                    distance = ObjectManipulation::mm_to_in * distance;
                ImGui::PushID("ClipboardDistanceXYZ");
                add_measure_row_to_table(_u8L("Distance XYZ"), ImGuiPureWrap::COL_ORANGE_LIGHT, format_vec3(distance),
                    ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ++measure_row_count;
                ImGui::PopID();
            }
        }

        // add dummy rows to keep dialog size fixed
        for (unsigned int i = measure_row_count; i < max_measure_row_count; ++i) {
            add_strings_row_to_table(" ", ImGuiPureWrap::COL_ORANGE_LIGHT, " ", ImGui::GetStyleColorVec4(ImGuiCol_Text));
        }
        ImGui::EndTable();
    }

    if (last_feature != m_curr_feature || last_mode != m_mode || last_selected_features != m_selected_features) {
        // the dialog may have changed its size, ask for an extra frame to render it properly
        last_feature = m_curr_feature;
        last_mode = m_mode;
        last_selected_features = m_selected_features;
        m_imgui->set_requires_extra_frame();
    }

    ImGuiPureWrap::end();
}

void GLGizmoMeasure::on_register_raycasters_for_picking()
{
    // the features are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoMeasure::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_raycasters.clear();
    m_selected_sphere_raycasters.clear();
}

void GLGizmoMeasure::remove_selected_sphere_raycaster(int id)
{
    auto it = std::find_if(m_selected_sphere_raycasters.begin(), m_selected_sphere_raycasters.end(),
        [id](std::shared_ptr<SceneRaycasterItem> item) { return SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, item->get_id()) == id; });
    if (it != m_selected_sphere_raycasters.end())
        m_selected_sphere_raycasters.erase(it);
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, id);
}

void GLGizmoMeasure::update_measurement_result()
{
    if (!m_selected_features.first.feature.has_value())
        m_measurement_result = Measure::MeasurementResult();
    else if (m_selected_features.second.feature.has_value())
        m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, *m_selected_features.second.feature, m_measuring.get());
    else if (!m_selected_features.second.feature.has_value() && m_selected_features.first.feature->get_type() == Measure::SurfaceFeatureType::Circle)
        m_measurement_result = Measure::get_measurement(*m_selected_features.first.feature, Measure::SurfaceFeature(std::get<0>(m_selected_features.first.feature->get_circle())), m_measuring.get());
}

} // namespace GUI
} // namespace Slic3r
