# Lexara 1.12 - HD MSDF font renderer for Turtle WoW (twmoa_1171)

A port of the MSDF-atlas font renderer from the 3.3.5a client to the 1.12 client.

Original: [Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5](https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5)
Licence: GPL-3.0 (same as the original) - see [LICENSE](LICENSE).

## Screenshots

The `/lexara` panel - the same text at 9 sizes (8-72 px) - with a large piece of UI
text above it. Same client, same typeface, same settings; only `msdf_enabled`
differs between the two shots.

**Before** - the client's own renderer. Glyphs come from a fixed-size bitmap cache,
so anything scaled up is blurred and the edges fall apart.

![Before - the client's own font renderer](img/before.png)

**After** - the MSDF renderer. The glyph outline is kept in the atlas and resolved
in the pixel shader, so the edges stay sharp at every size.

![After - Lexara's MSDF renderer](img/after.png)

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

## Documentation

- [`docs/lexara.md`](docs/lexara.md) - everything worth knowing: status, usage,
  **how this port differs from the original**, the list of modified Lexara files,
  pitfalls, the eight bugs that only showed up in game.
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
