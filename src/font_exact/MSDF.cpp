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

    // [1.12] Defined further down, next to CGxuFontRenderBatchHk; WriteGeometryHk
    // sits higher up in the file and is the first to call them.
    void BindMsdfShaders(IDirect3DDevice9* device);
    void UnbindMsdfShaders(IDirect3DDevice9* device);

    // ------------------------------------------------------------------
    // [1.12] Switches read from the `lexara112.cfg` file next to the client.
    //
    // Why: every hypothesis of the form "which patch breaks the text" used to cost
    // a rebuild plus a trip into the game. With this file the bisection is done in
    // a text editor. Format: one line per entry, `name=0` or `name=1`; lines
    // starting with `#` are skipped. No file = everything enabled (the port's
    // default behaviour).
    //
    // The names match the patch sites from lexara-port-map.md:
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
            Log("[MSDF] no lexara112.cfg - all patches ENABLED");
            return;
        }
        char buf[4096];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        g_cfgText.assign(buf, n);
        Log("[MSDF] wczytano lexara112.cfg (%u B)", (unsigned)n);
    }

    // [1.12] Probe guards - each probe is meant to report exactly ONCE. Without
    // them the log grows with every frame and becomes a performance problem itself.
    bool g_logBind = false, g_logWrite = false, g_logFont = false;

    // [1.12] Raised between BindMsdfShaders and UnbindMsdfShaders, so the draw
    // hook can tell which draws are ours.
    bool g_msdfBound = false;

    // [1.12] DIAGNOSTIC, opt-in (diag_damage=1): the damage numbers over mobs.
    // Raised while the client renders the damage text batch (00CE8800, created
    // at 006C8470 from DAMAGE_TEXT_FONT, rendered from 006C8820).
    bool g_diagDamageOn = false;
    bool g_inDamageBatch = false;
    int g_diagDamageGeom = 0, g_diagDamageSkip = 0, g_diagDamageWrite = 0, g_diagDamageDraw = 0;
    constexpr uintptr_t DAMAGE_TEXT_BATCH = 0x00CE8800;

    void DiagDamageString(const char* where, CGxString* s) {
        CGxFontGeomBatch* b = s->m_geomBuffers[0];
        Log("[MSDF] damage %s: str=%p '%.24s' flags=%08X size=%.5f mult=%.5f col=%08X shadow=%08X (%.4f,%.4f)"
            " finalPos=(%.1f,%.1f,%.1f) face=%p handle=%p verts=%u",
            where, s, s->m_text ? s->m_text : "", s->m_flags, s->m_fontSize, s->m_fontSizeMult,
            s->m_textColor, s->m_shadowColor, s->m_shadowOffset.x, s->m_shadowOffset.y,
            s->m_finalPos.X, s->m_finalPos.Y, s->m_finalPos.Z,
            s->GetFontFace(), MSDFFont::Get(s->GetFontFace()), b ? b->m_verts.m_count : 0u);
    }

    void DiagDamageQuad(const char* tag, CGxString* s) {
        CGxFontGeomBatch* b = s->m_geomBuffers[0];
        if (!b || !b->m_verts.m_data || b->m_verts.m_count < 4) return;
        const CGxFontVertex* v = b->m_verts.m_data;
        Log("       %s: (%.2f,%.2f,%.3f uv %.4f,%.4f) (%.2f,%.2f uv %.4f,%.4f) (%.2f,%.2f uv %.4f,%.4f) (%.2f,%.2f uv %.4f,%.4f)",
            tag, v[0].pos.X, v[0].pos.Y, v[0].pos.Z, v[0].u, v[0].v, v[1].pos.X, v[1].pos.Y, v[1].u, v[1].v,
            v[2].pos.X, v[2].pos.Y, v[2].u, v[2].v, v[3].pos.X, v[3].pos.Y, v[3].u, v[3].v);
    }

    // [1.12] Ten patch sites. Addresses from stage 1 (lexara-port-map.md); the
    // 3.3.5 counterpart is in the trailing comment. In 1.12 each site has at least
    // as many bytes as the detour needs. Only site 3 (CheckGeometry_site) had its
    // start moved, because of a jump into its middle - see its stub.
    auto(*CGxString__CheckGeometry_call)() = reinterpret_cast<void(*)()>(0x005C9019);          // 006C4B09
    constexpr uintptr_t CGxString__CheckGeometry_call_jmpback = 0x005C9020;                    // 006C4B10

    auto(*CGxString__CheckGeometry_site)() = reinterpret_cast<void(*)()>(0x005C9001);          // 006C4AF3 (moved from 005C9003)
    constexpr uintptr_t CGxString__CheckGeometry_site_loopstart = 0x005C9010;                  // 006C4B00

    auto(*CGxString__GetGlyphYMetrics_site)() = reinterpret_cast<void(*)()>(0x005D137A);       // 006C8C71
    constexpr uintptr_t CGxString__GetGlyphYMetrics_site_jmpback = 0x005D1380;                 // 006C8C77

    auto(*CGxDevice__AllocateFontIndexBuffer_site)() = reinterpret_cast<void(*)()>(0x005C933F);// 006C480C
    constexpr uintptr_t CGxDevice__AllocateFontIndexBuffer_site_jmpback = 0x005C9344;          // 006C4811

    auto(*CGxDevice__InitFontIndexBuffer_site)() = reinterpret_cast<void(*)()>(0x005C92F7);    // 006C47BD
    constexpr uintptr_t CGxDevice__InitFontIndexBuffer_site_jmpback = 0x005C930F;              // 006C47D8

    // NOTE (R1): these 5 bytes are also the target of the `je 005C91A9` jump from
    // 005C8FF3 (an early exit taken when the index buffer is not initialised) -
    // taking that jump after the patch means jumping into the middle of an
    // instruction. It was the same in 3.3.5; the guarantee is the Init hook, which
    // never lets the buffer pointer be null.
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
        // [1.12] With no device there is no atlas, so a prefetch would only
        // generate glyphs with nowhere to store them. We defer them instead.
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
        if (g_inDamageBatch && !(pThis->m_flags & 0x40000000) && g_diagDamageSkip < 3) {
            ++g_diagDamageSkip;
            DiagDamageString("ProcessGeometry SKIPPED (no 0x40000000)", pThis);
        }
        if (!(pThis->m_flags & 0x40000000)) return;
        const bool diag = g_inDamageBatch && g_diagDamageGeom < 12;
        if (diag) {
            ++g_diagDamageGeom;
            DiagDamageString("ProcessGeometry", pThis);
            DiagDamageQuad("before", pThis);
        }
        if (!MSDF::ENABLED) return;

        // [1.12] Without a device an atlas page cannot be created
        // (D3D::CreateTexture bails out on "if (!device) return false"), and every
        // glyph touched during that window ended up rejected. The log showed
        // "CreateAtlasPage: REFUSED (pages so far=0)" with no HRESULT at all,
        // i.e. a failure BEFORE device->CreateTexture was ever called.
        //
        // This function runs from CheckGeometry, i.e. during text layout, which can
        // get ahead of the client's first frame - and we only catch the device on
        // its EndScene. So we defer the work; the client recomputes the geometry in
        // the next frame anyway.
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
        if (diag) {
            DiagDamageQuad("after ", pThis);
            Log("       is3d=%d scale=%.5f pad=%.3f fontOffs=%.2f atlasFlags=%08X",
                is3d ? 1 : 0, scale, pad, fontOffs, flags);
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

        if (g_inDamageBatch && g_diagDamageWrite < 10) {
            ++g_diagDamageWrite;
            DiagDamageString("WriteGeometry", pThis);
            Log("       dest=%08X index=%d vertIndex=%d vertCount=%d bound=%d", destPtr, index, vertIndex, vertCount, g_msdfBound ? 1 : 0);
            // The client's font vertex: float3 position, D3DCOLOR, float2 uv - 24 bytes.
            const uint8_t* d = reinterpret_cast<const uint8_t*>(destPtr);
            for (int i = 0; d && i < vertCount && i < 8; ++i) {
                const float* f = reinterpret_cast<const float*>(d + i * 24);
                const uint32_t col = *reinterpret_cast<const uint32_t*>(d + i * 24 + 12);
                Log("       out v%d (%.2f, %.2f, %.3f) col=%08X uv=(%.5f, %.5f)", i, f[0], f[1], f[2], col, f[4], f[5]);
            }
        }

        // [1.12] A discriminating probe: RenderGlyph DOES recognise the font (it
        // shows in the vertical offset) while WriteGeometry does not. The only
        // difference is that RenderGlyph receives the FT_Face as an argument, while
        // here it comes through m_fontObj (+0x44) -> m_ftWrapper (+0x70) ->
        // GetFontFace (005D0370).
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
                // [1.12] Font not handled by MSDF - fall back to the fixed pipeline.
                UnbindMsdfShaders(device);
            }
            return;
        }

        IDirect3DDevice9* device = D3D::GetDevice();
        if (!device) {
            static bool logged = false;
            if (!logged) { logged = true; Log("[MSDF] WriteGeometry: NO DEVICE - the MSDF path is inactive"); }
            return;
        }

        if (!g_logWrite) {
            g_logWrite = true;
            CGxFontGeomBatch* b0 = pThis->m_geomBuffers[0];
            Log("[MSDF] first WriteGeometry with an MSDF font: batch=%p verts=%u",
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
                Log("[MSDF] atlas: pages=%u", pages);
                for (uint32_t i = 0; i < pages; ++i) {
                    auto* ap = fontHandle->GetAtlasPage(i);
                    Log("       page %u: entry=%p texture=%p", i, ap, ap ? ap->texture : nullptr);
                }
                const uint32_t flags = pThis->m_fontObj->m_atlasPages[0].m_flags;
                Log("[MSDF] font flags=0x%08X -> control.y=%.1f (0=no outline, 1/2=outline)",
                    flags, (double)((flags & 8) ? 2.0f : ((flags & 1) ? 1.0f : 0.0f)));
            }
        }

        // [1.12] Here, not in a client hook - see the comment at BindMsdfShaders.
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
    // [1.12] Shader binding. The 1.12 client draws text with the FIXED pipeline -
    // there is no font shader object whose bytecode could be swapped. So we set
    // our own shaders on the device before drawing a batch and take them off
    // afterwards, so that the rest of the interface returns to the fixed pipeline.
    //
    // Because vs_3_0 replaces the fixed-pipeline transform, the
    // World*View*Projection matrix has to be supplied by hand in c0..c3. The client
    // sets those three matrices normally, so we read them back from the device.
    // ------------------------------------------------------------------

    // With `mul(pos, WorldViewProj)` in HLSL and the default column-major packing,
    // the matrix has to be handed over TRANSPOSED. This is the one place in the
    // port that could not be settled statically - on the first in-game test the
    // symptom of the wrong choice is text off-screen or invisible.
    // In that case: flip this constant to false and rebuild.
    constexpr bool MSDF_WVP_TRANSPOSE = true;

    // [1.12] DIAGNOSTIC. Separates two different causes of "no text visible":
    //   - the geometry lands off-screen (wrong WVP matrix), or
    //   - the geometry is fine but the atlas is empty, so sd = 0 and opacity = 0.
    // With MSDF_DEBUG_SOLID the pixel shader paints the MSDF path a flat magenta at
    // full opacity, bypassing the atlas entirely. Then:
    //   magenta rectangles where the text should be -> geometry OK, atlas at fault
    //   nothing at all                              -> matrix/geometry at fault
    // Set it back to false once the question is settled.
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

    // [1.12] Shaders are compiled LAZILY, on first use.
    // The first version of the port did it in FreeType_InitHk, i.e. during font
    // initialisation - and that runs BEFORE a D3D device exists, so D3DCompile got
    // a null and both variables stayed zero (log: "shaders: vs=00000000
    // ps=00000000"). In 3.3.5 the problem did not arise, because there the shaders
    // came in through client callbacks, which fire later.
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

    void ComputeWvp(IDirect3DDevice9* device, float (&c)[16]) {
        D3DMATRIX world{}, view{}, proj{}, wv{}, wvp{};
        device->GetTransform(D3DTS_WORLD, &world);
        device->GetTransform(D3DTS_VIEW, &view);
        device->GetTransform(D3DTS_PROJECTION, &proj);
        MulMat4(&world.m[0][0], &view.m[0][0], &wv.m[0][0]);
        MulMat4(&wv.m[0][0], &proj.m[0][0], &wvp.m[0][0]);

        if (MSDF_WVP_TRANSPOSE) {
            for (int r = 0; r < 4; ++r)
                for (int col = 0; col < 4; ++col)
                    c[r * 4 + col] = wvp.m[col][r];
        } else {
            memcpy(c, &wvp.m[0][0], sizeof(c));
        }
    }

    // The WVP last written to c240..c243, so the draw hook can skip the upload
    // when nothing changed.
    float s_uploadedWvp[16] = {};

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

        // Only a first guess - SyncStateAtDraw replaces it right before the draw,
        // once the client has pushed its WORLD matrix to the device.
        float c[16];
        ComputeWvp(device, c);

        device->SetVertexShader(s_cachedVS);
        device->SetPixelShader(s_cachedPS);
        g_msdfBound = true;
        const HRESULT hrWvp = device->SetVertexShaderConstantF(MSDF::SDF_WVP_REG, c, 4);
        memcpy(s_uploadedWvp, c, sizeof(c));

        if (!g_logBind) {
            g_logBind = true;
            // [1.12] Registers above the client's range (see MSDF.h). If the driver
            // reports an error here it means the device has fewer constants than
            // vs_3_0 requires - then we have to move lower, but still above c186.
            D3DCAPS9 caps{};
            device->GetDeviceCaps(&caps);
            Log("[MSDF] constants: WorldViewProj -> c%u..c%u (hr=0x%08lX), control -> c%u."
                " Device MaxVertexShaderConst = %lu",
                MSDF::SDF_WVP_REG, MSDF::SDF_WVP_REG + 3, hrWvp,
                MSDF::SDF_CONTROL_REG, caps.MaxVertexShaderConst);
            if (FAILED(hrWvp) || caps.MaxVertexShaderConst < MSDF::SDF_WVP_REG + 4) {
                Log("[MSDF] WARNING: the device has too few constant registers for our layout."
                    " Text will be transformed incorrectly. Lower SDF_WVP_REG in MSDF.h,"
                    " but no lower than just above the client range (measured: up to c198).");
            }
            Log("[MSDF] first bind. WVP (after transposition, if any):");
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
        g_msdfBound = false;
    }

    // [1.12] Between BindMsdfShaders (in WriteGeometry) and the draw itself the
    // client runs IStateSync (005A1B20), which pushes its own lazily cached state
    // to the device. Two parts of it collide with us:
    //
    //  - Shaders. This client does use shaders in the 3D world, and it sets them
    //    through 005A0570 (pixel) / 005A05D0 (vertex) only when its cached state
    //    changes. After the world pass its cache holds a pixel shader, so the
    //    first draw that wants none - the damage numbers (DAMAGE_TEXT_FONT, batch
    //    00CE8800, drawn from 006C8820 straight after the world) - gets
    //    SetPixelShader(NULL) right on top of ours. Measured in game with
    //    diag_damage=1: vs = ours, ps = 00000000 at the draw. vs_3_0 with no pixel
    //    shader is not a valid pairing, so the damage numbers were not drawn at all,
    //    or came out as a flat rectangle. Interface text never saw this, because
    //    by then the client's cache is already empty and nothing gets set.
    //
    //  - WORLD. VIEW and PROJECTION go to the device as soon as they are set
    //    (005A1450, 005A11D0), but WORLD only lazily: IXformSync (005A1E70) sets
    //    it here, if it is dirty (+0x1894). A GetTransform at bind time can then
    //    return the WORLD of the last model. Reproduced on the login screen by
    //    forcing that state; not seen in game so far, but the matrices are taken
    //    here anyway, where they are exactly what the fixed pipeline would use.
    int g_wvpLateCount = 0, g_shaderLostCount = 0;

    void SyncStateAtDraw(IDirect3DDevice9* device) {
        if (!g_msdfBound || !device) return;

        static bool logged = false;
        if (!logged) { logged = true; Log("[MSDF] sync_at_draw: first font draw reached the draw hook"); }

        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        device->GetVertexShader(&vs);
        device->GetPixelShader(&ps);
        if (vs != s_cachedVS || ps != s_cachedPS) {
            if (g_shaderLostCount < 3) {
                ++g_shaderLostCount;
                Log("[MSDF] our shaders were replaced before the draw (#%d): vs=%p ps=%p (ours %p %p) - setting them again",
                    g_shaderLostCount, vs, ps, s_cachedVS, s_cachedPS);
            }
            device->SetVertexShader(s_cachedVS);
            device->SetPixelShader(s_cachedPS);
        }
        if (vs) vs->Release();
        if (ps) ps->Release();

        float c[16];
        ComputeWvp(device, c);
        if (memcmp(c, s_uploadedWvp, sizeof(c)) == 0) return;

        if (g_wvpLateCount < 3) {
            ++g_wvpLateCount;
            Log("[MSDF] WVP changed between bind and draw (#%d) - the client synced WORLD late:",
                g_wvpLateCount);
            Log("       bind: %9.5f %9.5f %9.5f %9.5f | %9.5f %9.5f %9.5f %9.5f",
                s_uploadedWvp[0], s_uploadedWvp[1], s_uploadedWvp[2], s_uploadedWvp[3],
                s_uploadedWvp[4], s_uploadedWvp[5], s_uploadedWvp[6], s_uploadedWvp[7]);
            Log("       draw: %9.5f %9.5f %9.5f %9.5f | %9.5f %9.5f %9.5f %9.5f",
                c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        }

        device->SetVertexShaderConstantF(MSDF::SDF_WVP_REG, c, 4);
        memcpy(s_uploadedWvp, c, sizeof(c));
    }

    // [1.12] DIAGNOSTIC (diag_damage=1): the device state for the first draws of
    // the damage text batch.
    void DiagDamageDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT minV, UINT numV, UINT start, UINT prim) {
        if (!g_inDamageBatch || g_diagDamageDraw >= 6) return;
        ++g_diagDamageDraw;

        IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
        d->GetVertexShader(&vs); d->GetPixelShader(&ps);
        DWORD fvf = 0; d->GetFVF(&fvf);
        IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0;
        d->GetStreamSource(0, &vb, &off, &stride);
        IDirect3DBaseTexture9* t0 = nullptr; IDirect3DBaseTexture9* t12 = nullptr;
        d->GetTexture(0, &t0); d->GetTexture(12, &t12);
        D3DVIEWPORT9 vp{}; d->GetViewport(&vp);
        RECT sc{}; d->GetScissorRect(&sc);
        float wvp[16] = {}, ctlV[4] = {}, ctlP[4] = {};
        d->GetVertexShaderConstantF(MSDF::SDF_WVP_REG, wvp, 4);
        d->GetVertexShaderConstantF(MSDF::SDF_CONTROL_REG, ctlV, 1);
        d->GetPixelShaderConstantF(MSDF::SDF_CONTROL_REG, ctlP, 1);

        Log("[MSDF] damage draw #%d: type=%d base=%d minV=%u numV=%u start=%u prim=%u bound=%d",
            g_diagDamageDraw, (int)type, base, minV, numV, start, prim, g_msdfBound ? 1 : 0);
        Log("       vs=%p (ours %p) ps=%p (ours %p) fvf=0x%lX vb=%p off=%u stride=%u tex0=%p tex12=%p",
            vs, s_cachedVS, ps, s_cachedPS, fvf, vb, off, stride, t0, t12);
        Log("       viewport=%lu,%lu %lux%lu z=%.2f..%.2f scissor=%ld,%ld,%ld,%ld",
            vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, sc.left, sc.top, sc.right, sc.bottom);
        Log("       c%u WVP: %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f",
            MSDF::SDF_WVP_REG, wvp[0], wvp[1], wvp[2], wvp[3], wvp[4], wvp[5], wvp[6], wvp[7],
            wvp[8], wvp[9], wvp[10], wvp[11], wvp[12], wvp[13], wvp[14], wvp[15]);
        Log("       c%u control vs=(%.2f %.2f %.2f %.2f) ps=(%.2f %.2f %.2f %.2f)",
            MSDF::SDF_CONTROL_REG, ctlV[0], ctlV[1], ctlV[2], ctlV[3], ctlP[0], ctlP[1], ctlP[2], ctlP[3]);

        static const struct { D3DRENDERSTATETYPE rs; const char* name; } kStates[] = {
            { D3DRS_ZENABLE, "ZENABLE" }, { D3DRS_ZFUNC, "ZFUNC" }, { D3DRS_ZWRITEENABLE, "ZWRITE" },
            { D3DRS_FILLMODE, "FILL" }, { D3DRS_CULLMODE, "CULL" },
            { D3DRS_ALPHABLENDENABLE, "BLEND" }, { D3DRS_SRCBLEND, "SRC" }, { D3DRS_DESTBLEND, "DST" },
            { D3DRS_BLENDOP, "BLENDOP" }, { D3DRS_SEPARATEALPHABLENDENABLE, "SEPALPHA" },
            { D3DRS_ALPHATESTENABLE, "ATEST" }, { D3DRS_ALPHAFUNC, "AFUNC" }, { D3DRS_ALPHAREF, "AREF" },
            { D3DRS_COLORWRITEENABLE, "CWRITE" }, { D3DRS_STENCILENABLE, "STENCIL" },
            { D3DRS_SCISSORTESTENABLE, "SCISSOR" }, { D3DRS_CLIPPING, "CLIPPING" },
            { D3DRS_CLIPPLANEENABLE, "CLIPPLANES" }, { D3DRS_FOGENABLE, "FOG" },
            { D3DRS_SRGBWRITEENABLE, "SRGBW" }, { D3DRS_LIGHTING, "LIGHTING" },
        };
        char line[768]; int len = 0;
        for (const auto& st : kStates) {
            DWORD v = 0; d->GetRenderState(st.rs, &v);
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, "%s=%lX ", st.name, v);
        }
        Log("       rs: %s", line);

        if (vs) vs->Release();
        if (ps) ps->Release();
        if (vb) vb->Release();
        if (t0) t0->Release();
        if (t12) t12->Release();
    }

    void __fastcall CGxuFontRenderBatchHk(CGxuFont* pThis) {
        g_inDamageBatch = g_diagDamageOn
            && reinterpret_cast<uintptr_t>(pThis) == *reinterpret_cast<const uintptr_t*>(DAMAGE_TEXT_BATCH);
        pThis->RenderBatch();
        g_inDamageBatch = false;
        if (IDirect3DDevice9* device = D3D::GetDevice()) {
            constexpr float resetControl[4] = { 0, 0, 0, 0 };
            device->SetPixelShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
            device->SetVertexShaderConstantF(MSDF::SDF_CONTROL_REG, resetControl, 1);
            // [1.12] Mandatory: without this the rest of the interface would be
            // drawn with our font shader. In 3.3.5 the client switched the shader
            // back itself.
            UnbindMsdfShaders(device);
        }
    }

    // [1.12] __fastcall, not __cdecl - the client passes face in ECX, fontSize in EDX.
    char __fastcall GxuFontGlyphRenderGlyphHk(FT_Face fontFace, uint32_t fontSize, uint32_t codepoint, uint32_t pageInfo, CGxGlyphMetrics* resultBuffer, uint32_t outline_flag, uint32_t pad) {
        const char result = CGxuFont::RenderGlyph(fontFace, fontSize, codepoint, pageInfo, resultBuffer, outline_flag, pad);
        if (!g_logFont) {
            g_logFont = true;
            Log("[MSDF] RenderGlyph #1: face=%p handle=%p cp=%u",
                fontFace, MSDFFont::Get(fontFace), codepoint);
        }
        if (MSDF::ENABLED && resultBuffer && MSDFFont::Get(fontFace)) {
            // [1.12] m_bearingY and m_verAdv are UNSIGNED. For characters with a
            // small rise above the baseline (period, hyphen, comma) the bearing is
            // SMALLER than the vertical advance, so the subtraction wrapped around
            // to a value in the billions. The quad for such a character then got
            // y ~ 1.77e7 and landed far off-screen, while e.g. 'm' in the same batch
            // had a correct y = -31..-12.
            // Measured with a comparison probe, not deduced.
            // [1.12] Subtraction WITHOUT clamping - a negative result is correct here.
            //
            // Clamping to zero when the bearing was smaller than the vertical
            // advance was tried, because '.', '-' and '_' were getting y around
            // 1.8e7. That was treating the symptom of SOMEONE ELSE'S bug: the real
            // cause sat in the GetGlyphYMetrics stub, which clobbered an EDX that
            // is live in 1.12. Once that was fixed the clamping became a bug in its
            // own right - it drove those characters' bearing to zero, i.e. planted
            // them against the TOP edge of the line. Measurement: with the hook
            // disabled every letter is misplaced, so the subtraction is necessary;
            // the client reads the value AS SIGNED, and a character below the
            // baseline is supposed to have a negative bearing.
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

    // [1.12] The site starts at 005C9001, on the `jne`, not at 005C9003 as in
    // Lexara (006C4AF3). The original code:
    //   005C8FFD  test bl,1
    //   005C9000  push edi
    //   005C9001  jne  005C9007       <- taken when [esi+24h] is odd (empty list)
    //   005C9003  test ebx,ebx
    //   005C9005  jne  005C9010
    //   005C9007  xor  ebx,ebx
    //   005C9009  lea  esp,[esp+0]
    //   005C9010  (loop)
    // A detour at 005C9003 writes `jmp` over 005C9003..005C9007 and int3 at
    // 005C9008, so that `jne` landed on the last byte of the jump displacement
    // (0x55, push ebp) and then on the int3: ERROR #132, BREAKPOINT at 005C9008.
    // It fires for a string whose geometry list is empty (the odd head is the
    // list's own terminator) - seen on the zone-discovery text. 3.3.5 has the
    // same jump (006C4AF1 jne 006C4AF7), so upstream Lexara carries it too.
    // With the `jne` inside the patch, the stub takes that branch itself.
    __declspec(naked) void CGxString__CheckGeometry_siteHk() {
        __asm {
            test bl, 1;             // the overwritten `jne 005C9007`:
            jz list_head_ok;        // an odd head means an empty list,
            xor ebx, ebx;           // which the client turns into ebx = 0
        list_head_ok:
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

    // [1.12] Called from the GetGlyphYMetrics assembly stub. When the renderer is
    // disabled we pretend "this is not an MSDF font" - that way a single check also
    // disables the assembly patch, which cannot be detached.
    bool __cdecl MSDFFont_Get(FT_Face face) { return MSDF::ENABLED && MSDFFont::Get(face); }
    // [1.12] Stub rewritten for the registers 1.12 actually uses.
    //
    // 3.3.5 (006C8C71):  mov edx,[ecx+54h] / mov ecx,[edx+68h]  - chain through EDX
    // 1.12  (005D137A):  mov ecx,[ecx+54h] / mov ecx,[ecx+68h]  - chain through ECX
    //
    // The version carried over verbatim from 3.3.5 clobbered EDX, which at this
    // point in 1.12 is LIVE and zeroed (005D136B "mov [ebp-4],edx",
    // 005D136F "xor edx,edx"). The client then computed from that garbage, which is
    // why some characters got an absurd vertical coordinate and vanished off-screen.
    // EDX is now left alone and the chain goes through ECX, as in the original.
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

    // [1.12] the loop counter is EDI, not EBX. The site is 5 bytes and
    // `mov edi, imm32` is 5 as well - it fits without moving the boundary.
    __declspec(naked) void CGxDevice__AllocateFontIndexBuffer_siteHk() {
        __asm {
            mov edi, 3FFFh;
            jmp CGxDevice__AllocateFontIndexBuffer_site_jmpback;
        }
    }
    // [1.12] Two substantive changes relative to 3.3.5:
    //  - `mov ecx,[00C5DF88]` is gone - 1.12 has no CGxDevice global, and
    //    PoolCreate is a free function rather than a device method;
    //  - the first two arguments go in ECX/EDX, the size stays on the stack.
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
    // [1.12] The texture batch array is shifted by -0x14, so the loop bound is
    // 0A0h, not 0B4h. EAX has to come out zeroed: the jmpback in 1.12 writes it to
    // [ebp-0Ch] and [ebp-18h] (3.3.5 wrote a literal 0 there).
    __declspec(naked) void bufalloc_1_siteHk() {
        __asm {
            xor eax, eax;
            mov esi, 0A0h;
            mov ebx, g_runtimeVBSize;
            jmp bufalloc_1_site_jmpback;
        }
    }
    // [1.12] bufalloc (005C8F40) uses a register convention: ecx = &stream,
    // edx = size, and it cleans the stack itself - no `add esp,8` after the call.
    // The stream local sits at [ebp-1Ch], not [ebp-18h].
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
    // [1.12] Here the `lea` for the stream sits INSIDE the patched range, so the
    // stub has to repeat it - in 3.3.5 the pointer arrived ready in EAX.
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
        Log("[MSDF] font registered: face=%p handle=%p size=%ld",
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

    // [1.12] FreeType_NewFaceHk removed: FT_New_Face was not found in 1.12 because
    // neither of the two clients calls it (risk R3 in the map). Lexara's hook was
    // precautionary.

    // [1.12] A hook Lexara did not need on 3.3.5, and without which 1.12 crashes
    // at start-up: 0xC0000005 at 007CECA4 (FT_Add_Module), called from 007CCFDA,
    // i.e. from inside FT_Add_Default_Modules; ESI pointed at the table of 11
    // default modules (0081E068) while EDI held 0x13 - a small number used as a
    // pointer, i.e. a read from the wrong offset inside the structure.
    //
    // The reason: the client's wrapper at 005C17F0 does, in order,
    //     call FT_New_Library   (our hook -> returns Lexara's FreeType 2.14.1
    //                            library, not the one built into the client)
    //     call FT_Add_Default_Modules  (the CLIENT'S OWN FreeType code)
    // i.e. it runs OLD code against a NEW FT_LibraryRec structure. The field
    // offsets differ between those versions.
    //
    // Our Init hook calls the full FT_Init_FreeType, which is equivalent to
    // FT_New_Library + FT_Add_Default_Modules, so the modules ARE already added -
    // this call is not merely harmful but redundant. We skip it only for OUR OWN
    // library; the CJK path, where the client gets its own library back, keeps
    // working as before.
    void __fastcall FreeType_AddDefaultModulesHk(FT_Library library) {
        if (library && library == MSDF::g_realFtLibrary) return;
        FreeType::AddDefaultModulesFn(library);
    }

    // [1.12] All FreeType hooks are __fastcall: the client calls them through
    // registers (ecx, edx, the rest on the stack). Argument order is unchanged from
    // 3.3.5. For this function the wrapper at 005C17F0 confirms it outright:
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
            // AddDefaultModules goes hand in hand with the Init hook - and that one
            // is gated higher up, in MSDF::initialize(), so by this point we are
            // only ever here with ft_hooks=1. Without this hook the client crashes
            // at start-up (007CECA4).
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

            // [1.12] This used to be ~50 lines writing the compiled shaders into
            // the client's ShaderData objects (00C7D2CC / 00C7D2D0) and registering
            // callbacks on its IShaderCreateVertex/Pixel hooks. The 1.12 client has
            // neither those globals nor those functions - we compile the shaders
            // once here and bind them on the device ourselves in BindMsdfShaders.
            // [1.12] Shader compilation moved to EnsureShaders - at this point the
            // D3D device does not exist yet.

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

    // [1.12] The configuration has to be loaded HERE, not in FreeType_InitHk: the
    // Init hook is itself gated, so the decision is made before it ever fires.
    //
    // The Init hook and the AddDefaultModules hook come as a PAIR and must be
    // enabled together with the rest of ft_hooks. The first version of the
    // bisection left them outside the gates, and with ft_hooks=0 the client got
    // Lexara's FreeType 2.14.1 library but called its OWN, old FreeType functions
    // on it - a state worse than either extreme, and a 0xC0000005 crash at
    // 77536085. The "everything disabled" control was then no control at all.
    LoadCfg();

    // [1.12] msdf_enabled comes from the file - set with the CTRL+ALT+F shortcut,
    // read once at start-up. Renderer disabled = every font hook skipped, so the
    // client draws text exactly as it would without this DLL.
    MSDF::ENABLED = CfgOn("msdf_enabled");
    if (!MSDF::ENABLED) {
        Log("[MSDF] msdf_enabled=0 - the MSDF renderer is disabled for this session");
        s_msdfInitHookArmed = true;
        return;
    }

    if (!CfgOn("ft_hooks")) {
        Log("[MSDF] ft_hooks=0: the client FreeType is left untouched,"
            " the whole MSDF renderer is disabled");
        s_msdfInitHookArmed = true;
        return;
    }

    // Registered here, before the device exists: InstallDeviceHooks only
    // detours the draw calls if someone is already listening.
    if (CfgOn("sync_at_draw")) {
        D3D::RegisterDrawIndexedPrimitiveCallback(
            [](IDirect3DDevice9* d, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) { SyncStateAtDraw(d); });
        D3D::RegisterDrawPrimitiveCallback(
            [](IDirect3DDevice9* d, D3DPRIMITIVETYPE, UINT, UINT) { SyncStateAtDraw(d); });
    }

    // After sync_at_draw, so the dump shows the state the draw really gets.
    if (MSDF::CfgFlagOptIn("diag_damage")) {
        g_diagDamageOn = true;
        D3D::RegisterDrawIndexedPrimitiveCallback(DiagDamageDraw);
        Log("[MSDF] diag_damage=1: logging the damage text batch (00CE8800)");
    }

    Hooks::Detour(&FreeType::InitFn, FreeType_InitHk);
    s_msdfInitHookArmed = true;
}

bool MSDF::CfgFlag(const char* key) {
    return CfgOn(key);
}

// [1.12] The inverse of CfgOn: a missing file OR a missing key means DISABLED.
// For patches that must not be enabled by default.
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
