# Lexara (3.3.5) -> Turtle WoW 1.12 : site map (stage 1)

Source: `lexara/src/font_exact/MSDF.cpp`, `GameClient.h`.
Binaries compared while drawing up this map: the 3.3.5a client's `Wow.exe`
(build 12340) and the Turtle WoW `twmoa_1171` client's `WoW.exe`.

Function boundaries were taken from the index of CALL targets (1.12 does not pad
functions with int3, so looking for the padding gives nothing but junk there).

## Functions

| role | 3.3.5 | 1.12 | notes |
|---|---|---|---|
| InitFontIndexBuffer | 006C47B0 | 005C92F0 | same code, same 0x1800/0xC00 constants |
| AllocateFontIndexBuffer | 006C47F0 | 005C9330 | loop counter in EDI, not EBX |
| bufalloc (grow stream) | 006C48D0 | 005C8F40 | arguments in ECX/EDX instead of on the stack |
| NativeFontRender | 006C4AD0 | 005C8FE0 | 171 vs 154 instructions, same structure |
| GetGlyphYMetrics | 006C8C60 | 005D1360 | same code, older compilation |
| CheckGeometry | 006C7480 | 005CD6A0 | from the call inside the loop |
| WriteGeometry | 006C5E90 | 005CE0C0 | from the call inside the loop |
| (006C63E0) | 006C63E0 | 005CE090 | |
| (006C9D50) init batch | 006C9D50 | 005CFC50 | |
| BufStream | 00684850 | 0058A140 | a free function, not a CGxDevice method |
| BufLock (vtable +0xD8) | method | 0058A080 | |
| BufUnlock (vtable +0xDC) | method | 0058A0A0 | |
| PoolCreate (VB) | 006876D0 | 0058A160 | |
| PoolCreate (IB) | 00687660 | 00589F80 | |
| SetTexture | 00685F50 | 00589E80 | **slot constant 0x15 -> 0x17** |
| HandleToPtr | 004B6CB0 | **absent** | 1.12 keeps the texture pointer directly |
| InitializeTextLine | 006C6CD0 | **not found** | the hook only wraps it (prefetch), optional |
| GetFontFace | 006C8080 | trivial | `return p ? *(void**)(p+0x24) : 0`, offset to be confirmed |
| global IB/VB | 00C7D2DC / 00C7D2E0 | 00C2B9D4 / 00C2B9D8 | |
| CGxDevice* | 00C5DF88 | **absent** | 1.12 does not load a device; the functions are free |

## The ten sites to patch

| # | name in Lexara | 3.3.5 site -> jmpback | 1.12 site -> jmpback | bytes 335/112 | verdict |
|---|---|---|---|---|---|
| 1 | InitFontIndexBuffer_site | 006C47BD -> 006C47D8 | 005C92F7 -> 005C930F | 27 / 24 | fits |
| 2 | AllocateFontIndexBuffer_site | 006C480C -> 006C4811 | 005C933F -> 005C9344 | 5 / 5 | fits, `mov edi,3FFFh` |
| 3 | CheckGeometry_site | 006C4AF3 -> 006C4B00 | **005C9001** -> 005C9010 | 13 / 15 | start moved from 005C9003, see risk R4 |
| 4 | CheckGeometry_call | 006C4B09 -> 006C4B10 | 005C9019 -> 005C9020 | 7 / 7 | byte for byte |
| 5 | BufStream_site | 006C4B40 -> 006C4B45 | 005C904A -> 005C904F | 5 / 5 | fits |
| 6 | bufalloc_1_site | 006C4B64 -> 006C4B70 | 005C9067 -> 005C9073 | 12 / 12 | fits, 0xB4 -> 0xA0 |
| 7 | bufalloc_3_site | 006C4C36 -> 006C4C4B | 005C9129 -> 005C913D | 21 / 20 | fits |
| 8 | bufalloc_2_site | 006C4C67 -> 006C4C8A | 005C915D -> 005C917B | 35 / 30 | fits |
| 9 | IGxuFontProcessBatch_site | 006C4CC4 -> 006C4CC9 | 005C91A8 -> 005C91AD | 5 / 5 | fits, see risk R1 |
| 10 | GetGlyphYMetrics_site | 006C8C71 -> 006C8C77 | 005D137A -> 005D1380 | 6 / 6 | fits, EDX -> ECX |

## Differences to account for

- **ABI.** In 3.3.5 these are `__thiscall` CGxDevice methods (ECX from the
  00C5DF88 global, the rest on the stack). In 1.12 they are free functions using a
  register convention (ECX/EDX + stack). Every `naked` stub has to be rewritten -
  a mechanical change, but each stub differs.
- **The texture batch array.** The 0x18C base is the same, but the loop range
  0xB4..0xD4 (3.3.5) corresponds to 0xA0..0xC0 (1.12). Both hold eight 4-byte slots.
- **The texture slot** in SetTexture: 0x15 -> 0x17.
- **No HandleToPtr** (004B6CB0). In 3.3.5 a texture handle is resolved by a call;
  in 1.12 it is read straight from the table. No Lexara hook touches this, but any
  code in the port that reads a texture has to take it into account.
