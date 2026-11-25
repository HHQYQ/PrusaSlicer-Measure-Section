///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "libslic3r/Measure.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::Measure;

namespace {
void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

bool close(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-7;
}
}

int main()
{
    try {
        const auto reference = circle_from_three_points({5, 0, 0}, {0, 5, 0}, {-5, 0, 0});
        const auto current = circle_from_three_points({30, 0, 3.2}, {24, -6, 3.2}, {18, 0, 3.2});
        require(reference && current, "fit both circular rims");
        const CircleComparison comparison = compare_circles(*reference, *current);
        require(comparison.delta.isApprox(Vec3d(24, 0, 3.2)) &&
            close(comparison.center_distance, std::hypot(24., 3.2)) && close(comparison.radial_offset, 24.) &&
            close(comparison.axial_offset, 3.2) && close(comparison.diameter_difference, 2.) &&
            close(comparison.normal_angle, 0.), "distinguish 3D and plane pitch with opposing circle normals");
        const auto swapped = compare_circles(*current, *reference);
        require(swapped.delta.isApprox(-comparison.delta) && close(swapped.diameter_difference, -2.) &&
            close(swapped.center_distance, comparison.center_distance), "swapping changes signed quantities");

        const double tilt = PI / 45.;
        const SurfaceFeature tilted(SurfaceFeatureType::Circle, {0, 0, 8},
            {std::sin(tilt), 0, std::cos(tilt)}, std::nullopt, 5.);
        const auto tilted_result = compare_circles(*reference, tilted);
        require(close(tilted_result.radial_offset, 0.) && close(tilted_result.normal_angle, tilt),
            "zero center offset must not hide a tilted circle");

        const double sample_tilt = PI / 9.;
        const Vec3d sample_center(28., 4., 2.);
        const Vec3d rim_x(6. * std::cos(sample_tilt), 0., -6. * std::sin(sample_tilt));
        const Vec3d rim_y(0., 6., 0.);
        const auto sample = circle_from_three_points(sample_center + rim_x, sample_center + rim_y, sample_center - rim_x);
        require(sample.has_value(), "fit the tilted sample from points on its rim");
        const auto sample_result = compare_circles(*reference, *sample);
        const auto sample_reversed = compare_circles(*sample, *reference);
        require(sample_result.delta.isApprox(sample_center) && sample_reversed.delta.isApprox(-sample_center) &&
            close(sample_result.center_distance, std::sqrt(804.)) &&
            close(sample_result.normal_angle, sample_tilt) && close(sample_reversed.normal_angle, sample_tilt),
            "separate signed world-axis dimensions from the winding-independent normal angle");
        require(!circle_from_three_points({0, 0, 0}, {1, 0, 0}, {2, 0, 0}), "reject collinear picks");

        indexed_triangle_set patch;
        patch.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
        patch.indices = {{0, 1, 2}, {0, 2, 3}};
        const Measuring measuring(patch, true);
        const auto plane = measuring.get_plane(1);
        require(plane && measuring.get_plane_triangle_indices(std::get<0>(plane->get_plane())).size() == 2 &&
            !measuring.get_plane(2), "reuse the full coplanar patch");
        const auto measure = [&](const Vec3d& point) {
            return get_measurement(SurfaceFeature(point), *plane).distance_infinite.value();
        };
        const auto first = measure({5, 6, 3.2});
        const auto second = measure({5, 8, -4.});
        require(close(first.dist, 3.2) && first.to.isApprox(Vec3d(5, 6, 0)) &&
            close(second.dist, 4.), "reuse the same datum, including when the projection lands outside the face");

        require(close(overhang_slope({0, 0, -1}).value(), 0.) &&
            close(overhang_slope({2, 0, -2}).value(), PI / 4.) &&
            close(overhang_slope({1, 0, -0.00001}).value(), std::atan2(1., 0.00001)) &&
            !overhang_slope(Vec3d::UnitX()) && !overhang_slope(Vec3d::UnitZ()) &&
            !overhang_slope(Vec3d::Zero()), "use support settings' slope convention on downward facets");
        TriangleMesh transformed(patch);
        Transform3d pose = Transform3d::Identity();
        pose.linear() = Eigen::AngleAxisd(PI * 0.75, Vec3d::UnitY()).toRotationMatrix() *
            Vec3d(-2., 3., 0.5).asDiagonal();
        transformed.transform(pose, true);
        const auto& vertices = transformed.its.vertices;
        const Vec3i& face = transformed.its.indices.front();
        const Vec3d world_normal = (vertices[face[1]] - vertices[face[0]]).cast<double>().cross(
            (vertices[face[2]] - vertices[face[0]]).cast<double>());
        require(close(overhang_slope(world_normal).value(), PI / 4.), "derive the slope in world space with mirrored winding");

        std::cout << "Measurement reference and slope checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
