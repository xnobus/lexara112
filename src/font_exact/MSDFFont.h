#pragma once
#include "MSDF.h"
#include "MSDFCache.h"
#include "MSDFWorker.h"
#include "../ShutdownCheck.h"

class MSDFFont {
    friend class MSDFCache;
    friend class MSDFPregen;

private:
    // [1.12] What identifies a cell in the shared atlas: the font FILE and the
    // character - never the face. Every face the client opens on one file renders
    // its glyphs through the same MSDFCache at the same fixed SDF_RENDER_SIZE, so
    // the cell is byte for byte the same whatever size the face was opened at; the
    // draw scales it (ProcessGeometry: `scale = effectiveHeight / SDF_RENDER_SIZE`).
    // See the note on s_glyphPool.
    struct GlyphKey {
        FontHash hash = 0;
        uint32_t codepoint = 0;
        bool operator==(const GlyphKey& other) const {
            return hash == other.hash && codepoint == other.codepoint;
        }
    };

    struct GlyphKeyHash {
        using is_avalanching = void;
        uint64_t operator()(const GlyphKey& k) const noexcept {
            // splitmix64 finaliser over the two fields, as in MSDFWorker
            uint64_t x = k.hash ^ (static_cast<uint64_t>(k.codepoint) * 0x9E3779B97F4A7C15ULL);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            return x ^ (x >> 31);
        }
    };

    struct AtlasPage {
        IDirect3DTexture9* texture = nullptr;
        int nextX = 0, nextY = 0;
        int rowHeight = 0;
        int g = 0;
        // [1.12] A page is SHARED by every typeface, so a character code alone no
        // longer identifies a glyph - we have to know which FILE it came from in
        // order to invalidate the right entry of s_glyphPool on eviction.
        std::vector<GlyphKey> entries;

        AtlasPage(int gutter) : nextX(gutter), nextY(gutter), g(gutter) {}
        ~AtlasPage() { 
            if (texture && !g_isProcessTerminating) texture->Release(); 
        }

        void Clear() {
            nextX = g;
            nextY = g;
            rowHeight = 0;
            entries.clear();
        }
    };

public:
    MSDFFont(FT_Face face, const FT_Byte* fontData, FT_Long dataSize);
    MSDFFont(FT_Face face, const FT_Byte* fontData, FT_Long dataSize, FT_Long faceIndex);
    ~MSDFFont();

    bool IsValid() const { return m_isValid; }

    // [1.12] Static, because there is one atlas per process. Called through `->`
    // from MSDF.cpp and that still compiles - the language allows reaching a static
    // member through an object.
    static AtlasPage* GetAtlasPage(size_t index);
    static size_t GetAtlasPageCount() { return s_atlasPages.size(); }
    static size_t GetAtlasEvictionCount() { return s_evictionCount; }

    // nullptr when the glyph cannot be drawn now. `pending` (optional) is set when
    // that is because it is being generated - the caller should lay the string out
    // again once GetReadyEpoch() moves.
    const GlyphMetrics* GetGlyph(uint32_t codepoint, bool* pending = nullptr);

    // Worker thread: everything GetGlyph used to do inline for a glyph the cache did
    // not have. `face` and `handle` belong to the calling thread.
    static void BuildGlyph(FT_Face face, msdfgen::FontHandle* handle, uint32_t codepoint, GlyphMetricsToStore& out);

    // Rendering thread: hands finished glyphs to their caches and moves the epoch.
    static void IntegrateGeneratedGlyphs();
    // Moves whenever glyphs arrive or the atlas evicts - a string laid out with
    // glyphs still pending is rebuilt when it differs from the value at layout.
    static uint32_t GetReadyEpoch() { return s_readyEpoch; }

    static MSDFFont* Get(FT_Face face);
    static void Register(FT_Face face, const FT_Byte* data, FT_Long size);
    static void Unregister(FT_Face face);
    static void ClearAllCache();
    static void Shutdown();

private:
    static bool CreateAtlasPage();
    static int EvictOldestPage();
    static void InvalidateGlyph(const GlyphKey& key);
    bool UploadGlyphToAtlas(GlyphMetrics& metrics, uint32_t codepoint);
    bool GenerateMSDF(std::vector<uint8_t>& outData, uint32_t codepoint, int sdfW, int sdfH) const;
    static bool GenerateMSDF(msdfgen::FontHandle* handle, std::vector<uint8_t>& outData, uint32_t codepoint, int sdfW, int sdfH);
    bool RequestGeneration(uint32_t codepoint);