- **Shaders.** Lexara compiles its own `ps_3_0` / `vs_3_0` through D3DX and sets
  them on the device. The 1.12 client uses `ps_2_0` internally, but that is not a
  hardware limit - SM 3.0 is available under DXVK. To be confirmed by measurement
  in stage 2.
- **The three whole-function hooks** (CheckGeometry, WriteGeometry,
  InitializeTextLine) **call the original** and merely add work around it. There is
  no need to reimplement InitializeTextLine's 627 instructions - the single most
  important piece of good news for the port's feasibility.

## Risks

- **R1.** Site 9 is exactly 5 bytes long, and `005C91A9` (the second byte of the
  patch) is the target of a `je` jump from `005C8FF3` - the early exit taken when
  the index buffer is not initialised. Taking that jump after the patch = jumping
  into the middle of an instruction. The same is true in 3.3.5 (`je 006C4CC5`,
  site 006C4CC4), so Lexara already lives with it: its Init hook guarantees the
  global pointer is never null. Port it unchanged, but know that it is a mine.
- **R2.** The CGxString layout (m_flags +0x24, m_geomBuffers +0x18, the text, the
  per-page vertex counter) is written out in Lexara from 3.3.5. It has to be
  derived afresh for 1.12; some offsets already match (+0x18, +0x1C, +0x24,
  +0x18C), but not all.
- **R3.** InitializeTextLine was not found in 1.12. The hook is optional
  (pre-loading characters into the atlas), so its absence blocks performance, not
  correctness.
- **R4 (found in game, 2026-09-11).** Site 3 as mapped (`005C9003`) is the target
  of `005C9001 jne 005C9007`, taken when the string's geometry list is empty (the
  head `[esi+0x24]` is odd - the list's own terminator). Detours puts `jmp` over
  `005C9003..005C9007` and `int3` at `005C9008`, so the branch executed the last
  displacement byte (`0x55`, `push ebp`) and then the `int3`: ERROR #132,
  BREAKPOINT at `005C9008`, seen when discovering a new zone. The site now starts
  at the `jne` itself and the stub reproduces that branch. 3.3.5 has the same jump
  (`006C4AF1 jne 006C4AF7`), so upstream Lexara carries the same mine. A sweep of
  every branch in `WoW.exe` against all ten sites found no other live target
  inside a patch besides R1.

## Conclusion of stage 1

The port is feasible. All ten sites exist in 1.12, each has enough bytes for a
detour, and the rendering function is practically the same code. The work is
rewriting the stubs for the register convention and deriving the CGxString layout -
not rewriting logic.

---

# Stage 2

## FreeType

Lexara **does not mix ABIs**: it replaces the client's entire FreeType with its own
(2.14.1), hooking the library's entry points in the binary. The faces on which it
later calls `FT_Load_Glyph` and so on are therefore created by ITS FreeType. That
removes the port's biggest risk. The 1.12 client also has a built-in FreeType (the
`sfnt` and `truetype` modules).

The identification chain: the string `truetype` -> the module class record -> the
default module table -> `FT_Add_Default_Modules` -> the client's wrapper. The
method was first checked against 3.3.5, where it produced addresses matching those
written out in Lexara.

| function | 3.3.5 | 1.12 | how confirmed |
|---|---|---|---|
| FT_New_Library (`InitFn`) | 00991320 | 007CF0E0 | client wrapper 006BE230 -> 005C17F0, instruction for instruction |
| FT_Add_Default_Modules | 00990650 | 007CCFC0 | the table of 11 modules: 00AA3944 -> 0081E068 |
| FT_Done_FreeType | 00992CB0 | 007CF160 | the only caller 006BF265 -> 005C18B3 |
| FT_New_Memory_Face | 00993370 | 007CDDE0 | same code, error 6, same caller |
| FT_Done_Face | 00992610 | 007CE2F0 | destructor 006C81C0 -> 005D0290, same layout |
| FT_Set_Pixel_Sizes | 00992780 | 007CE760 | same code, `[face+0x58]` |
| FT_Get_Char_Index | 009911A0 | 007CE960 | glyph function 006C8C10 -> 005D1300 |
| FT_Load_Glyph | 00992DA0 | 007CDB40 | same place, same `0x208A` constant and `0x38` mask |
| FT_Get_Kerning | 00991050 | 007CE830 | same code, error 0x23 |
| FT_Select_Charmap | 009910F0 | 007CE8D0 | 1.12 passes `edx='unic'` |
| (glyph render) | 00992B60 | 007CEC20 | same code, error 6 |
| **FT_New_Face** | 009931A0 | **not found** | no caller in the client in EITHER build; Lexara hooks it as a precaution |

Globals: `FT_Library` 00C7D2B4 -> **00C2B9A8**, `FT_Memory` 00AD9960 -> **0085F4C8**.

1.12 conventions: `FT_New_Memory_Face(ecx=library, edx=file_base, [+8]=size,
[+0xC]=index, [+0x10]=aface)`, `ret 0xC`. `FT_Done_Face(ecx=face)`.
`FT_Select_Charmap(ecx=face, edx=code)`.

An incidental confirmation of the stage 1 map: the 1.12 wrapper `005C17F0` starts
with `call 005C92F0` - exactly where 3.3.5's `006BE230` does `call 006C47B0`
(InitFontIndexBuffer). Two independent chains produce the same pair.

