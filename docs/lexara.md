# Lexara (HD MSDF) on 1.12 - everything worth knowing

A port of the MSDF-atlas font renderer from 3.3.5a to Turtle WoW 1.12.
Original: https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5

**Status as of 2026-09-09: WORKING in game.** Text is drawn through the MSDF atlas;
letters, digits, periods, hyphens and underscores all sit where they should.

## Where things live

| path | what it is |
|---|---|
| [`docs/lexara-port-map.md`](lexara-port-map.md) | **the full set of addresses and stages** - structure offsets, calling conventions, every finding taken from the binary |
| `build.bat` -> `build/out/Release/lexara112.dll` | building the project |
| `lexara112.dll` | the installed DLL in the client directory, injected by VanillaFixes (an entry in `dlls.txt`) |
| `lexara112.cfg` | patch switches - see below |
| `lexara112.log` | the DLL's log (sparse by default) |
| `Interface/AddOns/LexaraCompare/` | the `/lexara` addon - a comparison panel |
| the `LEXARA` directory in `%TEMP%` | cache of generated glyphs, safe to delete |

**Rollback:** remove `lexara112.dll` from `dlls.txt` (a copy is kept as
`dlls.txt.bak-before-lexara`). The DLL writes nothing into the client's files - it
patches process memory only.

## Usage

- **`lexara112.cfg`** - 24 switches, `name=0/1`, no file = everything enabled.
  10 patch sites (`site_*`), 6 font hooks (`hook_*`), `ft_hooks`, `shaders`,
  `msdf_enabled`. **A change needs only a game restart, not a DLL rebuild** -
  this is the first tool to reach for on any regression.
- **`CTRL+ALT+F11`** - writes `msdf_enabled` into the cfg; takes effect on the
  next start.
- **`/lexara`** (or `/lex`) - a panel with the same text at 9 sizes (8-72 px) and a
  full set of special characters; right-click changes the typeface.

## Architecture - how the port differs from the original

**The 1.12 client has no font shaders at all.** 3.3.5 (`006BE230`) creates a vertex
and a pixel shader object before it calls `InitFontIndexBuffer`; 1.12 (`005C17F0`)
starts with that call. `SetVertexShader` is not called even once in this binary,
and `RenderBatch` sets fixed-pipeline states exclusively. Lexara's entire `D3D.cpp`
layer (`IShaderCreate*` hooks, bytecode swapping) has no counterpart here - the
port compiles its own `vs_3_0`/`ps_3_0` and binds them itself.

**`ps_3_0` works under this client's DXVK** - measured with a separate probe, not
assumed: caps report SM 3.0/3.0, `fwidth` computes, FVF is enough in place of a
vertex declaration, and four 2048x2048 A8R8G8B8 atlases fit. The `ps_2_0` strings
in the client's `.data` (the `0085C608` table) are **dead code** - nothing in
`.text` touches them.

**The D3D device is captured through the shared virtual table.** We create our own
throwaway device on a hidden 8x8 window, swap `EndScene` (slot 42) in its vtable
and release our own device. Objects of the same C++ class share a vtable, so from
that moment on the client's device hands itself over in `this`.

## Seven bugs that only showed up in game

| symptom | the real cause |
|---|---|
| `0xC0000005` crash at `007CECA4` | the client called its OWN `FT_Add_Default_Modules` on Lexara's FreeType 2.14.1 library |
| `vs=0 ps=0` | shaders were compiled in `FreeType_InitHk`, i.e. before a device existed |
| no device despite `d3d9.dll` being caught | the client does not create it on the object we can see |
| `atlas: pages=0`, black rectangles | `ProcessGeometry` generated glyphs before the device was captured |
| letters disappearing | a failed upload reported "success", the entry kept `UV (0,0)` permanently |
| `.` `-` `_` with `y ~ 1.8e7` | the `GetGlyphYMetrics` stub clobbered EDX, which is live in 1.12 |
| the same characters against the top edge | a fix for the previous bug that outlived it |