    static msdfgen::FontHandle* CreateMSDFHandle(const FT_Byte* data, FT_Long size);

    FT_Face m_ftFace;
    msdfgen::FontHandle* m_msdfFont;
    bool m_isValid;
    uint32_t m_rejectedCodepoint = 0;  // the glyph that failed validation, for the log

    // The client's buffer, alive exactly as long as this face. Copied into m_blob
    // only when the first glyph has to be generated - a font whose glyphs are all
    // cached never pays for the copy.
    const FT_Byte* m_fontData = nullptr;
    FT_Long m_fontDataSize = 0;
    std::shared_ptr<const FontBlob> m_blob;

    std::shared_ptr<MSDFCache> m_cache;

    inline static ankerl::unordered_dense::map<FT_Face, std::unique_ptr<MSDFFont>> s_fontHandles;

    // [1.12] ONE glyph pool per process, keyed by (font file, codepoint) - it used
    // to be a member, one pool per FACE.
    //
    // The client opens a face per size and recreates them in bursts: Jhinzuo's CJK
    // session registered 23 distinct MSDFFont objects for the one 9 338 332-byte
    // file. Each had its own pool, and a pool miss ends in UploadGlyphToAtlas, so
    // every one of them cut its OWN cell for the same character out of the SHARED
    // atlas - 23 identical 101x101 copies of every hanzi drawn.
    //
    // The whole atlas holds about 1 192 CJK cells (2048^2, four pages, gutter 14,
    // measured on the font of issue #4), so that duplication left room for only a
    // few dozen distinct characters before every new one evicted a page - and an
    // eviction clears ~300 glyphs, memsets 16 MiB and rebuilds the geometry of
    // EVERY string. That is the "serious lag" of issue #4: it needs a font with
    // more characters than the atlas can hold divided by the number of open faces,
    // i.e. a CJK font. Latin never noticed.
    //
    // Sharing is sound because the cell does not depend on the face: the pixels come
    // from the one MSDFCache the file already shares between its faces, generated at
    // the fixed SDF_RENDER_SIZE, and the metrics are scaled at draw time.
    // It also removes the MSDFFont* that used to sit in AtlasPage::entries, so an
    // eviction can no longer reach into a destroyed typeface.
    inline static ankerl::unordered_dense::map<GlyphKey, GlyphMetrics, GlyphKeyHash> s_glyphPool;

    // [1.12] ONE atlas per process instead of one per typeface. Previously every
    // MSDFFont got its own 2048x2048 A8R8G8B8 pages in D3DPOOL_MANAGED, i.e. 16 MiB
    // per page and up to 64 MiB per typeface - with ten typefaces drawn in a single
    // session the 32-bit client ran out of address space and CreateTexture bounced
    // with D3DERR_OUTOFVIDEOMEMORY despite 4 GB of free VRAM (session 2026-09-09
    // 21:56, crash in DrawIndexedPrimitive). The upper bound is now
    // MAX_ATLAS_PAGES * ATLAS_SIZE^2 * 4 B = 64 MiB for the whole process,
    // regardless of how many typefaces there are.
    //
    // The page index still fits in two bits, because it travels in the signs of the
    // UVs (MSDF.cpp: uSign/vSign) and the shader uses it to pick sampler s12-s15.
    // A shared atlas does not change that encoding - it only changes whose glyphs
    // sit on those four pages.
    inline static std::vector<std::unique_ptr<AtlasPage>> s_atlasPages;
    inline static uint16_t s_oldestPage = 0;
    inline static uint32_t s_evictionCount = 0;
    inline static uint32_t s_readyEpoch = 0;

    // One copy of a file's bytes however many faces are open on it.
    inline static ankerl::unordered_dense::map<FontHash, std::weak_ptr<const FontBlob>> s_blobs;

    inline static thread_local VectorPool<float> m_msdfPool;
};
