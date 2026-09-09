#include "MSDF.h"
#include "MSDFFont.h"
#include "MSDFCache.h"
#include "MSDFManager.h"
#include "MSDFShaders.h"
#include "Utils.h"
#include "Hooks.h"
#include "../Logger.h"
#include <map>
#include <string>
#include <cstdio>
#include <ranges>

namespace {
    uint32_t g_runtimeVBSize = 0;

    IDirect3DPixelShader9* s_cachedPS = nullptr;
    IDirect3DVertexShader9* s_cachedVS = nullptr;

    // [1.12] Definicje nizej, przy CGxuFontRenderBatchHk; WriteGeometryHk stoi
    // wyzej w pliku i wola je pierwszy.
    void BindMsdfShaders(IDirect3DDevice9* device);
    void UnbindMsdfShaders(IDirect3DDevice9* device);

    // ------------------------------------------------------------------
    // [1.12] Przelaczniki z pliku `lexara112.cfg` obok klienta.
    //
    // Powod: kazda hipoteza "ktora latka psuje tekst" kosztowala dotad
    // przebudowe + wejscie do gry. Z tym plikiem bisekcje robi sie samym
    // edytorem tekstu. Format: jedna linia `nazwa=0` albo `nazwa=1`,
    // linie z `#` pomijane. Brak pliku = wszystko wlaczone (zachowanie
    // domyslne portu).
    //
    // Nazwy odpowiadaja miejscom latania z _lexara-port-mapa.md:
    //   site_initbuf   site_allocbuf  site_checkgeom  site_checkcall
    //   site_bufstream site_bufalloc1 site_bufalloc2  site_bufalloc3
    //   site_procbatch site_glyphy
    //   hook_renderbatch hook_renderglyph hook_getglyph
    //   hook_checkgeom hook_writegeom hook_initline
    //   ft_hooks  shaders
    // ------------------------------------------------------------------
    std::string g_cfgText;

    bool CfgOn(const char* key) {
        if (g_cfgText.empty()) return true;
        std::string needle = std::string(key) + "=";
        size_t pos = 0;
        while ((pos = g_cfgText.find(needle, pos)) != std::string::npos) {
            const bool lineStart = (pos == 0) || g_cfgText[pos - 1] == '\n' || g_cfgText[pos - 1] == '\r';
            if (lineStart) {
                const size_t v = pos + needle.size();
                if (v < g_cfgText.size()) return g_cfgText[v] != '0';
            }
            pos += needle.size();
        }
        return true;
    }