## The CGxString layout in 1.12

Derived by comparing pairs of functions instruction for instruction, not by
assumption.

| field | 3.3.5 | 1.12 | where confirmed |
|---|---|---|---|
| m_fontSizeMult | 0x1C | **0x1C** | 006C7B4C / 005CDC55, `fld [ebx+0x1c]` |
| m_textColor | 0x2C | **0x2C** | WriteGeometry, `add eax,0x2c` |
| m_shadowColor | 0x30 | **0x30** | WriteGeometry |
| m_shadowOffset | 0x34 | **0x34** | WriteGeometry |
| m_fontObj | 0x44 | **0x44** | 006C7480 / 005CD3F0, `mov ecx,[esi+0x44]` |
| m_text | 0x48 | **0x48** | same place, `mov eax,[esi+0x48]` |
| m_flags | 0x5C | **0x5C** | GetVertCountForPage, `test byte [ecx+0x5c],1` |
| m_isDirty | 0x64 | **0x64** | 005CD3F0 |
| (gradient len) | 0x6C | **0x6C** | 005CDC58 |
| m_finalPos | 0x70 | **0x70** | WriteGeometry, `lea edx,[eax+0x70]` |
| (counter) | 0xB0 | **0x9C** | CheckGeometry, `cmp [esi+0xb0]` -> `[esi+0x9c]` |
| **m_geomBuffers[8]** | 0xB4 | **0xA0** | GetVertCountForPage and WriteGeometry, both |
| m_timeSinceUpdate | 0xD4 | **0xC0** | CheckGeometry, `mov [esi+0xd4],0` -> `[esi+0xc0]` |
| sizeof | 0xD8 | **~0xC4** | from the shift |

**Everything up to and including 0x7C is identical.** The 0x14-byte hole sits
between 0x7C and 0xA0 (1.12 has fewer fields there: where 3.3.5 clears arrays with
`lea ecx,[ebx+0xa0]; call ...`, 1.12 zeroes a single dword with
`mov [ebx+0x90],edi`). No Lexara hook touches that range. From 0xA0 upwards
everything is shifted by **-0x14**.

## The CGxFont layout in 1.12

| field | 3.3.5 | 1.12 | where |
|---|---|---|---|
| m_ftWrapper | 0x70 | **0x70** | 006C22F0 / 005CA030, `mov ecx,[esi+0x70]` |
| m_atlasPages[0] | 0x178 | **0x178** | both use `[font+0x184]` (= page 0 + 0xC) |
| m_rasterTargetSize | 0x24C | **0x24C** | 005CA030 and the accessor 005CAE90 |
| FT_Face inside the FT wrapper | +0x24 | **+0x24** | destructor 006C81C0 / 005D0290 |

## Helper functions

| role | 3.3.5 | 1.12 | 1.12 convention |
|---|---|---|---|
| CheckGeometry | 006C7480 | **005CD6A0** | ecx = this |
| WriteGeometry | 006C5E90 | **005CE0C0** | ecx = this, `ret 0x10` |
| InitializeTextLine | 006C6CD0 | **005CCBE0** | ecx = this, `ret 0x18` - the same 6 arguments |
| ClearInstanceData | 006C6B90 | **005CDEF0** | ecx = this |
| GetVertCountForPage | 006C63E0 | **005CE090** | ecx = this, `ret 4` |
| GetFontFace | 006C8080 | **005D0370** | ecx = the FT wrapper |
| RenderBatch | 006C53A0 | **005C8B70** | ecx = this; guard 00C7D2C0 -> **00C2B9B0** |
| GetFontEffectiveHeight | 006C0B20 | **005C6FA0** | ecx = is3d, float on the stack |
| GetFontEffectiveWidth | 006C0B60 | **005C7010** | as above |
| RenderGlyph | 006C8CC0 | **005D1120** | |
| PoolCreate | 006876D0 | **0058A160** | ecx=1, edx=0, 3 arguments on the stack |
| bufalloc | 006C48D0 | **005C8F40** | ecx = &stream, edx = size |
| screen height | 00C7D2C4 | **00C2B9A0** | |
| screen width | 00C7D2C8 | **00C2B9A4** | |

`InitializeTextLine` was found by the fingerprint of the set of functions it calls,
not by its position in the call list - the call counts differ between the two
builds, and matching by order pointed at the wrong function (005CD310). What
settled it: the same caller (006C7B10 -> 005CDC20), the same `ret 0x18`, the same
frame of about 0x90, and the same set of callees.

**Conclusion of stage 2: nothing unknown is left except `FT_New_Face`, which
neither of the two clients calls.** Every field and every function Lexara actually
uses has a confirmed 1.12 counterpart.

---

# Stage 3 - the D3D layer and the first real measurement (2026-09-09)

## The discovery that changes the port's architecture: 1.12 HAS NO FONT SHADERS

Lexara has two layers: `MSDF.cpp` (the ten patches in the font code, described in
stage 1) and `D3D.cpp` - the `CGxDeviceD3d::IShaderCreateVertex` /
`IShaderCreatePixel` hooks, which replace **the bytecode of the client's font
shaders** with its own `vs_3_0` / `ps_3_0`. That second layer has no counterpart
whatsoever in 1.12.

