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

        // [1.12] Reset takes over the role played in 3.3.5 by the client hooks
        // IReleaseD3dResources (before device loss) and NotifyOnDeviceRestored
        // (after recovery). Neither of those functions is hooked here at all.
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

        // [1.12] Was: a hook on the 3.3.5 client's CGxDevice::DeviceCreate, which
        // then read the device out of the global at 00C5DF88. The 1.12 client has
        // no such global (checked - 00C0F464 nearby is viewport state, not the
        // device), and d3d9.dll is loaded DYNAMICALLY - it is not in the import
        // table. So the device arrives here through the chain LoadLibrary ->
        // Direct3DCreate9 -> IDirect3D9::CreateDevice. Zero client addresses in
        // this entire layer.
        void InstallDeviceHooks(IDirect3DDevice9* device) {
            if (g_isProcessTerminating || !device) return;
            g_device = device;
            Log("[MSDF] device captured: %p", device);
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
                            const LONG st = DetourTransactionCommit();
                            Log("[MSDF] device hooks: commit=%ld", st);
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}

                    for (auto& callback : g_onCreateCallbacks) callback();
                }
            }
        }

        // ------------------------------------------------------------------
        // [1.12] Capturing the device without a single client address.
        //
        // WoW.exe 1.12 does not import d3d9.dll statically (the import table holds
        // only opengl32.dll); it loads it through LoadLibrary and pulls
        // Direct3DCreate9 out with GetProcAddress. At the moment VanillaFixes
        // injects the DLL the d3d9 module is NOT loaded yet, so its export cannot
        // be hooked right away. Hence the chain: a hook on LoadLibrary* catches
        // the load, then Direct3DCreate9, then CreateDevice.
        // ------------------------------------------------------------------
        using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
        using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND,
                                                           DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
        using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
        using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
        using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

        Direct3DCreate9_t oDirect3DCreate9 = nullptr;

        using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
        Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;
        HRESULT WINAPI hkDirect3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);
        CreateDevice_t oCreateDevice = nullptr;
        LoadLibraryA_t oLoadLibraryA = nullptr;
        LoadLibraryW_t oLoadLibraryW = nullptr;
        LoadLibraryExW_t oLoadLibraryExW = nullptr;
        bool g_d3d9Hooked = false;

        using CreateDeviceEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND,
                                                             DWORD, D3DPRESENT_PARAMETERS*, void*, IDirect3DDevice9**);
        CreateDeviceEx_t oCreateDeviceEx = nullptr;

        // [1.12] Defined below. Declared here because hkDirect3DCreate9 calls it
        // right after swapping the vtable entries - see the comment at the call site.
        void CaptureDeviceViaSharedVtable(IDirect3D9* provided = nullptr);

        // [1.12] Raised while OUR OWN 8x8 stand-in device is being created - the
        // one used to obtain the shared vtable.
        //
        // Without this guard a cascade followed: vtbl[16] is already swapped, so
        // our own CreateDevice falls into hkCreateDevice, which calls
        // InstallDeviceHooks and stores the stand-in device as g_device - the very
        // device we release a moment later. From then on g_device points at a freed
        // object, and hkEndSceneShared never swaps it for the client's device,
        // because it sees the condition (device && !g_device) as false.
        // The log of such a run: "hkCreateDevice dev=0A041D00" followed at once by
        // "device captured: 0A041D00" with the same address, then silence.
        bool g_creatingStandIn = false;

        HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9* pThis, UINT adapter, D3DDEVTYPE type,
                                                   HWND hFocus, DWORD flags, D3DPRESENT_PARAMETERS* pPP,
                                                   void* pFullscreenMode, IDirect3DDevice9** ppDevice) {
            const HRESULT hr = oCreateDeviceEx(pThis, adapter, type, hFocus, flags, pPP, pFullscreenMode, ppDevice);
            Log("[MSDF] hkCreateDeviceEx: hr=0x%08lX dev=%p%s", hr, (ppDevice ? *ppDevice : nullptr),
                g_creatingStandIn ? " (our stand-in - SKIPPED)" : "");
            if (!g_creatingStandIn && SUCCEEDED(hr) && ppDevice && *ppDevice) {
                InstallDeviceHooks(*ppDevice);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* pThis, UINT adapter, D3DDEVTYPE type,
                                                 HWND hFocus, DWORD flags,
                                                 D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDevice) {
            const HRESULT hr = oCreateDevice(pThis, adapter, type, hFocus, flags, pPP, ppDevice);
            Log("[MSDF] hkCreateDevice: hr=0x%08lX dev=%p%s", hr, (ppDevice ? *ppDevice : nullptr),
                g_creatingStandIn ? " (our stand-in - SKIPPED)" : "");
            if (!g_creatingStandIn && SUCCEEDED(hr) && ppDevice && *ppDevice) {
                InstallDeviceHooks(*ppDevice);
            }
            return hr;
        }

        // [1.12] Remembered so that we can check later whether our entry is still
        // in place. hkCreateDevice did not fire for the client's device EVEN ONCE,
        // and modules from dlls.txt (SuperWoWhook, VfPatcher, no1600x1200) hook D3D
        // as well - if one of them overwrote slot 16 AFTER us and kept the original,
        // we are permanently bypassed, and that would explain everything.
        void** g_ifaceVtbl = nullptr;
        void*  g_ourCreateDevice = nullptr;

        void PatchInterfaceVtable(void* d3dRaw, const char* origin) {
            IDirect3D9* d3d = reinterpret_cast<IDirect3D9*>(d3dRaw);
            Log("[MSDF] %s -> d3d=%p", origin, d3d);
            if (d3d && !oCreateDevice) {
                __try {
                    // CreateDevice is slot 16 of the IDirect3D9 vtable.
                    void** vtbl = *reinterpret_cast<void***>(d3d);
                    oCreateDevice = reinterpret_cast<CreateDevice_t>(vtbl[16]);
                    // [1.12] A VTABLE ENTRY SWAP, not a detour on the function body.
                    //
                    // A detour on 6F3E27D0 installed cleanly (begin/update/
                    // attach/commit = 0), and yet the hook never fired once, even
                    // though the game was rendering - meaning the body at that
                    // address does not sit on the call path (thunk / alias / an
                    // effect of LTO in DXVK). Swapping the pointer in the virtual
                    // table alone sidesteps the problem: COM dispatch reads the
                    // entry, not the function address.
                    DWORD oldProt = 0;
                    if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                        vtbl[16] = reinterpret_cast<void*>(&hkCreateDevice);
                        DWORD tmp = 0;
                        VirtualProtect(&vtbl[16], sizeof(void*), oldProt, &tmp);
                        g_ifaceVtbl = vtbl;
                        g_ourCreateDevice = vtbl[16];
                        Log("[MSDF] vtbl[16] swapped: %p -> %p",
                            reinterpret_cast<void*>(oCreateDevice), vtbl[16]);
                    }

                    // [1.12] Slot 20 is IDirect3D9Ex::CreateDeviceEx. The 1.12
                    // client would never call it itself, but one of the mods
                    // (SuperWoWhook, VanillaUtils) may promote the device to D3D9Ex.
                    //
                    // NOTE: this slot EXISTS only if the object really implements
                    // IDirect3D9Ex. DXVK returns an Ex object from Direct3DCreate9,
                    // so it was safe there - but the Windows d3d9.dll returns a
                    // plain IDirect3D9 whose table ends at slot 16. Writing to
                    // vtbl[20] there was a write PAST the end of the table, i.e.
                    // over someone else's data in .rdata. So we ask the object
                    // before touching anything.
                    IDirect3D9Ex* ex = nullptr;
                    const bool hasEx = SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex),
                                                                    reinterpret_cast<void**>(&ex))) && ex;
                    if (ex) ex->Release();

                    if (!hasEx) {
                        Log("[MSDF] interface is not IDirect3D9Ex - NOT touching slot 20"
                            " (that is the case on the Windows d3d9, without DXVK)");
                    } else {
                        DWORD oldProtEx = 0;
                        if (VirtualProtect(&vtbl[20], sizeof(void*), PAGE_READWRITE, &oldProtEx)) {
                            oCreateDeviceEx = reinterpret_cast<CreateDeviceEx_t>(vtbl[20]);
                            vtbl[20] = reinterpret_cast<void*>(&hkCreateDeviceEx);
                            DWORD tmpEx = 0;
                            VirtualProtect(&vtbl[20], sizeof(void*), oldProtEx, &tmpEx);
                            Log("[MSDF] vtbl[20] (CreateDeviceEx) swapped: %p -> %p",
                                reinterpret_cast<void*>(oCreateDeviceEx), vtbl[20]);
                        } else {
                            Log("[MSDF] VirtualProtect on vtbl[20] REFUSED (error %lu)", GetLastError());
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { oCreateDevice = nullptr; }
            }
        }

        // [1.12] Reclaiming the entry when somebody hooked after us. Their entry
        // becomes our "original", so the chain is preserved and their hook keeps
        // working - we merely run in front of it. Called periodically, because
        // there is no telling which mod installs its hooks, or when.
        void ReassertInterfaceHook() {
            if (!g_ifaceVtbl || !g_ourCreateDevice) return;
            __try {
                void* current = g_ifaceVtbl[16];
                if (current == g_ourCreateDevice) return;

                DWORD oldProt = 0;
                if (VirtualProtect(&g_ifaceVtbl[16], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    oCreateDevice = reinterpret_cast<CreateDevice_t>(current);
                    g_ifaceVtbl[16] = g_ourCreateDevice;
                    DWORD t = 0;
                    VirtualProtect(&g_ifaceVtbl[16], sizeof(void*), oldProt, &t);
                    Log("[MSDF] vtbl[16] RECLAIMED: foreign entry %p becomes our original", current);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // [1.12] A guard over the vtbl[16] entry. Something restores DXVK's
        // original entry right after our swap, and the client creates its device in
        // that window - without this, hkCreateDevice did not fire for it EVEN ONCE.
        //
        // The thread exits as soon as the device is caught. That matters: this work
        // used to live in the probe thread, which kept running for two minutes after
        // the catch and called TestCooperativeLevel every half second from a FOREIGN
        // thread, taking DXVK's device lock right under the rendering thread's nose.
        // That was the source of the noticeable lag right after the game started.
        DWORD WINAPI VtableGuardThread(LPVOID) {
            for (int i = 0; i < 4000 && !g_device; ++i) {
                ReassertInterfaceHook();
                Sleep(1);
            }
            Log(g_device ? "[MSDF] vtbl[16] guard: device caught, thread exiting"
                         : "[MSDF] vtbl[16] guard: device never appeared, thread exiting");
            return 0;
        }

        void StartVtableGuard() {
            static bool started = false;
            if (started) return;
            started = true;
            if (HANDLE h = CreateThread(nullptr, 0, VtableGuardThread, nullptr, 0, nullptr)) CloseHandle(h);
        }

        IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdkVersion) {
            IDirect3D9* d3d = oDirect3DCreate9(sdkVersion);
            PatchInterfaceVtable(d3d, "hkDirect3DCreate9");
            if (d3d) StartVtableGuard();
            // [1.12] This is the earliest moment at which we hold a ready
            // IDirect3D9, and therefore access to the device vtable. The probe uses
            // it to install its captures independently of the font path - with
            // ft_hooks=0 that path does not exist and the probe had so far collected
            // not a single frame.
            //
            // We do NOT capture the client's device here: a vtable entry swap
            // installed this early does not catch it (verified), and our own
            // stand-in device would fall into our own CreateDevice hook.
            return d3d;
        }

        HRESULT WINAPI hkDirect3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D) {
            const HRESULT hr = oDirect3DCreate9Ex(sdkVersion, ppD3D);
            IDirect3D9Ex* d3d = (ppD3D ? *ppD3D : nullptr);
            Log("[MSDF] hkDirect3DCreate9Ex: sdk=%u hr=0x%08lX", sdkVersion, hr);
            if (SUCCEEDED(hr) && d3d) {
                PatchInterfaceVtable(d3d, "hkDirect3DCreate9Ex");
                StartVtableGuard();
            }
            return hr;
        }

        // Hooks the d3d9.dll export; called once the module is already in the process.
        void TryHookD3D9(HMODULE hMod) {
            if (!hMod) return;
            auto fn = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hMod, "Direct3DCreate9"));
            if (!fn) return;

            // [1.12] Until now this was "hook the first module named d3d9 and stop".
            // That is enough only when there is exactly one such module. In this
            // client dlls.txt holds a separate dxvk entry plus a few mods, so there
            // may be a proxy forwarding to the real DXVK - and then the client
            // creates its device through THAT OTHER module and our hook never sees
            // it once. That is exactly what the log looked like all day:
            // hkDirect3DCreate9 kept firing while hkCreateDevice fired only for OUR
            // OWN stand-in device. So we hook every module that exports it,
            // filtering by function address.
            char moduleName[MAX_PATH] = {0};
            GetModuleFileNameA(hMod, moduleName, MAX_PATH);
            // Careful with comparisons against oDirect3DCreate9: after Hooks::Detour
            // that variable holds the TRAMPOLINE, not the export address. The first
            // version of this check reported "another module" for the same file.
            if (g_d3d9Hooked) {
                Log("[MSDF] another module exports Direct3DCreate9: %s (%p) - detour stays on the first",
                    moduleName, fn);
                return;
            }
            Log("[MSDF] d3d9 caught: %s, Direct3DCreate9 = %p", moduleName, fn);
            g_d3d9Hooked = true;
            oDirect3DCreate9 = fn;

            // [1.12] Direct3DCreate9Ex is the SECOND entry point into d3d9 and had
            // been left unhooked. The suspicion: hkDirect3DCreate9 fires once
            // (probably from some mod), we patch THAT object's vtable, and the client
            // takes its IDirect3D9Ex from here - which is why hkCreateDevice did not
            // fire for the client's device EVEN ONCE all day. That is where the whole
            // stand-in device workaround came from, the one that turned out to be the
            // cause of the stutter.
            if (auto fnEx = reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(hMod, "Direct3DCreate9Ex"))) {
                oDirect3DCreate9Ex = fnEx;
                Log("[MSDF] Direct3DCreate9Ex = %p", fnEx);
            } else {
                Log("[MSDF] no Direct3DCreate9Ex export");
            }
            // [1.12] The transaction status MUST be checked: this runs from inside
            // the LoadLibrary hook, i.e. under the loader lock, where Detours can
            // refuse. The first version ignored the result and the device was
            // silently never captured.
            const LONG b1 = DetourTransactionBegin();
            const LONG u1 = DetourUpdateThread(GetCurrentThread());
            const LONG a1 = Hooks::Detour(&oDirect3DCreate9, hkDirect3DCreate9);
            const LONG a2 = oDirect3DCreate9Ex
                ? Hooks::Detour(&oDirect3DCreate9Ex, hkDirect3DCreate9Ex) : 0;
            const LONG c1 = DetourTransactionCommit();
            Log("[MSDF] detour Direct3DCreate9: begin=%ld update=%ld attach=%ld attachEx=%ld commit=%ld",
                b1, u1, a1, a2, c1);
        }

        bool IsD3D9Name(const char* name) {
            if (!name) return false;
            if (const char* slash = strrchr(name, '\\')) name = slash + 1;
            // Forward slash too: the dxvk entry in dlls.txt can spell the path as
            // "dxvk/d3d9.dll", and then a backslash alone is not enough.
            if (const char* fwd = strrchr(name, '/')) name = fwd + 1;
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
    // [1.12] Capturing the device INDEPENDENTLY of whoever creates it.
    //
    // Three approaches failed in turn: a detour on CGxDevice (absent in 1.12),
    // a detour on the CreateDevice body (installed cleanly, never fired),
    // a vtbl[16] entry swap (swapped, never called). Conclusion: the client does
    // not create its device on the object we can see - there is something else
    // standing between it and DXVK.
    //
    // The way around it: objects of the same C++ class SHARE the virtual table.
    // So we create our own throwaway device on a hidden window, read the device
    // vtable out of it, swap EndScene inside that table - and release our own
    // device. From then on EVERY DXVK device in this process, the client's
    // included, goes through our EndScene and hands itself over in `this`.
    //
    // The feasibility probe (a separate EXE, outside this repo) created a device
    // with the very same d3d9.dll with no side effects whatsoever.
    // ------------------------------------------------------------------
    using EndSceneDev_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    EndSceneDev_t oEndSceneShared = nullptr;
    bool g_sharedTried = false;

    // [1.12] The renderer toggle on CTRL+ALT+F, serviced once per frame.
    // Edge detection (not level) - otherwise a single held press would flip the
    // flag dozens of times a second.
    // [1.12] CTRL+ALT+F writes the choice to lexara112.cfg and takes effect on the
    // NEXT launch of the game. Live toggling was removed deliberately.
    //
    // The reason is in the binary, not in convenience: GetOrCreateGlyphEntry
    // (005CABD0) is a plain lookup in a hash table keyed by character code - it
    // returns an existing entry WITHOUT inspecting its fields. Zeroing
    // m_cellIndexMin/Max and m_texturePageIndex (which is what Lexara does)
    // therefore does NOT force the glyph to be regenerated.
    // Worse still, with the renderer enabled the client never rendered those glyphs
    // itself and never allocated them a cell in its own texture - there is no
    // "original state" to restore. That is why both writing zeros and saving the
    // fields before overwriting them ended in garbage on screen.
    //
    // Live toggling would require REMOVING the entry from the client's table. Worth
    // doing once a removal function turns up - for now a safe choice beats a
    // convenient one.
    void WriteToggleToCfg(bool enabled) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) *(slash + 1) = 0;
        strcat_s(path, "lexara112.cfg");

        std::string content;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") == 0 && f) {
            char buf[4096];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            content.assign(buf, n);
            fclose(f);
        }

        const std::string key = "msdf_enabled=";
        const std::string line = key + (enabled ? "1" : "0");
        const size_t pos = content.find(key);
        if (pos != std::string::npos) {
            size_t end = content.find('\n', pos);
            if (end == std::string::npos) end = content.size();
            content = content.substr(0, pos) + line + content.substr(end);
        } else {
            if (!content.empty() && content[content.size() - 1] != '\n') content += "\n";
            content += line + "\n";
        }

        if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
        }
    }

    void PollToggleKey() {
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                       && (GetAsyncKeyState(VK_MENU) & 0x8000)
                       && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (down && !wasDown) {
            static bool choice = true;
            choice = !choice;
            WriteToggleToCfg(choice);
            Log("[MSDF] CTRL+ALT+F -> wrote msdf_enabled=%d."
                " Takes effect after the game is restarted.", choice ? 1 : 0);
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

    // [1.12] Obtaining the IDirect3DDevice9 vtable without waiting for the client's
    // device. Creates a throwaway 8x8 device, reads the virtual table pointer out
    // of it and releases it immediately. The table itself is static and shared by
    // every device from that d3d9.dll, so it stays valid after the release.
    void** AcquireSharedDeviceVtable(IDirect3D9* provided) {
        static void** cached = nullptr;
        static bool tried = false;
        if (tried) return cached;
        tried = true;

        // [1.12] The guard MUST be here, not in CaptureDeviceViaSharedVtable.
        // Placed one level up it did not work: the probe calls this function
        // directly and the stand-in device was created anyway, which the log only
        // reported AFTER the fact ("SKIPPED" three lines below "CreateDevice
        // dev=..."). For why this is disabled by default - see below, at
        // CaptureDeviceViaSharedVtable.
        if (!MSDF::CfgFlagOptIn("shared_vtable")) {
            Log("[MSDF] stand-in device will NOT be created (shared_vtable disabled)");
            return nullptr;
        }

        // When the caller already holds an IDirect3D9 (hkDirect3DCreate9) we use
        // theirs - calling Direct3DCreate9 ourselves would recurse into the
        // detoured export.
        IDirect3D9* d3d = provided;
        const bool ownD3d = (provided == nullptr);

        if (ownD3d) {
            HMODULE hMod = GetModuleHandleA("d3d9.dll");
            if (!hMod) { Log("[MSDF] shared vtable: no d3d9.dll"); return nullptr; }
            auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(hMod, "Direct3DCreate9"));
            if (!create) { Log("[MSDF] shared vtable: no Direct3DCreate9"); return nullptr; }
            d3d = create(D3D_SDK_VERSION);
        }
        if (!d3d) { Log("[MSDF] shared vtable: no IDirect3D9"); return nullptr; }

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
        g_creatingStandIn = true;
        const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &tmp);
        g_creatingStandIn = false;
        Log("[MSDF] shared vtable: CreateDevice hr=0x%08lX dev=%p", hr, tmp);

        if (SUCCEEDED(hr) && tmp) {
            cached = *reinterpret_cast<void***>(tmp);
            tmp->Release();
        }
        if (ownD3d) d3d->Release();   // we never release someone else's IDirect3D9
        if (hwnd) DestroyWindow(hwnd);
        return cached;
    }

    void CaptureDeviceViaSharedVtable(IDirect3D9* provided) {
        if (g_sharedTried) return;
        g_sharedTried = true;

        // [1.12] THIS IS THE CAUSE OF THE ANIMATION STUTTER - DISABLED by default.
        //
        // Measured: with the font renderer off, the probe installing no captures
        // and the vtbl[42] entry untouched - that is, with NOTHING of ours running
        // in the frame - merely creating and releasing this 8x8 stand-in device
        // once is enough to make every model's animation start stuttering. Without
        // it, everything else being identical, it is clean.
        //
        // What happens here is that a second D3D9 device is created on DXVK, with
        // its own window, before the client creates its own. The client then gets a
        // device in a different driver state than usual. Frame time stays the same,
        // so this is not a cost - it is a change of conditions.
        //
        // Enable only through the shared_vtable=1 entry, and only for diagnostics.
        // The guard itself lives in AcquireSharedDeviceVtable.

        void** vtbl = AcquireSharedDeviceVtable(provided);
        if (!vtbl) return;

        // [1.12] A vtable ENTRY SWAP, not a body detour. It only works when done
        // AFTER the client's device has been created - verified: installed early,
        // from hkDirect3DCreate9, it never caught the client's device once
        // (the log ended on that line and "device captured" never appeared).
        // That is why this call stays lazy, from GetDevice(), i.e. from the font
        // path.
        DWORD oldProt = 0;
        if (VirtualProtect(&vtbl[42], sizeof(void*), PAGE_READWRITE, &oldProt)) {
            oEndSceneShared = reinterpret_cast<EndSceneDev_t>(vtbl[42]);
            vtbl[42] = reinterpret_cast<void*>(&hkEndSceneShared);
            DWORD t = 0;
            VirtualProtect(&vtbl[42], sizeof(void*), oldProt, &t);
            Log("[MSDF] shared vtable: EndScene %p -> %p (waiting for the client device)",
                reinterpret_cast<void*>(oEndSceneShared), vtbl[42]);
        } else {
            Log("[MSDF] shared vtable: VirtualProtect refused (%lu)", GetLastError());
        }
    }
}


    IDirect3DDevice9* GetDevice() {
        if (!g_device) CaptureDeviceViaSharedVtable();
        __try {
            // [1.12] Was: *(0x00C5DF88) + 0x397C, i.e. the 3.3.5 client's CGxDevice
            // global. Here the device is stored by the CreateDevice hook.
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
            Log("[MSDF] CreateTexture %ux%u fmt=%d pool=%d hr=0x%08lX free=%lu MB",
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
    // [1.12] Not a single client address. If d3d9.dll is already in the process we
    // hook it right away; if not - we wait for LoadLibrary.
    // We do NOT call LoadLibrary ourselves here: initialize() runs from DllMain, and
    // loading a library under the loader lock is asking for a deadlock.
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
    // [1.12] Replaces the CGxDeviceD3d::IDestroyD3d hook from 3.3.5 - there the
    // client reported device destruction itself, here we do it when the DLL
    // detaches.
    for (auto& cb : g_onDestroyCallbacks) cb();
    g_device = nullptr;
}
