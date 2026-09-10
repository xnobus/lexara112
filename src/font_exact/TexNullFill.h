#pragma once

// A patch for the 1.12 client crash on a texture that cannot be loaded.
// Details and disassembly: docs/tex-null-fill.md.
namespace TexNullFill {
    // Called from Entry.cpp, INSIDE an open Detours transaction and AFTER
    // MSDF::initialize() (which is what loads lexara112.cfg). The patch is
    // independent of msdf_enabled - it concerns textures, not fonts.
    void initialize();
}
