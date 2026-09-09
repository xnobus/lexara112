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
        // [1.12] Strona jest WSPOLNA dla wszystkich krojow, wiec sam kod znaku
        // nie identyfikuje juz glifu - trzeba wiedziec, czyj on jest, zeby
        // przy eksmisji uniewaznic wpis we wlasciwym m_glyphPool.
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

    // [1.12] Statyczne, bo atlas jest jeden na proces. Wolane przez `->`
    // z MSDF.cpp i to nadal sie kompiluje - jezyk pozwala siegnac do skladowej
    // statycznej przez obiekt.
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

    // [1.12] Ile wpisow ten kroj ma we wspolnym atlasie. Zero znaczy, ze przy
    // niszczeniu nie ma po co dotykac stron - i to wlasnie ratuje pregen,
    // ktory tworzy MSDFFont na watkach roboczych i NIGDY nic nie uklada
    // w atlasie (idzie prosto do GenerateMSDF). Bez tego straznika ich
    // destruktory chodzilyby po stanie dzielonym z watkiem rysujacym.
    size_t m_atlasEntryCount = 0;

    std::unique_ptr<MSDFCache> m_cache;

    ankerl::unordered_dense::map<uint32_t, GlyphMetrics> m_glyphPool;

    inline static ankerl::unordered_dense::map<FT_Face, std::unique_ptr<MSDFFont>> s_fontHandles;

    // [1.12] JEDEN atlas na proces zamiast jednego na kroj. Wczesniej kazdy
    // MSDFFont dostawal wlasne strony 2048x2048 A8R8G8B8 w D3DPOOL_MANAGED,
    // czyli 16 MiB na strone i do 64 MiB na kroj - przy dziesieciu krojach
    // narysowanych w jednej sesji klient 32-bitowy wyczerpywal przestrzen
    // adresowa i CreateTexture odbijalo D3DERR_OUTOFVIDEOMEMORY mimo 4 GB
    // wolnego VRAM-u (sesja 2026-09-09 21:56, crash w DrawIndexedPrimitive).
    // Teraz gorna granica to MAX_ATLAS_PAGES * ATLAS_SIZE^2 * 4 B = 64 MiB
    // na caly proces, niezaleznie od liczby krojow.
    //
    // Indeks strony dalej mieszczy sie na dwoch bitach, bo jedzie znakami UV
    // (MSDF.cpp: uSign/vSign) i shader wybiera nim sampler s12-s15. Wspolny
    // atlas nie zmienia tego kodowania - zmienia tylko to, czyje glify leza
    // na tych czterech stronach.
    inline static std::vector<std::unique_ptr<AtlasPage>> s_atlasPages;
    inline static uint16_t s_oldestPage = 0;
    inline static uint32_t s_evictionCount = 0;

    inline static thread_local VectorPool<float> m_msdfPool;
};
