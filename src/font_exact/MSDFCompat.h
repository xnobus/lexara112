#pragma once
// Compatibility with an msdfgen built WITHOUT Skia.
//
// Lexara calls `msdfgen::resolveShapeGeometry`, which exists only in
// `ext/resolve-shape-geometry.cpp` and is compiled only when MSDFGEN_USE_SKIA is
// set. Skia is hundreds of megabytes through vcpkg, and the port does not need it:
// the Skia path resolves SELF-INTERSECTIONS of contours and ends the same way the
// preparation without it does.
//
// Source: msdfgen/ext/resolve-shape-geometry.cpp:127 ends the function with a call
// to `shape.orientContours()`, and msdfgen/main.cpp:1155 uses exactly that same
// call both as the WINDING_PREPROCESS mode and as the fallback under the `#else`
// to MSDFGEN_USE_SKIA.
//
// Practical consequence: glyphs with overlapping contours (some decorative and
// very heavy typefaces) may show an artefact at the intersection. To be checked by
// measurement on the typefaces actually used, not assumed.

#include <msdfgen.h>

namespace MSDFCompat {

    inline bool ResolveShapeGeometry(msdfgen::Shape& shape) {
        shape.orientContours();
        return true;
    }

} // namespace MSDFCompat
