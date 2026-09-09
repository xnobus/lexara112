// FontExact — standalone HD font DLL for WoW 3.3.5 (build 12340)
// Drop dinput8.dll in the WoW root folder. No addon required.
// Derived from Transmorpher 2.0.0 (StealthMorpher) — font subsystem only.

#include "ShutdownCheck.h"
extern "C" volatile bool g_isProcessTerminating = false;

#include <windows.h>
#include "Proxy.h"
#include "MSDF.h"

// Logging disabled — no files or folders created.
void Log(const char* fmt, ...) { (void)fmt; }

bool FontExact_OnAttach();

HMODULE g_hThisModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason, LPVOID /*lpReserved*/)
{
    switch (ul_reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hThisModule = hModule;
        DisableThreadLibraryCalls(hModule);
        SetupProxy();
        FontExact_OnAttach();
        LoadLibraryA("CameraHeight335.dll"); // Camera height mod
        break;

    case DLL_PROCESS_DETACH:
        g_isProcessTerminating = true;
        MSDF::shutdown();
        break;
    }
    return TRUE;
}