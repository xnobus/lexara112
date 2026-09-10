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
	// [1.12] The constant registers used by our shaders. Do NOT move them down
	// without measuring.
	//
	// In 3.3.5 Lexara swapped the bytecode inside the client's own shader objects,
	// so it shared the client's register layout and c23 was safe. The 1.12 port
	// sets its own shaders directly on the device, behind CGxDevice's back - and
	// CGxDevice caches constants and does not refresh what it does not know about.
	// Measured in a running client, the range the client actually writes to in the
	// world is vs c2..c186. The previous layout (WorldViewProj in the default
	// c0..c3 plus control in c23) therefore collided with the client on c2, c3 and
	// c23 - about 190 overwrites per frame.
	//
	// The layout below sits above the client's entire range. Hardware limits:
	// vs_3_0 has c0..c255, ps_3_0 has c0..c223 - which is why control (read by BOTH
	// shaders) sits lower than WorldViewProj (read by the vs only).
	// The values must match the register(cNN) declarations in MSDFShaders.h.
	inline constexpr uint32_t SDF_CONTROL_REG = 220;   // vs + ps
	inline constexpr uint32_t SDF_WVP_REG     = 240;   // vs only, 4 registers
	inline constexpr uint32_t ATLAS_GUTTER = 14; // usually spread + 2-4
	inline constexpr uint32_t SDF_RENDER_SIZE = 96; // 48-128
	inline constexpr uint32_t SDF_SPREAD = 12; // 6-12
	inline constexpr D3DFORMAT D3DFMT = D3DFMT_A8R8G8B8; // D3DFMT_A8R8G8B8-D3DFMT_A16B16G16R16
	// ----

	// [1.12] REMOVED. In 3.3.5 the client kept pointers to its own font shader
	// objects here (00C7D2CC / 00C7D2D0) and Lexara swapped their bytecode. The
	// 1.12 client creates no font shaders at all - the wrapper at 005C17F0 goes
	// straight to InitFontIndexBuffer - so those globals do not exist. The port
	// sets its own shaders on the device instead (see MSDF.cpp: BindMsdfShaders /
	// UnbindMsdfShaders).

	inline FT_Library g_realFtLibrary = nullptr;
	inline msdfgen::FreetypeHandle* g_msdfFreetype = nullptr;

    inline constexpr uint32_t MAX_ATLAS_PAGES = 4;
    inline constexpr size_t CJK_CACHE_THRESHOLD = 16661;

	// [1.12] The renderer switch, flipped with the CTRL+ALT+F shortcut (handled in
	// D3D.cpp, inside the EndScene hook). It exists for before/after comparisons.
	// Checked in all five places that tell the MSDF path apart from the client's
	// fixed pipeline.
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

    // Reads a single flag from lexara112.cfg, exposed for patches outside this
    // file (TexNullFill). Call it only after MSDF::initialize(), because that is
    // what loads the file; before that every flag answers "enabled".
    bool CfgFlag(const char* key);

    // As above, but a missing file or a missing key means DISABLED.
    bool CfgFlagOptIn(const char* key);
};
