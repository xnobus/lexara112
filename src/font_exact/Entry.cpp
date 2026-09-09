#include "D3D.h"
#include "Hooks.h"
#include "MSDF.h"
#include "TexNullFill.h"
#include "../Logger.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../third_party/Detours/detours.h"

namespace {
    // FontExact standalone: HD fonts are ALWAYS enabled.
    // The original Transmorpher gated this behind state\msdf_mode.txt.
    // That check is intentionally removed here.
    bool OnAttach() {
        Hooks::initialize();

        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR) {
            Log("[MSDF] DetourTransactionBegin failed (%ld)", status);
            return false;
        }

        status = DetourUpdateThread(GetCurrentThread());
        if (status != NO_ERROR) {
            DetourTransactionAbort();
            Log("[MSDF] DetourUpdateThread failed (%ld)", status);
            return false;
        }

        D3D::initialize();
        MSDF::initialize();
        // Po MSDF::initialize(), bo to ono wczytuje lexara112.cfg, a latka
        // czyta z niego wlasna flage. Celowo POZA bramka msdf_enabled:
        // dotyczy tekstur, nie czcionek, i ma dzialac takze przy wylaczonym
        // rendererze MSDF.
        TexNullFill::initialize();

        status = DetourTransactionCommit();
        if (status != NO_ERROR) {
            Log("[MSDF] DetourTransactionCommit failed (%ld)", status);
            return false;
        }

        Log("[MSDF] Initialization committed — HD fonts active");

        const std::string locale = MSDF::GetGameLocale();
        const bool isCjk = (locale == "zhCN" || locale == "zhTW" || locale == "koKR");
        if (isCjk) {
            Log("[MSDF] CJK locale detected (%s)", locale.c_str());
        } else {
            Log("[MSDF] Locale: %s", locale.c_str());
        }

        return true;
    }
}

bool FontExact_OnAttach() {
    return OnAttach();
}
