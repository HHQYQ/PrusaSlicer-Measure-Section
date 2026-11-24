///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GLGizmoTransparency_hpp_
#define slic3r_GLGizmoTransparency_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/ViewTransparency.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include <optional>

namespace Slic3r {
class GLVolume;
class ModelInstance;
class ModelObject;
class ModelVolume;
class TriangleMesh;
namespace MeshEdgeFinish { struct Analysis; }
namespace GUI {

class GLGizmoTransparency : public GLGizmoBase
{
public:
    explicit GLGizmoTransparency(GLCanvas3D &canvas);
    bool on_mouse(const wxMouseEvent &event) override;
    void data_changed(bool) override;
    bool picking_faces() const { return m_mode == Mode::Face; }

private:
    enum class Mode { Whole, Body, Face };
    struct Target { ModelObject *object; ModelInstance *instance; int object_index, instance_index; };
    struct BodyPick {
        size_t volume;
        int shell = -1;
        bool operator==(const BodyPick &o) const { return volume == o.volume && shell == o.shell; }
    };
    struct FacePick {
        size_t volume;
        std::shared_ptr<const TriangleMesh> mesh;
        std::vector<size_t> facets;
    };
    struct MeshCache {
        std::shared_ptr<const TriangleMesh> mesh;
        std::unique_ptr<ViewTransparency::Regions> regions;
        std::optional<std::vector<std::vector<size_t>>> solids;
        std::shared_ptr<const MeshEdgeFinish::Analysis> holes;
        bool checked_holes = false;
        uint64_t signature = 0;
    };

    bool on_init() override { return true; }
    bool on_is_selectable() const override;
    bool on_is_activable() const override;
    std::string on_get_name() const override;
    void on_set_state() override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::optional<Target> target() const;
    void sync_selection();
    MeshCache &mesh_cache(const ModelVolume &volume);
    void pick_face(const Vec2d &mouse);
    void set_percent(const Target &target, ViewTransparency::Percent percent);
    void build_outlines(const Target &target);

    Mode m_mode = Mode::Whole;
    std::pair<size_t, size_t> m_target_id{};
    std::vector<BodyPick> m_native_bodies, m_bodies;
    std::vector<FacePick> m_faces;
    std::optional<FacePick> m_hover;
    std::optional<Vec2d> m_down;
    std::map<size_t, MeshCache> m_mesh_cache;
    bool m_through = false, m_hole_wall = false;
    bool m_slider_snapshot = false, m_outline_dirty = true;
    GLModel m_selected_outline, m_hover_outline;
};

} // namespace GUI
} // namespace Slic3r
#endif
