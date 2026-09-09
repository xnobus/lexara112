# Lexara 1.12 - HD MSDF font renderer na Turtle WoW (twmoa_1171)

Port renderera czcionek przez atlas MSDF z klienta 3.3.5a na klienta 1.12.

Oryginal: [Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5](https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5)
Licencja: GPL-3.0 (jak oryginal) - patrz [LICENSE](LICENSE).

**Stan: dziala w grze od 2026-09-09.** Tekst rysuje sie przez atlas MSDF.

## Czym ten port rozni sie od oryginalu

Trzy rzeczy, ktore trzeba wiedziec, zanim sie tu cokolwiek ruszy:

1. **Klient 1.12 nie ma shaderow czcionek w ogole.** 3.3.5 (`006BE230`) tworzy
   obiekt shadera wierzcholkow i pikseli, zanim wywola `InitFontIndexBuffer`; 1.12
   (`005C17F0`) zaczyna od tego wywolania. `SetVertexShader` nie jest w tej binarce
   wolane ani razu, a `RenderBatch` ustawia wylacznie stany potoku stalego. Cala
   warstwa `D3D.cpp` Lexary (haki `IShaderCreate*`, podmiana bajtkodu) nie ma tu
   odpowiednika - port kompiluje wlasne `vs_3_0`/`ps_3_0` i wiaze je sam.
2. **Urzadzenie D3D przechwytujemy przez wspoldzielona tablice wirtualna.** Tworzymy
   wlasne, jednorazowe urzadzenie na ukrytym oknie 8x8, podmieniamy w jego vtable
   `EndScene` (slot 42) i zwalniamy swoje. Obiekty tej samej klasy C++ dziela vtable,
   wiec od tej chwili urzadzenie klienta samo sie przedstawia w `this`.
3. **Ladowanie idzie przez VanillaFixes, nie przez proxy `dinput8.dll`.** Stad
   `src/dllmain112.cpp` zamiast `dllmain.cpp` + `Proxy.cpp`.

Kazda zmiana merytoryczna (nie sam adres) ma w kodzie komentarz `[1.12]`.

## Instalacja

1. Zbudowac (`build.bat`) albo wziac gotowy `lexara112.dll`.
2. Skopiowac `lexara112.dll` do katalogu klienta.
3. Dopisac linie `lexara112.dll` do `dlls.txt` obok `WoW.exe`.
4. Opcjonalnie: `lexara112.cfg.example` -> `lexara112.cfg` w katalogu klienta.
5. Opcjonalnie: `addon/LexaraCompare` -> `Interface/AddOns/LexaraCompare`.

**Wycofanie:** usunac linie z `dlls.txt`. DLL nie zapisuje niczego w plikach
klienta - lata pamiec procesu, wiec restart bez wpisu wraca do stanu sprzed.

## Obsluga

- **`lexara112.cfg`** - 24 przelaczniki `nazwa=0/1`, brak pliku = wszystko wlaczone.
  10 miejsc latania (`site_*`), 6 hakow czcionek (`hook_*`), `ft_hooks`, `shaders`,
  `msdf_enabled`. **Zmiana wymaga tylko restartu gry, nie przebudowy DLL-a** -
  to narzedzie pierwszego wyboru przy kazdej regresji.
- **`CTRL+ALT+F11`** - zapisuje `msdf_enabled` do cfg; dziala od nastepnego startu.
- **`/lexara`** (albo `/lex`) - panel z tym samym tekstem w 9 rozmiarach (8-72 px),
  pelny zestaw znakow specjalnych; prawy przycisk zmienia kroj.
- **`lexara112.log`** - log DLL-a, domyslnie oszczedny.
- Cache glifow: katalog `LEXARA` w `%TEMP%`, mozna kasowac.

## Budowanie

```
build.bat
```

Wymaga **VS 2022 BuildTools** (toolset x86; Community bez toolsetu nie wystarczy)
i CMake. `build.bat` bierze `cmake` z PATH, a gdy go tam nie ma - ze zmiennej
`LEXARA_CMAKE`. Wynik: `build/out/Release/lexara112.dll`.