## Pitfalls worth remembering beyond this project

- **`commit = 0` from Detours means only "no error", NOT "the hook works".** Three
  different ways of intercepting device creation installed cleanly and were never
  called once. The only probe is a log line from INSIDE the hook itself.
- **With an assembly stub, check the LIVE REGISTERS, not just the site length and
  the return address.** `GetGlyphYMetrics`: 3.3.5 runs the chain through EDX
  (`mov edx,[ecx+0x54]` / `mov ecx,[edx+0x68]`), 1.12 through ECX
  (`mov ecx,[ecx+0x54]` / `mov ecx,[ecx+0x68]`). A literal port clobbered a live,
  deliberately zeroed EDX. The length check passed all the same.
- **Two overlapping bugs.** The symptom of someone else's bug was treated; the fix
  did not help, but it STAYED in the code, and once the real cause was fixed the
  fix itself became a bug. Measure after each single change, and back out old
  "fixes".
- **A `static bool` probe guard hides the distribution.** The upload probe printed
  only the FIRST occurrence of each cause; the single entry it produced was a
  space, i.e. the least interesting case of all. Counters on every exit showed the
  truth immediately: 392 rejections, all from one branch.
- **The uninstrumented branch is the one that fails.** `CreateAtlasPage` was the
  only `return false` without a log line, and that is exactly where the bug sat.
- **A check with a known answer settles the question.** Putting the vanishing
  period (`y = 1.8e7`) side by side with a visible `m` (`y = -31..-12`) FROM THE
  SAME BATCH showed it was an anomaly, not a normal coordinate for this pipeline.
- **A log inside the draw loop becomes a problem in its own right.** A dump every
  200 calls, at 2.3 million `GetGlyph` calls per session, is 11,000 file opens.
- **The switch file paid for itself many times over.** Bisecting without a rebuild
  shortened a round from "rebuild and log in" to just logging in. The condition:
  the gates must cover the COMPLETE set of hooks in a layer - leaving
  `FT_New_Library` outside `ft_hooks` produced a state worse than either extreme
  (a client with a foreign library but its own functions) and a crash.

## Project dependencies

`third_party/` is fetched separately (Lexara does not bundle it): FreeType
`VER-2-14-1`, msdfgen (SVG and PNG **disabled** - they pull in tinyxml2/libpng),
Detours (with an added `CMakeLists.txt` and a `detours.h` forwarder),
unordered_dense.

**msdfgen without Skia has no `resolveShapeGeometry`** - replaced in `MSDFCompat.h`
with the same thing msdfgen itself uses in its place (`shape.orientContours()`,
`main.cpp:1155`). The price: glyphs with overlapping contours may show an artefact
at the intersection. Not observed, but not deliberately measured either.

**Toolchain:** neither the system nor VS provides CMake (Community 2022 comes
without the toolset - the toolset is in **BuildTools**). CMake comes from pip. Run
`.bat` files from PowerShell via `& cmd.exe /c '<full path>'`.

## What was left unfinished

- **Live renderer toggling.** `GetOrCreateGlyphEntry` (`005CABD0`) is a plain
  lookup in a hash table - it returns an entry WITHOUT inspecting its fields, so
  zeroing `m_cellIndexMin/Max` and `m_texturePageIndex` (which is what Lexara does)
  does NOT force the glyph to be regenerated. With the renderer enabled the client
  never rendered those glyphs itself, so there is nothing to restore. Live toggling
  requires REMOVING the entry from the client's table - to be done once a removal
  function turns up. Hence the switch only takes effect after a restart.
- `metrics.pixelData` points at the `ownedPixelData` of a LOCAL variable
  (`MSDFFont.cpp`) - harmless in the original, because the upload happens
  immediately, but any retry of the upload from the pool would read freed memory.
- Performance and memory measurements against `d3d9.textureMemory = 64` (the limit
  is the number of LIVE TEXTURE MAPPINGS, not the amount of memory itself).
- The quality of decorative typefaces without `resolveShapeGeometry` (see above).