The proof is in the same pair of functions that closed out stage 2 (the font
initialisation wrapper):

- 3.3.5 `006BE230`: twice
  `mov ecx,[00C5DF88]; mov edx,[eax+0x110]; push 0x00C7D2D0 / 0x00C7D2CC; call edx`
  - i.e. it creates a **vertex shader object** (global `00C7D2D0`) and a **pixel
  shader object** (`00C7D2CC`) - and only then does `call 006C47B0`
  (InitFontIndexBuffer).
- 1.12 `005C17F0`: the **first instruction** is `call 005C92F0`
  (InitFontIndexBuffer). No shader creation, no globals next to the index buffer.

Additional confirmations:

- 1.12's `RenderBatch` (`005C8B70`) sets fixed-pipeline states exclusively, through
  `005A9E60` (`GxRenderState`: `mov ecx,<state>; mov edx,<value>; call`) - nothing
  shader-related.
- In the whole of 1.12's `.text` there is **not a single** call to
  `IDirect3DDevice9::SetVertexShader`.
- The profile strings `vs_2_0 / vs_1_1 / ps_2_0 / ps_1_4..1_1` sit in a `.data`
  table at `0085C608`, but **nothing in `.text` touches that table** - it is dead
  library code. The note in CLAUDE.md that "the 1.12 client uses ps_2_0" should be
  read as: it contains such strings, it does not use them.

**Conclusion: the port cannot be done by swapping the client's shaders.** Our own
shaders have to be created and bound directly on `IDirect3DDevice9`, around the
drawing of a font batch.

## The replacement architecture for 1.12

All the rest of Lexara (FreeType, msdfgen, the atlas, the cache, the ten patches
from stage 1) stays unchanged. Only the way shaders reach the device changes:

| layer | 3.3.5 (Lexara) | 1.12 (the port) |
|---|---|---|
| shader source | bytecode swapped inside the client's `ShaderData` | our own `CreateVertexShader/CreatePixelShader` on the device |
| binding moment | the client calls `SetShader` itself | a `DrawIndexedPrimitive` hook + an "I am inside a font batch" flag |
| WVP matrix | the client's constant buffer | `GetTransform(WORLD/VIEW/PROJECTION)` in the hook, multiply, `SetVertexShaderConstantF(0,...)` |
| MSDF atlas | slots s12..s15 through `ShaderData` | `SetTexture(12..15, atlas)` in the hook, restored after drawing |
| vertex declaration | its own | **the client's FVF is enough** (measured, see below) |

Two facts make this possible:

- **A single draw site.** In the whole of 1.12's `.text` there is **exactly one**
  `DrawIndexedPrimitive` call (`005A109A`, `call [eax+0x148]`) and **zero**
  `DrawPrimitive` calls. All geometry, fonts included, passes through that one
  funnel - a hook on the device method catches everything, and a flag set by the
  `MSDF.cpp` patches says which of those calls are text.
- **The stage 1 patches already provide the flag.** Sites 3 and 4 (`CheckGeometry`)
  cover entry into the batch loop, and site 9 (`ProcessBatch`) is the epilogue of
  `NativeFontRender`. Setting the flag at 3 and clearing it at 9 gives the exact
  "this is text" range.

Lexara's `D3D.cpp` layer remains **partly** useful: the `Present`, `Reset`,
`CreateTexture` and `DrawIndexedPrimitive` hooks and the callback registration
mechanism are independent of the client version. What has to go is
`CGxDevice::IShaderCreate*`, `ShaderData` and `MSDF.h`'s `g_FontPixelShader` /
`g_FontVertexShader` (`00C7D2CC` / `00C7D2D0`) - those globals do not exist in 1.12.

## Measurement: does `ps_3_0` work under this client's DXVK - YES

A separate probe (not part of this repository): a small EXE run from the client
directory so that it loads **the client's** `d3d9.dll`. It compiles Lexara's
ORIGINAL shaders unchanged, creates them and **draws with them into a render
target, then reads the pixels back**.

```
adapter: AMD Radeon RX 5600 XT / aticfx32.dll
VertexShaderVersion = 3.0        PixelShaderVersion = 3.0
MaxSimultaneousTextures = 8      MaxTextureBlendStages = 8
MaxVertexShaderConst = 256       MaxTextureWidth/Height = 16384
D3DCompile vs_3_0 : OK (528 B)   D3DCompile ps_3_0 : OK (1584 B)
CreateVertexShader : OK          CreatePixelShader : OK
2048x2048 atlases: 4/4           AvailableTextureMem 1015 MB before and after
K1 Clear                : 0xFF203040 OK
K2 texture path         : 0xFF3366CC OK
K3 MSDF path (fwidth)   : alpha 142, computed 142 OK
K4 MSDF sd=0 + blend    : RGB 23/45/90, computed 23/45/90 OK
```

What this settles:

- `ps_3_0` / `vs_3_0` **compile, are created and execute** - the client's `ps_2_0`
  is not a limit of the hardware or of DXVK. The assumption from stage 1 is
  confirmed by measurement.
- **`fwidth` works** - it is the one instruction in Lexara's shader that requires
  SM 3.0 and the one that could have failed under DXVK's translation.
