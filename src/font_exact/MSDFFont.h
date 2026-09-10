#pragma once
#include "MSDF.h"
#include "MSDFCache.h"
#include "../ShutdownCheck.h"

class MSDFFont {
    friend class MSDFCache;
    friend class MSDFPregen;

private:
    struct AtlasPage {
        IDirect3DTexture9* texture = nullptr;
        int nextX = 0, nextY = 0;
        int rowHeight = 0;
        int g = 0;
        // [1.12] A page is SHARED by every typeface, so a character code alone no
        // longer identifies a glyph - we have to know whose it is in order to
        // invalidate the entry in the right m_glyphPool on eviction.
        std::vector<std::pair<MSDFFont*, uint32_t>> entries;

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

    const GlyphMetrics* GetGlyph(uint32_t codepoint);

    static MSDFFont* Get(FT_Face face);
    static void Register(FT_Face face, const FT_Byte* data, FT_Long size);
    static void Unregister(FT_Face face);
    static void ClearAllCache();
    static void Shutdown();

private:
    static bool CreateAtlasPage();
    static int EvictOldestPage();
    static void ForgetFontEntries(MSDFFont* font);
    void InvalidateGlyph(uint32_t codepoint);
    bool UploadGlyphToAtlas(GlyphMetrics& metrics, uint32_t codepoint);
    bool GenerateMSDF(std::vector<uint8_t>& outData, uint32_t codepoint, int sdfW, int sdfH) const;

    static msdfgen::FontHandle* CreateMSDFHandle(const FT_Byte* data, FT_Long size);

    FT_Face m_ftFace;
    msdfgen::FontHandle* m_msdfFont;
    bool m_isValid;

    // [1.12] How many entries this typeface has in the shared atlas. Zero means
    // there is no reason to touch the pages on destruction - and that is exactly
    // what saves the pregen, which creates MSDFFont objects on worker threads and
    // NEVER places anything in the atlas (it goes straight to GenerateMSDF).
    // Without this guard their destructors would walk over state shared with the
    // rendering thread.
    size_t m_atlasEntryCount = 0;

    std::unique_ptr<MSDFCache> m_cache;

    ankerl::unordered_dense::map<uint32_t, GlyphMetrics> m_glyphPool;

    inline static ankerl::unordered_dense::map<FT_Face, std::unique_ptr<MSDFFont>> s_fontHandles;

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

    inline static thread_local VectorPool<float> m_msdfPool;
};
