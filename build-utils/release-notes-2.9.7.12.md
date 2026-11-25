PrusaSlicer Measure & Section 2.9.7.12 adds three measurement workflows to the Windows FFF editor.

- **Perpendicular distance.** Lock a reference plane once and measure successive points. Hold Shift to snap a target to a visible mesh vertex.
- **Circle center distance.** Measure two circles from detected edges or three points. The viewport shows the 3D distance, signed X, Y and Z offsets, and the angle between the circle normals. Reselect the second circle to compare more holes against the first.
- **Overhang slope.** Hover over a downward-facing model surface to read its slope beside the pointer. The angle is measured from the horizontal build plate.

Circle and perpendicular measurement accept a click to pick and a drag to rotate the view. Three-point circle modes enable vertex snapping by default. Pick-through transparency can reach opaque bottom faces and select a partially transparent back face when the ray has no opaque target.

Windows x64 downloads include an installer, a portable ZIP, and the matching source ZIP. The installer supports an in-place upgrade from 2.9.7.11.