- **FVF is enough in place of a vertex declaration.** The probe sets
  `SetFVF(XYZRHW|DIFFUSE|TEX1)` and draws with `vs_3_0` - it works. That matters,
  because 1.12 never sets its own declaration, only an FVF; the port does not have
  to change that.
- **Four 2048x2048 A8R8G8B8 atlases fit** (64 MB) and do not move the free texture
  memory counter (1015 MB before and after - DXVK counts differently from how it
  allocates). Note: `d3d9.textureMemory = 64` in `dxvk.conf` refers to a different
  quantity; the crash history tells us the limit is the **number of live mappings**,
  not the amount of memory.

**What was wrong in K3/K4 was my expectation, not the shader.** The first version
of the probe wrote the "expected" pixel by eye (`0xFF3366CC`, `0xFF000000`) and
both checks came out as MISMATCH. Working the shader's formula through
(`screenPxRange = control.z / (fwidth(uv) * control.a) *
(1 - min(0.3, fontSize * 0.0035))`, `opacity = saturate((sd - 0.5) * spr + 0.5)`)
gives exactly `142` and `23/45/90`. The probe now computes this itself - a check
without a computed expectation is not a check, it is guesswork.

## Toolchain on this machine

- **There is no CMake.** There is MSVC BuildTools 2022 (`14.44.35207`,
  `Hostx86/x86/cl.exe`), MSBuild and WinSDK `10.0.22621.0`. `vcvarsall.bat` is in
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build`.
  VS Community 2022 has only `VC\Auxiliary` and `Redist` - **no toolset**, do not
  use it.
- `d3dcompiler_47.dll` is in `SysWOW64`, so runtime HLSL compilation is available
  without shipping extra files (Lexara compiles its shaders on the fly anyway).
- The probe's `build.bat` calls `cl` directly, without CMake. FreeType/msdfgen will
  need CMake - either `pip install cmake` or a hand-assembled project.
- **Running `.bat` files from PowerShell:** `cmd /c "cd /d <dir> && file.bat"`
  produces "not recognized"; `& cmd.exe /c '<full path to the .bat>'` works.

## The port project - built (`_lexara-port/`)

`build.bat` -> `build\out\Release\lexara112.dll` (PE32, 964 kB). Its own README is
in that directory. **The DLL MUST NOT be loaded into the game yet** - see below.

Dependencies fetched separately (Lexara does not bundle them): FreeType
`VER-2-14-1`, msdfgen (master), Detours, unordered_dense. Detours got an added
`CMakeLists.txt` and a `detours.h` forwarder (Microsoft's repo keeps the header in
`src/`). In msdfgen **SVG and PNG are disabled** - otherwise
`find_package(tinyxml2)` aborts configuration, and the port reads neither format.

**msdfgen without Skia has no `resolveShapeGeometry`.** Lexara builds with Skia
through vcpkg; it is replaced in `MSDFCompat.h` with the same thing msdfgen itself
uses in its place: `shape.orientContours()`. This is not an eyeball approximation -
`resolve-shape-geometry.cpp:127` ends with exactly that call, and `main.cpp:1155`
uses it as the fallback under the `#else` to `MSDFGEN_USE_SKIA`. The price: glyphs
with overlapping contours may show an artefact at the intersection - to be checked
by measurement.

*Corrected 2026-09-14:* `main.cpp:1155` is the opt-in `-windingpreprocess`; msdfgen's
default without Skia is no preprocessing (`main.cpp:577`). Measured, `orientContours`
did cut overlaps out of glyphs, and the port now does no preprocessing - see
[`third-party-fonts.md`](third-party-fonts.md).

### Ported to 1.12 and built into the DLL

Post-build check (searching the binary for constants): all the new addresses are
present, and **not a single old 3.3.5 font address is left in the file**.

- the ten patch sites (stage 1) plus four `naked` stubs rewritten from
  `_lexara-port-stuby.h`: the index pool without the device global, the counter in
  EDI, the loop bound `0A0h`, `bufalloc` in the register convention with
  `lea ecx,[ebp-1Ch]`;
- `CGxString`: the 0x14-byte hole between 0x7C and 0xA0 as `unk_7C[9]`,
  `m_geomBuffers` at 0xA0, `m_timeSinceUpdate` at 0xC0, `sizeof` 0xC4;
- the conventions that DIFFER, not just the addresses: `RenderGlyph`
  (`__fastcall`, ecx = face, edx = fontSize), `GetFontEffectiveWidth/Height`
  (`__fastcall`), `PoolCreate` (ecx = 1, edx = 0), `bufalloc` (ecx = &stream,
  edx = size), `GetFontFace` (`__fastcall`).

### Two functions pinned down in this stage

Stage 2 did not have them, because Lexara's `GameClient.h` reaches beyond what the
map covered.

| role | 3.3.5 | 1.12 | how confirmed |
|---|---|---|---|
| `CGxFont::GetOrCreateGlyphEntry` | 006C3FC0 | **005CABD0** | sits directly before GetBearingX in 005C6B70; also called from InitializeTextLine |
| `CGxFont::GetBearingX` | 006C24F0 | **005CB080** | instruction for instruction: `fld [eax+0x50]`, `ret 0xC`, the else branch calls GetFontEffectiveHeight |
| (caller of both) | 006C09A0 | **005C6B70** | the only caller of GetBearingX besides InitializeTextLine in both builds |
| (caller of RenderGlyph) | 006C2480 | **005CA160** | the only caller in both builds |

