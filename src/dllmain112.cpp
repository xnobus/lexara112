// Lexara on Turtle WoW 1.12 - DLL entry point.
//
// The difference from the original: 3.3.5 loaded Lexara as a dinput8.dll proxy.
// This client injects the DLLs listed in `dlls.txt` through VanillaFixes.exe, so
// the proxy is unnecessary - a plain DLL added to that list is enough. The
// original's proxy layer (dllmain.cpp, Proxy.cpp, dinput8_exports.def) is not
// present in this repository.
//
// LuxShoulderCam.dll is not loaded either (a 3.3.5 mod; it does not exist here).

#include "ShutdownCheck.h"
extern "C" volatile bool g_isProcessTerminating = false;

#include <windows.h>
#include "MSDF.h"
#include "font_exact/D3D.h"

// [1.12] Logging is ENABLED for the first round of testing. It writes to
// lexara112.log next to the client. Turn it off once the port is working - every
// entry opens the file, so this must never be called from the draw loop without a
// guard.
#include <cstdio>
#include <cstdarg>

void Log(const char* fmt, ...) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) *(slash + 1) = 0;
    strcat_s(path, "lexara112.log");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}

bool FontExact_OnAttach();

HMODULE g_hThisModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason, LPVOID lpReserved)
{
    switch (ul_reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hThisModule = hModule;
        DisableThreadLibraryCalls(hModule);
        FontExact_OnAttach();
        break;

    case DLL_PROCESS_DETACH:
        g_isProcessTerminating = true;
        // [1.12] lpReserved != NULL means the process is exiting, not a FreeLibrary.
        // VanillaFixes injects us BEFORE the client loads d3d9.dll (DXVK), which in
        // turn loads the Vulkan driver (nvoglv32.dll), and the loader detaches in
        // reverse order - by the time we get here both are already torn down.
        // Releasing our shaders then walks DXVK -> vkDestroyPipeline
        // (d3d9.trackPipelineLifetime) into the dead driver: "The instruction at
        // nvoglv32+0x855FFD referenced memory at 0x00000010" on EVERY exit.
        // The OS reclaims the D3D objects with the process; ~AtlasPage already
        // skips its texture for the same reason.
        if (!lpReserved) D3D::shutdown();
        MSDF::shutdown();
        break;
    }
    return TRUE;
}