    void LoadCfg() {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) *(slash + 1) = 0;
        strcat_s(path, "lexara112.cfg");

        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) {
            Log("[MSDF] brak lexara112.cfg - wszystkie latki WLACZONE");
            return;
        }
        char buf[4096];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        g_cfgText.assign(buf, n);
        Log("[MSDF] wczytano lexara112.cfg (%u B)", (unsigned)n);
    }

    // [1.12] Straznice sond - kazda sonda ma wypisac sie RAZ. Bez tego log
    // rosnie z kazda klatka i sam staje sie problemem wydajnosciowym.
    bool g_logBind = false, g_logWrite = false, g_logFont = false;

    // [1.12] Dziesiec miejsc latania. Adresy z etapu 1 (_lexara-port-mapa.md),
    // w komentarzu odpowiednik 3.3.5. Kazde ma w 1.12 tyle samo albo wiecej bajtow
    // niz detour, wiec zaden nie wymagal przesuwania granicy.
    auto(*CGxString__CheckGeometry_call)() = reinterpret_cast<void(*)()>(0x005C9019);          // 006C4B09
    constexpr uintptr_t CGxString__CheckGeometry_call_jmpback = 0x005C9020;                    // 006C4B10

    auto(*CGxString__CheckGeometry_site)() = reinterpret_cast<void(*)()>(0x005C9003);          // 006C4AF3
    constexpr uintptr_t CGxString__CheckGeometry_site_loopstart = 0x005C9010;                  // 006C4B00

    auto(*CGxString__GetGlyphYMetrics_site)() = reinterpret_cast<void(*)()>(0x005D137A);       // 006C8C71
    constexpr uintptr_t CGxString__GetGlyphYMetrics_site_jmpback = 0x005D1380;                 // 006C8C77

    auto(*CGxDevice__AllocateFontIndexBuffer_site)() = reinterpret_cast<void(*)()>(0x005C933F);// 006C480C
    constexpr uintptr_t CGxDevice__AllocateFontIndexBuffer_site_jmpback = 0x005C9344;          // 006C4811

    auto(*CGxDevice__InitFontIndexBuffer_site)() = reinterpret_cast<void(*)()>(0x005C92F7);    // 006C47BD
    constexpr uintptr_t CGxDevice__InitFontIndexBuffer_site_jmpback = 0x005C930F;              // 006C47D8

    // UWAGA (R1): te 5 bajtow jest rowniez celem skoku `je 005C91A9` z 005C8FF3
    // (wczesne wyjscie, gdy bufor indeksow nie jest zainicjowany) - wejscie w ten
    // skok po zalataniu to skok w srodek instrukcji. Tak samo bylo w 3.3.5;
    // gwarancja jest hak Init, ktory nie pozwala wskaznikowi bufora byc zerem.
    auto(*IGxuFontProcessBatch_site)() = reinterpret_cast<void(*)()>(0x005C91A8);              // 006C4CC4
    constexpr uintptr_t IGxuFontProcessBatch_site_jmpback = 0x005C91AD;                        // 006C4CC9

    auto(*CGxDevice__BufStream_site)() = reinterpret_cast<void(*)()>(0x005C904A);              // 006C4B40
    constexpr uintptr_t CGxDevice__BufStream_site_jmpback = 0x005C904F;                        // 006C4B45

    auto(*bufalloc_1_site)() = reinterpret_cast<void(*)()>(0x005C9067);                        // 006C4B64
    constexpr uintptr_t bufalloc_1_site_jmpback = 0x005C9073;                                  // 006C4B70

    auto(*bufalloc_2_site)() = reinterpret_cast<void(*)()>(0x005C915D);                        // 006C4C67
    constexpr uintptr_t bufalloc_2_site_jmpback = 0x005C917B;                                  // 006C4C8A

    auto(*bufalloc_3_site)() = reinterpret_cast<void(*)()>(0x005C9129);                        // 006C4C36
    constexpr uintptr_t bufalloc_3_site_jmpback = 0x005C913D;                                  // 006C4C4B

    bool s_msdfInitHookArmed = false;
    std::vector<uint8_t> s_prefetchPayload;

    void __cdecl PrefetchCodepoints(CGxString* pThis) {
        if (s_prefetchPayload.empty()) return;
        // [1.12] Bez urzadzenia nie ma atlasu, wiec prefetch tylko wygenerowalby
        // glify, ktorych nie ma gdzie zapisac. Odkladamy je na pozniej.
        if (!D3D::GetDevice()) return;
        if (!pThis || reinterpret_cast<uintptr_t>(pThis) & 1) return;

        if (MSDFFont* fontHandle = MSDFFont::Get(pThis->GetFontFace())) {
            std::ranges::sort(s_prefetchPayload);
            s_prefetchPayload.erase(std::ranges::unique(s_prefetchPayload).begin(), s_prefetchPayload.end());
            for (uint32_t codepoint : s_prefetchPayload) {
                fontHandle->GetGlyph(codepoint);
            }
        }
        s_prefetchPayload.clear();
    }

    void __fastcall ProcessGeometry(CGxString* pThis) {
        if (!(pThis->m_flags & 0x40000000)) return;
        if (!MSDF::ENABLED) return;

        // [1.12] Bez urzadzenia nie da sie utworzyc strony atlasu
        // (D3D::CreateTexture odbija na "if (!device) return false"), a kazdy
        // glif dotkniety w tym czasie konczyl jako odrzucony. Log pokazywal
        // "CreateAtlasPage: ODMOWIL (stron dotad=0)" bez zadnego HRESULT-u,
        // czyli porazke PRZED wywolaniem device->CreateTexture.
        //
        // Ta funkcja leci z CheckGeometry, czyli podczas ukladania tekstu,
        // ktore potrafi wyprzedzic pierwsza klatke klienta - a urzadzenie
        // lapiemy dopiero na jego EndScene. Odkladamy wiec robote na pozniej;
        // klient i tak przeliczy geometrie w kolejnej klatce.
        if (!D3D::GetDevice()) return;

        MSDFFont* fontHandle = MSDF::ENABLED ? MSDFFont::Get(pThis->GetFontFace()) : nullptr;
        if (!fontHandle) return;

        CGxFontGeomBatch* batch = pThis->m_geomBuffers[0];
        if (!batch || batch->m_verts.m_count < 4) return;

        TSGrowableArray<CGxFontVertex>& verts = batch->m_verts;
        if (verts.m_count < 4) return;

        CGxFont* fontObj = pThis->m_fontObj;
        const uint32_t flags = fontObj->m_atlasPages[0].m_flags;
        const bool is3d = pThis->m_flags & 0x80;
        const double fontSizeMult = pThis->m_fontSizeMult;
        const double fontOffs = !is3d ? ((flags & 8) ? 4.5 : ((flags & 1) ? 2.5 : 0.0)) : 0.0;
        const double baselineOffs = (fontOffs > 0.0) ? 1.0 : 0.0;
        const double scale = (is3d ? fontSizeMult : CGxuFont::GetFontEffectiveHeight(is3d, fontSizeMult) * 0.98) / MSDF::SDF_RENDER_SIZE;
        const double pad = MSDF::SDF_SPREAD * scale;

        for (uint32_t q = 0; q < verts.m_count; q += 4) {
            CGxFontVertex* vBase = &verts.m_data[q];
            if (vBase[0].u > 1.0f) {
                const uint32_t codepoint = vBase[0].u - 1.0f;

                const GlyphMetrics* gm = fontHandle->GetGlyph(codepoint);
                if (!gm) continue;

                CGxGlyphCacheEntry* entry = fontObj->GetOrCreateGlyphEntry(codepoint);
                if (!entry) continue;

                CGxFontVertex* vert0 = &vBase[0];
                CGxFontVertex* vert1 = &vBase[1];
                CGxFontVertex* vert2 = &vBase[2];
                CGxFontVertex* vert3 = &vBase[3];

                const double leftOffs = fontObj->GetBearingX(entry, is3d, fontSizeMult);
                const double bitmapLeft = is3d ? leftOffs : gm->bitmapLeft * scale - leftOffs;

                const double newLeft = static_cast<double>(vert0->pos.X) + (bitmapLeft != leftOffs ? bitmapLeft + 1.0 : 0.0) - pad + fontOffs * 0.5;
                const double newRight = newLeft + (gm->width * scale);

                const double newTop = static_cast<double>(vert1->pos.Y) + (gm->bitmapTop * scale) + pad - baselineOffs;
                const double newBottom = newTop - (gm->height * scale);

                vert0->pos.X = static_cast<float>(newLeft);  vert0->pos.Y = static_cast<float>(newBottom);
                vert1->pos.X = static_cast<float>(newLeft);  vert1->pos.Y = static_cast<float>(newTop);
                vert2->pos.X = static_cast<float>(newRight); vert2->pos.Y = static_cast<float>(newBottom);
                vert3->pos.X = static_cast<float>(newRight); vert3->pos.Y = static_cast<float>(newTop);

                const float u0 = gm->u0;
                const float u1 = gm->u1;
                const float v0 = gm->v0;
                const float v1 = gm->v1;

                const float uSign = (gm->atlasPageIndex & 1) ? -1.0f : 1.0f;
                const float vSign = (gm->atlasPageIndex & 2) ? -1.0f : 1.0f;

                vert0->u = u0 * uSign; vert0->v = v0 * vSign;
                vert1->u = u0 * uSign; vert1->v = v1 * vSign;
                vert2->u = u1 * uSign; vert2->v = v0 * vSign;
                vert3->u = u1 * uSign; vert3->v = v1 * vSign;
            }
        }
        pThis->m_flags &= ~0x40000000;

        uint32_t versionToken = (fontHandle->GetAtlasEvictionCount() & 0x7F) | 0x80;
        pThis->m_flags = (pThis->m_flags & 0x00FFFFFF) | (versionToken << 24);
    }

    bool __fastcall CGxString__CheckGeometryHk(CGxString* pThis) {
        if (MSDFFont* fontHandle = MSDFFont::Get(pThis->GetFontFace())) {
            uint32_t highByte = (pThis->m_flags >> 24) & 0xFF;
            if ((highByte & 0x80) != 0) {
                uint8_t storedVersion = highByte & 0x7F;
                uint8_t currentVersion = static_cast<uint8_t>(fontHandle->GetAtlasEvictionCount() & 0x7F);
                if (storedVersion != currentVersion) {
                    pThis->ClearInstanceData();
                    pThis->m_flags &= 0x00FFFFFF;
                }
            }
        }
        CGxFontGeomBatch* batch = pThis->m_geomBuffers[0];
        if (!batch || !batch->m_verts.m_data) return pThis->CheckGeometry();

        g_runtimeVBSize += pThis->GetVertCountForPage(0);
        return pThis->CheckGeometry();
    }

    void __fastcall CGxString__WriteGeometryHk(CGxString* pThis, void* edx, int destPtr, int index, int vertIndex, int vertCount) {
        pThis->WriteGeometry(destPtr, index, vertIndex, vertCount);

        MSDFFont* fontHandle = MSDFFont::Get(pThis->GetFontFace());

        // [1.12] Sonda rozdzielajaca: RenderGlyph czcionke ROZPOZNAJE (widac to po
        // przesunieciu w pionie), a WriteGeometry nie. Roznica jest tylko taka, ze
        // RenderGlyph dostaje FT_Face w argumencie, a tu idzie ono przez
        // m_fontObj (+0x44) -> m_ftWrapper (+0x70) -> GetFontFace (005D0370).
        if (!g_logWrite) {
            g_logWrite = true;
            CGxFont* fo = pThis->m_fontObj;
            void* wrap = fo ? fo->m_ftWrapper : nullptr;
            Log("[MSDF] WriteGeometry #1: fontObj=%p ftWrapper=%p face=%p handle=%p",
                fo, wrap, pThis->GetFontFace(), fontHandle);
        }

        if (!fontHandle) {
            if (IDirect3DDevice9* device = D3D::GetDevice()) {
                constexpr float resetControl[4] = { 0, 0, 0, 0 };
                device->SetPixelShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
                device->SetVertexShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
                // [1.12] Czcionka nieobslugiwana przez MSDF - wracamy na potok staly.
                UnbindMsdfShaders(device);
            }
            return;
        }

        IDirect3DDevice9* device = D3D::GetDevice();
        if (!device) {
            static bool logged = false;
            if (!logged) { logged = true; Log("[MSDF] WriteGeometry: BRAK URZADZENIA - sciezka MSDF nieaktywna"); }
            return;
        }

        if (!g_logWrite) {
            g_logWrite = true;
            CGxFontGeomBatch* b0 = pThis->m_geomBuffers[0];
            Log("[MSDF] pierwszy WriteGeometry z czcionka MSDF: batch=%p verts=%u",
                b0, b0 ? b0->m_verts.m_count : 0u);
            if (b0 && b0->m_verts.m_data && b0->m_verts.m_count >= 4) {
                CGxFontVertex* v = b0->m_verts.m_data;
                for (int i = 0; i < 4; ++i) {
                    Log("       v%d pos=(%.2f, %.2f, %.2f) uv=(%.4f, %.4f)",
                        i, v[i].pos.X, v[i].pos.Y, v[i].pos.Z, v[i].u, v[i].v);
                }
            }
        }

        {
            static bool logged = false;
            if (!logged) {
                logged = true;
                const uint32_t pages = fontHandle->GetAtlasPageCount();
                Log("[MSDF] atlas: stron=%u", pages);
                for (uint32_t i = 0; i < pages; ++i) {
                    auto* ap = fontHandle->GetAtlasPage(i);
                    Log("       strona %u: wpis=%p tekstura=%p", i, ap, ap ? ap->texture : nullptr);
                }
                const uint32_t flags = pThis->m_fontObj->m_atlasPages[0].m_flags;
                Log("[MSDF] flagi czcionki=0x%08X -> control.y=%.1f (0=bez obwodki, 1/2=obwodka)",
                    flags, (double)((flags & 8) ? 2.0f : ((flags & 1) ? 1.0f : 0.0f)));
            }
        }

        // [1.12] Tu, a nie w haku klienta - patrz komentarz przy BindMsdfShaders.
        BindMsdfShaders(device);

        for (uint32_t pageIdx = 0; pageIdx < fontHandle->GetAtlasPageCount(); ++pageIdx) {
            auto* atlasTexture = fontHandle->GetAtlasPage(pageIdx);
            if (atlasTexture && atlasTexture->texture) {
                uint32_t slot = (15 - MSDF::MAX_ATLAS_PAGES + 1) + pageIdx;
                device->SetTexture(slot, atlasTexture->texture);
                device->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                device->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                device->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            }
        }

        const uint32_t flags = pThis->m_fontObj->m_atlasPages[0].m_flags;
        const bool is3d = pThis->m_flags & 0x80;
        const float controlFlag[4] = {
            is3d ? pThis->m_fontObj->m_rasterTargetSize : static_cast<float>(CGxuFont::GetFontEffectiveHeight(is3d, pThis->m_fontSizeMult)),
            is3d ? 0.0f : ((flags & 8) ? 2.0f : ((flags & 1) ? 1.0f : 0.0f)),
            MSDF::SDF_SPREAD, MSDF::ATLAS_SIZE
        };
        device->SetPixelShaderConstantF(MSDF::SDF_CONTROL_REG, controlFlag, 1);
        device->SetVertexShaderConstantF(MSDF::SDF_CONTROL_REG, controlFlag, 1);
    }

    // ------------------------------------------------------------------
    // [1.12] Wiazanie shaderow. Klient 1.12 rysuje tekst potokiem STALYM -
    // nie ma obiektu shadera czcionek, ktoremu mozna podmienic bajtkod.
    // Ustawiamy wiec wlasne shadery na urzadzeniu przed rysowaniem partii
    // i zdejmujemy je po, zeby reszta interfejsu wrocila na potok staly.
    //
    // Poniewaz vs_3_0 zastepuje transformacje potoku stalego, macierz
    // World*View*Projection trzeba podac samemu w c0..c3. Klient ustawia te
    // trzy macierze normalnie, wiec czytamy je z urzadzenia.
    // ------------------------------------------------------------------

    // Przy `mul(pos, WorldViewProj)` w HLSL i domyslnym pakowaniu kolumnowym
    // macierz podaje sie TRANSPONOWANA. To jedyne miejsce w porcie, ktorego
    // nie dalo sie rozstrzygnac statycznie - przy pierwszym tescie w grze
    // objawem zlego wyboru jest tekst poza ekranem albo niewidoczny.
    // Wtedy: zmienic te stala na false i zbudowac ponownie.
    constexpr bool MSDF_WVP_TRANSPOSE = true;

    // [1.12] DIAGNOSTYKA. Rozdziela dwie rozne przyczyny "nie widac tekstu":
    //   - geometria trafia poza ekran (zla macierz WVP), albo
    //   - geometria jest dobra, ale atlas jest pusty, wiec sd = 0 i krycie = 0.
    // Przy MSDF_DEBUG_SOLID pixel shader maluje sciezke MSDF na jednolita
    // magente z pelnym kryciem, calkowicie pomijajac atlas. Wtedy:
    //   widac magentowe prostokaty w miejscach tekstu -> geometria OK, wina atlasu
    //   nie widac nic                                  -> wina macierzy/geometrii
    // Ustawic z powrotem na false po rozstrzygnieciu.
    constexpr bool MSDF_DEBUG_SOLID = false;


    void MulMat4(const float* a, const float* b, float* out) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c]
                               + a[r * 4 + 1] * b[1 * 4 + c]
                               + a[r * 4 + 2] * b[2 * 4 + c]
                               + a[r * 4 + 3] * b[3 * 4 + c];
            }
        }
    }

    // [1.12] Shadery kompilujemy LENIWIE, przy pierwszym uzyciu.
    // Pierwsza wersja portu robila to w FreeType_InitHk, czyli przy
    // inicjalizacji czcionek - a to leci ZANIM istnieje urzadzenie D3D,
    // wiec D3DCompile dostawalo null i obie zmienne zostawaly zerami
    // (log: "shadery: vs=00000000 ps=00000000"). W 3.3.5 problemu nie bylo,
    // bo tam shadery wchodzily przez callbacki klienta, odpalane pozniej.
    void EnsureShaders(IDirect3DDevice9* device) {
        if (!device || (s_cachedVS && s_cachedPS)) return;
        if (!CfgOn("shaders")) return;

        if (!s_cachedVS) {
            s_cachedVS = D3D::CompileVertexShader({
                .shaderCode = vertexShaderHLSL,
                .target = "vs_3_0"
            });
        }
        if (!s_cachedPS) {
            s_cachedPS = D3D::CompilePixelShader({
                .shaderCode = MSDF_DEBUG_SOLID ? pixelShaderDebugHLSL : pixelShaderHLSL,
                .target = "ps_3_0"
            });
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("[MSDF] kompilacja leniwa: vs=%p ps=%p (tryb %s)",
                s_cachedVS, s_cachedPS, MSDF_DEBUG_SOLID ? "DEBUG SOLID" : "normalny");
        }
    }

    void BindMsdfShaders(IDirect3DDevice9* device) {
        EnsureShaders(device);
        if (!device || !s_cachedVS || !s_cachedPS) {
            if (!g_logBind) {
                g_logBind = true;
                Log("[MSDF] BindMsdfShaders ODRZUCONE: device=%p vs=%p ps=%p",
                    device, s_cachedVS, s_cachedPS);
            }
            return;
        }

        D3DMATRIX world{}, view{}, proj{}, wv{}, wvp{};
        device->GetTransform(D3DTS_WORLD, &world);
        device->GetTransform(D3DTS_VIEW, &view);
        device->GetTransform(D3DTS_PROJECTION, &proj);
        MulMat4(&world.m[0][0], &view.m[0][0], &wv.m[0][0]);
        MulMat4(&wv.m[0][0], &proj.m[0][0], &wvp.m[0][0]);

        float c[16];
        if (MSDF_WVP_TRANSPOSE) {
            for (int r = 0; r < 4; ++r)
                for (int col = 0; col < 4; ++col)
                    c[r * 4 + col] = wvp.m[col][r];
        } else {
            memcpy(c, &wvp.m[0][0], sizeof(c));
        }

        device->SetVertexShader(s_cachedVS);
        device->SetPixelShader(s_cachedPS);
        const HRESULT hrWvp = device->SetVertexShaderConstantF(MSDF::SDF_WVP_REG, c, 4);

        if (!g_logBind) {
            g_logBind = true;
            // [1.12] Rejestry lezace ponad zakresem klienta (patrz MSDF.h). Jesli
            // sterownik zglosi tu blad, znaczy ze urzadzenie ma mniej stalych
            // niz vs_3_0 - wtedy trzeba zejsc nizej, ale nadal ponad c186.
            D3DCAPS9 caps{};
            device->GetDeviceCaps(&caps);
            Log("[MSDF] stale: WorldViewProj -> c%u..c%u (hr=0x%08lX), control -> c%u."
                " MaxVertexShaderConst urzadzenia = %lu",
                MSDF::SDF_WVP_REG, MSDF::SDF_WVP_REG + 3, hrWvp,
                MSDF::SDF_CONTROL_REG, caps.MaxVertexShaderConst);
            if (FAILED(hrWvp) || caps.MaxVertexShaderConst < MSDF::SDF_WVP_REG + 4) {
                Log("[MSDF] UWAGA: urzadzenie ma za malo rejestrow stalych na nasz uklad."
                    " Tekst bedzie zle transformowany. Zejsc z SDF_WVP_REG w MSDF.h,"
                    " ale nie nizej niz ponad zakres klienta (zmierzone: do c198).");
            }
            Log("[MSDF] pierwszy bind. WVP (po ewentualnej transpozycji):");
            Log("       %8.3f %8.3f %8.3f %8.3f", c[0], c[1], c[2], c[3]);
            Log("       %8.3f %8.3f %8.3f %8.3f", c[4], c[5], c[6], c[7]);
            Log("       %8.3f %8.3f %8.3f %8.3f", c[8], c[9], c[10], c[11]);
            Log("       %8.3f %8.3f %8.3f %8.3f", c[12], c[13], c[14], c[15]);
        }
    }

    void UnbindMsdfShaders(IDirect3DDevice9* device) {
        if (!device) return;
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
    }

    void __fastcall CGxuFontRenderBatchHk(CGxuFont* pThis) {
        pThis->RenderBatch();
        if (IDirect3DDevice9* device = D3D::GetDevice()) {
            constexpr float resetControl[4] = { 0, 0, 0, 0 };
            device->SetPixelShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
            device->SetVertexShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
            // [1.12] Obowiazkowe: bez tego reszta interfejsu rysowalaby sie
            // naszym shaderem czcionek. W 3.3.5 klient przestawial shader sam.
            UnbindMsdfShaders(device);
        }
    }

    // [1.12] __fastcall, nie __cdecl - klient podaje face w ECX, fontSize w EDX.
    char __fastcall GxuFontGlyphRenderGlyphHk(FT_Face fontFace, uint32_t fontSize, uint32_t codepoint, uint32_t pageInfo, CGxGlyphMetrics* resultBuffer, uint32_t outline_flag, uint32_t pad) {
        const char result = CGxuFont::RenderGlyph(fontFace, fontSize, codepoint, pageInfo, resultBuffer, outline_flag, pad);
        if (!g_logFont) {
            g_logFont = true;
            Log("[MSDF] RenderGlyph #1: face=%p handle=%p cp=%u",
                fontFace, MSDFFont::Get(fontFace), codepoint);
        }
        if (MSDF::ENABLED && resultBuffer && MSDFFont::Get(fontFace)) {
            // [1.12] m_bearingY i m_verAdv sa BEZ ZNAKU. Dla znakow o malym
            // wyniesieniu nad linie bazowa (kropka, myslnik, przecinek)
            // wyniesienie jest MNIEJSZE od odstepu pionowego, wiec odejmowanie
            // zawijalo sie na wartosc rzedu miliardow. Czworokat takiego znaku
            // dostawal wtedy y ~ 1.77e7 i ladowal daleko poza ekranem, podczas
            // gdy np. 'm' w tej samej partii miala poprawne y = -31..-12.
            // Zmierzone sonda porownawcza, nie wydedukowane.
            // [1.12] Odejmowanie BEZ przycinania - wynik ujemny jest tu poprawny.
            //
            // Probowalem przycinac do zera, gdy wyniesienie bylo mniejsze od
            // odstepu pionowego, bo znaki '.', '-' i '_' dostawaly wtedy y rzedu
            // 1.8e7. Bylo to leczenie objawu CUDZEJ usterki: prawdziwa przyczyna
            // siedziala w stubie GetGlyphYMetrics, ktory nadpisywal EDX zywy
            // w 1.12. Po jego naprawie przycinanie samo stalo sie bledem -
            // sprowadzalo wyniesienie tych znakow do zera, czyli sadzalo je przy
            // GORNEJ krawedzi wiersza. Pomiar: przy wylaczonym haku wszystkie
            // litery sa zle ustawione, wiec odejmowanie jest konieczne; wartosc
            // czyta klient ZE ZNAKIEM, a znak ponizej linii bazowej ma miec
            // wyniesienie ujemne.
            resultBuffer->m_bearingY -= resultBuffer->m_verAdv;
        }
        return result;
    }

    CGxGlyphCacheEntry* __fastcall CGxString__GetOrCreateGlyphEntryHk(CGxFont* fontObj, void* edx, uint32_t codepoint) {
        if (!fontObj) {
            return nullptr;
        }

        CGxGlyphCacheEntry* result = fontObj->GetOrCreateGlyphEntry(codepoint);

        if (result && MSDF::ENABLED && MSDFFont::Get(CGxString::GetFontFace(fontObj->m_ftWrapper))) {
            result->m_metrics.u0 = 1.0f + codepoint;
            result->m_cellIndexMin = 0;
            result->m_cellIndexMax = 0;
            result->m_texturePageIndex = 0;
        }
        return result;
    }

    int __fastcall CGxString__InitializeTextLineHk(CGxString* pThis, void* edx, char* text, int textLength, int* a4, C3Vector* startPos, void* a6, int a7) {
        const int result = pThis->InitializeTextLine(text, textLength, a4, startPos, a6, a7);

        if (pThis->m_flags & 0x40000000) return result;
        for (char* p = pThis->m_text; *p; ++p) {
            s_prefetchPayload.push_back(static_cast<uint8_t>(*p));
        }
        pThis->m_flags |= 0x40000000;
        return result;
    }

    __declspec(naked) void CGxString__CheckGeometry_siteHk() {
        __asm {
            pushad;
            mov edi, ebx;

            test ebx, ebx;
            jnz pre_pass_loop;

            xor ebx, ebx;
            lea esp, [esp + 0];

        pre_pass_loop:
            test edi, edi;
            jz pre_pass_done;
            test di, 1;
            jnz pre_pass_done;

            mov ecx, edi;
            call CGxString__CheckGeometryHk;

            mov eax, [esi + 1Ch];
            add eax, edi;
            mov edi, [eax + 4];
            jmp pre_pass_loop;

        pre_pass_done:
            popad;
            push ebx;
            call PrefetchCodepoints;
            add esp, 4;
            jmp CGxString__CheckGeometry_site_loopstart;
        }
    }

    __declspec(naked) void CGxString__CheckGeometry_callHk() {
        __asm {
            mov ecx, ebx;
            call ProcessGeometry;
            jmp CGxString__CheckGeometry_call_jmpback;
        }
    }

    // [1.12] Wolane ze stuba asemblerowego GetGlyphYMetrics. Gdy renderer jest
    // wylaczony, udajemy "to nie jest czcionka MSDF" - dzieki temu jedno
    // sprawdzenie wylacza takze latke asemblerowa, ktorej nie da sie odpiac.
    bool __cdecl MSDFFont_Get(FT_Face face) { return MSDF::ENABLED && MSDFFont::Get(face); }
    // [1.12] Stub przepisany na rejestry, ktorych naprawde uzywa 1.12.
    //
    // 3.3.5 (006C8C71):  mov edx,[ecx+54h] / mov ecx,[edx+68h]  - lancuch przez EDX
    // 1.12  (005D137A):  mov ecx,[ecx+54h] / mov ecx,[ecx+68h]  - lancuch przez ECX
    //
    // Przeniesiona doslownie wersja z 3.3.5 nadpisywala EDX, ktory w 1.12 jest
    // w tym miejscu ZYWY i wyzerowany (005D136B "mov [ebp-4],edx",
    // 005D136F "xor edx,edx"). Klient liczyl potem z tego smiecia, przez co
    // czesc znakow dostawala absurdalna wspolrzedna pionowa i znikala z ekranu.
    // Teraz EDX nie jest ruszany, a lancuch idzie przez ECX jak w oryginale.
    __declspec(naked) void CGxString_GetGlyphYMetrics_siteHk() {
        __asm {
            pushad;
            push ecx;
            call MSDFFont_Get;
            add esp, 4;
            test al, al;
            popad;
            jz font_unsafe;
            xor ecx, ecx;
            jmp CGxString__GetGlyphYMetrics_site_jmpback;
        font_unsafe:
            mov ecx, [ecx + 54h];
            mov ecx, [ecx + 68h];
            jmp CGxString__GetGlyphYMetrics_site_jmpback;
        }
    }

    // [1.12] licznikiem petli jest EDI, nie EBX. Miejsce ma 5 bajtow,
    // `mov edi, imm32` tez ma 5 - wchodzi bez przesuwania granicy.
    __declspec(naked) void CGxDevice__AllocateFontIndexBuffer_siteHk() {
        __asm {
            mov edi, 3FFFh;
            jmp CGxDevice__AllocateFontIndexBuffer_site_jmpback;
        }
    }
    // [1.12] Dwie zmiany merytoryczne wobec 3.3.5:
    //  - znika `mov ecx,[00C5DF88]` - w 1.12 nie ma globalu CGxDevice,
    //    PoolCreate to wolna funkcja, a nie metoda urzadzenia;
    //  - dwa pierwsze argumenty ida w ECX/EDX, rozmiar zostaje na stosie.
    __declspec(naked) void CGxDevice__InitFontIndexBuffer_siteHk() {
        __asm {
            push 30000h;
            xor edx, edx;
            mov ecx, 1;
            call CGxDevice::PoolCreateFn;
            push 0;
            push 1801Ah;
            jmp CGxDevice__InitFontIndexBuffer_site_jmpback;
        }
    }
    __declspec(naked) void IGxuFontProcessBatch_siteHk() {
        __asm {
            mov g_runtimeVBSize, 0;
            pop ebx;
            pop esi;
            mov esp, ebp;
            pop ebp;
            jmp IGxuFontProcessBatch_site_jmpback;
        }
    }
    __declspec(naked) void CGxDevice__BufStream_siteHk() {
        __asm {
            mov eax, g_runtimeVBSize;
            cmp eax, 800h;
            jge check_upper;
            mov eax, 800h;
            jmp do_push;
        check_upper:
            cmp eax, 0FFFCh;
            jle do_push;
            mov eax, 0FFFCh;
        do_push:
            mov g_runtimeVBSize, eax;
            push eax;
            jmp CGxDevice__BufStream_site_jmpback;
        }
    }
    // [1.12] Tablica partii tekstur jest przesunieta o -0x14, wiec granica
    // petli to 0A0h, nie 0B4h. EAX musi wyjsc wyzerowany: jmpback w 1.12
    // wpisuje go do [ebp-0Ch] i [ebp-18h] (3.3.5 wpisywalo tam stala 0).
    __declspec(naked) void bufalloc_1_siteHk() {
        __asm {
            xor eax, eax;
            mov esi, 0A0h;
            mov ebx, g_runtimeVBSize;
            jmp bufalloc_1_site_jmpback;
        }
    }
    // [1.12] bufalloc (005C8F40) to konwencja rejestrowa: ecx = &stream,
    // edx = rozmiar, sprzata stos sam - zadnego `add esp,8` po wywolaniu.
    // Lokalna ze strumieniem lezy pod [ebp-1Ch], nie [ebp-18h].
    __declspec(naked) void bufalloc_2_siteHk() {
        __asm {
            mov eax, g_runtimeVBSize;
            cmp ebx, eax;
            jz orig_skip;
            mov edx, eax;
            sub edx, ebx;
            lea ecx, [ebp - 1Ch];
            call CGxDevice::FlushBufferFn;
            mov edi, eax;
            mov ebx, g_runtimeVBSize;
        orig_skip:
            jmp bufalloc_2_site_jmpback;
        }
    }
    // [1.12] Tu `lea` na strumien lezy WEWNATRZ latanego zakresu, wiec stub
    // musi je powtorzyc - w 3.3.5 wskaznik przychodzil gotowy w EAX.
    __declspec(naked) void bufalloc_3_siteHk() {
        __asm {
            mov edx, g_runtimeVBSize;
            lea ecx, [ebp - 1Ch];
            call CGxDevice::FlushBufferFn;
            mov edi, eax;
            mov ebx, g_runtimeVBSize;
            jmp bufalloc_3_site_jmpback;
        }
    }

    int __fastcall FreeType_NewMemoryFaceHk(FT_Library library, const FT_Byte* file_base,
        FT_Long file_size, FT_Long face_index, FT_Face* aface) {
        if (!MSDF::g_realFtLibrary && FT_Init_FreeType(&MSDF::g_realFtLibrary) != 0) return -1;

        const int result = FT_New_Memory_Face(library, file_base, file_size, face_index, aface);
        if (result != 0 || !aface || !*aface) return result;

        MSDFFont::Register(*aface, file_base, file_size);
        Log("[MSDF] zarejestrowana czcionka: face=%p handle=%p rozmiar=%ld",
            *aface, MSDFFont::Get(*aface), (long)file_size);
        return result;
    }

    int __fastcall FreeType_SetPixelSizesHk(FT_Face face, FT_UInt pixel_width, FT_UInt pixel_height) {
        return FT_Set_Pixel_Sizes(face, pixel_width, pixel_height);
    }

    int __fastcall FreeType_LoadGlyphHk(FT_Face face, FT_ULong glyph_index, FT_Int32 load_flags) {
        return FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_NO_HINTING);
    }

    FT_UInt __fastcall FreeType_GetCharIndexHk(FT_Face face, FT_ULong charcode) {
        return FT_Get_Char_Index(face, charcode);
    }

    int __fastcall FreeType_GetKerningHk(FT_Face face, FT_UInt left_glyph, FT_UInt right_glyph, FT_UInt kern_mode, FT_Vector* akerning) {
        return FT_Get_Kerning(face, left_glyph, right_glyph, kern_mode, akerning);
    }

    int __fastcall FreeType_Done_FaceHk(FT_Face face) {
        MSDFFont::Unregister(face);
        return FT_Done_Face(face);
    }

    int __fastcall FreeType_Done_FreeTypeHk(FT_Library library) {
        MSDFFont::Shutdown();
        if (MSDF::g_msdfFreetype) {
            msdfgen::deinitializeFreetype(MSDF::g_msdfFreetype);
            MSDF::g_msdfFreetype = nullptr;
        }
        if (MSDF::g_realFtLibrary) {
            FT_Done_FreeType(MSDF::g_realFtLibrary);
            MSDF::g_realFtLibrary = nullptr;
        }
        return 0;
    }

    // [1.12] FreeType_NewFaceHk usuniety: FT_New_Face nie zostala znaleziona
    // w 1.12, bo zaden z dwoch klientow jej nie wola (ryzyko R3 w mapie).
    // Hak Lexary byl zapobiegawczy.

    // [1.12] Hak, ktorego Lexara na 3.3.5 nie potrzebowala, a bez ktorego 1.12
    // wywala sie przy starcie: 0xC0000005 pod 007CECA4 (FT_Add_Module), wolane
    // z 007CCFDA, czyli ze srodka FT_Add_Default_Modules; ESI wskazywalo tablice
    // 11 domyslnych modulow (0081E068), a EDI mialo 0x13 - mala liczbe uzyta jako
    // wskaznik, czyli odczyt spod zlego offsetu w strukturze.
    //
    // Powod: wrapper klienta 005C17F0 robi po kolei
    //     call FT_New_Library   (nasz hak -> zwraca biblioteke FreeType 2.14.1
    //                            Lexary, nie te wbudowana w klienta)
    //     call FT_Add_Default_Modules  (kod FreeType-a Z KLIENTA)
    // czyli puszcza STARY kod na NOWEJ strukturze FT_LibraryRec. Offsety pol
    // miedzy tymi wersjami sie roznia.
    //
    // Nasz hak Init wola pelne FT_Init_FreeType, ktore jest rownowazne
    // FT_New_Library + FT_Add_Default_Modules, wiec moduly SA juz dodane -
    // to wywolanie jest nie tylko szkodliwe, ale i zbedne. Pomijamy je
    // wylacznie dla NASZEJ biblioteki; sciezka CJK, gdzie oddajemy klientowi
    // jego wlasna, dziala dalej normalnie.
    void __fastcall FreeType_AddDefaultModulesHk(FT_Library library) {
        if (library && library == MSDF::g_realFtLibrary) return;
        FreeType::AddDefaultModulesFn(library);
    }

    // [1.12] Wszystkie haki FreeType sa __fastcall: klient wola je rejestrowo
    // (ecx, edx, reszta na stosie). Kolejnosc argumentow bez zmian wobec 3.3.5.
    // Dla tej funkcji potwierdza to wprost wrapper 005C17F0:
    // `mov edx, 0xC2B9A8` (alibrary) i `mov ecx, 0x85F4C8` (memory).
    int __fastcall FreeType_InitHk(void* memory, FT_Library* alibrary) {
        if (!MSDF::INITIALIZED) {
            std::string localeStr = MSDF::GetGameLocale();
            const char* locale = localeStr.c_str();
            MSDF::IS_CJK = locale && (strcmp(locale, "zhCN") == 0 ||
                strcmp(locale, "zhTW") == 0 ||
                strcmp(locale, "koKR") == 0);

            MSDF::INITIALIZED = true;

            if (MSDF::IS_CJK) return FreeType::InitFn(memory, alibrary);

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            // AddDefaultModules idzie w parze z hakiem Init - a ten jest bramkowany
            // wyzej, w MSDF::initialize(), wiec tutaj jestesmy juz tylko przy
            // ft_hooks=1. Bez tego haka klient wywala sie przy starcie (007CECA4).
            Hooks::Detour(&FreeType::AddDefaultModulesFn, FreeType_AddDefaultModulesHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::NewMemoryFaceFn, FreeType_NewMemoryFaceHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::Done_FaceFn, FreeType_Done_FaceHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::SetPixelSizesFn, FreeType_SetPixelSizesHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::GetCharIndexFn, FreeType_GetCharIndexHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::LoadGlyphFn, FreeType_LoadGlyphHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::GetKerningFn, FreeType_GetKerningHk);
            if (CfgOn("ft_hooks")) Hooks::Detour(&FreeType::Done_FreeTypeFn, FreeType_Done_FreeTypeHk);

            if (CfgOn("hook_renderbatch")) Hooks::Detour(&CGxuFont::RenderBatchFn, CGxuFontRenderBatchHk);
            if (CfgOn("hook_renderglyph")) Hooks::Detour(&CGxuFont::RenderGlyphFn, GxuFontGlyphRenderGlyphHk);
            if (CfgOn("hook_getglyph")) Hooks::Detour(&CGxFont::GetOrCreateGlyphEntryFn, CGxString__GetOrCreateGlyphEntryHk);

            if (CfgOn("site_allocbuf")) Hooks::Detour(&CGxDevice__AllocateFontIndexBuffer_site, CGxDevice__AllocateFontIndexBuffer_siteHk);
            if (CfgOn("site_initbuf")) Hooks::Detour(&CGxDevice__InitFontIndexBuffer_site, CGxDevice__InitFontIndexBuffer_siteHk);

            if (CfgOn("site_glyphy")) Hooks::Detour(&CGxString__GetGlyphYMetrics_site, CGxString_GetGlyphYMetrics_siteHk);
            if (CfgOn("site_checkgeom")) Hooks::Detour(&CGxString__CheckGeometry_site, CGxString__CheckGeometry_siteHk);
            if (CfgOn("site_checkcall")) Hooks::Detour(&CGxString__CheckGeometry_call, CGxString__CheckGeometry_callHk);

            if (CfgOn("site_procbatch")) Hooks::Detour(&IGxuFontProcessBatch_site, IGxuFontProcessBatch_siteHk);
            if (CfgOn("site_bufstream")) Hooks::Detour(&CGxDevice__BufStream_site, CGxDevice__BufStream_siteHk);
            if (CfgOn("site_bufalloc1")) Hooks::Detour(&bufalloc_1_site, bufalloc_1_siteHk);
            if (CfgOn("site_bufalloc2")) Hooks::Detour(&bufalloc_2_site, bufalloc_2_siteHk);
            if (CfgOn("site_bufalloc3")) Hooks::Detour(&bufalloc_3_site, bufalloc_3_siteHk);

            if (CfgOn("hook_checkgeom")) Hooks::Detour(&CGxString::CheckGeometryFn, CGxString__CheckGeometryHk);
            if (CfgOn("hook_writegeom")) Hooks::Detour(&CGxString::WriteGeometryFn, CGxString__WriteGeometryHk);
            if (CfgOn("hook_initline")) Hooks::Detour(&CGxString::InitializeTextLineFn, CGxString__InitializeTextLineHk);
            DetourTransactionCommit();

            D3D::RegisterOnDestroy([]() {
                if (s_cachedPS) { s_cachedPS->Release(); s_cachedPS = nullptr; }
                if (s_cachedVS) { s_cachedVS->Release(); s_cachedVS = nullptr; }
                MSDFFont::ClearAllCache();
            });

            // [1.12] Bylo tu ~50 linii wpisujacych skompilowane shadery do obiektow
            // ShaderData klienta (00C7D2CC / 00C7D2D0) i rejestrujacych callbacki
            // na jego haki IShaderCreateVertex/Pixel. Klient 1.12 nie ma ani tych
            // globali, ani tych funkcji - shadery kompilujemy raz tutaj i wiazemy
            // sami na urzadzeniu w BindMsdfShaders.
            // [1.12] Kompilacja shaderow przeniesiona do EnsureShaders -
            // tutaj urzadzenie D3D jeszcze nie istnieje.

            s_prefetchPayload.reserve(16383);
            CGxDevice::InitFontIndexBufferFn();
        }
        else if (MSDF::IS_CJK) {
            return FreeType::InitFn(memory, alibrary);
        }
        if (const FT_Error error = FT_Init_FreeType(&MSDF::g_realFtLibrary)) return error;

        if (alibrary) *alibrary = MSDF::g_realFtLibrary;

        MSDF::g_msdfFreetype = msdfgen::initializeFreetype();
        if (!MSDF::g_msdfFreetype) {
            FT_Done_FreeType(MSDF::g_realFtLibrary);
            MSDF::g_realFtLibrary = nullptr;
            return -1;
        }
        return 0;
    }
}

