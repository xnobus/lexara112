# Detours (Microsoft) - minimalny projekt statyczny x86.
# Repozytorium Microsoftu ma tylko NMAKE-owy Makefile; to jedyny dodany plik
# obok forwardera detours.h. Zrodel nie ruszamy.
cmake_minimum_required(VERSION 3.15)
project(Detours LANGUAGES CXX)

add_library(Detours STATIC
    src/detours.cpp
    src/modules.cpp
    src/disasm.cpp
    src/image.cpp
    src/creatwth.cpp
    src/disolx86.cpp
)

target_include_directories(Detours PUBLIC src)
target_compile_definitions(Detours PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # Detours nie kompiluje sie czysto pod /W4; wylaczamy tylko ostrzezenia,
    # nie zmieniamy zachowania.
    target_compile_options(Detours PRIVATE /W0 /wd4267 /wd4244)
endif()