## Zaleznosci

`third_party/` jest wendorowane w repo (oryginalna Lexara ich nie dolacza):

| katalog | zrodlo | uwagi |
|---|---|---|
| `freetype-2.14.1` | github.com/freetype/freetype, tag `VER-2-14-1` | wersja, ktorej uzywa Lexara |
| `msdfgen` | github.com/Chlumsky/msdfgen | SVG i PNG **wylaczone** (ciagna tinyxml2/libpng) |
| `Detours` | github.com/microsoft/Detours | dodane: `CMakeLists.txt` i forwarder `detours.h` |
| `unordered_dense_src` | github.com/martinus/unordered_dense | tylko naglowek |

**msdfgen bez Skii** nie ma `resolveShapeGeometry`; zastapione w
`src/font_exact/MSDFCompat.h` tym samym, czym zastepuje to sam msdfgen
(`shape.orientContours()`, `main.cpp:1155`).

## Zmienione pliki Lexary

Kopie oryginalow 3.3.5 leza obok, jako `.bak-335`:

- `src/font_exact/GameClient.h` - adresy i konwencje 1.12, uklad `CGxString`,
  FreeType na `__fastcall`.
- `src/font_exact/MSDF.cpp` - dziesiec miejsc latania, cztery stuby `naked`,
  `BindMsdfShaders`/`UnbindMsdfShaders`, haki FreeType na `__fastcall`.
- `src/font_exact/D3D.cpp` - warstwa urzadzenia przepisana: zamiast siedmiu hakow
  `CGxDevice` lancuch `LoadLibrary -> Direct3DCreate9 -> CreateDevice`.
- `src/font_exact/MSDF.h` - usuniete globale shaderow czcionek.
- `src/font_exact/MSDFValidator.h`, `MSDFFont.cpp` - `MSDFCompat::ResolveShapeGeometry`.

## Dokumentacja

- [`docs/lexara.md`](docs/lexara.md) - komplet wiedzy: stan, obsluga, pulapki,
  siedem usterek znalezionych dopiero w grze.
- [`docs/lexara-port-mapa.md`](docs/lexara-port-mapa.md) - **adresy 1.12, uklad
  struktur, konwencje wywolan, przebieg etapow 1-5.** Zaczac tutaj przy kazdej
  pracy nad haczeniem klienta.
- [`docs/README-portu-etap-pierwszy-test.md`](docs/README-portu-etap-pierwszy-test.md) -
  README z chwili przed pierwszym uruchomieniem; historyczny, ale trzyma liste
  rzeczy, ktorych nie dalo sie rozstrzygnac statycznie.

## Zglaszanie bledow

W zgloszeniu podac:

1. **`lexara112.log`** - w calosci, jest krotki.
2. **`lexara112.cfg`** - albo informacje, ze pliku nie ma.
3. **Wersje klienta** i liste z `dlls.txt` (kolejnosc ma znaczenie).
4. **Wynik bisekcji przez cfg**: wylaczyc `msdf_enabled`, potem `shaders`, potem
   `ft_hooks`, potem `site_*` grupami. Ktory przelacznik usuwa objaw - to jest
   polowa diagnozy i nie wymaga przebudowy DLL-a.
5. Przy crashu: zawartosc `Errors/` klienta i log DXVK (`*_d3d9.log`).

## Pulapki (skrot)

- **`commit = 0` z Detours znaczy "brak bledu", NIE "hak dziala".** Jedyna sonda
  na dzialanie haka to log z jego wnetrza.
- **Przy stubie asemblerowym sprawdzac ZYWE REJESTRY**, nie tylko dlugosc miejsca
  i adres powrotu - 3.3.5 i 1.12 potrafia prowadzic ten sam lancuch przez inny
  rejestr, a kontrola dlugosci tego nie wylapie.
- Miejsce `ProcessBatch` (`005C91A8`) ma dokladnie 5 bajtow i jest CELEM skoku
  `je 005C91A9` z `005C8FF3` - wejscie w ten skok po zalataniu to skok w srodek
  instrukcji.
