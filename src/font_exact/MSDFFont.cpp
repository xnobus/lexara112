#include "MSDFFont.h"
#include "../Logger.h"
#include "MSDFCache.h"
#include "MSDFValidator.h"
#include "MSDFUtils.h"
#include <atomic>

MSDFFont::MSDFFont(FT_Face face, const FT_Byte* fontData, FT_Long dataSize)
    : m_ftFace(face), m_msdfFont(nullptr), m_isValid(false), m_fontData(fontData), m_fontDataSize(dataSize)
{
    if (!face) return;

    m_msdfFont = CreateMSDFHandle(fontData, dataSize);
    if (!m_msdfFont) return;

    m_cache = MSDFCache::Acquire(fontData, dataSize,
        face->family_name ? face->family_name : "Unknown", face->style_name ? face->style_name : "",
        MSDF::SDF_RENDER_SIZE, MSDF::SDF_SPREAD);

    m_isValid = m_cache->GetManifestSize() || MSDF::ALLOW_UNSAFE_FONTS ||
        MSDFValidator::IsFontMSDFCompatible(m_msdfFont, &m_rejectedCodepoint);
    if (m_isValid && s_glyphPool.empty()) s_glyphPool.reserve(4096);
}

MSDFFont::MSDFFont(FT_Face face, const FT_Byte* fontData, FT_Long dataSize, FT_Long)
    : MSDFFont(face, fontData, dataSize) {
}

MSDFFont::~MSDFFont() {
    // [1.12] The pages are shared, so we do NOT release them here - that would take
    // them away from typefaces that are still drawing.
    //
    // Nor do we sweep anything out of them any more. The cells belong to the FILE
    // (s_glyphPool is keyed by its hash), not to this face, and the client drops and
    // recreates faces in bursts while it runs: a sweep would throw away cells the
    // very next face is about to ask for again. Keeping them is what makes a face
    // burst free - the recreated face finds the entries already in the pool and
    // uploads nothing. There is also no `this` left in AtlasPage::entries for an
    // eviction to reach into, which is what the old sweep was really protecting.
    m_cache.reset();
    if (m_msdfFont) {
        msdfgen::destroyFont(m_msdfFont);
        m_msdfFont = nullptr;
    }
}

MSDFFont* MSDFFont::Get(FT_Face face) {
    auto it = s_fontHandles.find(face);
    if (it != s_fontHandles.end() && it->second->IsValid()) {
        return it->second.get();
    }
    return nullptr;
}

void MSDFFont::Register(FT_Face face, const FT_Byte* data, FT_Long size) {
    if (s_fontHandles.find(face) != s_fontHandles.end()) return;
    auto font = std::make_unique<MSDFFont>(face, data, size);
    if (font->m_msdfFont && font->m_isValid) {
        s_fontHandles[face] = std::move(font);
        return;
    }
    // [1.12] A font turned away here is drawn by the client's own renderer, and issue
    // #3 arrived with nothing in the log but `handle=00000000` to show for it. Once
    // per file - the client opens a face per size and recreates them in bursts.
    static ankerl::unordered_dense::set<FontHash> s_reported;
    if (!s_reported.insert(HashFont(data, size)).second) return;
    const char* family = face->family_name ? face->family_name : "";
    if (!font->m_msdfFont) {
        Log("[MSDF] font NOT drawn by MSDF: msdfgen cannot load '%s' (size=%ld)", family, (long)size);
    } else {
        Log("[MSDF] font NOT drawn by MSDF: '%s' glyph U+%04X failed validation (size=%ld)",
            family, font->m_rejectedCodepoint, (long)size);
    }
}

void MSDFFont::Unregister(FT_Face face) {
    auto it = s_fontHandles.find(face);
    if (it != s_fontHandles.end()) {
        s_fontHandles.erase(it);
    }
}

void MSDFFont::ClearAllCache() {
    // Called from D3D::RegisterOnDestroy - the device is going away, so the page
    // textures have to be let go, and every typeface loses its glyphs at once.
    s_glyphPool.clear();
    s_atlasPages.clear();
    s_oldestPage = 0;
    ++s_evictionCount;
    ++s_readyEpoch;
}

