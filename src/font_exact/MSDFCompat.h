#pragma once
// Zgodnosc z msdfgen zbudowanym BEZ Skii.
//
// Lexara wola `msdfgen::resolveShapeGeometry`, ktore istnieje wylacznie
// w `ext/resolve-shape-geometry.cpp`, kompilowanym tylko przy
// MSDFGEN_USE_SKIA. Skia to setki megabajtow przez vcpkg, a port jej
// nie potrzebuje: sciezka Skii rozwiazuje SAMOPRZECIECIA konturow
// i konczy sie tak samo, jak przygotowanie bez niej.
//
// Zrodlo: msdfgen/ext/resolve-shape-geometry.cpp:127 konczy funkcje
// wywolaniem `shape.orientContours()`, a msdfgen/main.cpp:1155 uzywa
// dokladnie tego samego wywolania jako trybu WINDING_PREPROCESS
// i jako sciezki zastepczej pod `#else` do MSDFGEN_USE_SKIA.
//
// Skutek praktyczny: glify o zachodzacych na siebie konturach (czesc
// krojow ozdobnych i bardzo grubych) moga miec artefakt na przecieciu.
// Do sprawdzenia pomiarem na krojach uzywanych w profilu, nie zalozeniem.

#include <msdfgen.h>

namespace MSDFCompat {

    inline bool ResolveShapeGeometry(msdfgen::Shape& shape) {
        shape.orientContours();
        return true;
    }

} // namespace MSDFCompat
