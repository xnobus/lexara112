#pragma once
#include <windows.h>
#include <filesystem>
#undef min
#undef max

#include "GameClient.h"
#include "D3D.h"

#include <msdfgen.h>
#include <msdfgen-ext.h>

struct GlyphMetrics {
    uint16_t width = 0;
    uint16_t height = 0;
	FT_Int bitmapTop = 0;
	FT_Int bitmapLeft = 0;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    uint16_t atlasPageIndex = 0;
    const uint8_t* pixelData = nullptr;
};

struct GlyphMetricsToStore {
    uint32_t codepoint = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    FT_Int bitmapTop = 0;
	FT_Int bitmapLeft = 0;
    std::vector<uint8_t> ownedPixelData;
    uint32_t dataSize = 0;
};

class MSDFCache;
class MSDFFont;

namespace MSDF {
	// ----  if you want overkill quality, try raising these
	inline constexpr uint32_t ATLAS_SIZE = 2048; // 1024-2048
	inline constexpr uint32_t PREGEN_START_KEY = VK_F11;
	// [1.12] Rejestry stalych naszych shaderow. NIE ruszac w dol bez pomiaru.
	//
	// W 3.3.5 Lexara podmieniala bajtkod w obiektach shaderow klienta, wiec
	// dzielila z nim uklad rejestrow i c23 bylo bezpieczne. Port 1.12 ustawia
	// wlasne shadery wprost na urzadzeniu, za plecami CGxDevice - a ten cachuje
	// stale i nie odswieza tego, o czym nie wie. Pomiar w dzialajacym kliencie
	// po czym klient naprawde pisze w swiecie: vs c2..c186. Poprzedni uklad
	// (WorldViewProj w c0..c3 domyslnie + control w c23) kolidowal wiec z
	// klientem na c2, c3 i c23 - okolo 190 nadpisan na klatke.
	//
	// Uklad ponizej lezy powyzej calego zakresu klienta. Granice sprzetowe:
	// vs_3_0 ma c0..c255, ps_3_0 c0..c223 - dlatego control (czytany przez OBA
	// shadery) siedzi nizej niz WorldViewProj (czytany tylko przez vs).
	// Wartosci musza sie zgadzac z register(cNN) w MSDFShaders.h.
	inline constexpr uint32_t SDF_CONTROL_REG = 220;   // vs + ps
	inline constexpr uint32_t SDF_WVP_REG     = 240;   // tylko vs, 4 rejestry
	inline constexpr uint32_t ATLAS_GUTTER = 14; // usually spread + 2-4
	inline constexpr uint32_t SDF_RENDER_SIZE = 96; // 48-128
	inline constexpr uint32_t SDF_SPREAD = 12; // 6-12
	inline constexpr D3DFORMAT D3DFMT = D3DFMT_A8R8G8B8; // D3DFMT_A8R8G8B8-D3DFMT_A16B16G16R16
	// ----

	// [1.12] USUNIETE. W 3.3.5 klient trzymal tu wskazniki na wlasne obiekty
	// shaderow czcionek (00C7D2CC / 00C7D2D0) i Lexara podmieniala w nich
	// bajtkod. Klient 1.12 nie tworzy zadnych shaderow czcionek - wrapper
	// 005C17F0 idzie wprost do InitFontIndexBuffer - wiec te globale nie
	// istnieja. Port ustawia wlasne shadery na urzadzeniu (patrz MSDF.cpp:
	// BindMsdfShaders / UnbindMsdfShaders).

	inline FT_Library g_realFtLibrary = nullptr;
	inline msdfgen::FreetypeHandle* g_msdfFreetype = nullptr;

    inline constexpr uint32_t MAX_ATLAS_PAGES = 4;
    inline constexpr size_t CJK_CACHE_THRESHOLD = 16661;

	// [1.12] Wlacznik renderera na zywo, przelaczany skrotem CTRL+ALT+F
	// (obsluga w D3D.cpp, w haku EndScene). Sluzy do porownania "przed/po"
	// bez restartu gry. Sprawdzany we wszystkich pieciu miejscach, ktore
	// odrozniaja sciezke MSDF od potoku stalego klienta.
	inline bool ENABLED = true;

	inline bool IS_CJK = false;
	inline bool INITIALIZED = false;
	inline bool ALLOW_UNSAFE_FONTS = false;

	inline const bool IS_WIN10 = []() {
		HMODULE hKernel = GetModuleHandleW(L"kernelbase.dll");
		if (!hKernel) return false;

		return (GetProcAddress(hKernel, "VirtualAlloc2") != nullptr &&
			GetProcAddress(hKernel, "MapViewOfFile3") != nullptr &&
			GetProcAddress(hKernel, "UnmapViewOfFile2") != nullptr);
		}();

	inline std::string GetGameLocale() {
		static const std::string locale = []() {
			char exePath[MAX_PATH] = {0};
			if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
				return std::string{"enUS"};
			}

			std::filesystem::path gameRoot(exePath);
			gameRoot = gameRoot.parent_path();

			constexpr const char* locales[] = {
				"enUS", "enGB", "deDE", "frFR", "esES", "esMX", "ptBR", "ruRU", "zhCN", "zhTW", "koKR"
			};

			std::error_code ec;
			for (const char* candidate : locales) {
				const std::filesystem::path localeDir = gameRoot / "Data" / candidate;
				if (!std::filesystem::exists(localeDir, ec)) {
					ec.clear();
					continue;
				}

				if (std::filesystem::exists(localeDir / (std::string("locale-") + candidate + ".MPQ"), ec) ||
					std::filesystem::exists(localeDir / (std::string("patch-") + candidate + ".MPQ"), ec) ||
					std::filesystem::exists(localeDir / (std::string("base-") + candidate + ".MPQ"), ec)) {
					return std::string{candidate};
				}
				ec.clear();
			}

			return std::string{"enUS"};
		}();

		return locale;
	}

    void initialize();

    // Odczyt jednej flagi z lexara112.cfg, wystawiony dla latek spoza tego
    // pliku (TexNullFill). Wolac dopiero po MSDF::initialize(), bo to ono
    // wczytuje plik; wczesniej kazda flaga odpowie "wlaczona".
    bool CfgFlag(const char* key);

    // Jak wyzej, ale brak pliku albo brak klucza = WYLACZONE.
    bool CfgFlagOptIn(const char* key);
};
