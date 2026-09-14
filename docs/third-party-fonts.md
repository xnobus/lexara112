# Replaced fonts drawn by the client instead of Lexara (issue #3)

## Symptom

Reported by EzioAu (issue #3): with the game's fonts replaced by other fonts, Lexara
"stops working" - the text is the client's own soft rendering, while the same client
with the default font is sharp. `dlls.txt` is a long one (ClassicAPI, SuperWoW,
nampower, UnitXP, WeirdUtils, DXVK and more).

The log shows it directly. Every face of the replaced font - 8 499 016 bytes,
`family='Microsoft YaHei' style='Bold'` - is registered with `handle=00000000`,
while the addon fonts loaded next to it (Hooge 05_55, Myriad Pro, Roboto Mono,
Myriad Condensed Web) all get a handle. The handle is `MSDFFont::Get(face)`: null
means `MSDFFont::Register` dropped the face, and the client draws that font through
its own bitmap glyph cache, blurred wherever it is scaled.

## Cause

`Register` drops a face when msdfgen cannot load the file or when
`MSDFValidator::IsFontMSDFCompatible` fails for any glyph in U+0020..U+007E. One of
the validator's tests rejects a glyph that has a contour crossing itself.

Upstream Lexara runs that validator on shapes that went through Skia's
`resolveShapeGeometry` (`Simplify`), which removes self-intersections and overlaps -
so on upstream the test practically never fires. The port has no Skia, and
`MSDFCompat::ResolveShapeGeometry` stood in `orientContours` for it, which leaves the
contours as they are. The port therefore judged a font file's raw contours by a
standard only Skia's output meets.

Measured with a test tool that runs the port's own validator, msdfgen and FreeType
builds on font files:

- **9 of 228** fonts in `C:\Windows\Fonts` are rejected: Bahnschrift, Cascadia Code
  and Mono, Hack Bold and Bold Italic, Segoe Print Bold, Segoe Script Bold,
  SimSun-ExtG, YD2002.
- **3 of 32** addon fonts in the test client: Inconsolata Condensed, The Bad Times,
  Vegur.
- Every one of those rejections is the self-intersection test. No other check fired
  on any of the 260 fonts.

The reporter's font file was not available, so that this test is what rejects it is
inferred, not read: it is the only check that fired on any font measured.

### The other half: `orientContours` in the generator

Letting those fonts through the validator is not enough on its own. The same
stand-in ran in `GenerateMSDF`, and `orientContours` is msdfgen's
`-windingpreprocess`, which "attempts to fix only the contour windings assuming no
self-intersections and even-odd fill rule" (`msdfgen/main.cpp:523`). On a glyph built
from overlapping contours - variable-font instances, merged fonts - it can reverse
one of them, and the nonzero sign pass then cuts the overlap out of the glyph.

Measured by comparing what the pixel shader draws (bilinear sample of the 8-bit
texels, `median > 0.5`, and `alpha > 0.5` for the outline channel) with the nonzero
fill of the untouched outline, at 4 subsamples per texel, counting only mismatches
wider than half a texel (edge quantization is thinner), for U+0021..U+007E of all
260 fonts:

| font | with `orientContours` | without |
|---|---|---|
| Cascadia Code, Cascadia Mono (each) | 17 139 px in 16 glyphs (`A` `B` `E` `H` `I` `R` `Z` ...) | 0 |
| Bahnschrift | 10 104 px in 9 glyphs (`A` `H` `R` `k` `#` `$` ...) | 0 |
| Inconsolata Condensed | 47 px in `A` | 0 |
| the other 255 fonts | 0 | 0 |

Cascadia's `E` loses a band across its stem, Bahnschrift's `A` a block of its
crossbar. Without `orientContours` no glyph of any font strays from its outline
beyond the edge; the only alpha-channel leftovers (7 px in Cascadia's `e`, 3 px in
The Bad Times' `1`) are identical with and without it. The glyphs the validator
rejected come out correct either way.

msdfgen built without Skia does no geometry preprocessing by default
(`main.cpp:577-583`) and relies on `overlapSupport` plus the scanline sign pass
(`main.cpp:586-587`) - exactly what `GenerateMSDF` already sets up. The earlier
statement that msdfgen uses `orientContours` as its fallback without Skia was
wrong: that call is only the opt-in `-windingpreprocess`.

## The change

- `MSDFValidator`: the self-intersection test and its helpers are gone. The other
  checks stay - the glyph loads, every contour has edges and at least three points,
  coordinates are finite, not every edge is zero-length.
- `MSDFFont::GenerateMSDF`: no geometry preprocessing. `MSDFCompat.h`, whose one
  function was the `orientContours` call, is removed.
- `MSDFCache`: the cache folder name ends in `_g2` (`GENERATOR_REVISION`). A font
  that passed the validator can still have overlapping contours outside U+0020..U+007E,
  and its glyphs cached by an older build may carry the holes above - they must not
  be served again, so every font is regenerated once. Not `CACHE_VERSION`: a manifest
  of another version fails to load, and `StoreGlyph` then refuses every glyph of that
  folder for the session. Old folders are left behind, as in
  [`glyph-cache-collision.md`](glyph-cache-collision.md).
- `MSDFFont::Register` logs, once per file, why a font is left to the client:
  `font NOT drawn by MSDF: '<family>' glyph U+XXXX failed validation` or
  `... msdfgen cannot load '<family>'`. Issue #3 arrived with only
  `handle=00000000` to go on.

## What this does NOT change

- A font that fails one of the remaining checks is still drawn by the client - now
  with a log line naming the glyph.
- Overlap handling beyond what msdfgen does without Skia. Skia would also merge the
  contours before edge coloring; none of the 260 fonts measured needed it.

## Verification

Built 2026-09-14, no new warnings. Test client, login screen only,
`bahnschrift.ttf` copied to `Fonts\FRIZQT__.TTF`, cold glyph cache:

- **Old DLL:** `font registered: ... handle=00000000 size=371380
  family='Bahnschrift'` - the reporter's signature - and soft login text.
- **New DLL:** `handle=01246BC0` for the same face, folder
  `Bahnschrift_Regular_353DB1A1EA0DF18D_s96_sp12_g2` with `block_0.dat` written; the
  login text is sharp, and the `A` of "Account" - one of the glyphs `orientContours`
  cut - has its crossbar whole.
- **Warm run:** no new folders, same rendering.

Not exercised in game: the new `NOT drawn by MSDF` line - no font at hand fails the
remaining checks.

**To be confirmed on the reporter's client** - the replaced font's
`font registered` lines should carry a handle, and its text should be as sharp as the
default font's.