void MSDFFont::Shutdown() {
    s_fontHandles.clear();
    s_glyphPool.clear();
    s_atlasPages.clear();
}

// [1.12] Invalidation instead of removal. s_glyphPool is a dense map
// (unordered_dense) - erase moves other elements around in it, and
// UploadGlyphToAtlas receives a `GlyphMetrics&` from that very map and holds that
// reference across the whole eviction. Zeroing the UVs does not disturb the map's
// layout, and GetGlyph already recognises an entry with a zero u1/v1 as "never
// uploaded" and sends it to the atlas again - that retry path already exists and is
// walked at every start-up, before the first frame.
void MSDFFont::InvalidateGlyph(const GlyphKey& key) {
    auto it = s_glyphPool.find(key);
    if (it == s_glyphPool.end()) return;
    it->second.u0 = 0.0f;
    it->second.v0 = 0.0f;
    it->second.u1 = 0.0f;
    it->second.v1 = 0.0f;
    it->second.atlasPageIndex = 0;
}

// [1.12] Clears the oldest page and invalidates EVERY glyph sitting on it,
// whoever owns it. The eviction counter is global, so CheckGeometryHk rebuilds the
// geometry of every string, not just of the typeface that happened to trigger the
// eviction - which is the point, because a page mixes typefaces.
int MSDFFont::EvictOldestPage() {
    if (s_atlasPages.empty()) return -1;
    if (s_oldestPage >= s_atlasPages.size()) s_oldestPage = 0;

    AtlasPage* page = s_atlasPages[s_oldestPage].get();
    const size_t dropped = page->entries.size();
    for (const GlyphKey& key : page->entries) InvalidateGlyph(key);
    page->Clear();

    if (page->texture) {
        D3DLOCKED_RECT fullRect;
        if (SUCCEEDED(page->texture->LockRect(0, &fullRect, nullptr, 0))) {
            memset(fullRect.pBits, 0, MSDF::ATLAS_SIZE * fullRect.Pitch);
            page->texture->UnlockRect(0);
        }
    }

    ++s_evictionCount;
    // Strings laid out with glyphs pending are versioned by this one instead; they
    // hold glyphs from the cleared page as well.
    ++s_readyEpoch;
    // [1.12] docs/shared-atlas.md tells the reader to watch for evictions
    // multiplying in the log - and nothing ever wrote a line for one, which is why
    // issue #2 arrived with a log that shows the symptom and not the cause. An
    // eviction costs a full-page clear plus a geometry rebuild of EVERY string, so
    // it is worth a line; throttled, because Log() opens the file per entry and a
    // thrashing atlas evicts often.
    if (s_evictionCount <= 10 || (s_evictionCount % 100) == 0) {
        Log("[MSDF] atlas eviction #%u: page %u cleared (%u glyphs dropped) - "
            "every string's geometry is rebuilt",
            s_evictionCount, (unsigned)s_oldestPage, (unsigned)dropped);
    }
    const int cleared = s_oldestPage;
    s_oldestPage = static_cast<uint16_t>((s_oldestPage + 1) % s_atlasPages.size());
    return cleared;
}

// [1.12] Counters for the ways GetGlyph can exit. The previous probe had a
// "static bool" guard and reported ONLY the first occurrence of each cause - which
// hid the distribution, and the single entry that did reach the log (a space) was
// the least interesting case of all. Counters show them all at once.
// The generation counters are touched by the worker threads, hence atomic.
static int gNullUploadCache = 0, gNullUploadGen = 0, gNullRetry = 0, gOkCache = 0, gOkPool = 0, gRequested = 0, gTotal = 0;
static std::atomic<int> gNullPixelSize{ 0 }, gNullLoadGlyph{ 0 }, gOkGen{ 0 }, gNoOutline{ 0 };

static void DumpGlyphStats() {
    Log("[MSDF] GetGlyph after %d calls: pool=%d cache=%d requested=%d gen=%d noOutline=%d",
        gTotal, gOkPool, gOkCache, gRequested, gOkGen.load(), gNoOutline.load());
    Log("       NULL: pixelSize=%d loadGlyph=%d uploadCache=%d uploadGen=%d retry=%d",
        gNullPixelSize.load(), gNullLoadGlyph.load(), gNullUploadCache, gNullUploadGen, gNullRetry);
}