void MSDF::initialize() {
    if (s_msdfInitHookArmed) {
        return;
    }

    // [1.12] Konfiguracje trzeba wczytac TUTAJ, nie w FreeType_InitHk: sam hak
    // Init jest bramkowany, wiec decyzja zapada zanim on sie odpali.
    //
    // Hak Init i hak AddDefaultModules chodza PARAMI i musza byc wlaczane
    // razem z reszta ft_hooks. Pierwsza wersja bisekcji zostawiala je poza
    // bramkami i przy ft_hooks=0 klient dostawal biblioteke FreeType 2.14.1
    // Lexary, ale wolal na niej WLASNE, stare funkcje FreeType - stan gorszy
    // niz obie skrajnosci i crash 0xC0000005 pod 77536085. Kontrola "wszystko
    // wylaczone" nie byla wtedy zadna kontrola.
    LoadCfg();

    // [1.12] msdf_enabled z pliku - ustawiane skrotem CTRL+ALT+F, czytane raz
    // przy starcie. Renderer wylaczony = wszystkie haki czcionek pomijane,
    // wiec klient rysuje tekst dokladnie tak jak bez tego DLL-a.
    MSDF::ENABLED = CfgOn("msdf_enabled");
    if (!MSDF::ENABLED) {
        Log("[MSDF] msdf_enabled=0 - renderer MSDF wylaczony na te sesje");
        s_msdfInitHookArmed = true;
        return;
    }

    if (!CfgOn("ft_hooks")) {
        Log("[MSDF] ft_hooks=0: FreeType klienta zostaje nietkniety,"
            " caly renderer MSDF jest wylaczony");
        s_msdfInitHookArmed = true;
        return;
    }

    Hooks::Detour(&FreeType::InitFn, FreeType_InitHk);
    s_msdfInitHookArmed = true;
}

bool MSDF::CfgFlag(const char* key) {
    return CfgOn(key);
}

// [1.12] Odwrotnosc CfgOn: brak pliku ALBO brak klucza znaczy WYLACZONE.
// Dla latek, ktorych domyslnie wlaczac nie wolno.
bool MSDF::CfgFlagOptIn(const char* key) {
    if (g_cfgText.empty()) return false;
    const std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while ((pos = g_cfgText.find(needle, pos)) != std::string::npos) {
        const bool lineStart = (pos == 0) || g_cfgText[pos - 1] == 10 || g_cfgText[pos - 1] == 13;
        if (lineStart) {
            const size_t v = pos + needle.size();
            return (v < g_cfgText.size()) && g_cfgText[v] != '0';
        }
        pos += needle.size();
    }
    return false;
}
