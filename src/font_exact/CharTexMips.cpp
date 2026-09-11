#include "CharTexMips.h"

#include "Hooks.h"
#include "MSDF.h"
#include "../Logger.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Crash 0x0047801D: a component texture with more mips than the skin canvas
// ---------------------------------------------------------------------------
//
// The client composites a character's skin on the CPU: every body region
// (arms, torso, legs, the guild tabard...) is pasted into ONE 256x256 RGB565
// canvas, allocated once at 0x00476183 into [0x00B422D8] as a table of 9 level
// pointers followed directly by the pixels. 0x00477460 picks one of three paste
// loops by blend mode:
//
//   004774B2  call 00477C80            ; mode 0 - plain copy
//   004774D6  call 00477DB0            ; mode 1
//   004774E1  call 00477F20            ; mode 2 - 2-bit alpha (the crash)
//
// All three walk the mip chain with the same bound - the SOURCE's level count
// (info->mipCount, the 6th argument), never the canvas's:
//
//   00477F40  mov  dword ptr [ebp-8], 200h   ; canvas pitch, level 0 = 256 px
//   ...
//   00478139  mov  esi, [ecx+0Ch]            ; info->mipCount
//   0047813E  inc  eax
//   00478142  cmp  eax, esi
//   0047814D  jb   00477F6F                  ; next level
//
// No vanilla component is larger than the canvas (the biggest is the 256x256 base
// skin; a region like the upper torso is 128x64), so none has more than 9 levels.
// A 512x256 one has 10: on level 9 the loop reads canvas_table[9],
// which is the first 4 bytes of the canvas's level 0 - two skin pixels - and
// writes through them. Nobus, 2026-09-11 21:32:58: the source was
// Textures\GuildEmblems\Background_45_TU_U at 512x256, EAX=AD54AD54 (two AD54
// skin-coloured pixels). Whether that write crashes depends only on whether the
// "address" happens to be mapped in the large-address-aware process, which is why
// it crashed with lexara112.dll loaded and silently corrupted memory without it.
//
// The patch hooks the dispatcher and caps info->mipCount to the canvas's level
// count before any of the three loops starts. `info` is a stack copy the callers
// (0x00477070, 0x004770F0) fill through 0x0047B150 and drop right after the call,
// so the texture's own cached description is not touched.
//
// Levels 0-8 are pasted exactly as before; only the out-of-range level is
// skipped. The oversized texture still lands cropped to its 128x64 region, as it
// always did - that part is the texture pack's problem, not the client's.

namespace {
    // The skin canvas: 256x256, so 9 mip levels (256 .. 1). Valid only while the
    // three paste loops still start from a 0x200-byte pitch - checked in
    // initialize() before the hook is installed.
    constexpr uint32_t CANVAS_LEVELS = 9;

    // The 6th argument of 0x00477460, filled by 0x0047B150 (a 6-dword copy of the
    // texture's description).
    struct CharTexInfo {
        uint32_t width;
        uint32_t height;
        uint32_t unk08;
        uint32_t mipCount;  // the bound of the level loop in all three paste loops
        uint32_t unk10;
        uint32_t mode;      // also passed separately as the 2nd argument
    };

    using PasteComponent_t = int(__thiscall*)(void* pThis, void* srcMips, uint32_t mode,
        void* dstRect, void* srcRect, void* size, CharTexInfo* info);
    auto CharComponent__PasteComponent = reinterpret_cast<PasteComponent_t>(0x00477460);

    int s_logged = 0;

    int __fastcall CharComponent__PasteComponentHk(void* pThis, void* /*edx*/, void* srcMips,
        uint32_t mode, void* dstRect, void* srcRect, void* size, CharTexInfo* info) {
        if (info && info->mipCount > CANVAS_LEVELS) {
            if (s_logged < 5) {
                ++s_logged;
                Log("[MSDF] site_chartexmips: component %ux%u has %u mip levels, the skin canvas has %u"
                    " - capped (mode %u). The texture comes from a patch, not the vanilla client",
                    info->width, info->height, info->mipCount, CANVAS_LEVELS, mode);
            }
            info->mipCount = CANVAS_LEVELS;
        }
        return CharComponent__PasteComponent(pThis, srcMips, mode, dstRect, srcRect, size, info);
    }

    struct ExpectedBytes {
        uintptr_t at;
        unsigned char bytes[7];
        size_t len;
        const char* what;
    };

    // Everything the fixed CANVAS_LEVELS rests on. A mismatch means another
    // client build, or another DLL (e.g. a skin-resolution patch) got here first -
    // in both cases the cap would be wrong, so nothing is installed.
    const ExpectedBytes kExpected[] = {
        { 0x00477460, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 }, 6, "dispatcher prologue (the Detours site)" },
        { 0x004774B2, { 0xE8, 0xC9, 0x07, 0x00, 0x00 }, 5, "call 00477C80" },
        { 0x004774D6, { 0xE8, 0xD5, 0x08, 0x00, 0x00 }, 5, "call 00477DB0" },
        { 0x004774E1, { 0xE8, 0x3A, 0x0A, 0x00, 0x00 }, 5, "call 00477F20" },
        { 0x00477C9E, { 0xC7, 0x45, 0xF8, 0x00, 0x02, 0x00, 0x00 }, 7, "00477C80 canvas pitch 200h" },
        { 0x00477DCE, { 0xC7, 0x45, 0xF8, 0x00, 0x02, 0x00, 0x00 }, 7, "00477DB0 canvas pitch 200h" },
        { 0x00477F40, { 0xC7, 0x45, 0xF8, 0x00, 0x02, 0x00, 0x00 }, 7, "00477F20 canvas pitch 200h" },
        { 0x00477D90, { 0x8B, 0x7A, 0x0C }, 3, "00477C80 loop bound info->mipCount" },
        { 0x00477EFC, { 0x8B, 0x51, 0x0C }, 3, "00477DB0 loop bound info->mipCount" },
        { 0x00478139, { 0x8B, 0x71, 0x0C }, 3, "00477F20 loop bound info->mipCount" },
    };
}

void CharTexMips::initialize() {
    if (!MSDF::CfgFlag("site_chartexmips")) {
        Log("[MSDF] site_chartexmips=0 - the 0x0047801D crash patch is skipped");
        return;
    }

    for (const ExpectedBytes& e : kExpected) {
        const unsigned char* at = reinterpret_cast<const unsigned char*>(e.at);
        if (memcmp(at, e.bytes, e.len) != 0) {
            Log("[MSDF] site_chartexmips: unexpected bytes at %08X (%s) - patch SKIPPED",
                static_cast<unsigned>(e.at), e.what);
            return;
        }
    }

    // `commit = 0` from Detours means only "no error", not "the hook works" - the
    // proof is the "capped" line from inside the hook on a character that wears
    // an oversized component (docs/char-tex-mips.md).
    Hooks::Detour(&CharComponent__PasteComponent, CharComponent__PasteComponentHk);
    Log("[MSDF] site_chartexmips: the 0x0047801D crash patch is installed");
}
