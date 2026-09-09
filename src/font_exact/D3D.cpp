#include "Hooks.h"
#include "D3D.h"
#include "../Logger.h"
#include "MSDF.h"
#include <string>
#include <cstdio>
#include "../ShutdownCheck.h"
#include <Detours/detours.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace D3D {
    Present_t oPresent = nullptr;
    BeginScene_t oBeginScene = nullptr;
    EndScene_t oEndScene = nullptr;
    DrawPrimitive_t oDrawPrimitive = nullptr;
    DrawIndexedPrimitive_t oDrawIndexedPrimitive = nullptr;
    SetTexture_t oSetTexture = nullptr;
    SetRenderState_t oSetRenderState = nullptr;
    SetVertexShader_t oSetVertexShader = nullptr;
    SetPixelShader_t oSetPixelShader = nullptr;
    CreateTexture_t oCreateTexture = nullptr;
    SetRenderTarget_t oSetRenderTarget = nullptr;
    Clear_t oClear = nullptr;
    Reset_t oReset = nullptr;

    namespace {
        void LogShaderError(ID3DBlob* pError, uint32_t type) {
            if (pError) {
//#ifdef _DEBUG
                char buf[2048];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[AwesomeWotlk] %s Shader Error: %s\n",
                    type == 1 ? "Vertex" : "Pixel", reinterpret_cast<const char*>(pError->GetBufferPointer()));
                OutputDebugStringA(buf);
//#endif
                pError->Release();
            }
        }

        enum class ResourceType : uint8_t {
            Texture,
            RenderTarget,
            ShaderVertex,
            ShaderPixel
        };

        struct ManagedResource {
            IUnknown** ppResource;
            ResourceType type;
            ResourceParams params;
        };
        std::vector<ManagedResource> g_managedResources;

        void CleanupManagedResources() {
            if (g_isProcessTerminating) return;
            for (auto& managed : g_managedResources) {
                if (managed.ppResource && *managed.ppResource) {
                    (*managed.ppResource)->Release();
                    *managed.ppResource = nullptr;
                }
                if (managed.params.ppSurface && *managed.params.ppSurface) {
                    (*managed.params.ppSurface)->Release();
                    *managed.params.ppSurface = nullptr;
                }
            }
        }

        void RestoreManagedResources() {
            if (g_isProcessTerminating) return;
            if (!GetDevice()) return;
            for (auto& managed : g_managedResources) {
                if (!managed.ppResource || *managed.ppResource) continue;

                bool needsCleanup = managed.params.autoCleanup;
                managed.params.autoCleanup = false;

                switch (managed.type) {
                case ResourceType::Texture:
                    CreateTexture(reinterpret_cast<IDirect3DTexture9**>(managed.ppResource), managed.params);
                    break;
                case ResourceType::RenderTarget:
                    CreateRenderTarget(reinterpret_cast<IDirect3DSurface9**>(managed.ppResource), managed.params);
                    break;
                case ResourceType::ShaderVertex:
                    *managed.ppResource = CompileVertexShader(managed.params);
                    break;
                case ResourceType::ShaderPixel:
                    *managed.ppResource = CompilePixelShader(managed.params);
                    break;
                }
                managed.params.autoCleanup = needsCleanup;
            }
        }

        void RegisterForCleanup(IUnknown** ppRes, ResourceType type, const ResourceParams& p) {
            for (auto& managed : g_managedResources) {
                if (managed.ppResource == ppRes) {
                    managed.params = p;
                    return;
                }
            }
            g_managedResources.push_back({
		        .ppResource = ppRes,
		        .type = type,
		        .params = p
                });
        }

        constexpr ShaderEntry s_shaders_engine[] {
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 76, "ps_3_0",  "float4 main() : COLOR { return float4(1, 0, 1, 1); }"}, // UI
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x3E991687 }, 128, "ps_3_0", "float4 main() : COLOR { return float4(1, 0, 0, 1); }" }, // UI
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x90080000 }, 88, "ps_3_0",  "float4 main() : COLOR { return float4(0, 1, 1, 1); }" }, // Water trails
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x3E99999A }, 336, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 1, 1); }" }, // Unk env/world?
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x3F800000 }, 124, "ps_3_0", "float4 main() : COLOR { return float4(0, 0, 1, 1); }" }, // Unk
            //{ { 0xFFFF0200, 0x05000051, 0xA00F0000, 0x3E800000 }, 304, "ps_2_0", "float4 main() : COLOR { return float4(1, 0, 0, 1); }" }, // Unk
            //{ { 0xFFFF0200, 0x05000051, 0xA00F0000, 0x3EC00000 }, 280, "ps_2_0", "float4 main() : COLOR { return float4(1, 0, 0, 1); }" }, // Unk
            //{ { 0xFFFF0200, 0x05000051, 0xA00F0000, 0x3F800000 }, 204, "ps_2_0", "float4 main() : COLOR { return float4(0, 0, 1, 1); }" }, // Unk
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 124, "ps_3_0", "float4 main() : COLOR { return float4(0, 1, 0, 1); }" }, // Obj and non-live model bodies
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x40000000 }, 260, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 0, 1); }" }, // Weps/Armor on non-live models
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 196, "ps_3_0", "float4 main() : COLOR { return float4(0, 0, 1, 1); }" }  // Materials on non-live models
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 128, "ps_3_0", "float4 main() : COLOR { return float4(0, 1, 0, 1); }" }, // Glow/Fog FX
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x00000000 }, 244, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 0, 1); }" }, // Lightning FX
            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 200, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 0, 1); }" },  // Ambient glow FX (consecration)

            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0xC05CB08D }, 2728, "ps_3_0", "float4 main() : COLOR { return float4(0, 1, 1, 1); }" }, // Ground, structures base
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0xC05CB08D }, 1948, "ps_3_0", "float4 main() : COLOR { return float4(1, 0, 0, 1); }" }, // Unk (some terrain)
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0xC05CB08D }, 2284, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 0, 1); }" }, // Unk

            //{ { 0xFFFF0300, 0x0200001F, 0x8000000A, 0x900F0000 }, 200, "ps_3_0", "float4 main() : COLOR { return float4(1, 1, 0, 1); }" },  // Ambient glow FX (consecration)
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0xC05CB08D }, 2816, "ps_3_0", "float4 main() : COLOR { return float4(0, 0, 1, 1); }" }, // Ambient glow FX 1 (characters)
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0000, 0x00000000 }, 2836, "ps_3_0", "float4 main() : COLOR { return float4(0, 1, 1, 1); }" }, // Ambient glow FX 2 (characters)

            ShaderEntry{ .data={ 0xFFFF0300, 0x05000051, 0xA00F0000, 0x00000000 }, .length = 1428, .profile = "ps_3_0" }, // Water

            //{ { 0xFFFF0300, 0x05000051, 0xA00F0001, 0x00000000 }, 88, "ps_3_0",   "float4 main() : COLOR { return float4(0, 1, 1, 1); }" }, // White
            //{ { 0xFFFF0300, 0x05000051, 0xA00F0001, 0x00000000 }, 152, "ps_3_0",  "float4 main() : COLOR { return float4(0, 1, 0, 1); }" }, // Gray
        };

        std::vector<VertexShaderInitCallback> g_vertexShaderCallbacks;
        std::vector<PixelShaderInitCallback> g_pixelShaderCallbacks;

        std::vector<ResourceCallback> g_onCreateCallbacks;
        std::vector<ResourceCallback> g_onDestroyCallbacks;
        std::vector<ResourceCallback> g_onReleaseCallbacks;
        std::vector<ResourceCallback> g_onRestoreCallbacks;

        std::vector<PresentCallback> g_presentCallbacks;
        std::vector<BeginSceneCallback> g_beginSceneCallbacks;
        std::vector<EndSceneCallback> g_endSceneCallbacks;
        std::vector<DrawPrimitiveCallback> g_drawPrimitiveCallbacks;
        std::vector<DrawIndexedPrimitiveCallback> g_drawIndexedPrimitiveCallbacks;
        std::vector<SetTextureCallback> g_setTextureCallbacks;
        std::vector<SetRenderStateCallback> g_setRenderStateCallbacks;
        std::vector<SetVertexShaderCallback> g_setVertexShaderCallbacks;
        std::vector<SetPixelShaderCallback> g_setPixelShaderCallbacks;
        std::vector<CreateTextureCallback> g_createTextureCallbacks;
        std::vector<SetRenderTargetCallback> g_setRenderTargetCallbacks;
        std::vector<ClearCallback> g_clearCallbacks;
        std::vector<ResetCallback> g_resetCallbacks;


        HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* device, const RECT* pSrcRect, const RECT* pDestRect, HWND hDestWnd, const RGNDATA* pDirtyRegion) {
            if (g_isProcessTerminating) return oPresent(device, pSrcRect, pDestRect, hDestWnd, pDirtyRegion);
            for (auto& cb : g_presentCallbacks) cb(device, pSrcRect, pDestRect, hDestWnd, pDirtyRegion);
            return oPresent(device, pSrcRect, pDestRect, hDestWnd, pDirtyRegion);
        }

        HRESULT STDMETHODCALLTYPE hkBeginScene(IDirect3DDevice9* device) {
            if (g_isProcessTerminating) return oBeginScene(device);
            for (auto& cb : g_beginSceneCallbacks) cb(device);
            return oBeginScene(device);
        }

        HRESULT STDMETHODCALLTYPE hkEndScene(IDirect3DDevice9* device) {
            if (g_isProcessTerminating) return oEndScene(device);
            for (auto& cb : g_endSceneCallbacks) cb(device);
            return oEndScene(device);
        }

        HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount) {
            if (g_isProcessTerminating) return oDrawPrimitive(device, type, startVertex, primCount);
            for (auto& cb : g_drawPrimitiveCallbacks) cb(device, type, startVertex, primCount);
            return oDrawPrimitive(device, type, startVertex, primCount);
        }

        HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex, UINT numVertices, UINT startIndex, UINT primCount) {
            if (g_isProcessTerminating) return oDrawIndexedPrimitive(device, type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
            for (auto& cb : g_drawIndexedPrimitiveCallbacks) cb(device, type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
            return oDrawIndexedPrimitive(device, type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
        }

        HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* pTexture) {
            if (g_isProcessTerminating) return oSetTexture(device, stage, pTexture);
            for (auto& cb : g_setTextureCallbacks) cb(device, stage, pTexture);
            return oSetTexture(device, stage, pTexture);
        }

        HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value) {
            if (g_isProcessTerminating) return oSetRenderState(device, state, value);
            for (auto& cb : g_setRenderStateCallbacks) cb(device, state, value);
            return oSetRenderState(device, state, value);
        }

        HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* pShader) {
            if (g_isProcessTerminating) return oSetVertexShader(device, pShader);
            for (auto& cb : g_setVertexShaderCallbacks) cb(device, pShader);
            return oSetVertexShader(device, pShader);
        }

        HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* pShader) {
            if (g_isProcessTerminating) return oSetPixelShader(device, pShader);
            for (auto& cb : g_setPixelShaderCallbacks) cb(device, pShader);
            return oSetPixelShader(device, pShader);
        }

        HRESULT STDMETHODCALLTYPE hkCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
            if (g_isProcessTerminating) return oCreateTexture(device, width, height, levels, usage, format, pool, ppTexture, pSharedHandle);
            for (auto& cb : g_createTextureCallbacks) cb(device, width, height, levels, usage, format, pool, ppTexture, pSharedHandle);
            return oCreateTexture(device, width, height, levels, usage, format, pool, ppTexture, pSharedHandle);
        }

        HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* device, DWORD renderTargetIndex, IDirect3DSurface9* pRenderTarget) {
            if (g_isProcessTerminating) return oSetRenderTarget(device, renderTargetIndex, pRenderTarget);
            for (auto& cb : g_setRenderTargetCallbacks) cb(device, renderTargetIndex, pRenderTarget);
            return oSetRenderTarget(device, renderTargetIndex, pRenderTarget);
        }

        HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* pRects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
            if (g_isProcessTerminating) return oClear(device, count, pRects, flags, color, z, stencil);
            for (auto& cb : g_clearCallbacks) cb(device, count, pRects, flags, color, z, stencil);
            return oClear(device, count, pRects, flags, color, z, stencil);
        }

        // [1.12] Reset przejmuje role, ktore w 3.3.5 pelnily haki klienta
        // IReleaseD3dResources (przed utrata urzadzenia) i NotifyOnDeviceRestored
        // (po odzyskaniu). Tamtych funkcji tu nie hakujemy w ogole.
        HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPP) {
            if (g_isProcessTerminating) return oReset(device, pPP);
            for (auto& cb : g_resetCallbacks) cb(device, pPP);

            CleanupManagedResources();
            for (auto& cb : g_onReleaseCallbacks) cb();

            const HRESULT hr = oReset(device, pPP);

            if (SUCCEEDED(hr)) {
                RestoreManagedResources();
                for (auto& cb : g_onRestoreCallbacks) cb();
            }
            return hr;
        }

        IDirect3DDevice9* g_device = nullptr;

        // [1.12] Bylo: hak na CGxDevice::DeviceCreate klienta 3.3.5, ktory potem
        // czytal urzadzenie z globalu 00C5DF88. Klient 1.12 takiego globalu nie ma
        // (sprawdzone - 00C0F464 w tej okolicy to stan viewportu, nie urzadzenie),
        // a d3d9.dll laduje DYNAMICZNIE - nie ma go w tablicy importow. Dlatego
        // urzadzenie przychodzi tu z lancucha LoadLibrary -> Direct3DCreate9 ->
        // IDirect3D9::CreateDevice. Zero adresow klienta w calej tej warstwie.
        void InstallDeviceHooks(IDirect3DDevice9* device) {
            if (g_isProcessTerminating || !device) return;
            g_device = device;
            Log("[MSDF] urzadzenie przechwycone: %p", device);
            {
                {
                    __try {
                        if (IDirect3DDevice9Vtbl* vtbl = *reinterpret_cast<IDirect3DDevice9Vtbl**>(device)) {
                            DetourTransactionBegin();

                            if (!g_presentCallbacks.empty()) {
                                oPresent = reinterpret_cast<Present_t>(vtbl->Present);
                                Hooks::Detour(&oPresent, hkPresent);
                            }
                            if (!g_beginSceneCallbacks.empty()) {
                                oBeginScene = reinterpret_cast<BeginScene_t>(vtbl->BeginScene);
                                Hooks::Detour(&oBeginScene, hkBeginScene);
                            }
                            if (!g_endSceneCallbacks.empty()) {
                                oEndScene = reinterpret_cast<EndScene_t>(vtbl->EndScene);
                                Hooks::Detour(&oEndScene, hkEndScene);
                            }
                            if (!g_drawPrimitiveCallbacks.empty()) {
                                oDrawPrimitive = reinterpret_cast<DrawPrimitive_t>(vtbl->DrawPrimitive);
                                Hooks::Detour(&oDrawPrimitive, hkDrawPrimitive);
                            }
                            if (!g_drawIndexedPrimitiveCallbacks.empty()) {
                                oDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitive_t>(vtbl->DrawIndexedPrimitive);
                                Hooks::Detour(&oDrawIndexedPrimitive, hkDrawIndexedPrimitive);
                            }
                            if (!g_setTextureCallbacks.empty()) {
                                oSetTexture = reinterpret_cast<SetTexture_t>(vtbl->SetTexture);
                                Hooks::Detour(&oSetTexture, hkSetTexture);
                            }
                            if (!g_setRenderStateCallbacks.empty()) {
                                oSetRenderState = reinterpret_cast<SetRenderState_t>(vtbl->SetRenderState);
                                Hooks::Detour(&oSetRenderState, hkSetRenderState);
                            }
                            if (!g_setVertexShaderCallbacks.empty()) {
                                oSetVertexShader = reinterpret_cast<SetVertexShader_t>(vtbl->SetVertexShader);
                                Hooks::Detour(&oSetVertexShader, hkSetVertexShader);
                            }
                            if (!g_setPixelShaderCallbacks.empty()) {
                                oSetPixelShader = reinterpret_cast<SetPixelShader_t>(vtbl->SetPixelShader);
                                Hooks::Detour(&oSetPixelShader, hkSetPixelShader);
                            }
                            if (!g_createTextureCallbacks.empty()) {
                                oCreateTexture = reinterpret_cast<CreateTexture_t>(vtbl->CreateTexture);
                                Hooks::Detour(&oCreateTexture, hkCreateTexture);
                            }
                            if (!g_setRenderTargetCallbacks.empty()) {
                                oSetRenderTarget = reinterpret_cast<SetRenderTarget_t>(vtbl->SetRenderTarget);
                                Hooks::Detour(&oSetRenderTarget, hkSetRenderTarget);
                            }
                            if (!g_clearCallbacks.empty()) {
                                oClear = reinterpret_cast<Clear_t>(vtbl->Clear);
                                Hooks::Detour(&oClear, hkClear);
                            }
                            if (!g_resetCallbacks.empty()) {
                                oReset = reinterpret_cast<Reset_t>(vtbl->Reset);
                                Hooks::Detour(&oReset, hkReset);
                            }
                            DetourTransactionCommit();
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}

                    for (auto& callback : g_onCreateCallbacks) callback();
                }
            }
        }

        // ------------------------------------------------------------------
        // [1.12] Przechwycenie urzadzenia bez ani jednego adresu klienta.
        //
        // WoW.exe 1.12 nie importuje d3d9.dll staycznie (w tablicy importow jest
        // tylko opengl32.dll), tylko laduje ja przez LoadLibrary i wyciaga
        // Direct3DCreate9 przez GetProcAddress. W chwili wstrzykniecia DLL-a
        // przez VanillaFixes modul d3d9 jeszcze NIE JEST zaladowany, wiec nie da
        // sie od razu podpiac pod jego eksport. Stad lancuch: hak na LoadLibrary*
        // lapie moment zaladowania, potem Direct3DCreate9, potem CreateDevice.
        // ------------------------------------------------------------------
        using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
        using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND,
                                                           DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
        using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
        using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
        using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

        Direct3DCreate9_t oDirect3DCreate9 = nullptr;
        CreateDevice_t oCreateDevice = nullptr;
        LoadLibraryA_t oLoadLibraryA = nullptr;
        LoadLibraryW_t oLoadLibraryW = nullptr;
        LoadLibraryExW_t oLoadLibraryExW = nullptr;
        bool g_d3d9Hooked = false;

        using CreateDeviceEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND,
                                                             DWORD, D3DPRESENT_PARAMETERS*, void*, IDirect3DDevice9**);
        CreateDeviceEx_t oCreateDeviceEx = nullptr;

        HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9* pThis, UINT adapter, D3DDEVTYPE type,
                                                   HWND hFocus, DWORD flags, D3DPRESENT_PARAMETERS* pPP,
                                                   void* pFullscreenMode, IDirect3DDevice9** ppDevice) {
            const HRESULT hr = oCreateDeviceEx(pThis, adapter, type, hFocus, flags, pPP, pFullscreenMode, ppDevice);
            Log("[MSDF] hkCreateDeviceEx: hr=0x%08lX dev=%p", hr, (ppDevice ? *ppDevice : nullptr));
            if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
                InstallDeviceHooks(*ppDevice);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* pThis, UINT adapter, D3DDEVTYPE type,
                                                 HWND hFocus, DWORD flags,
                                                 D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDevice) {
            const HRESULT hr = oCreateDevice(pThis, adapter, type, hFocus, flags, pPP, ppDevice);
            Log("[MSDF] hkCreateDevice: hr=0x%08lX dev=%p", hr, (ppDevice ? *ppDevice : nullptr));
            if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
                InstallDeviceHooks(*ppDevice);
            }
            return hr;
        }

        IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdkVersion) {
            IDirect3D9* d3d = oDirect3DCreate9(sdkVersion);
            Log("[MSDF] hkDirect3DCreate9: sdk=%u -> d3d=%p", sdkVersion, d3d);
            if (d3d && !oCreateDevice) {
                __try {
                    // CreateDevice to slot 16 vtable IDirect3D9.
                    void** vtbl = *reinterpret_cast<void***>(d3d);
                    oCreateDevice = reinterpret_cast<CreateDevice_t>(vtbl[16]);
                    // [1.12] PODMIANA WPISU W VTABLE, nie detour ciala funkcji.
                    //
                    // Detour na 6F3E27D0 instalowal sie czysto (begin/update/
                    // attach/commit = 0), a mimo to hak nie odpalal sie ani razu,
                    // choc gra renderowala - czyli cialo pod tym adresem nie lezy
                    // na drodze wywolania (thunk / alias / efekt LTO w DXVK).
                    // Podmiana samego wskaznika w tablicy wirtualnej omija ten
                    // problem: dispatch COM czyta wpis, a nie adres funkcji.
                    DWORD oldProt = 0;
                    if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                        vtbl[16] = reinterpret_cast<void*>(&hkCreateDevice);
                        DWORD tmp = 0;
                        VirtualProtect(&vtbl[16], sizeof(void*), oldProt, &tmp);
                        Log("[MSDF] vtbl[16] podmieniony: %p -> %p",
                            reinterpret_cast<void*>(oCreateDevice), vtbl[16]);
                    }

                    // [1.12] Slot 20 to IDirect3D9Ex::CreateDeviceEx. Klient 1.12
                    // sam by go nie wolal, ale ktorys z modow (SuperWoWhook,
                    // VanillaUtils) moze podnosic urzadzenie do D3D9Ex.
                    DWORD oldProtEx = 0;
                    if (VirtualProtect(&vtbl[20], sizeof(void*), PAGE_READWRITE, &oldProtEx)) {
                        oCreateDeviceEx = reinterpret_cast<CreateDeviceEx_t>(vtbl[20]);
                        vtbl[20] = reinterpret_cast<void*>(&hkCreateDeviceEx);
                        DWORD tmpEx = 0;
                        VirtualProtect(&vtbl[20], sizeof(void*), oldProtEx, &tmpEx);
                        Log("[MSDF] vtbl[20] (CreateDeviceEx) podmieniony: %p -> %p",
                            reinterpret_cast<void*>(oCreateDeviceEx), vtbl[20]);
                    } else {
                        Log("[MSDF] VirtualProtect na vtbl[16] ODMOWIL (blad %lu)", GetLastError());
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { oCreateDevice = nullptr; }
            }
            return d3d;
        }

        // Podpiecie pod eksport d3d9.dll; wolane, gdy modul jest juz w procesie.
        void TryHookD3D9(HMODULE hMod) {
            if (g_d3d9Hooked || !hMod) return;
            auto fn = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hMod, "Direct3DCreate9"));
            if (!fn) return;
            Log("[MSDF] d3d9.dll zlapana, Direct3DCreate9 = %p", fn);
            g_d3d9Hooked = true;
            oDirect3DCreate9 = fn;
            // [1.12] Status transakcji MUSI byc sprawdzony: to leci z wnetrza
            // haka LoadLibrary, czyli pod blokada loadera, gdzie Detours potrafi
            // odmowic. Pierwsza wersja ignorowala wynik i urzadzenie po cichu
            // nigdy nie bylo przechwytywane.
            const LONG b1 = DetourTransactionBegin();
            const LONG u1 = DetourUpdateThread(GetCurrentThread());
            const LONG a1 = Hooks::Detour(&oDirect3DCreate9, hkDirect3DCreate9);
            const LONG c1 = DetourTransactionCommit();
            Log("[MSDF] detour Direct3DCreate9: begin=%ld update=%ld attach=%ld commit=%ld",
                b1, u1, a1, c1);
        }

        bool IsD3D9Name(const char* name) {
            if (!name) return false;
            const char* slash = strrchr(name, '\\');
            if (slash) name = slash + 1;
            return _stricmp(name, "d3d9.dll") == 0 || _stricmp(name, "d3d9") == 0;
        }

        bool IsD3D9NameW(const wchar_t* name) {
            if (!name) return false;
            const wchar_t* slash = wcsrchr(name, L'\\');
            if (slash) name = slash + 1;
            return _wcsicmp(name, L"d3d9.dll") == 0 || _wcsicmp(name, L"d3d9") == 0;
        }

        HMODULE WINAPI hkLoadLibraryA(LPCSTR name) {
            HMODULE h = oLoadLibraryA(name);
            if (h && IsD3D9Name(name)) TryHookD3D9(h);
            return h;
        }

        HMODULE WINAPI hkLoadLibraryW(LPCWSTR name) {
            HMODULE h = oLoadLibraryW(name);
            if (h && IsD3D9NameW(name)) TryHookD3D9(h);
            return h;
        }

        HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE hFile, DWORD flags) {
            HMODULE h = oLoadLibraryExW(name, hFile, flags);
            if (h && IsD3D9NameW(name)) TryHookD3D9(h);
            return h;
        }
    }

    std::span<const ShaderEntry> s_shaders{ s_shaders_engine };


    void RegisterPresentCallback(const PresentCallback& callback) {
        if (callback) g_presentCallbacks.push_back(callback);
    }

    void RegisterBeginSceneCallback(const BeginSceneCallback& callback) {
        if (callback) g_beginSceneCallbacks.push_back(callback);
    }

    void RegisterEndSceneCallback(const EndSceneCallback& callback) {
        if (callback) g_endSceneCallbacks.push_back(callback);
    }

    void RegisterDrawPrimitiveCallback(const DrawPrimitiveCallback& callback) {
        if (callback) g_drawPrimitiveCallbacks.push_back(callback);
    }

    void RegisterDrawIndexedPrimitiveCallback(const DrawIndexedPrimitiveCallback& callback) {
        if (callback) g_drawIndexedPrimitiveCallbacks.push_back(callback);
    }

    void RegisterSetTextureCallback(const SetTextureCallback& callback) {
        if (callback) g_setTextureCallbacks.push_back(callback);
    }

    void RegisterSetRenderStateCallback(const SetRenderStateCallback& callback) {
        if (callback) g_setRenderStateCallbacks.push_back(callback);
    }

    void RegisterSetVertexShaderCallback(const SetVertexShaderCallback& callback) {
        if (callback) g_setVertexShaderCallbacks.push_back(callback);
    }

    void RegisterSetPixelShaderCallback(const SetPixelShaderCallback& callback) {
        if (callback) g_setPixelShaderCallbacks.push_back(callback);
    }

    void RegisterCreateTextureCallback(const CreateTextureCallback& callback) {
        if (callback) g_createTextureCallbacks.push_back(callback);
    }

    void RegisterSetRenderTargetCallback(const SetRenderTargetCallback& callback) {
        if (callback) g_setRenderTargetCallbacks.push_back(callback);
    }

    void RegisterClearCallback(const ClearCallback& callback) {
        if (callback) g_clearCallbacks.push_back(callback);
    }

    void RegisterResetCallback(const ResetCallback& callback) {
        if (callback) g_resetCallbacks.push_back(callback);
    }


    void RegisterOnCreate(const ResourceCallback& callback) {
        if (callback) g_onCreateCallbacks.push_back(callback);
    }

    void RegisterOnDestroy(const ResourceCallback& callback) {
        if (callback) g_onDestroyCallbacks.push_back(callback);
    }

    void RegisterOnRelease(const ResourceCallback& callback) {
        if (callback) g_onReleaseCallbacks.push_back(callback);
    }

    void RegisterOnRestore(const ResourceCallback& callback) {
        if (callback) g_onRestoreCallbacks.push_back(callback);
    }


    void RegisterVertexShaderInit(const VertexShaderInitCallback& callback) {
        if (callback) g_vertexShaderCallbacks.push_back(callback);
    }

    void RegisterPixelShaderInit(const PixelShaderInitCallback& callback) {
        if (callback) g_pixelShaderCallbacks.push_back(callback);
    }


