// Lexara na Turtle WoW 1.12 - punkt wejscia DLL.
//
// Roznica wobec oryginalu: 3.3.5 ladowal Lexare jako proxy dinput8.dll
// (stad Proxy.cpp i dinput8_exports.def). Ten klient wstrzykuje DLL-e
// z listy `dlls.txt` przez VanillaFixes.exe, wiec proxy jest zbedne -
// wystarczy zwykly DLL dopisany do tej listy.
//
// Nie ladujemy tez LuxShoulderCam.dll (mod do 3.3.5, nie istnieje tutaj).

#include "ShutdownCheck.h"
extern "C" volatile bool g_isProcessTerminating = false;

#include <windows.h>
#include "MSDF.h"
#include "font_exact/D3D.h"

// [1.12] Log WLACZONY na czas pierwszych testow. Pisze do lexara112.log
// obok klienta. Wylaczyc, gdy port bedzie dzialal - kazdy wpis to otwarcie
// pliku, wiec nie wolno tego wolac z petli rysowania bez straznika.
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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason, LPVOID)
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
        D3D::shutdown();
        MSDF::shutdown();
        break;
    }
    return TRUE;
}