const GlyphMetrics* MSDFFont::GetGlyph(uint32_t codepoint, bool* pending) {
    // [1.12] This used to run every 200 calls. At 2.3 million calls per session
    // that meant over 11,000 opens of the log file and hurt performance by itself.
    // [1.12] Statistics are now dumped on demand only (DumpGlyphStats from a
    // debugger, or after adding a call) - periodic logging cost more than it
    // measured: at 2.3 million calls per session it meant thousands of file opens.
    ++gTotal;
    const GlyphKey key{ m_cache->GetFontHash(), codepoint };
    auto pit = s_glyphPool.find(key);
    if (pit != s_glyphPool.end()) {
        // [1.12] An entry with zero UVs but a non-zero size is a glyph whose
        // upload to the atlas FAILED - usually because there was no D3D device yet
        // when it was generated (the prefetch runs from CheckGeometry, i.e. before
        // the client's first frame, while we only catch the device on its
        // EndScene). Without a retry such a glyph kept UVs of (0,0)-(0,0) forever:
        // the shader sampled the corner of the atlas, got sd = 0 and, with the
        // outline enabled, painted a SOLID BLACK rectangle.
        // That is exactly what was showing up instead of letters.
        GlyphMetrics& cached = pit->second;
        const bool neverUploaded = (cached.u1 == 0.0f && cached.v1 == 0.0f);
        if (neverUploaded && cached.width > 0 && cached.height > 0) {
            // [1.12] pixelData IS ONLY VALID UNTIL THE END OF THE CURRENT CALL.
            // It points either into the `storage` buffer, which right after the
            // upload goes to MSDFCache::StoreGlyph and dies at the next
            // FlushPendingWrites, or into a block of the cache file mapped by
            // MSDFManager, which can also disappear. That is why a successful
            // upload nulls this pointer (see the end of UploadGlyphToAtlas), and
            // why NULL here means "the pixels have to be fetched again".
            //
            // Without this, eviction from the shared atlas was a death sentence: an
            // invalidated glyph came back through here and the memcpy in the row
            // loop read freed memory. Crash 2026-09-09 22:59 (`ECX=EDX=0x180` =
            // width*4 at width=96, source `ESI` unmapped) - a regression introduced
            // together with the shared atlas.
            //
            // We drop the entry and take the full path (cache -> generation). That
            // is safe despite the dense map: no reference into the pool is in
            // anyone's hands yet, ours or the caller's.
            if (!cached.pixelData) {
                s_glyphPool.erase(pit);
                return GetGlyph(codepoint, pending);
            }
            if (!UploadGlyphToAtlas(cached, codepoint)) { ++gNullRetry; return nullptr; }
        }
        ++gOkPool;
        return &cached;
    }

    auto [it, inserted] = s_glyphPool.try_emplace(key);
    GlyphMetrics& metrics = it->second;

    if (m_cache->TryLoadGlyph(codepoint, metrics)) {
        // [1.12] The upload result MUST be checked. It used to be ignored, so a
        // failed upload left an entry with zero UVs in the pool.
        if (!UploadGlyphToAtlas(metrics, codepoint)) {
            ++gNullUploadCache;
            s_glyphPool.erase(it);
            return nullptr;
        }
        ++gOkCache;
        return &metrics;
    }

    // [1.12] Not in the cache: a worker generates it (MSDFWorker.h has the numbers),
    // IntegrateGeneratedGlyphs puts the result in the cache, and the string is laid
    // out again - then TryLoadGlyph above finds it. This used to be generated right
    // here, on the client's rendering thread.
    s_glyphPool.erase(it);
    if (!RequestGeneration(codepoint)) return nullptr;
    ++gRequested;
    if (pending) *pending = true;
    return nullptr;
}