namespace {
    // ------------------------------------------------------------------
    // [1.12] Przechwycenie urzadzenia NIEZALEZNE od tego, kto je tworzy.
    //
    // Trzy podejscia po kolei zawiodly: detour CGxDevice (brak w 1.12),
    // detour ciala CreateDevice (instalowal sie czysto, nie odpalal),
    // podmiana wpisu vtbl[16] (podmieniona, nie wolana). Wniosek: klient nie
    // tworzy urzadzenia na obiekcie, ktory widzimy - miedzy nim a DXVK-iem
    // stoi cos jeszcze.
    //
    // Obejscie: obiekty tej samej klasy C++ WSPOLDZIELA tablice wirtualna.
    // Tworzymy wiec wlasne, jednorazowe urzadzenie na ukrytym oknie, czytamy
    // z niego vtable urzadzenia, podmieniamy w niej EndScene - i zwalniamy
    // swoje urzadzenie. Od tej chwili KAZDE urzadzenie DXVK-a w tym procesie,
    // takze to klienta, przechodzi przez nasz EndScene i podaje sie w `this`.
    //
    // Sonda dowodzaca wykonalnosci: _lexara-probe utworzyl urzadzenie tym samym
    // d3d9.dll bez zadnych skutkow ubocznych.
    // ------------------------------------------------------------------
    using EndSceneDev_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    EndSceneDev_t oEndSceneShared = nullptr;
    bool g_sharedTried = false;