Once again what worked was the caller-callee relation, not a position in a call
list: the call lists of `InitializeTextLine` have DIFFERENT lengths in the two
builds (24 vs 25) and diverge after the fourth entry.

### What was left - exactly two things

1. **`D3D.cpp` - the entire device layer.** The addresses of
   `CGxDevice::DeviceCreate`, `NotifyOnDeviceRestored`, `DeviceSetFormat`,
   `IDestroyD3d` and `IReleaseD3dResources` are still 3.3.5 ones, and
   `IShaderCreateVertex/Pixel` have no counterpart at all. The direction: do not
   map them, but take the device from the D3D9 vtable and bind our own shaders in
   the `DrawIndexedPrimitive` hook (a single site in the entire client: 005A109A).
2. **The FreeType hooks in `MSDF.cpp`** - declared `__cdecl`, while 1.12 calls them
   through registers. The addresses are ready from stage 2; only the conventions
   are missing: `FT_New_Memory_Face(ecx = library, edx = file_base, [+8] size,
   [+C] index, [+10] aface)`, `FT_Done_Face(ecx = face)`,
   `FT_Select_Charmap(ecx = face, edx = code)`.

Until both are done, **`lexara112.dll` must not go into `dlls.txt`** - it would
patch the client with addresses from a different build.

# Stage 4 - the D3D and FreeType layers ported (2026-09-09)

After this stage there is **not a single 3.3.5 address** left in the port's binary.

## The D3D device without client addresses

Lexara took the device from the global `*(0x00C5DF88) + 0x397C` and hung itself on
seven `CGxDevice` functions. 1.12 has neither that global nor those functions. The
search for an equivalent global **failed and was abandoned** - `00C0F464`, which
came out of walking the call chain around `DrawIndexedPrimitive`, turned out to be
a set of viewport state scalars, not a device object.

**Correction (2026-09-11):** the global exists - `00C0ED38` is the Gx layer's
`CGxDevice*` (138 references), found in ClassicAPI's `Offsets.h`. For the D3D
device its vtable is `00809EF8` (stored by the ctor at `00598D05`) and the
`IDirect3DDevice9*` sits at `+0x38A8` - the `ppDevice` of the client's own
`CreateDevice` (`lea edi,[esi+38A8]` at `00599603`, `call [ecx+40h]` at
`00599627`). It is now the fallback when the chain below misses (bug 10).

Instead of searching further: **1.12's `WoW.exe` has no d3d9.dll in its import
table** (its only graphics import is `opengl32.dll`), but it does hold the strings
`d3d9.dll` and `Direct3DCreate9` in its data - so it loads it dynamically. Hence
the chain, with not a single client address:

```
LoadLibraryA/W/ExW  ->  (when the module is d3d9.dll)  GetProcAddress("Direct3DCreate9")
                    ->  Direct3DCreate9 hook  ->  IDirect3D9 vtable slot 16
                    ->  CreateDevice hook     ->  IDirect3DDevice9* remembered
```

At the moment VanillaFixes injects the DLL, `d3d9.dll` is not loaded yet, so
hooking the export straight away will not work - hence the `LoadLibrary*` hooks.
`D3D::initialize()` **does not call LoadLibrary itself**: it runs from DllMain, and
loading a library under the loader lock is asking for a deadlock.

The roles of the client hooks were taken over by device methods:

| was (3.3.5) | is (1.12) |
|---|---|
| `CGxDevice::DeviceCreate` | an `IDirect3D9::CreateDevice` hook |
| `IReleaseD3dResources` | `IDirect3DDevice9::Reset`, the before phase |
| `NotifyOnDeviceRestored`, `DeviceSetFormat` | `Reset`, the after phase (when `SUCCEEDED`) |
| `IDestroyD3d` | `D3D::shutdown()` from `DllMain(DETACH)` |
| `IShaderCreateVertex/Pixel` | **does not exist** - shaders are bound by us |

## Shader binding

`BindMsdfShaders` / `UnbindMsdfShaders` in `MSDF.cpp`. Binding happens in
`WriteGeometryHk` (where the atlases and constants are being set anyway), and
unbinding in `CGxuFontRenderBatchHk` and on the "this font is not an MSDF font"
path - without the latter the whole interface would be drawn with the font shader.

Because `vs_3_0` replaces the fixed pipeline's transform, the
`World*View*Projection` matrix has to be supplied in `c0..c3`; the port reads the
three matrices from the device (`GetTransform`) and multiplies them itself, without
D3DX.

**`MSDF_WVP_TRANSPOSE` is the one thing in the whole port that could not be settled
statically.** With `mul(pos, WVP)` and the default column-major packing the matrix
is handed over transposed - and that is how it is set. The symptom of the wrong
choice: text invisible or off-screen. In that case, `false` and a rebuild.

## FreeType - eight functions, all `__fastcall`

3.3.5 called them `__cdecl`. Verified from the prologue and from `ret N` matching
the argument count (the first two in `ecx`/`edx`, the rest on the stack, cleaned up
by the callee):