bool MSDFFont::RequestGeneration(uint32_t codepoint) {
    if (!m_blob) {
        if (!m_cache || !m_fontData || m_fontDataSize <= 0) return false;
        std::weak_ptr<const FontBlob>& slot = s_blobs[m_cache->GetFontHash()];
        m_blob = slot.lock();
        if (!m_blob) {
            auto blob = std::make_shared<FontBlob>();
            blob->hash = m_cache->GetFontHash();
            blob->bytes.assign(m_fontData, m_fontData + m_fontDataSize);
            m_blob = std::move(blob);
            slot = m_blob;
        }
    }
    MSDFWorker::Request(m_blob, codepoint);
    return true;
}

void MSDFFont::IntegrateGeneratedGlyphs() {
    static std::vector<MSDFWorker::Result> results;
    results.clear();
    if (!MSDFWorker::Drain(results)) return;

    for (MSDFWorker::Result& r : results) {
        // No cache means every face of that file is gone - so is every string that
        // wanted the glyph.
        if (const std::shared_ptr<MSDFCache> cache = MSDFCache::Find(r.hash)) {
            cache->StoreGlyph(std::move(r.glyph));
        }
    }
    results.clear();
    MSDFCache::FlushOverCeiling(1);
    ++s_readyEpoch;
}

void MSDFFont::BuildGlyph(FT_Face face, msdfgen::FontHandle* handle, uint32_t codepoint, GlyphMetricsToStore& storage) {
    storage.codepoint = codepoint;

    if (FT_Set_Pixel_Sizes(face, MSDF::SDF_RENDER_SIZE, MSDF::SDF_RENDER_SIZE) != 0) {
        ++gNullPixelSize;
        return;
    }

    FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0) {
        ++gNullLoadGlyph;
        return;
    }

    storage.bitmapLeft = face->glyph->bitmap_left;
    storage.bitmapTop = face->glyph->bitmap_top;

    const bool hasOutline = face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
        face->glyph->outline.n_contours > 0;
    if (!hasOutline) {
        ++gNoOutline;
        return;
    }

    FT_BBox bbox;
    FT_Outline_Get_BBox(&face->glyph->outline, &bbox);

    uint16_t w = static_cast<uint16_t>(std::max(0, static_cast<int>(((bbox.xMax + 63) >> 6) - (bbox.xMin >> 6))));
    uint16_t h = static_cast<uint16_t>(std::max(0, static_cast<int>(((bbox.yMax + 63) >> 6) - (bbox.yMin >> 6))));

    if (w == 0 || h == 0) {
        static std::atomic<int> n{ 0 };
        if (++n <= 10) Log("[MSDF] glyph code=%u has a ZERO bbox (w=%u h=%u) - it will stay empty", codepoint, w, h);
        return;
    }

    uint16_t sdfW = w + 2 * MSDF::SDF_SPREAD;
    uint16_t sdfH = h + 2 * MSDF::SDF_SPREAD;
    storage.ownedPixelData.reserve(static_cast<size_t>(sdfW) * sdfH * 4);
    if (!GenerateMSDF(handle, storage.ownedPixelData, codepoint, sdfW, sdfH)) {
        // [1.12] A silent failure in the original: the block was skipped,
        // the glyph kept a zero size and vanished without a trace in the log.
        static std::atomic<int> n{ 0 };
        if (++n <= 10) Log("[MSDF] GenerateMSDF REFUSED for code=%u (sdf %ux%u)", codepoint, sdfW, sdfH);
        storage.ownedPixelData.clear();
        return;
    }
    storage.width = sdfW;
    storage.height = sdfH;
    storage.dataSize = static_cast<uint32_t>(storage.ownedPixelData.size());
    ++gOkGen;
}

MSDFFont::AtlasPage* MSDFFont::GetAtlasPage(size_t index) {
    if (index < s_atlasPages.size()) {
        return s_atlasPages[index].get();
    }
    return nullptr;
}