    // [1.12] Przelacznik renderera pod CTRL+ALT+F, obslugiwany raz na klatke.
    // Wykrywanie zbocza (a nie stanu) - inaczej jedno przytrzymanie przelaczaloby
    // flage kilkadziesiat razy na sekunde.
    // [1.12] CTRL+ALT+F zapisuje wybor do lexara112.cfg i dziala od NASTEPNEGO
    // uruchomienia gry. Zywe przelaczanie zostalo usuniete swiadomie.
    //
    // Powod jest w binarce, nie w wygodzie: GetOrCreateGlyphEntry (005CABD0) to
    // czysty odczyt z tablicy haszujacej po kodzie znaku - zwraca istniejacy wpis
    // BEZ sprawdzania jego pol. Zerowanie m_cellIndexMin/Max i m_texturePageIndex
    // (tak robi Lexara) NIE wymusza wiec ponownego wygenerowania glifu.
    // Co gorsza, przy wlaczonym rendererze klient nigdy nie wyrenderowal tych
    // glifow u siebie i nie przydzielil im komorki w swojej teksturze - nie ma
    // zadnego "stanu oryginalnego" do przywrocenia. Dlatego i wpisanie zera,
    // i zapamietywanie pol przed nadpisaniem konczylo sie smieciami na ekranie.
    //
    // Zywe przelaczanie wymagaloby USUNIECIA wpisu z tablicy klienta. Do zrobienia,
    // gdy znajdzie sie funkcja usuwajaca - na razie pewny wybor bije wygodny.
    void ZapiszWyborDoCfg(bool wlaczony) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) *(slash + 1) = 0;
        strcat_s(path, "lexara112.cfg");

        std::string tresc;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") == 0 && f) {
            char buf[4096];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            tresc.assign(buf, n);
            fclose(f);
        }

        const std::string klucz = "msdf_enabled=";
        const std::string linia = klucz + (wlaczony ? "1" : "0");
        const size_t poz = tresc.find(klucz);
        if (poz != std::string::npos) {
            size_t koniec = tresc.find('\n', poz);
            if (koniec == std::string::npos) koniec = tresc.size();
            tresc = tresc.substr(0, poz) + linia + tresc.substr(koniec);
        } else {
            if (!tresc.empty() && tresc[tresc.size() - 1] != '\n') tresc += "\n";
            tresc += linia + "\n";
        }

        if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(tresc.data(), 1, tresc.size(), f);
            fclose(f);
        }
    }

    void PollToggleKey() {
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                       && (GetAsyncKeyState(VK_MENU) & 0x8000)
                       && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (down && !wasDown) {
            static bool wybor = true;
            wybor = !wybor;
            ZapiszWyborDoCfg(wybor);
            Log("[MSDF] CTRL+ALT+F -> zapisano msdf_enabled=%d."
                " Zadziala po ponownym uruchomieniu gry.", wybor ? 1 : 0);
        }
        wasDown = down;
    }

    HRESULT STDMETHODCALLTYPE hkEndSceneShared(IDirect3DDevice9* device) {
        if (device && !g_device) {
            InstallDeviceHooks(device);
        }
        PollToggleKey();
        return oEndSceneShared(device);
    }

    void CaptureDeviceViaSharedVtable() {
        if (g_sharedTried) return;
        g_sharedTried = true;

        HMODULE hMod = GetModuleHandleA("d3d9.dll");
        if (!hMod) { Log("[MSDF] wspoldzielona vtable: brak d3d9.dll"); return; }
        auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(hMod, "Direct3DCreate9"));
        if (!create) { Log("[MSDF] wspoldzielona vtable: brak Direct3DCreate9"); return; }

        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        if (!d3d) { Log("[MSDF] wspoldzielona vtable: Direct3DCreate9 dal null"); return; }

        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "LexaraVtblProbe";
        RegisterClassA(&wc);
        HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 8, 8,
                                  nullptr, nullptr, wc.hInstance, nullptr);

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = 8;
        pp.BackBufferHeight = 8;
        pp.hDeviceWindow = hwnd;

        IDirect3DDevice9* tmp = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &tmp);
        Log("[MSDF] wspoldzielona vtable: CreateDevice hr=0x%08lX dev=%p", hr, tmp);

        if (SUCCEEDED(hr) && tmp) {
            void** vtbl = *reinterpret_cast<void***>(tmp);
            // EndScene to slot 42 w vtable IDirect3DDevice9.
            DWORD oldProt = 0;
            if (VirtualProtect(&vtbl[42], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                oEndSceneShared = reinterpret_cast<EndSceneDev_t>(vtbl[42]);
                vtbl[42] = reinterpret_cast<void*>(&hkEndSceneShared);
                DWORD t = 0;
                VirtualProtect(&vtbl[42], sizeof(void*), oldProt, &t);
                Log("[MSDF] wspoldzielona vtable: EndScene %p -> %p (czekam na urzadzenie klienta)",
                    reinterpret_cast<void*>(oEndSceneShared), vtbl[42]);
            } else {
                Log("[MSDF] wspoldzielona vtable: VirtualProtect odmowil (%lu)", GetLastError());
            }
            tmp->Release();
        }
        d3d->Release();
        if (hwnd) DestroyWindow(hwnd);
    }
}

    IDirect3DDevice9* GetDevice() {
        if (!g_device) CaptureDeviceViaSharedVtable();
        __try {
            // [1.12] Bylo: *(0x00C5DF88) + 0x397C, czyli global CGxDevice klienta
            // 3.3.5. Tutaj urzadzenie zapamietuje hak CreateDevice.
            IDirect3DDevice9* device = g_device;
            if (device && device->TestCooperativeLevel() == D3D_OK) {
                return device;
            }
            return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    IDirect3DVertexShader9* CompileVertexShader(const ResourceParams& p) {
        if (p.shaderCode.empty()) return nullptr;
        ID3DBlob* pCode = nullptr, * pError = nullptr;
        HRESULT hr = D3DCompile(p.shaderCode.data(), p.shaderCode.size(), nullptr, nullptr,
            nullptr, p.entryPoint.c_str(), p.target.c_str(), 0, 0, &pCode, &pError);
        if (FAILED(hr)) { LogShaderError(pError, 1); return nullptr; }
        IDirect3DVertexShader9* shader = nullptr;
        IDirect3DDevice9* device = GetDevice();
        if (!device) { pCode->Release(); return nullptr; }
        hr = device->CreateVertexShader(static_cast<const DWORD*>(pCode->GetBufferPointer()), &shader);
        pCode->Release();
        if (SUCCEEDED(hr)) {
            if (p.autoCleanup && p.ppResourceAddress) {
                *p.ppResourceAddress = reinterpret_cast<IUnknown*>(shader);
                RegisterForCleanup(p.ppResourceAddress, ResourceType::ShaderVertex, p);
            }
            return shader;
        }
        return nullptr;
    }

    IDirect3DPixelShader9* CompilePixelShader(const ResourceParams& p) {
        if (p.shaderCode.empty()) return nullptr;
        ID3DBlob* pCode = nullptr, * pError = nullptr;
        HRESULT hr = D3DCompile(p.shaderCode.data(), p.shaderCode.size(), nullptr, nullptr,
            nullptr, p.entryPoint.c_str(), p.target.c_str(), 0, 0, &pCode, &pError);
        if (FAILED(hr)) { LogShaderError(pError, 0); return nullptr; }
        IDirect3DPixelShader9* shader = nullptr;
        IDirect3DDevice9* device = GetDevice();
        if (!device) { pCode->Release(); return nullptr; }
        hr = device->CreatePixelShader(static_cast<const DWORD*>(pCode->GetBufferPointer()), &shader);
        pCode->Release();
        if (SUCCEEDED(hr)) {
            if (p.autoCleanup && p.ppResourceAddress) {
                *p.ppResourceAddress = reinterpret_cast<IUnknown*>(shader);
                RegisterForCleanup(p.ppResourceAddress, ResourceType::ShaderPixel, p);
            }
            return shader;
        }
        return nullptr;
    }

    bool CreateTexture(IDirect3DTexture9** ppTexture, ResourceParams p) {
        if (!ppTexture) return false;
        *ppTexture = nullptr; if (p.ppSurface) *p.ppSurface = nullptr;
        IDirect3DDevice9* device = GetDevice();
        if (!device) return false;
        if (p.width == 0 || p.height == 0) {
            D3DVIEWPORT9 vp;
            if (FAILED(device->GetViewport(&vp))) return false;
            if (p.width == 0) p.width = vp.Width;
            if (p.height == 0) p.height = vp.Height;
        }
        const HRESULT hrTex = device->CreateTexture(p.width, p.height, p.levels, p.usage, p.format, p.pool, ppTexture, p.pSharedHandle);
        if (FAILED(hrTex)) {
            Log("[MSDF] CreateTexture %ux%u fmt=%d pool=%d hr=0x%08lX wolne=%lu MB",
                p.width, p.height, (int)p.format, (int)p.pool, hrTex,
                device->GetAvailableTextureMem() / (1024u * 1024u));
            return false;
        }
        if (p.clearToZero && p.pool != D3DPOOL_DEFAULT) {
            D3DLOCKED_RECT lr;
            if (FAILED((*ppTexture)->LockRect(0, &lr, nullptr, 0))) return false;
            std::memset(lr.pBits, 0, p.height * lr.Pitch);
            (*ppTexture)->UnlockRect(0);
        }
        if (p.autoCleanup) RegisterForCleanup(reinterpret_cast<IUnknown**>(ppTexture), ResourceType::Texture, p);
        if (p.ppSurface) (*ppTexture)->GetSurfaceLevel(p.surfLevel, p.ppSurface);
        return true;
    }

    bool CreateRenderTarget(IDirect3DSurface9** ppSurface, ResourceParams p) {
        if (!ppSurface) return false;
        IDirect3DDevice9* device = GetDevice();
        if (!device) return false;
        if (p.width == 0 || p.height == 0) {
            D3DVIEWPORT9 vp;
            device->GetViewport(&vp);
            if (p.width == 0) p.width = vp.Width;
            if (p.height == 0) p.height = vp.Height;
        }
        if (SUCCEEDED(device->CreateRenderTarget(p.width, p.height, p.format, p.multisample, p.quality, p.lockable, ppSurface, p.pSharedHandle))) {
            if (p.autoCleanup)  RegisterForCleanup(reinterpret_cast<IUnknown**>(ppSurface), ResourceType::RenderTarget, p);
            return true;
        }
        return false;
    }
}

void D3D::initialize() {
    // [1.12] Zadnego adresu klienta. Jesli d3d9.dll juz jest w procesie,
    // podpinamy sie od razu; jesli nie - czekamy na LoadLibrary.
    // NIE wolamy tu LoadLibrary sami: initialize() leci z DllMain, a ladowanie
    // biblioteki pod blokada loadera to proszenie sie o zakleszczenie.
    if (HMODULE hMod = GetModuleHandleA("d3d9.dll")) {
        TryHookD3D9(hMod);
        return;
    }

    oLoadLibraryA = LoadLibraryA;
    oLoadLibraryW = LoadLibraryW;
    oLoadLibraryExW = LoadLibraryExW;
    Hooks::Detour(&oLoadLibraryA, hkLoadLibraryA);
    Hooks::Detour(&oLoadLibraryW, hkLoadLibraryW);
    Hooks::Detour(&oLoadLibraryExW, hkLoadLibraryExW);
}

void D3D::shutdown() {
    // [1.12] Zastepuje hak CGxDeviceD3d::IDestroyD3d z 3.3.5 - tam klient
    // sam zglaszal zniszczenie urzadzenia, tu robimy to przy odpinaniu DLL-a.
    for (auto& cb : g_onDestroyCallbacks) cb();
    g_device = nullptr;
}
