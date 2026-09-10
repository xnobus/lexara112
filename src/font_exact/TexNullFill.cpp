#include "TexNullFill.h"

#include "Hooks.h"
#include "MSDF.h"
#include "../Logger.h"

#include <Windows.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Crash 0x00448955: the fallback "fill with white" writes through a NULL pointer
// ---------------------------------------------------------------------------
//
// 0x00448920 is the loop over the mipmap chain of the global placeholder texture,
// which the engine fills with 0xFFFFFFFF when it failed to load a file. It takes
// the address of the table of pointers to each level's bits from the global at
// [0x00B05D14]. The table exists, but its entries are NULL - the placeholder buffer
// is never allocated in this client - so `rep stosd` runs against address zero:
//
//   00448936  cmp edx, 1              ; loop over mipmap levels
//   00448939  ja  448940
//   0044893B  cmp esi, 1
//   0044893E  jbe 448985              ; exit
//   00448940  mov edi, [ebp-4]        ; next table entry
//   00448943  mov edi, [edi]          ; <-- NULL
//   00448945  mov ecx, esi
//   00448947  imul ecx, edx
//   0044894A  shl  ecx, 2
//   0044894D  mov ebx, ecx            ; ebx = size in bytes
//   0044894F  shr  ecx, 2             ; ecx = size in dwords
//   00448952  or  eax, 0FFFFFFFFh     ; <-- START OF THE PATCH SITE (5 bytes)
//   00448955  rep stosd               ; <-- CRASH
//   00448957  mov ecx, ebx            ; <-- return here when edi != 0
//   00448959  and ecx, 3
//   0044895C  rep stosb               ; a second write, also through NULL
//   0044895E  mov edi, [ebp-4]        ; <-- return here when edi == 0
//   00448961  ...                     ; on to the next level
//
// The patch checks `edi` and, for a pointer that cannot be written to, skips BOTH
// writes at once - the second one (`rep stosb` at 0044895C) would fault just the
// same, so in that case the return goes straight to 0044895E. There is no way to
// do that with a single return address, hence two jmpbacks instead of one.
//
// "Cannot be written to" is a range, not just zero: on 2026-09-09 the entry turned
// out to be NULL once and 0xFFFFFFFF another time - details at the stub itself
// below.
//
// Skipping a level is safe: the loop recomputes ecx (0x448945) and ebx from scratch
// anyway, and takes edi again from [ebp-4] (0x44895E). No state is carried between
// iterations in the registers the patch leaves behind.
//
// Detours takes exactly 5 bytes: `83 C8 FF` + `F3 AB`. Both instructions are
// complete and the site is on an instruction boundary at both ends.
//
// The effect: the path that kills the process today ends in a missing texture
// (a white/blank square) instead of an ACCESS_VIOLATION. This does NOT fix the
// cause - a path to a file that does not exist - only the engine's error handling.

namespace {
    auto (*CGxTexture__FillMissing_site)() = reinterpret_cast<void (*)()>(0x00448952);
    constexpr uintptr_t CGxTexture__FillMissing_jmpback_fill = 0x00448957;
    constexpr uintptr_t CGxTexture__FillMissing_jmpback_skip = 0x0044895E;

    // A range, not just zero. The first version checked `test edi,edi` and that
    // was enough right up to 2026-09-09 21:39, when the client crashed inside THIS
    // very stub (Errors\2026-09-09 21.39.32 Crash.txt): the registers were the same
    // as always on this path, but EDI=FFFFFFFF instead of 0, so the guard let it
    // through and `rep stosd` ran against 0xFFFFFFFF. An entry in the placeholder
    // table can therefore be not only NULL but also the -1 sentinel.
    //
    // The lower bound 0x10000 is the first page of the process, which on Windows is
    // never mapped; the upper bound 0x7FFFFFFF cuts off -1 and half of kernel space.
    // Both are `cmp`, i.e. they touch only EFLAGS - the code at 00448957 reads ecx
    // and ebx, not the flags left by `or`, so the live registers stay untouched.
    __declspec(naked) void CGxTexture__FillMissing_siteHk() {
        __asm {
            or   eax, 0FFFFFFFFh;
            cmp  edi, 10000h;
            jb   no_buffer;
            cmp  edi, 7FFFFFFFh;
            ja   no_buffer;
            rep  stosd;
            jmp  CGxTexture__FillMissing_jmpback_fill;
        no_buffer:
            jmp  CGxTexture__FillMissing_jmpback_skip;
        }
    }
}

void TexNullFill::initialize() {
    if (!MSDF::CfgFlag("site_texnullfill")) {
        Log("[MSDF] site_texnullfill=0 - the 0x00448955 crash patch is skipped");
        return;
    }

    // A check with a known answer: if the patch site does not hold exactly these
    // bytes, this is not that client and nothing may be written there. Without it,
    // after a swap of WoW_Modernized.exe the patch would write a jump into the
    // middle of something else.
    const unsigned char expected[5] = { 0x83, 0xC8, 0xFF, 0xF3, 0xAB };
    const unsigned char* at = reinterpret_cast<const unsigned char*>(0x00448952);
    if (memcmp(at, expected, sizeof(expected)) != 0) {
        Log("[MSDF] site_texnullfill: the bytes at 00448952 are %02X %02X %02X %02X %02X,"
            " expected 83 C8 FF F3 AB - patch SKIPPED",
            at[0], at[1], at[2], at[3], at[4]);
        return;
    }

    // `commit = 0` from Detours means only "no error", not "the hook works" - the
    // proof that it works is the absence of a crash on the probe from
    // docs/tex-null-fill.md.
    Hooks::Detour(&CGxTexture__FillMissing_site, CGxTexture__FillMissing_siteHk);
    Log("[MSDF] site_texnullfill: the 0x00448955 crash patch is installed");
}