bool MSDFFont::CreateAtlasPage() {
    auto page = std::make_unique<AtlasPage>(MSDF::ATLAS_GUTTER);
    if (!D3D::CreateTexture(&page->texture, {
        .width = MSDF::ATLAS_SIZE,
        .height = MSDF::ATLAS_SIZE,
        .format = MSDF::D3DFMT,
        .pool = D3DPOOL_MANAGED
        })) {
        // [1.12] This was the one path with no instrumentation, and it was the one
        // responsible for 392 rejected glyphs: the first atlas page fills up and the
        // next one is never created.
        Log("[MSDF] CreateAtlasPage: CreateTexture %ux%u REFUSED (pages so far=%u, max=%u)",
            MSDF::ATLAS_SIZE, MSDF::ATLAS_SIZE,
            (unsigned)s_atlasPages.size(), (unsigned)MSDF::MAX_ATLAS_PAGES);
        return false;
    }
    Log("[MSDF] CreateAtlasPage: page %u created (texture=%p, shared atlas)",
        (unsigned)s_atlasPages.size(), (void*)page->texture);
    s_atlasPages.push_back(std::move(page));
    return true;
}

bool MSDFFont::UploadGlyphToAtlas(GlyphMetrics& metrics, uint32_t codepoint) {
    // [1.12] Two cases the original treated identically are now separated.
    // An EMPTY glyph (a space) is a success - there is nothing to write. But a
    // glyph with a non-zero size and NO pixels is a failure: this used to return
    // "success", so the entry stayed in the pool with UVs of (0,0)-(0,0) forever
    // and the letter could be invisible. That matches the missing letters in the UI.
    if (metrics.width > 0 && metrics.height > 0 && !metrics.pixelData) {
        static int n = 0;
        if (++n <= 5) Log("[MSDF] upload: size %ux%u but NO pixels (code=%u) - rejecting",
                          metrics.width, metrics.height, codepoint);
        return false;
    }
    if (!metrics.pixelData || metrics.width == 0 || metrics.height == 0) {
        static bool l = false;
        if (!l) { l = true; Log("[MSDF] upload: no pixels (code=%u px=%p w=%u h=%u) - reporting success without a write",
                                codepoint, (const void*)metrics.pixelData, metrics.width, metrics.height); }
        return true;
    }

    int16_t pageIndex = -1;
    AtlasPage* targetPage = nullptr;

    for (size_t i = 0; i < s_atlasPages.size(); ++i) {
        AtlasPage* page = s_atlasPages[i].get();
        if (page->nextX + metrics.width + MSDF::ATLAS_GUTTER <= MSDF::ATLAS_SIZE &&
            page->nextY + metrics.height + MSDF::ATLAS_GUTTER <= MSDF::ATLAS_SIZE) {
            pageIndex = static_cast<int16_t>(i);
            targetPage = page;
            break;
        }
        int nextY = page->nextY + page->rowHeight + MSDF::ATLAS_GUTTER;
        if (nextY + metrics.height + MSDF::ATLAS_GUTTER <= MSDF::ATLAS_SIZE) {
            page->nextX = MSDF::ATLAS_GUTTER;
            page->nextY = nextY;
            page->rowHeight = 0;
            pageIndex = static_cast<int16_t>(i);
            targetPage = page;
            break;
        }
    }
    if (pageIndex == -1) {
        // [1.12] First try to add a page, and only then evict - and eviction is
        // now ALSO the answer to a CreateTexture refusal, not just to reaching
        // MAX_ATLAS_PAGES. A refused allocation used to end in returning false,
        // i.e. the glyph was lost for good; now that the pages are shared, there is
        // something to share out.
        if (s_atlasPages.size() < MSDF::MAX_ATLAS_PAGES && CreateAtlasPage()) {
            pageIndex = static_cast<int16_t>(s_atlasPages.size() - 1);
            targetPage = s_atlasPages.back().get();
        }
        else {
            const int cleared = EvictOldestPage();
            if (cleared < 0) { ++gNullUploadGen; return false; }
            pageIndex = static_cast<int16_t>(cleared);
            targetPage = s_atlasPages[cleared].get();
        }
    }
    // [1.12] A probe on EVERY failure path - without them "the upload failed" is
    // not enough to fix anything.
    if (pageIndex == -1 || !targetPage) {
        static bool l = false; if (!l) { l = true; Log("[MSDF] upload: no atlas page (code=%u)", codepoint); }
        return false;
    }
    if (!targetPage->texture) {
        static bool l = false; if (!l) { l = true; Log("[MSDF] upload: page without a texture (code=%u)", codepoint); }
        return false;
    }

    // [1.12] LOCK ONLY THE GLYPH'S OWN RECTANGLE, never the whole surface.
    //
    // This used to be LockRect(0, &r, nullptr, 0). On a D3DPOOL_MANAGED texture a
    // NULL rect makes the dirty region the ENTIRE subresource, and the runtime
    // (here DXVK) then re-uploads all of it on the next draw that samples the page:
    // 2048*2048*4 = 16 MiB PER GLYPH. A single atlas eviction invalidates roughly
    // 410 glyphs, every one of them comes straight back through here while the
    // strings are rebuilt, and the frame that does it moves ~6.5 GiB - the "huge
    // stutter every 15-20s for 2-3s" of issue #2. With the rect passed in, the same
    // upload is a padded glyph cell, on the order of 40 KiB.
    //
    // Pitch still describes a row of the LOCKED REGION, and pBits now points at its
    // top-left corner, so the destination is pBits itself - the nextY/nextX offset
    // that belonged to a full-surface lock has to go with it.
    const RECT glyphRect = {
        static_cast<LONG>(targetPage->nextX),
        static_cast<LONG>(targetPage->nextY),
        static_cast<LONG>(targetPage->nextX + metrics.width),
        static_cast<LONG>(targetPage->nextY + metrics.height)
    };
    D3DLOCKED_RECT lockedRect;
    const HRESULT hrLock = targetPage->texture->LockRect(0, &lockedRect, &glyphRect, 0);
    if (FAILED(hrLock)) {
        static bool l = false; if (!l) { l = true; Log("[MSDF] upload: LockRect hr=0x%08lX (code=%u)", hrLock, codepoint); }
        return false;
    }
    if (lockedRect.Pitch < metrics.width * 4) {
        static bool l = false; if (!l) { l = true; Log("[MSDF] upload: pitch=%d < %d (code=%u)", lockedRect.Pitch, metrics.width * 4, codepoint); }
        targetPage->texture->UnlockRect(0);
        return false;
    }

    const unsigned char* src = metrics.pixelData;
    unsigned char* dest = static_cast<unsigned char*>(lockedRect.pBits);
    for (uint16_t y = 0; y < metrics.height; ++y) {
        memcpy(dest, src, metrics.width * 4);
        dest += lockedRect.Pitch;
        src += metrics.width * 4;
    }
    targetPage->texture->UnlockRect(0);

    float atlasSize = static_cast<float>(MSDF::ATLAS_SIZE);
    metrics.u0 = static_cast<float>(targetPage->nextX) / atlasSize;
    metrics.v0 = static_cast<float>(targetPage->nextY) / atlasSize;
    metrics.u1 = static_cast<float>(targetPage->nextX + metrics.width) / atlasSize;
    metrics.v1 = static_cast<float>(targetPage->nextY + metrics.height) / atlasSize;
    metrics.atlasPageIndex = pageIndex;

    targetPage->nextX += metrics.width + MSDF::ATLAS_GUTTER;
    targetPage->rowHeight = std::max(targetPage->rowHeight, static_cast<int>(metrics.height));
    targetPage->entries.push_back(GlyphKey{ m_cache->GetFontHash(), codepoint });

    // [1.12] The pixels are in the atlas now, and this pointer survives at most
    // until the next cache flush. We null it so that a stale read is impossible by
    // construction - GetGlyph recognises the NULL and fetches the pixels again
    // instead of reading freed memory.
    metrics.pixelData = nullptr;

    return true;
}

