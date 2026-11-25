///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_TransparentFacePicking_hpp_
#define slic3r_TransparentFacePicking_hpp_

#include "MeshUtils.hpp"

namespace Slic3r::GUI {

struct TransparentFaceHit {
    Vec3f position;
    Vec3f normal;
    size_t facet;
    bool translucent;
};

// Prefer the nearest opaque face on either side. Use the nearest partially
// transparent back face only if no opaque face is available.
std::optional<TransparentFaceHit> pick_through_transparency(const MeshRaycaster& raycaster,
    const Vec2d& mouse, const Transform3d& world, const Camera& camera,
    const std::function<int(size_t)>& transparency, const ClippingPlane* section = nullptr,
    const ClippingPlane* compound_section = nullptr, bool reject_below_bed = false);

} // namespace Slic3r::GUI
#endif
