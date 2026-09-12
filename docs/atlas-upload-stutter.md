# The 2-3 second freeze every 15-20 seconds (issue #2)

## Symptom

[Issue #2](https://github.com/xnobus/lexara112/issues/2): *"works great but huge
stutter every 15s for 2s"*. A fresh install, no `lexara112.cfg`, so everything is
enabled. The game runs well and then freezes for 2-3 seconds, over and over, every
15-20 seconds.

The log attached to the issue shows nothing wrong: the device is caught, the shaders
bind, the capped `WVP changed between bind and draw` and `our shaders were replaced`
probes stop at #3 as they are supposed to. What it does show, twice, is a burst of
**sixteen** `font registered` lines in the middle of a session, several of them with
the same file size and one with a **face address that had already been used for a
font of a different size** - the client destroys and recreates its `FT_Face` objects
while it runs. The reporter's client is a repack whose faces include three CJK files
of 8-9 MiB.

Nothing in the log named the atlas, because nothing ever logged an eviction.

## Cause

Two things multiplied together: a per-glyph cost that was ~350x larger than it
looked, and a cycle that kept paying it.

### The amplifier: a full-surface lock on a MANAGED texture, once per glyph

`MSDFFont::UploadGlyphToAtlas` wrote its glyph like this:

```cpp
targetPage->texture->LockRect(0, &lockedRect, nullptr, 0);
...
unsigned char* dest = pBits + nextY * Pitch + nextX * 4;
```

The pages are `D3DPOOL_MANAGED` (`CreateAtlasPage`), which means the runtime keeps a
system-memory copy and re-uploads whatever the lock marked dirty. **A NULL rect marks
the whole subresource dirty.** The page is 2048x2048 A8R8G8B8, so every single glyph
- a 40 KiB cell - cost a **16 MiB** texture upload on the next draw that sampled the
page. DXVK does that upload through a staging slice, in a 32-bit process.

### The cycle: atlas evictions that could never stop

`docs/shared-atlas.md` ends on "What this does NOT fix: Capacity" - four shared pages
hold roughly 1650 glyphs, and ten typefaces at ~120 used characters each is ~1200,
"it fits, but with no headroom". Two things eat that headroom:

- **Sixteen faces, not ten.** The reporter's client registers sixteen, several of
  them the same file opened twice.
- **`ForgetFontEntries` reclaimed nothing.** When the client destroyed a face,
  `~MSDFFont` swept that typeface's entries out of the pages - correctly, they hold a
  `this` that is about to die - but left the shelf cursor (`nextX`/`nextY`/
  `rowHeight`) exactly where the dead typeface had pushed it. The space was gone for
  the rest of the session. The recreated face then laid the same glyphs out again in
  **fresh** cells. Every recreation burst therefore consumed atlas capacity that was
  never given back, and marched the atlas into eviction on a timer.

Once the atlas is full, an eviction is not a small event. `EvictOldestPage` clears a
page, invalidates the ~410 glyphs sitting on it whoever owns them, and bumps the
global `s_evictionCount` - and that counter is the version token in the top byte of
`CGxString::m_flags`, so `CheckGeometryHk` calls `ClearInstanceData()` on **every**
string. Every string is rebuilt, every invalidated glyph is asked for again in that
same frame, and each one comes back through `UploadGlyphToAtlas`.

410 glyphs x 16 MiB = **~6.5 GiB of texture uploads in one frame**. That is the 2-3
seconds. The 15-20 second spacing is how long the next batch of face churn takes to
fill the atlas again.

## The change

### 1. Lock only the glyph's rectangle (`MSDFFont::UploadGlyphToAtlas`)

The rect is passed to `LockRect`, so the dirty region - and the upload - is the glyph
cell instead of the page: **16 MiB -> ~40 KiB per glyph.** `pBits` then points at the
top-left of the locked region, so the `nextY * Pitch + nextX * 4` offset that belonged
to a full-surface lock goes with it.

A side effect worth naming: the placement search guarantees the cell is inside the
page, but the eviction fallback did not re-check it. With a rect, an out-of-range cell
makes `LockRect` fail and the glyph is rejected with a log line; before, the row loop
would have run off the end of the surface.

### 2. Give the space back when a typeface dies (`MSDFFont::ForgetFontEntries`)

If sweeping a typeface's entries leaves a page's `entries` **empty**, the page's shelf
cursor is rewound and the page is reused.

This is safe by construction: every successful upload records itself in
`page->entries`, and eviction clears the entries and the cursor together, so an empty
`entries` means nothing alive points into that page. It needs no eviction, no
full-page clear and no geometry rebuild. The stale pixels left behind are in the same
position as those on a freshly created page, which `CreateAtlasPage` does not clear
either.

### 3. Bound `HashFont` (`MSDFUtils.h`)

The hash walked every byte of the file with a 64-bit multiply - a three-multiply
sequence on the 32-bit build, about 30 ms for each of the 8-9 MiB CJK faces, paid
again for every face the client recreates. It now hashes the length plus a head,
middle and tail window of 64 KiB each.

Nothing on disk carries this value, so no user's cache is invalidated:
`MSDFManager::RegisterFont` turns it into a `fontId` that is only the first half of
the in-memory `BlockKey`, and the cache directory is named from the family and style
(`MSDFCache::GetCacheBasePath`).

### 4. Log the eviction

`docs/shared-atlas.md` tells the reader to watch for evictions multiplying in the log,
and nothing ever wrote a line for one - which is why issue #2 arrived with a log that
shows the symptom and not the cause. `EvictOldestPage` now logs the first ten and then
every hundredth (throttled because `Log()` opens the file per entry).

## Verification

Built 2026-09-12 (`build.bat` with `LEXARA_CMAKE` set), no new warnings.
**Not verified in game** - confirming it needs a session on a client that registers
this many faces.

What to look for in `lexara112.log`:

| line | meaning |
|---|---|
| no `atlas eviction` lines at all | the working set now fits; this is the good case |
| `atlas eviction #1..#10` and then silence | it settled after start-up |
| `atlas eviction` climbing steadily | capacity is still short - see below |

If evictions keep climbing, the lever `docs/shared-atlas.md` already names is still
there and is untouched by this change: **`SDF_RENDER_SIZE` 96 -> 64** in `MSDF.h`,
which cuts the glyph area by about 2.2x and raises the atlas to ~3600 glyphs, at the
cost of sharpness in very large text. It changes `MSDFCache`'s cache key, so the
`LEXARA` directory in `%TEMP%` is rebuilt on the next start.
