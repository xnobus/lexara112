#pragma once

// A patch for the 1.12 client's out-of-bounds write when a character texture
// component has more mip levels than the 256x256 skin canvas it is pasted into.
// Details and disassembly: docs/char-tex-mips.md.
namespace CharTexMips {
    // Called from Entry.cpp, INSIDE an open Detours transaction and AFTER
    // MSDF::initialize() (which is what loads lexara112.cfg). Independent of
    // msdf_enabled - it concerns character textures, not fonts.
    void initialize();
}
