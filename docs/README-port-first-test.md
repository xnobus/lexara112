# Lexara -> Turtle WoW 1.12 (twmoa_1171)

> **Historical document - frozen.** This is the README as it stood just before the
> port was first launched in game. It is not kept up to date: it describes the
> state before that test, and some of the files mentioned here (the `.bak-335`
> copies, the `dllmain.cpp` / `Proxy.cpp` proxy layer) no longer exist in the
> repository. What this text is worth is the list of questions that **could not be
> settled statically** - for the current state see [`../README.md`](../README.md)
> and [`lexara.md`](lexara.md).

A port of the MSDF font renderer from
https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5.
Address map and findings: [`lexara-port-map.md`](lexara-port-map.md).
A summary of everything known about the port: [`lexara.md`](lexara.md).

## Status: ready for the FIRST in-game test

`lexara112.dll` already has the complete set of 1.12 addresses. Post-build check:
**not a single 3.3.5 address is left in the binary** (`00C5DF88`, `00C7D2CC/D0`,
`00991320`, `00993370`, `00682CB0`, `006AA0D0` among others were checked), and all
the new ones are present.

That does not mean it will work first time - it means there is nothing left that
could be settled without running it. Four things to check by measurement, in this
order:

1. **Whether the client starts.** If it does not - most likely R1: the
   `ProcessBatch` site (`005C91A8`) is exactly 5 bytes long and is the TARGET of
   the `je 005C91A9` jump from `005C8FF3`. Taking that jump after the patch means
   jumping into the middle of an instruction.
2. **Whether any text is visible at all.** If the text disappears or lands
   off-screen, it is the matrix: `MSDF_WVP_TRANSPOSE` in
   `src/font_exact/MSDF.cpp`. Change it to `false` and rebuild. This is the one
   place in the port that could not be established statically.
3. **Whether the interface outside the text looks normal.** If the whole UI is
   painted with the font shader, then `UnbindMsdfShaders` is not catching every
   exit from a batch.
4. **Texture memory.** Four 2048x2048 atlases are 64 MB. The probe confirmed they
   fit, but the client has `d3d9.textureMemory = 64` and a history of crashes with
   a large number of LIVE MAPPINGS (not with the amount of memory).

**Rollback:** remove the entry from `dlls.txt`. The DLL writes nothing to disk and
does not modify the client's files - it patches process memory, so a restart
without the entry returns to the previous state.

## Building

```
build.bat
```

Requires VS 2022 **BuildTools** (Community on this machine has no toolset) and
CMake from pip (`python -m pip install --user cmake`) - the path is written into
`build.bat`. Result: `build\out\Release\lexara112.dll`.

## Loading (as intended)

3.3.5 loaded Lexara as a `dinput8.dll` proxy. Here the client has VanillaFixes, so
it is enough to copy the DLL into the client directory and add its name to
`dlls.txt`. Hence `src/dllmain112.cpp` instead of `dllmain.cpp` + `Proxy.cpp`.

A copy is already in the client directory as `lexara112.dll`. All that is missing
to enable it is the `lexara112.dll` line in `dlls.txt` - **deliberately not added**,
so that untested code does not run at the next start of the game.

## Dependencies

`third_party/` is fetched separately (Lexara does not bundle it):

| directory | source | notes |
|---|---|---|
| `freetype-2.14.1` | github.com/freetype/freetype, tag `VER-2-14-1` | the version Lexara uses |
| `msdfgen` | github.com/Chlumsky/msdfgen | SVG and PNG **disabled** (they pull in tinyxml2/libpng) |
| `Detours` | github.com/microsoft/Detours | added: `CMakeLists.txt` and a `detours.h` forwarder |
| `unordered_dense_src` | github.com/martinus/unordered_dense | header only |

**msdfgen without Skia** has no `resolveShapeGeometry`; it is replaced in
`src/font_exact/MSDFCompat.h` with the same thing msdfgen itself uses in its place
(`shape.orientContours()`, `main.cpp:1155`). Details in that file's header.

## Modified Lexara files

The originals sit alongside as `.bak-335`:

- `src/font_exact/GameClient.h` - 1.12 addresses and conventions, the `CGxString`
  layout, FreeType on `__fastcall`.
- `src/font_exact/MSDF.cpp` - ten patch sites, four `naked` stubs,
  `BindMsdfShaders`/`UnbindMsdfShaders`, FreeType hooks on `__fastcall`.
- `src/font_exact/D3D.cpp` - the device layer rewritten: instead of seven
  `CGxDevice` hooks, the chain `LoadLibrary -> Direct3DCreate9 -> CreateDevice`.
- `src/font_exact/MSDF.h` - font shader globals removed.
- `src/font_exact/MSDFValidator.h`, `MSDFFont.cpp` -
  `MSDFCompat::ResolveShapeGeometry`.

Every substantive change (not a mere address) carries a `[1.12]` comment in the
code.