| function | 1.12 | `ret` | arguments |
|---|---|---|---|
| `FT_New_Library` (Init) | 007CF0E0 | 0 | 2 |
| `FT_New_Memory_Face` | 007CDDE0 | 0xC | 5 |
| `FT_Done_Face` | 007CE2F0 | 0 | 1 |
| `FT_Set_Pixel_Sizes` | 007CE760 | 4 | 3 |
| `FT_Get_Char_Index` | 007CE960 | 0 | 2 |
| `FT_Load_Glyph` | 007CDB40 | 4 | 3 |
| `FT_Get_Kerning` | 007CE830 | 0xC | 5 |
| `FT_Done_FreeType` | 007CF160 | 0 | 1 |

Argument order is unchanged from 3.3.5. For `FT_New_Library` the wrapper
`005C17F0` confirms it independently: `mov edx, 0xC2B9A8` (alibrary),
`mov ecx, 0x85F4C8` (memory). `FT_New_Face` is **omitted** - risk R3; neither of
the two clients calls it.

## What remained: run it

The list of four things to check in game, and the rollback procedure, are in
`_lexara-port/README.md`. In short: (1) does the client start - if not, R1 is the
suspect; (2) is the text visible - if not, `MSDF_WVP_TRANSPOSE`; (3) is the rest of
the UI not painted with the font shader; (4) texture memory against
`d3d9.textureMemory = 64`.

## What is still missing (as of stage 4)

- Lexara's `third_party/` **is not in the repository** - FreeType 2.14.1, msdfgen,
  Detours and unordered_dense have to be fetched. The `src/font_exact` code itself
  is complete.
- 1.12 addresses for the shader-independent hooks that stage 2 did not map:
  `DeviceCreate` (3.3.5 `00682CB0`), `NotifyOnDeviceRestored` (`006843B0`),
  `DeviceSetFormat` (`006904D0`), `IDestroyD3d` (`006903B0`),
  `IReleaseD3dResources` (`00690150`). The alternative: do not map them at all and
  take the device from the `Direct3DCreate9` hook / the vtable, as most 1.12 addons
  do - then this layer has zero client addresses.
- `IShaderCreateVertex` / `IShaderCreatePixel` (`006AA0D0` / `006AA070`) - **do not
  look for them**, they have no 1.12 counterpart and the port does not need them.

---

# Stage 5 - first launch and in-game debugging (2026-09-09)

**The renderer works.** Text is drawn through the MSDF atlas; periods, hyphens and
letters sit where they should. Below is what had to be fixed along the way - every
item from measurement, not deduction.

## Ten bugs that only showed up in game

| # | symptom | the real cause |
|---|---|---|
| 1 | crash at start-up, `0xC0000005` at `007CECA4` | the client called its OWN `FT_Add_Default_Modules` on Lexara's FreeType 2.14.1 library |
| 2 | shaders `vs=0 ps=0` | compilation in `FreeType_InitHk`, i.e. BEFORE a D3D device exists |
| 3 | no device despite `d3d9.dll` being caught | the client does not create the device on the object we can see |
| 4 | `atlas: pages=0`, black rectangles | `ProcessGeometry` generated glyphs before the device was captured |
| 5 | letters disappearing despite a correct atlas | a failed upload reported "success", the entry kept `UV (0,0)-(0,0)` permanently |
| 6 | `.`, `-`, `_` with `y ~ 1.8e7` | the `GetGlyphYMetrics` stub clobbered EDX, which is live in 1.12 |
| 7 | the same characters against the TOP edge of the line | my own clamping of the subtraction - a fix for bug 6 that outlived it |
| 8 | "Windows - Application Error" on every exit: `nvoglv32+0x855FFD` referenced `0x00000010` | `DllMain(DETACH)` released our shaders at process exit; we are injected before `d3d9.dll` (DXVK) loads the Vulkan driver, the loader detaches in reverse order, and `d3d9.trackPipelineLifetime` sent the release straight into the dead driver |
| 9 | ERROR #132, `0x80000003` BREAKPOINT at `005C9008` on zone discovery | `005C9001 jne 005C9007` jumps into the middle of site 3; Detours had put the tail of its `jmp` and an `int3` there (risk R4) |
| 10 | no text at all on one machine (reported with ClassicAPI.dll loaded): `WriteGeometry: NO DEVICE`, no `device captured` | `d3d9.dll` is unloaded and loaded again before the client creates its device. At the same base the vtbl[16] guard re-swaps the fresh table and the capture works; at another base (`5EF314B0` -> `5EEF14B0`) the chain stays on the dead copy, and the guard read `83440486` out of the new image as a "foreign" CreateDevice and wrote into it. Fix: the guard checks which module owns the table, and `GetDevice` reads the client's own pointer `[00C0ED38]+0x38A8` |

## Capturing the device - three approaches failed, the fourth works

1.12's `WoW.exe` **does not import d3d9.dll** (its only graphics import is
`opengl32.dll`); it loads it through `LoadLibrary`. These fell away in turn:

1. `CGxDevice` hooks - 1.12 has neither the `00C5DF88` global nor those functions;
2. a detour on the body of `Direct3DCreate9`/`CreateDevice` - it **installed
   cleanly** (`begin/update/attach/commit = 0`) and the hook never fired once;