bool MSDFFont::GenerateMSDF(std::vector<uint8_t>& outData, uint32_t codepoint, int sdfW, int sdfH) const {
    return GenerateMSDF(m_msdfFont, outData, codepoint, sdfW, sdfH);
}

bool MSDFFont::GenerateMSDF(msdfgen::FontHandle* handle, std::vector<uint8_t>& outData, uint32_t codepoint, int sdfW, int sdfH) {
    if (!handle || sdfW <= 0 || sdfH <= 0 || sdfW > 512 || sdfH > 512) return false;

    msdfgen::Shape shape;
    if (!msdfgen::loadGlyph(shape, handle, codepoint)) return false;

    if (shape.contours.empty()) {
        outData.assign(sdfW * sdfH * 4, 0);
        return true;
    }

    // [1.12] No geometry preprocessing - msdfgen's own default without Skia
    // (main.cpp:577). Upstream calls resolveShapeGeometry here, which needs Skia. The
    // port first stood orientContours in for it, but that one assumes even-odd fill
    // and no self-intersections (main.cpp:523): on overlapping contours it reversed
    // one of them, and the nonzero pass below then cut the overlap out of the glyph -
    // a gap across Cascadia Code's `E` stem, a hole in Bahnschrift's `A` crossbar.
    // overlapSupport and the sign pass handle overlapping and self-intersecting
    // contours by themselves. See docs/third-party-fonts.md.
    msdfgen::edgeColoringInkTrap(shape, 3.0, 0);

    auto bounds = shape.getBounds();
    double shapeW = bounds.r - bounds.l;
    double shapeH = bounds.t - bounds.b;
    if (shapeW <= 0 || shapeH <= 0) return false;

    double usableW = static_cast<double>(sdfW) - 2.0 * MSDF::SDF_SPREAD;
    double usableH = static_cast<double>(sdfH) - 2.0 * MSDF::SDF_SPREAD;
    if (usableW <= 0 || usableH <= 0) return false;

    double scale = std::min(usableW / shapeW, usableH / shapeH);
    msdfgen::Projection projection(
        msdfgen::Vector2(scale, scale),
        msdfgen::Vector2(MSDF::SDF_SPREAD / scale - bounds.l, MSDF::SDF_SPREAD / scale - bounds.b)
    );

    auto msdfBuf = m_msdfPool.AcquireSized(sdfW * sdfH * 3);
    auto sdfBuf = m_msdfPool.AcquireSized(sdfW * sdfH);

    msdfgen::BitmapRef<float, 3> msdfBitmap(msdfBuf.data(), sdfW, sdfH);
    msdfgen::BitmapRef<float, 1> sdfBitmap(sdfBuf.data(), sdfW, sdfH);

    msdfgen::MSDFGeneratorConfig config;
    config.overlapSupport = true;

    msdfgen::Range msdfRange(MSDF::SDF_SPREAD / scale);
    msdfgen::generateMSDF(msdfBitmap, shape, projection, msdfRange, config);
    msdfgen::SDFTransformation msdfTransform(projection, msdfRange);
    msdfgen::distanceSignCorrection(msdfBitmap, shape, msdfTransform, msdfgen::FillRule::FILL_NONZERO);

    msdfgen::Range sdfRange(MSDF::SDF_SPREAD / scale * 5.0);
    msdfgen::generateSDF(sdfBitmap, shape, projection, sdfRange);
    msdfgen::SDFTransformation sdfTransform(projection, sdfRange);
    msdfgen::distanceSignCorrection(sdfBitmap, shape, sdfTransform, msdfgen::FillRule::FILL_NONZERO);

    outData.resize(sdfW * sdfH * 4);
    uint8_t* dest = outData.data();
    const float* srcMSDF = msdfBuf.data();
    const float* srcSDF = sdfBuf.data();

    for (int i = 0; i < sdfW * sdfH; ++i) {
        dest[i * 4 + 0] = static_cast<uint8_t>(std::clamp(srcMSDF[i * 3 + 0] * 255.f, 0.f, 255.f));
        dest[i * 4 + 1] = static_cast<uint8_t>(std::clamp(srcMSDF[i * 3 + 1] * 255.f, 0.f, 255.f));
        dest[i * 4 + 2] = static_cast<uint8_t>(std::clamp(srcMSDF[i * 3 + 2] * 255.f, 0.f, 255.f));
        dest[i * 4 + 3] = static_cast<uint8_t>(std::clamp(srcSDF[i] * 255.f, 0.f, 255.f));
    }
    m_msdfPool.Release(std::move(msdfBuf));
    m_msdfPool.Release(std::move(sdfBuf));

    return true;
}

msdfgen::FontHandle* MSDFFont::CreateMSDFHandle(const FT_Byte* data, FT_Long size) {
    return !MSDF::g_msdfFreetype ? nullptr : msdfgen::loadFontData(MSDF::g_msdfFreetype, data, size);
}
