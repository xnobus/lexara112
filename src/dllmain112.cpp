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

    // [1.12] Time and process on every line: two clients started from one directory
    // write this file interleaved (seen in a report), and "it stutters when X
    // happens" cannot be matched to anything without a clock. The line is built
    // first and written in one call, because the glyph workers log too.
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%lu] ",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    if (len < 0) len = 0;

    va_list a;
    va_start(a, fmt);
    const int body = _vsnprintf_s(line + len, sizeof(line) - len - 1, _TRUNCATE, fmt, a);
    va_end(a);
    len = body < 0 ? static_cast<int>(strlen(line)) : len + body;
    line[len++] = '\n';
    line[len] = 0;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    fputs(line, f);
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
