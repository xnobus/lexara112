cmake_minimum_required(VERSION 3.15)
project(Lexara112 LANGUAGES CXX C)

# ---------------------------------------------------------------
# Lexara (HD MSDF font renderer) -> Turtle WoW 1.12 / twmoa_1171.
# Cel: lexara112.dll, wstrzykiwany przez VanillaFixes (wpis w dlls.txt).
# Oryginal: https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5
# Mapa portu: ..\_lexara-port-mapa.md
# ---------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- FreeType: bez zaleznosci zewnetrznych (jak w oryginale) ---
set(FT_DISABLE_ZLIB      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG       ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ  ON CACHE BOOL "" FORCE)

# --- msdfgen: tylko biblioteki, bez narzedzia, bez Skii, bez vcpkg ---
# SVG i PNG wylaczone celowo: ciagna tinyxml2 i libpng, a port czyta glify
# przez FreeType i wypisuje je wprost do atlasu - zadnego z tych formatow
# nie dotyka. Bez tego msdfgen-ext zada find_package(tinyxml2).
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME  OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG      ON  CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG      ON  CACHE BOOL "" FORCE)

add_subdirectory(third_party/Detours          ${CMAKE_BINARY_DIR}/tp/Detours)
add_subdirectory(third_party/freetype-2.14.1  ${CMAKE_BINARY_DIR}/tp/freetype)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
add_subdirectory(third_party/msdfgen          ${CMAKE_BINARY_DIR}/tp/msdfgen)

# ---------------------------------------------------------------
# Zrodla. font_exact/MSDF.cpp jest WLACZANY przez src/MSDF.cpp,
# wiec nie moze trafic na liste osobno.
# ---------------------------------------------------------------
file(GLOB FONT_EXACT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/font_exact/*.cpp")
list(REMOVE_ITEM FONT_EXACT_SOURCES "${CMAKE_SOURCE_DIR}/src/font_exact/MSDF.cpp")

set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/dllmain112.cpp"   # zamiast dllmain.cpp + Proxy.cpp (brak proxy dinput8)
    "${CMAKE_SOURCE_DIR}/src/MSDF.cpp"
    ${FONT_EXACT_SOURCES}
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

add_library(lexara112 SHARED ${SOURCES})

target_include_directories(lexara112 PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party"
    "${CMAKE_SOURCE_DIR}/third_party/unordered_dense_src/include"
)

target_link_libraries(lexara112
    psapi
    d3d9
    Detours
    Freetype::Freetype
    msdfgen-core
    msdfgen-ext
)

set_target_properties(lexara112 PROPERTIES
    OUTPUT_NAME "lexara112"
    PREFIX      ""
    SUFFIX      ".dll"
)

if(MSVC)
    target_compile_options(lexara112 PRIVATE /W3)
endif()
