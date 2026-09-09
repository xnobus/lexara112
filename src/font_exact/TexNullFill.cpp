#include "TexNullFill.h"

#include "Hooks.h"
#include "MSDF.h"
#include "../Logger.h"

#include <Windows.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Crash 0x00448955: awaryjne "wypelnij bialym" pisze przez wskaznik NULL
// ---------------------------------------------------------------------------
//
// 0x00448920 to petla po lancuchu mipmap globalnej tekstury zastepczej, ktora
// silnik wypelnia wartoscia 0xFFFFFFFF, gdy nie udalo mu sie wczytac pliku.
// Adres tablicy wskaznikow na bity kolejnych poziomow bierze z globalnej
// [0x00B05D14]. Tablica istnieje, ale jej wpisy sa NULL - bufor zastepczy nie
// jest w tym kliencie nigdy alokowany - wiec `rep stosd` leci pod adres zero:
//
//   00448936  cmp edx, 1              ; petla po poziomach mipmapy
//   00448939  ja  448940
//   0044893B  cmp esi, 1
//   0044893E  jbe 448985              ; wyjscie
//   00448940  mov edi, [ebp-4]        ; kolejny wpis tablicy
//   00448943  mov edi, [edi]          ; <-- NULL
//   00448945  mov ecx, esi
//   00448947  imul ecx, edx
//   0044894A  shl  ecx, 2
//   0044894D  mov ebx, ecx            ; ebx = rozmiar w bajtach
//   0044894F  shr  ecx, 2             ; ecx = rozmiar w dwordach
//   00448952  or  eax, 0FFFFFFFFh     ; <-- POCZATEK MIEJSCA LATANIA (5 bajtow)
//   00448955  rep stosd               ; <-- CRASH
//   00448957  mov ecx, ebx            ; <-- powrot, gdy edi != 0
//   00448959  and ecx, 3
//   0044895C  rep stosb               ; drugi zapis, tez pod NULL
//   0044895E  mov edi, [ebp-4]        ; <-- powrot, gdy edi == 0
//   00448961  ...                     ; przejscie do nastepnego poziomu
//
// Latka sprawdza `edi` i przy wskazniku nie do zapisu przeskakuje OBA zapisy
// naraz - drugi (`rep stosb` pod 0044895C) wywalilby sie tak samo, wiec powrot
// idzie wtedy od razu na 0044895E. Nie da sie tego zrobic jednym adresem
// powrotu, stad dwa jmpbacki zamiast jednego.
//
// "Nie do zapisu" to zakres, nie samo zero: 2026-09-09 wpis okazal sie raz
// NULL-em, a raz 0xFFFFFFFF - szczegoly przy samym stubie nizej.
//
// Pominiecie poziomu jest bezpieczne: petla i tak przelicza od nowa ecx
// (0x448945) i ebx, a edi bierze ponownie z [ebp-4] (0x44895E). Zaden stan nie
// jest przenoszony miedzy obrotami przez rejestry, ktore latka zostawia.
//
// Detours zajmuje dokladnie 5 bajtow: `83 C8 FF` + `F3 AB`. Obie instrukcje sa
// kompletne i miejsce jest na granicy instrukcji z obu stron.
//
// Efekt: sciezka, ktora dzisiaj zabija proces, konczy sie brakiem tekstury
// (bialy/pusty kwadrat) zamiast ACCESS_VIOLATION. NIE naprawia to przyczyny -
// sciezki do nieistniejacego pliku - tylko obsluge bledu po stronie silnika.

namespace {
    auto (*CGxTexture__FillMissing_site)() = reinterpret_cast<void (*)()>(0x00448952);
    constexpr uintptr_t CGxTexture__FillMissing_jmpback_fill = 0x00448957;
    constexpr uintptr_t CGxTexture__FillMissing_jmpback_skip = 0x0044895E;

    // Zakres, nie samo zero. Pierwsza wersja sprawdzala `test edi,edi` i to
    // wystarczylo dokladnie do 2026-09-09 21:39, kiedy klient wywalil sie
    // wewnatrz TEGO stuba (Errors\2026-09-09 21.39.32 Crash.txt): rejestry
    // identyczne jak zawsze na tej sciezce, ale EDI=FFFFFFFF zamiast 0, wiec
    // straznik przepuscil i `rep stosd` poszlo pod 0xFFFFFFFF. Wpis w tablicy
    // zastepczej bywa wiec nie tylko NULL-em, ale i sentinelem -1.
    //
    // Dolna granica 0x10000 to pierwsza strona procesu, ktora na Windows nigdy
    // nie jest zmapowana; gorna 0x7FFFFFFF odcina -1 i polowe jadra. Obie to
    // `cmp`, czyli ruszaja wylacznie EFLAGS - kod pod 00448957 czyta ecx i ebx,
    // nie flagi po `or`, wiec zywe rejestry zostaja nietkniete.
    __declspec(naked) void CGxTexture__FillMissing_siteHk() {
        __asm {
            or   eax, 0FFFFFFFFh;
            cmp  edi, 10000h;
            jb   no_buffer;
            cmp  edi, 7FFFFFFFh;
            ja   no_buffer;
            rep  stosd;
            jmp  CGxTexture__FillMissing_jmpback_fill;
        no_buffer:
            jmp  CGxTexture__FillMissing_jmpback_skip;
        }
    }
}

void TexNullFill::initialize() {
    if (!MSDF::CfgFlag("site_texnullfill")) {
        Log("[MSDF] site_texnullfill=0 - latka na crash 0x00448955 pominieta");
        return;
    }

    // Kontrola o znanym wyniku: jesli pod miejscem latania nie leza dokladnie
    // te bajty, to nie jest ten klient i nie wolno w to pisac. Bez tego latka
    // po podmianie WoW_Modernized.exe wpisalaby skok w srodek czegos innego.
    const unsigned char expected[5] = { 0x83, 0xC8, 0xFF, 0xF3, 0xAB };
    const unsigned char* at = reinterpret_cast<const unsigned char*>(0x00448952);
    if (memcmp(at, expected, sizeof(expected)) != 0) {
        Log("[MSDF] site_texnullfill: bajty pod 00448952 to %02X %02X %02X %02X %02X,"
            " oczekiwano 83 C8 FF F3 AB - latka POMINIETA",
            at[0], at[1], at[2], at[3], at[4]);
        return;
    }

    // `commit = 0` z Detours znaczy tylko "brak bledu", nie "hak dziala" -
    // dowodem na dzialanie jest dopiero brak crasha na sondzie z
    // docs/tex-null-fill.md.
    Hooks::Detour(&CGxTexture__FillMissing_site, CGxTexture__FillMissing_siteHk);
    Log("[MSDF] site_texnullfill: latka na crash 0x00448955 zalozona");
}
