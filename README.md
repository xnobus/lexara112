# Lexara 1.12 - HD MSDF font renderer for Turtle WoW (twmoa_1171)

A port of the MSDF-atlas font renderer from the 3.3.5a client to the 1.12 client.

Original: [Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5](https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5)
Licence: GPL-3.0 (same as the original) - see [LICENSE](LICENSE).

**Status: working in game since 2026-09-09.** Text is drawn through the MSDF atlas.

## How this port differs from the original

Three things to know before touching anything here:

1. **The 1.12 client has no font shaders at all.** 3.3.5 (`006BE230`) creates a
   vertex and a pixel shader object before calling `InitFontIndexBuffer`; 1.12
   (`005C17F0`) starts with that call. `SetVertexShader` is not called even once in
   this binary, and `RenderBatch` sets fixed-pipeline states exclusively. Lexara's
   entire `D3D.cpp` layer (`IShaderCreate*` hooks, bytecode swapping) has no
   counterpart here - the port compiles its own `vs_3_0`/`ps_3_0` and binds them
   itself.
2. **The D3D device is captured through the shared virtual table.** We create our
   own throwaway device on a hidden 8x8 window, swap `EndScene` (slot 42) in its
   vtable and release our own. Objects of the same C++ class share a vtable, so
   from that moment on the client's device hands itself over in `this`.
3. **Loading goes through VanillaFixes, not a `dinput8.dll` proxy.** The 1.12
   client injects the DLLs listed in `dlls.txt`, so the whole proxy layer
   (`dllmain.cpp`, `Proxy.cpp`, `dinput8_exports.def` in the original) is
   unnecessary here - `src/dllmain112.cpp` alone replaces it.

Every substantive change (not a mere address) carries a `[1.12]` comment in the
code.

## Installation

1. Build it (`build.bat`) or take a prebuilt `lexara112.dll`.
2. Copy `lexara112.dll` into the client directory.
3. Add a `lexara112.dll` line to `dlls.txt` next to `WoW.exe`.
4. Optional: `lexara112.cfg.example` -> `lexara112.cfg` in the client directory.
5. Optional: `addon/LexaraCompare` -> `Interface/AddOns/LexaraCompare`.

**Rollback:** remove the line from `dlls.txt`. The DLL writes nothing into the
client's files - it patches process memory, so a restart without the entry returns
to the previous state.

## Usage

- **`lexara112.cfg`** - 24 switches of the form `name=0/1`; no file = everything
  enabled. 10 patch sites (`site_*`), 6 font hooks (`hook_*`), `ft_hooks`,
  `shaders`, `msdf_enabled`. **A change needs only a game restart, not a DLL
  rebuild** - this is the first tool to reach for on any regression.
- **`CTRL+ALT+F11`** - writes `msdf_enabled` into the cfg; takes effect on the next
  start.
- **`/lexara`** (or `/lex`) - a panel with the same text at 9 sizes (8-72 px) and a
  full set of special characters; right-click changes the typeface.
- **`lexara112.log`** - the DLL's log, sparse by default.
- Glyph cache: the `LEXARA` directory in `%TEMP%`, safe to delete.

## Building

```
build.bat
```

Requires **VS 2022 BuildTools** (the x86 toolset; Community without the toolset is
not enough) and CMake. `build.bat` takes `cmake` from PATH, and when it is not
there, from the `LEXARA_CMAKE` variable. Result:
`build/out/Release/lexara112.dll`.

## Dependencies

`third_party/` is vendored in this repository (the original Lexara does not bundle
it):

| directory | source | notes |
|---|---|---|
| `freetype-2.14.1` | github.com/freetype/freetype, tag `VER-2-14-1` | the version Lexara uses |
| `msdfgen` | github.com/Chlumsky/msdfgen | SVG and PNG **disabled** (they pull in tinyxml2/libpng) |
| `Detours` | github.com/microsoft/Detours | added: `CMakeLists.txt` and a `detours.h` forwarder |
| `unordered_dense_src` | github.com/martinus/unordered_dense | header only |

**msdfgen without Skia** has no `resolveShapeGeometry`; it is replaced in
`src/font_exact/MSDFCompat.h` with the same thing msdfgen itself uses in its place
(`shape.orientContours()`, `main.cpp:1155`).

## Modified Lexara files

The starting versions of these files are in the original's repository
([Stormhand-dev/Lexara](https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5));
the full diff of the port lives in the git history.

- `src/font_exact/GameClient.h` - 1.12 addresses and conventions, the `CGxString`
  layout, FreeType on `__fastcall`.
- `src/font_exact/MSDF.cpp` - ten patch sites, four `naked` stubs,
  `BindMsdfShaders`/`UnbindMsdfShaders`, FreeType hooks on `__fastcall`.
- `src/font_exact/D3D.cpp` - the device layer rewritten: instead of seven
  `CGxDevice` hooks, the chain `LoadLibrary -> Direct3DCreate9 -> CreateDevice`.
- `src/font_exact/MSDF.h` - font shader globals removed.
- `src/font_exact/MSDFValidator.h`, `MSDFFont.cpp` -
  `MSDFCompat::ResolveShapeGeometry`.

## Documentation

- [`docs/lexara.md`](docs/lexara.md) - everything worth knowing: status, usage,
  pitfalls, the seven bugs that only showed up in game.
- [`docs/lexara-port-map.md`](docs/lexara-port-map.md) - **1.12 addresses,
  structure layouts, calling conventions, the course of stages 1-5.** Start here
  for any work on hooking the client.
- [`docs/README-port-first-test.md`](docs/README-port-first-test.md) - the README
  as it stood just before the first launch; historical, but it holds the list of
  things that could not be settled statically.

## Reporting bugs

Please include:

1. **`lexara112.log`** - in full, it is short.
2. **`lexara112.cfg`** - or a note that the file is absent.
3. **The client version** and the list from `dlls.txt` (the order matters).
4. **The result of bisecting through the cfg**: disable `msdf_enabled`, then
   `shaders`, then `ft_hooks`, then the `site_*` entries in groups. Which switch
   removes the symptom is half the diagnosis and needs no DLL rebuild.
5. On a crash: the contents of the client's `Errors/` directory and the DXVK log
   (`*_d3d9.log`).

## Pitfalls (short version)

- **`commit = 0` from Detours means "no error", NOT "the hook works".** The only
  probe for whether a hook works is a log line from inside it.
- **With an assembly stub, check the LIVE REGISTERS**, not just the site length and
  the return address - 3.3.5 and 1.12 can run the same chain through a different
  register, and a length check will not catch that.
- The `ProcessBatch` site (`005C91A8`) is exactly 5 bytes long and is the TARGET of
  the `je 005C91A9` jump from `005C8FF3` - taking that jump after the patch means
  jumping into the middle of an instruction.
