///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GLGizmoEdgeFinish_hpp_
#define slic3r_GLGizmoEdgeFinish_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/MeshEdgeFinish.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include <atomic>
#include <future>

namespace Slic3r {
class TriangleMesh;
class GLVolume;
namespace GUI {

class GLGizmoEdgeFinish : public GLGizmoBase
{
public:
    explicit GLGizmoEdgeFinish(GLCanvas3D &canvas);
    ~GLGizmoEdgeFinish() override;
    bool on_mouse(const wxMouseEvent &event) override;
    void data_changed(bool serializing) override;
    bool escape();
    bool replaces_volume(const GLVolume &volume) const;

private:
    enum class PickMode { Direct, Straight, Rim };
    struct Target {
        std::pair<size_t, size_t> id;
        std::shared_ptr<const TriangleMesh> mesh;
        Transform3d pose = Transform3d::Identity();
        bool operator==(const Target &other) const
        {
            return id == other.id && mesh == other.mesh && pose.matrix() == other.pose.matrix();
        }
    };
    struct Pick {
        size_t edge;
        MeshEdgeFinish::Vec3 point;
        double pixels;
    };
    struct Work {
        std::shared_ptr<const MeshEdgeFinish::Analysis> analysis;
        std::shared_ptr<const TriangleMesh> mesh, hull;
        std::array<GLModel::Geometry, 2> preview_surfaces;
        MeshEdgeFinish::Error error = MeshEdgeFinish::Error::None;
        size_t edge = 0;
        std::optional<size_t> other_edge;
        bool thin_wall = false;
        double dz = 0.;
    };

    bool on_init() override { return true; }
    bool on_is_selectable() const override;
    bool on_is_activable() const override;
    std::string on_get_name() const override;
    void on_set_state() override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;

    std::optional<Target> selected_target() const;
    void invalidate_preview();
    void sync_target();
    void poll();
    void start_preview();
    void apply();
    void close();
    std::vector<Pick> pick(const Vec2d &mouse) const;
    void accept_pick(std::vector<Pick> picks);
    void update_lines(GLModel &model, const std::vector<size_t> &ids);

    PickMode m_mode = PickMode::Direct;
    MeshEdgeFinish::Operation m_operation = MeshEdgeFinish::Operation::Chamfer;
    double m_size = 1.;
    std::optional<Target> m_target;
    std::shared_ptr<const MeshEdgeFinish::Analysis> m_analysis;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::future<Work> m_worker;
    Work m_preview;
    bool m_analysis_failed = false;
    bool m_show_preview = false;
    bool m_lines_dirty = true;
    std::optional<Vec2d> m_down;
    std::vector<size_t> m_selected;
    std::vector<std::vector<Pick>> m_anchors;
    std::optional<size_t> m_hover_edge, m_error_edge, m_related_edge;
    std::string m_status;
    GLModel m_selection_lines, m_hover_lines, m_preview_model, m_preview_cut;
};

} // namespace GUI
} // namespace Slic3r
#endif
