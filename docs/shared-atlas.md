# The shared MSDF atlas - one per process instead of one per typeface

## Symptom

`Errors\2026-09-09 21.56.59 Crash.txt`: an `ACCESS_VIOLATION` in
`ucrtbase!memcpy+78`, called from `d3d9.dll` (DXVK 2.6.1 **x86**). The frame below
it is `0x005A10A0`, i.e. the return from `call [ebx+0x148]` at `0x005A109A` -
`IDirect3DDevice9::DrawIndexedPrimitive` (index 82 in the vtable; the seven pushes
match the signature). Registers: `ECX=EDX=0x10000` (64 KiB), source
`ESI=EBX=0x000CCDC0` - an unmapped address.

The last two lines of `lexara112.log` before the process died:

```
[MSDF] CreateTexture 2048x2048 fmt=21 pool=1 hr=0x8876017C free=4048 MB
[MSDF] CreateAtlasPage: CreateTexture 2048x2048 REFUSED (pages so far=0, max=4)
```

`0x8876017C` is `D3DERR_OUTOFVIDEOMEMORY`, yet `GetAvailableTextureMem()` reports
4048 MB free. **This is not VRAM - it is the process's 32-bit address space.**
`D3DPOOL_MANAGED` makes DXVK keep a full copy of the texture in process memory.

## Cause

`MSDFFont::CreateAtlasPage` allocated pages **per typeface**: every `MSDFFont` got
its own 2048x2048 A8R8G8B8 pages in `D3DPOOL_MANAGED`, i.e. 16 MiB per page and up
to `MAX_ATLAS_PAGES` = 4 pages, so up to **64 MiB for a single typeface**. There
was no upper bound on the total - it grew with the number of typefaces actually
drawn.

In the fatal session 10 atlases were created (160 MiB), an eleventh was refused
twice, and moments later DXVK died copying a buffer during a draw. The chain "out
of address space -> crash in memcpy" is an inference from the order of events, not
something proven in the dump - but `REFUSED` appears in the entire log **only twice,
and only in that session**.

## The change

`s_atlasPages`, `s_oldestPage` and `s_evictionCount` are now **static**: one atlas
per process, shared by every typeface. The upper bound is
`MAX_ATLAS_PAGES * ATLAS_SIZE^2 * 4 B` = **64 MiB for the whole process**,
regardless of how many typefaces there are.

**The page encoding does not change.** The page index travels in the signs of the
UVs (`MSDF.cpp`: `uSign`/`vSign`), and the shader uses it to pick sampler `s12`-`s15`
(`MSDFShaders.h`). That is two bits, so 4 pages is a ceiling imposed by the client's
vertex format (`CGxFontVertex` has only `pos`, `u`, `v` - and `pos.z` carries the
depth of 3D text, so it cannot be taken). The shared atlas does not touch that
encoding - it only changes whose glyphs sit on those four pages.

### Eviction

A page now mixes typefaces, so `AtlasPage::codepoints` (`vector<uint32_t>`) became
`entries` (`vector<pair<MSDFFont*, uint32_t>>`) - a character code alone no longer
identifies a glyph.