3. swapping the `vtbl[16]` entry - the entry was swapped and still never called.

Conclusion: the client creates its device outside the object we can see (something
else stands between it and DXVK - most likely another mod).

**The fourth approach works:** objects of the same C++ class SHARE a virtual table.
So we create our own throwaway device on a hidden 8x8 window, swap `EndScene`
(slot 42) in its vtable and release our own device. From that moment on every DXVK
device in the process - the client's included - goes through our hook and hands
itself over in `this`.

**A fifth, race-free source (2026-09-11):** the client's own pointer,
`[00C0ED38] + 0x38A8` (see stage 4). `GetDevice` reads it on every call and, if it
differs from what the chain caught, takes it. On a normal run the log shows
`client device pointer: X, capture chain: X (same)`; where the chain missed it shows
`device read from the client`. `client_device=0` turns it off.

**`commit = 0` does NOT mean "the hook works".** It means only that Detours
reported no error. For three rounds zero was read as success; what settled it was a
log line from INSIDE the hook, which had been missing all along.

## The GetGlyphYMetrics stub - registers, not just addresses

```
3.3.5 (006C8C71):  mov edx, [ecx+0x54]  ->  mov ecx, [edx+0x68]   chain through EDX
1.12  (005D137A):  mov ecx, [ecx+0x54]  ->  mov ecx, [ecx+0x68]   chain through ECX
```

A stub carried over verbatim from 3.3.5 clobbered EDX, which at this point in 1.12
is **live and deliberately zeroed** (`005D136B mov [ebp-4],edx`,
`005D136F xor edx,edx`). The client then computed the vertical coordinate from that
garbage.

The checks on site length and return address **both passed** - the patch looked
correct. With every stub, also check which registers are live.

## Two overlapping bugs - a methodological trap

Bug 6 produced `y ~ 1.8e7`. I took that for an unsigned overflow in
`m_bearingY -= m_verAdv` and added clamping to zero. It did not help, but it
**stayed in the code**. Once the real cause (the stub) was fixed, the clamping
itself became bug 7: it drove the bearing of the period and the hyphen to zero, i.e.
planted them against the top edge of the line.

Two measurements settled it, not code analysis:
- a probe showed the clamping fired only 3 times, and with `code=0` - i.e. in calls
  without meaningful metrics, and never for `.` or `-`;
- the "disable the hook and see" test (`hook_renderglyph=0`) showed that without
  the subtraction ALL letters go astray, so it is necessary and a negative result
  is correct - the client reads that value AS SIGNED.

## The switch file - bisection without a rebuild

`lexara112.cfg` next to the client, `name=0/1`, no file = everything enabled.
23 keys: 10 patch sites, 6 font hooks, `ft_hooks`, `shaders`. A change needs only a
game restart, not a DLL rebuild - which shortened every diagnostic round from
"rebuild and log in" to just logging in.

**The trap:** the `FT_New_Library` and `FT_Add_Default_Modules` hooks have to sit
under THE SAME key as the rest of `ft_hooks`. The first version left them outside
the gates "because they are infrastructure", and with `ft_hooks=0` the client got
Lexara's library but called its own old functions on it - a state worse than either
extreme, and a crash. The "everything disabled" control was then no control at all.

## Probes - what not to do

- **A `static bool` guard hides the distribution.** The upload probe printed only
  the FIRST occurrence of each cause; the single entry that reached the log was a
  space, i.e. the least interesting case of all. Counters on every exit showed the
  truth immediately: 392 rejections, all from one branch.
- **The uninstrumented branch is the one that fails.** `CreateAtlasPage` was the
  only `return false` without a log line, and that is exactly where the bug sat.
- **A log inside the draw loop becomes a problem in its own right.** A dump every
  200 calls, at 2.3 million calls per session, is 11,000 file opens.
- **A check with a known answer settled the vanishing-character case.** Only by
  putting `.` (`y = 1.8e7`) next to the letter `m` (`y = -31..-12`) FROM THE SAME
  BATCH did it become clear this was an anomaly, not a normal coordinate for this
  pipeline.

## The in-game toggle

`CTRL+ALT+F` toggles the renderer live (the `MSDF::ENABLED` flag, handled in the
`EndScene` hook, with key edge detection). The `LexaraCompare` addon (`/lexara`)
provides a fixed text sample at seven sizes; right-click changes the typeface.

**Known limitation:** after toggling, some strings can stay in their previous form,
because glyph entries with the `u = 1 + code` marker remain in the client's cache.
Those fields are zeroed on disable, but strings the client has already laid out are
not recomputed. To be closed out.

## Status and what next

Working: the atlas, the shaders, horizontal and vertical geometry, device capture,
periods, hyphens, letters. To be checked and closed out:

- the underscore `_` - reported as wrong, to be confirmed after the latest fixes;
- artefacts after the toggle (see above);
- `metrics.pixelData` points at the `storage.ownedPixelData` of a LOCAL variable
  (`MSDFFont.cpp`) - harmless in the original, because the upload happens
  immediately, but a retry of the upload from the pool would read freed memory;
- performance and memory measurements against `d3d9.textureMemory = 64`.