`EvictOldestPage()` **invalidates entries instead of erasing them**, and that is a
fix, not cosmetics: `m_glyphPool` is an `ankerl::unordered_dense` - a dense map
whose `erase` moves other elements around. `UploadGlyphToAtlas` receives a
`GlyphMetrics&` **from that very map** and holds that reference across the whole
eviction, so any `erase` during it can invalidate the reference. The old code did
exactly that (on its own typeface's entries) and got away with it only because
evictions were rare. Zeroing `u0/v0/u1/v1` does not disturb the map's layout, and
`GetGlyph` already recognises an entry with a zero `u1`/`v1` as "never uploaded"
and sends it to the atlas again - that retry path already exists and is walked at
every start-up, before the first frame.

The eviction counter is global, so `CheckGeometryHk` rebuilds the geometry of
**every** string, not just of the typeface that triggered the eviction. That is
intended: a page mixes typefaces.

### A refused allocation is no longer the end of a glyph

Previously `CreateAtlasPage() == false` ended in `return false` - the glyph was
lost for good (those 392 rejected glyphs from the comment in the code). A refused
allocation now falls onto the same path as reaching `MAX_ATLAS_PAGES`: evict the
oldest page and place the glyph there. Now that the pages are shared, there is
something to share out.

### Lifetime

- `~MSDFFont` **does not release pages** - that would take them away from
  typefaces that are still drawing. What it does is sweep out this typeface's
  entries (`ForgetFontEntries`), because they hold `this` and an eviction would
  reach into a dead object.
- The `m_atlasEntryCount != 0` guard before that sweep is **necessary for thread
  safety**: `MSDFPregen` creates `MSDFFont` objects on worker threads and those
  objects never place anything in the atlas (they go straight to `GenerateMSDF`).
  Without the guard their destructors would walk over state shared with the
  rendering thread.
- `ClearAllCache()` (from `D3D::RegisterOnDestroy`) releases the pages and clears
  the glyph pools of every typeface - the device is going away, so the textures
  must go.
- `Shutdown()` clears `s_fontHandles` and `s_atlasPages`.

## What this does NOT fix

Capacity. Previously each typeface had 4 pages to itself; now it shares 4 pages
with all the others. At `SDF_RENDER_SIZE = 96` and `SDF_SPREAD = 12` a typical
Latin glyph takes about 74x94 px, which with `ATLAS_GUTTER = 14` gives about
23x18 = ~410 glyphs per page, i.e. **~1650 for the whole atlas**. Ten typefaces at
roughly 120 used characters each is ~1200 - it fits, but with no headroom.

If evictions start multiplying in the log (`s_evictionCount` rising, text flickering
as the geometry is rebuilt), the next lever is **`SDF_RENDER_SIZE` 96 -> 64**: the
glyph area then drops by about 2.2x, so the atlas capacity rises to ~3600 glyphs.
The cost is the sharpness of very large text.

## The regression this change caused - and its fix (22:59)

The first version of the shared atlas killed the client after 45 minutes:
`Errors\2026-09-09 22.59.20 Crash.txt`, an `ACCESS_VIOLATION` **entirely inside
lexara112.dll** (base `0x68DD0000`, top `+0x95D8E`, frame below `+0x1AEEA`). The
registers name the location unambiguously: `ECX=EDX=0x180`, `EAX=ESI+0x180`, `ESI`
unmapped. `0x180` = 384 = `metrics.width * 4` at `width = 96`, i.e. **the row loop
in `UploadGlyphToAtlas`** - `memcpy` was reading freed memory.

### Cause

`GlyphMetrics::pixelData` is valid **only until the end of the current call**. It
points either into the `storage` buffer, which right after the upload goes to
`MSDFCache::StoreGlyph` and dies at the next `FlushPendingWrites` (`m_pendingWrites`
is cleared after `WRITE_BATCH_SIZE` entries), or - on the `TryLoadGlyph` path -
into a block of the cache file mapped by `MSDFManager`
(`outMetrics.pixelData = blockPtr->payload + ge.dataOffset`), which can also
disappear.

The upload retry path (`neverUploaded`, i.e. `u1 == 0 && v1 == 0`) existed before,
but fired **only at start-up**, for glyphs touched before a D3D device appeared -
and then `pixelData` was fresh, a few instructions old. The shared atlas routed
**evictions** into the same place: an invalidated glyph came back through it
minutes later, with a long-dead pointer.

This is not an inherited bug - it is a side effect of this change. The old
per-typeface eviction **erased** the entry from `m_glyphPool`, so the glyph always
came back through the full path (cache -> generation) and never read the stale
pointer.

### Fix

Two strokes, both in `MSDFFont.cpp`:

1. A successful `UploadGlyphToAtlas` **nulls `metrics.pixelData`**. The pixels are
   already in the atlas and the pointer will not survive anyway - a stale read
   becomes impossible by construction, instead of depending on whoever remembers
   the lifetime rules.
2. The retry path checks `pixelData`. NULL means "fetch the pixels again": we drop
   the entry from the pool and take the full path through `GetGlyph`. Erasing is
   safe here despite the dense map, because no reference into the pool is in
   anyone's hands yet - ours or the caller's.

The recursion goes one level deep: after the `erase` the entry is gone, so the call
goes through `try_emplace` and ends at `TryLoadGlyph` or at generation.

## Verification

Built 2026-09-09 22:14, the pixelData fix at 23:02 (`build.bat` requires cmake;
here MSBuild was run directly on the generated `build\lexara112.vcxproj`, because
the system has no cmake in PATH). **Not verified in game** - confirming it needs a
session in which a dozen or so different typefaces get drawn.

What to look for in `lexara112.log`:

| line | meaning |
|---|---|
| `page N created (..., shared atlas)` | at most 4 times per session, not 4 times per typeface |
| `REFUSED` | should no longer appear once 4 pages have been built |

The state before the move to a shared atlas is in the git history (`MSDFFont.h`,
`MSDFFont.cpp` before the commit that introduced this change).
