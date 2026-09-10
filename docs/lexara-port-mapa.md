# Lexara (3.3.5) -> Turtle WoW 1.12 : mapa miejsc (etap 1)

Zrodlo: `lexara/src/font_exact/MSDF.cpp`, `GameClient.h`.
Binarki porownywane przy sporzadzaniu mapy: `Wow.exe` klienta 3.3.5a (build 12340)
i `WoW.exe` klienta Turtle WoW `twmoa_1171`.

Granice funkcji brane z indeksu celow CALL (1.12 nie wyrownuje funkcji bajtami int3,
wiec szukanie po wypelnieniu daje tam smieci).

## Funkcje

| rola | 3.3.5 | 1.12 | uwagi |
|---|---|---|---|
| InitFontIndexBuffer | 006C47B0 | 005C92F0 | ten sam kod, te same stale 0x1800/0xC00 |
| AllocateFontIndexBuffer | 006C47F0 | 005C9330 | licznik petli w EDI, nie EBX |
| bufalloc (grow stream) | 006C48D0 | 005C8F40 | argumenty w ECX/EDX zamiast na stosie |
| NativeFontRender | 006C4AD0 | 005C8FE0 | 171 vs 154 instr, ta sama struktura |
| GetGlyphYMetrics | 006C8C60 | 005D1360 | ten sam kod, starsza kompilacja |
| CheckGeometry | 006C7480 | 005CD6A0 | z wywolania w petli |
| WriteGeometry | 006C5E90 | 005CE0C0 | z wywolania w petli |
| (006C63E0) | 006C63E0 | 005CE090 | |
| (006C9D50) init batch | 006C9D50 | 005CFC50 | |
| BufStream | 00684850 | 0058A140 | wolna funkcja, nie metoda CGxDevice |
| BufLock (vtable +0xD8) | metoda | 0058A080 | |
| BufUnlock (vtable +0xDC) | metoda | 0058A0A0 | |
| PoolCreate (VB) | 006876D0 | 0058A160 | |
| PoolCreate (IB) | 00687660 | 00589F80 | |
| SetTexture | 00685F50 | 00589E80 | **stala slotu 0x15 -> 0x17** |
| HandleToPtr | 004B6CB0 | **brak** | 1.12 trzyma wskaznik tekstury wprost |
| InitializeTextLine | 006C6CD0 | **nie znaleziona** | hak tylko opakowuje (prefetch), opcjonalny |
| GetFontFace | 006C8080 | trywialna | `return p ? *(void**)(p+0x24) : 0`, offset do potwierdzenia |
| globalne IB/VB | 00C7D2DC / 00C7D2E0 | 00C2B9D4 / 00C2B9D8 | |
| CGxDevice* | 00C5DF88 | **brak** | 1.12 nie laduje urzadzenia, funkcje wolne |

## Dziesiec miejsc do zalatania

| # | nazwa w Lexarze | 3.3.5 site -> jmpback | 1.12 site -> jmpback | bajtow 335/112 | werdykt |
|---|---|---|---|---|---|
| 1 | InitFontIndexBuffer_site | 006C47BD -> 006C47D8 | 005C92F7 -> 005C930F | 27 / 24 | pasuje |
| 2 | AllocateFontIndexBuffer_site | 006C480C -> 006C4811 | 005C933F -> 005C9344 | 5 / 5 | pasuje, `mov edi,3FFFh` |
| 3 | CheckGeometry_site | 006C4AF3 -> 006C4B00 | 005C9003 -> 005C9010 | 13 / 13 | bajt w bajt |
| 4 | CheckGeometry_call | 006C4B09 -> 006C4B10 | 005C9019 -> 005C9020 | 7 / 7 | bajt w bajt |
| 5 | BufStream_site | 006C4B40 -> 006C4B45 | 005C904A -> 005C904F | 5 / 5 | pasuje |
| 6 | bufalloc_1_site | 006C4B64 -> 006C4B70 | 005C9067 -> 005C9073 | 12 / 12 | pasuje, 0xB4 -> 0xA0 |
| 7 | bufalloc_3_site | 006C4C36 -> 006C4C4B | 005C9129 -> 005C913D | 21 / 20 | pasuje |
| 8 | bufalloc_2_site | 006C4C67 -> 006C4C8A | 005C915D -> 005C917B | 35 / 30 | pasuje |
| 9 | IGxuFontProcessBatch_site | 006C4CC4 -> 006C4CC9 | 005C91A8 -> 005C91AD | 5 / 5 | pasuje, patrz ryzyko R1 |
| 10 | GetGlyphYMetrics_site | 006C8C71 -> 006C8C77 | 005D137A -> 005D1380 | 6 / 6 | pasuje, EDX -> ECX |

## Roznice do uwzglednienia

- **ABI.** W 3.3.5 to metody `__thiscall` CGxDevice (ECX z globalu 00C5DF88, reszta na stos).
  W 1.12 wolne funkcje z konwencja rejestrowa (ECX/EDX + stos). Wszystkie stuby `naked`
  trzeba napisac od nowa - zmiana mechaniczna, ale kazdy stub inaczej.
- **Tablica partii tekstur.** Baza 0x18C ta sama, ale zakres petli 0xB4..0xD4 (3.3.5)
  odpowiada 0xA0..0xC0 (1.12). W obu osiem slotow po 4 bajty.
- **Slot tekstury** przy SetTexture: 0x15 -> 0x17.
- **Brak HandleToPtr** (004B6CB0). W 3.3.5 uchwyt tekstury jest rozwiazywany wywolaniem,
  w 1.12 czytany wprost z tablicy. Zaden hak Lexary tego nie dotyka, ale kod czytajacy
  teksture w porcie musi to uwzglednic.
- **Shadery.** Lexara kompiluje wlasne `ps_3_0` / `vs_3_0` przez D3DX i ustawia je na
  urzadzeniu. Klient 1.12 uzywa u siebie `ps_2_0`, ale to nie jest limit sprzetu -
  pod DXVK SM 3.0 jest dostepny. Do potwierdzenia pomiarem w etapie 2.
- **Trzy haki calofunkcyjne** (CheckGeometry, WriteGeometry, InitializeTextLine) **wolaja
  oryginal** i tylko dokladaja prace wokol. Nie trzeba reimplementowac 627 instrukcji
  InitializeTextLine - to najwazniejsza dobra wiadomosc dla wykonalnosci portu.

## Ryzyka

- **R1.** Miejsce 9 ma dokladnie 5 bajtow, a `005C91A9` (drugi bajt latki) jest celem
  skoku `je` z `005C8FF3` - wczesnego wyjscia, gdy bufor indeksow nie jest zainicjowany.
  Wejscie w ten skok po zalataniu = skok w srodek instrukcji. To samo jest w 3.3.5
  (`je 006C4CC5`, site 006C4CC4), wiec Lexara juz z tym zyje: jej hak Init gwarantuje,
  ze globalny wskaznik nigdy nie jest zerem. Przenosic bez zmian, ale wiedziec, ze to mina.
- **R2.** Uklad struktury CGxString (m_flags +0x24, m_geomBuffers +0x18, tekst, licznik
  wierzcholkow na strone) jest w Lexarze wypisany z 3.3.5. Trzeba go wyprowadzic od nowa
  dla 1.12; czesc offsetow juz sie zgadza (+0x18, +0x1C, +0x24, +0x18C), ale nie wszystkie.
- **R3.** InitializeTextLine w 1.12 nieznaleziona. Hak jest opcjonalny (wstepne
  wczytanie znakow do atlasu), wiec brak blokuje wydajnosc, nie poprawnosc.

## Wniosek etapu 1

Port jest wykonalny. Wszystkie dziesiec miejsc istnieje w 1.12, kazde ma dosc bajtow
na detour, a funkcja renderujaca jest praktycznie tym samym kodem. Praca to przepisanie
stubow na konwencje rejestrowa i wyprowadzenie ukladu CGxString - nie przepisywanie logiki.

---

# Etap 2

## FreeType

Lexara **nie miesza ABI**: podmienia caly FreeType klienta na wlasny (2.14.1), podpinajac
sie pod punkty wejscia biblioteki w binarce. Twarze, na ktorych potem wola `FT_Load_Glyph`
itd., sa wiec tworzone przez JEJ FreeType. To usuwa najwieksze ryzyko portu.
Klient 1.12 tez ma wbudowany FreeType (moduly `sfnt`, `truetype`).

Lancuch identyfikacyjny: napis `truetype` -> rekord klasy modulu -> tablica domyslnych
modulow -> `FT_Add_Default_Modules` -> wrapper klienta. Metoda sprawdzona najpierw
na 3.3.5, gdzie dala adresy zgodne z tymi wypisanymi w Lexarze.

| funkcja | 3.3.5 | 1.12 | jak potwierdzone |
|---|---|---|---|
| FT_New_Library (`InitFn`) | 00991320 | 007CF0E0 | wrapper klienta 006BE230 -> 005C17F0, instrukcja w instrukcje |
| FT_Add_Default_Modules | 00990650 | 007CCFC0 | tablica 11 modulow: 00AA3944 -> 0081E068 |
| FT_Done_FreeType | 00992CB0 | 007CF160 | jedyny wolajacy 006BF265 -> 005C18B3 |
| FT_New_Memory_Face | 00993370 | 007CDDE0 | ten sam kod, blad 6, ten sam wolajacy |
| FT_Done_Face | 00992610 | 007CE2F0 | destruktor 006C81C0 -> 005D0290, ten sam uklad |
| FT_Set_Pixel_Sizes | 00992780 | 007CE760 | ten sam kod, `[face+0x58]` |
| FT_Get_Char_Index | 009911A0 | 007CE960 | funkcja glifu 006C8C10 -> 005D1300 |
| FT_Load_Glyph | 00992DA0 | 007CDB40 | tam samo, ta sama stala `0x208A` i maska `0x38` |
| FT_Get_Kerning | 00991050 | 007CE830 | ten sam kod, blad 0x23 |
| FT_Select_Charmap | 009910F0 | 007CE8D0 | 1.12 przekazuje `edx='unic'` |
| (render glifu) | 00992B60 | 007CEC20 | ten sam kod, blad 6 |
| **FT_New_Face** | 009931A0 | **nieznaleziona** | brak wolajacego w kliencie w OBU buildach; Lexara podpina ja zapobiegawczo |

Globalne: `FT_Library` 00C7D2B4 -> **00C2B9A8**, `FT_Memory` 00AD9960 -> **0085F4C8**.

Konwencje 1.12: `FT_New_Memory_Face(ecx=library, edx=file_base, [+8]=size, [+0xC]=index,
[+0x10]=aface)`, `ret 0xC`. `FT_Done_Face(ecx=face)`. `FT_Select_Charmap(ecx=face, edx=kod)`.

Uboczne potwierdzenie mapy z etapu 1: wrapper 1.12 `005C17F0` zaczyna sie od
`call 005C92F0` - dokladnie tam, gdzie 3.3.5 `006BE230` robi `call 006C47B0`
(InitFontIndexBuffer). Dwa niezalezne lancuchy daja te sama pare.

## Uklad CGxString w 1.12

Wyprowadzony przez porownanie par funkcji instrukcja w instrukcje, nie przez zalozenie.

| pole | 3.3.5 | 1.12 | gdzie potwierdzone |
|---|---|---|---|
| m_fontSizeMult | 0x1C | **0x1C** | 006C7B4C / 005CDC55, `fld [ebx+0x1c]` |
| m_textColor | 0x2C | **0x2C** | WriteGeometry, `add eax,0x2c` |
| m_shadowColor | 0x30 | **0x30** | WriteGeometry |
| m_shadowOffset | 0x34 | **0x34** | WriteGeometry |
| m_fontObj | 0x44 | **0x44** | 006C7480 / 005CD3F0, `mov ecx,[esi+0x44]` |
| m_text | 0x48 | **0x48** | tam samo, `mov eax,[esi+0x48]` |
| m_flags | 0x5C | **0x5C** | GetVertCountForPage, `test byte [ecx+0x5c],1` |
| m_isDirty | 0x64 | **0x64** | 005CD3F0 |
| (gradient len) | 0x6C | **0x6C** | 005CDC58 |
| m_finalPos | 0x70 | **0x70** | WriteGeometry, `lea edx,[eax+0x70]` |
| (licznik) | 0xB0 | **0x9C** | CheckGeometry, `cmp [esi+0xb0]` -> `[esi+0x9c]` |
| **m_geomBuffers[8]** | 0xB4 | **0xA0** | GetVertCountForPage i WriteGeometry, oba |
| m_timeSinceUpdate | 0xD4 | **0xC0** | CheckGeometry, `mov [esi+0xd4],0` -> `[esi+0xc0]` |
| sizeof | 0xD8 | **~0xC4** | z przesuniecia |

**Wszystko do 0x7C wlacznie jest identyczne.** Dziura 0x14 bajtow siedzi miedzy 0x7C a 0xA0
(1.12 ma tam mniej pol: w miejscu, gdzie 3.3.5 czysci tablice `lea ecx,[ebx+0xa0]; call ...`,
1.12 zeruje pojedynczy dword `mov [ebx+0x90],edi`). Zaden hak Lexary tego zakresu nie dotyka.
Od 0xA0 w gore wszystko jest przesuniete o **-0x14**.

## Uklad CGxFont w 1.12

| pole | 3.3.5 | 1.12 | gdzie |
|---|---|---|---|
| m_ftWrapper | 0x70 | **0x70** | 006C22F0 / 005CA030, `mov ecx,[esi+0x70]` |
| m_atlasPages[0] | 0x178 | **0x178** | oba uzywaja `[font+0x184]` (= strona 0 + 0xC) |
| m_rasterTargetSize | 0x24C | **0x24C** | 005CA030 i akcesor 005CAE90 |
| FT_Face w owijce FT | +0x24 | **+0x24** | destruktor 006C81C0 / 005D0290 |

## Funkcje pomocnicze

| rola | 3.3.5 | 1.12 | konwencja 1.12 |
|---|---|---|---|
| CheckGeometry | 006C7480 | **005CD6A0** | ecx = this |
| WriteGeometry | 006C5E90 | **005CE0C0** | ecx = this, `ret 0x10` |
| InitializeTextLine | 006C6CD0 | **005CCBE0** | ecx = this, `ret 0x18` - te same 6 argumentow |
| ClearInstanceData | 006C6B90 | **005CDEF0** | ecx = this |
| GetVertCountForPage | 006C63E0 | **005CE090** | ecx = this, `ret 4` |
| GetFontFace | 006C8080 | **005D0370** | ecx = owijka FT |
| RenderBatch | 006C53A0 | **005C8B70** | ecx = this; straznik 00C7D2C0 -> **00C2B9B0** |
| GetFontEffectiveHeight | 006C0B20 | **005C6FA0** | ecx = is3d, float na stosie |
| GetFontEffectiveWidth | 006C0B60 | **005C7010** | jw. |
| RenderGlyph | 006C8CC0 | **005D1120** | |
| PoolCreate | 006876D0 | **0058A160** | ecx=1, edx=0, 3 argumenty na stosie |
| bufalloc | 006C48D0 | **005C8F40** | ecx = &stream, edx = rozmiar |
| wysokosc ekranu | 00C7D2C4 | **00C2B9A0** | |
| szerokosc ekranu | 00C7D2C8 | **00C2B9A4** | |

`InitializeTextLine` znaleziona przez odcisk zbioru wolanych funkcji, nie przez pozycje
w liscie wywolan - liczby wywolan w obu buildach sie roznia i dopasowanie po kolejnosci
wskazalo zla funkcje (005CD310). Rozstrzygnely: ten sam wolajacy (006C7B10 -> 005CDC20),
ten sam `ret 0x18`, ta sama ramka rzedu 0x90 i ten sam zestaw callee.

**Wniosek etapu 2: nie zostalo nic nieznanego poza `FT_New_Face`, ktorej zaden z dwoch
klientow nie wola.** Wszystkie pola i wszystkie funkcje, ktorych Lexara faktycznie uzywa,
maja potwierdzone odpowiedniki w 1.12.

---

# Etap 3 - warstwa D3D i pierwszy realny pomiar (2026-09-09)

## Odkrycie, ktore zmienia architekture portu: 1.12 NIE MA SHADEROW CZCIONEK

Lexara ma dwie warstwy: `MSDF.cpp` (dziesiec latek w kodzie czcionek, opisane w etapie 1)
oraz `D3D.cpp` - haki `CGxDeviceD3d::IShaderCreateVertex` / `IShaderCreatePixel`, ktore
podmieniaja **bajtkod shaderow czcionek klienta** na wlasny `vs_3_0` / `ps_3_0`.
Ta druga warstwa nie ma w 1.12 zadnego odpowiednika.

Dowod jest w tej samej parze funkcji, ktora domknela etap 2 (wrapper inicjalizacji czcionek):

- 3.3.5 `006BE230`: dwa razy `mov ecx,[00C5DF88]; mov edx,[eax+0x110]; push 0x00C7D2D0 / 0x00C7D2CC; call edx`
  - czyli tworzy **obiekt shadera wierzcholkow** (global `00C7D2D0`) i **pikseli** (`00C7D2CC`) -
  i dopiero potem `call 006C47B0` (InitFontIndexBuffer).
- 1.12 `005C17F0`: **pierwsza instrukcja** to `call 005C92F0` (InitFontIndexBuffer).
  Zadnego tworzenia shaderow, zadnych globali obok bufora indeksow.

Potwierdzenia dodatkowe:

- `RenderBatch` 1.12 (`005C8B70`) ustawia wylacznie stany potoku stalego przez
  `005A9E60` (`GxRenderState`: `mov ecx,<stan>; mov edx,<wartosc>; call`) - nic shaderowego.
- W calym `.text` 1.12 **nie ma ani jednego** wywolania `IDirect3DDevice9::SetVertexShader`.
- Napisy profili `vs_2_0 / vs_1_1 / ps_2_0 / ps_1_4..1_1` leza w tablicy `.data` pod
  `0085C608`, ale **nikt w `.text` tej tablicy nie dotyka** - to martwy kod biblioteki.
  Zapis w CLAUDE.md "klient 1.12 uzywa ps_2_0" trzeba czytac jako: ma takie napisy,
  nie uzywa ich.

**Wniosek: portu nie da sie zrobic przez podmiane shaderow klienta.** Trzeba tworzyc
i wiazac wlasne shadery bezposrednio na `IDirect3DDevice9`, wokol rysowania partii czcionki.

## Architektura zamienna dla 1.12

Cala reszta Lexary (FreeType, msdfgen, atlas, cache, dziesiec latek z etapu 1) zostaje
bez zmian. Zmienia sie tylko sposob doprowadzenia shaderow do urzadzenia:

| warstwa | 3.3.5 (Lexara) | 1.12 (port) |
|---|---|---|
| zrodlo shadera | podmiana bajtkodu w `ShaderData` klienta | wlasne `CreateVertexShader/CreatePixelShader` na urzadzeniu |
| moment wiazania | klient sam robi `SetShader` | hak `DrawIndexedPrimitive` + flaga "jestem w partii czcionki" |
| macierz WVP | staly bufor klienta | `GetTransform(WORLD/VIEW/PROJECTION)` w haku, mnozenie, `SetVertexShaderConstantF(0,...)` |
| atlas MSDF | sloty s12..s15 przez `ShaderData` | `SetTexture(12..15, atlas)` w haku, przywracane po rysowaniu |
| deklaracja wierzcholkow | wlasna | **FVF klienta wystarcza** (zmierzone, patrz nizej) |

Dwa fakty, ktore to umozliwiaja:

- **Jedno miejsce rysowania.** W calym `.text` 1.12 jest **dokladnie jedno** wywolanie
  `DrawIndexedPrimitive` (`005A109A`, `call [eax+0x148]`) i **zero** `DrawPrimitive`.
  Cala geometria, wiec i czcionki, przechodzi tym jednym lejkiem - hak na metodzie
  urzadzenia lapie wszystko, a flaga z latek `MSDF.cpp` mowi, ktore z tych wywolan
  sa czcionka.
- **Latki z etapu 1 juz daja flage.** Miejsce 3 i 4 (`CheckGeometry`) obejmuja wejscie
  w petle partii, a miejsce 9 (`ProcessBatch`) jest epilogiem `NativeFontRender`.
  Ustawienie flagi w 3 i zdjecie jej w 9 daje dokladny zakres "to jest tekst".

Warstwa `D3D.cpp` Lexary zostaje **w czesci** przydatna: haki `Present`, `Reset`,
`CreateTexture`, `DrawIndexedPrimitive` i mechanizm rejestrowania callbackow sa
niezalezne od wersji klienta. Wyciac trzeba `CGxDevice::IShaderCreate*`, `ShaderData`
i `MSDF.h`-owe `g_FontPixelShader` / `g_FontVertexShader` (`00C7D2CC` / `00C7D2D0`) -
te globale w 1.12 nie istnieja.

## Pomiar: czy `ps_3_0` dziala pod DXVK tego klienta - TAK

Osobna sonda (nie wchodzi w sklad tego repozytorium): maly EXE uruchamiany
z katalogu klienta, zeby ladowac **jego** `d3d9.dll`. Kompiluje ORYGINALNE shadery
Lexary bez zmian, tworzy je i **rysuje nimi do celu renderowania, po czym czyta piksele**.

```
adapter: AMD Radeon RX 5600 XT / aticfx32.dll
VertexShaderVersion = 3.0        PixelShaderVersion = 3.0
MaxSimultaneousTextures = 8      MaxTextureBlendStages = 8
MaxVertexShaderConst = 256       MaxTextureWidth/Height = 16384
D3DCompile vs_3_0 : OK (528 B)   D3DCompile ps_3_0 : OK (1584 B)
CreateVertexShader : OK          CreatePixelShader : OK
atlasy 2048x2048 A8R8G8B8: 4/4   AvailableTextureMem 1015 MB przed i po
K1 Clear                : 0xFF203040 OK
K2 sciezka tex          : 0xFF3366CC OK
K3 sciezka MSDF (fwidth): alpha 142, policzone 142 OK
K4 MSDF sd=0 + blend    : RGB 23/45/90, policzone 23/45/90 OK
```

Co to rozstrzyga:

- `ps_3_0` / `vs_3_0` **kompiluja sie, tworza i wykonuja** - `ps_2_0` klienta nie jest
  ograniczeniem sprzetu ani DXVK. Zalozenie z etapu 1 potwierdzone pomiarem.
- **`fwidth` dziala** - to jedyna instrukcja w shaderze Lexary wymagajaca SM 3.0
  i jedyna, ktora mogla polec pod tlumaczeniem DXVK.
- **FVF wystarcza za deklaracje wierzcholkow.** Sonda ustawia `SetFVF(XYZRHW|DIFFUSE|TEX1)`
  i rysuje `vs_3_0` - dziala. To istotne, bo 1.12 nigdzie nie ustawia wlasnej deklaracji,
  tylko FVF; port nie musi tego zmieniac.
- **Cztery atlasy 2048x2048 A8R8G8B8 mieszcza sie** (64 MB) i nie ruszaja licznika
  wolnej pamieci tekstur (1015 MB przed i po - DXVK liczy inaczej niz alokuje).
  Uwaga: `d3d9.textureMemory = 64` w `dxvk.conf` dotyczy innej wielkosci; z historii
  crashy wiadomo, ze ograniczeniem jest **liczba zywych mapowan**, nie ilosc pamieci.

**Bledne bylo moje oczekiwanie w K3/K4, nie shader.** Pierwsza wersja sondy wpisywala
"oczekiwany" piksel na oko (`0xFF3366CC`, `0xFF000000`) i obie kontrole wyszly ROZNICA.
Po policzeniu wzoru z shadera (`screenPxRange = control.z / (fwidth(uv) * control.a) *
(1 - min(0.3, fontSize * 0.0035))`, `opacity = saturate((sd - 0.5) * spr + 0.5)`) wychodzi
dokladnie `142` i `23/45/90`. Sonda liczy to teraz sama - kontrola bez policzonego
oczekiwania nie jest kontrola, tylko zgadywanka.

## Toolchain na tej maszynie

- **Nie ma CMake.** Jest MSVC BuildTools 2022 (`14.44.35207`, `Hostx86/x86/cl.exe`),
  MSBuild i WinSDK `10.0.22621.0`. `vcvarsall.bat` w
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build`.
  VS Community 2022 ma tylko `VC\Auxiliary` i `Redist` - **bez toolsetu**, nie uzywac.
- `d3dcompiler_47.dll` jest w `SysWOW64`, wiec kompilacja HLSL w czasie dzialania jest
  dostepna bez dokladania plikow (Lexara i tak kompiluje shadery w locie).
- `build.bat` sondy woła `cl` wprost, bez CMake. Do freetype/msdfgen CMake bedzie
  potrzebny - albo `pip install cmake`, albo recznie zlozony projekt.
- **Uruchamianie `.bat` z PowerShella:** `cmd /c "cd /d <kat> && plik.bat"` sypie
  "not recognized"; dziala `& cmd.exe /c '<pelna sciezka do .bat>'`.

## Projekt portu - zbudowany (`_lexara-port/`)

`build.bat` -> `build\out\Release\lexara112.dll` (PE32, 964 kB). Wlasny README
w katalogu. **DLL-a NIE WOLNO jeszcze ladowac do gry** - patrz nizej.

Zaleznosci sciagniete osobno (Lexara ich nie dolacza): FreeType `VER-2-14-1`,
msdfgen (master), Detours, unordered_dense. Do Detoursa dopisany `CMakeLists.txt`
i forwarder `detours.h` (repo Microsoftu trzyma naglowek w `src/`).
W msdfgenie **wylaczone SVG i PNG** - inaczej `find_package(tinyxml2)` przerywa
konfiguracje, a port zadnego z tych formatow nie czyta.

**msdfgen bez Skii nie ma `resolveShapeGeometry`.** Lexara buduje z Skia przez
vcpkg; zastapione w `MSDFCompat.h` tym samym, czym zastepuje to sam msdfgen:
`shape.orientContours()`. To nie jest przyblizenie na oko - `resolve-shape-geometry.cpp:127`
konczy sie dokladnie tym wywolaniem, a `main.cpp:1155` uzywa go jako sciezki
zastepczej pod `#else` do `MSDFGEN_USE_SKIA`. Cena: glify o zachodzacych na
siebie konturach moga miec artefakt na przecieciu - do sprawdzenia pomiarem.

### Przeniesione na 1.12 i wbudowane w DLL

Kontrola po buildzie (szukanie stalych w binarce): wszystkie nowe adresy obecne,
**zadnego starego adresu czcionek z 3.3.5 juz w pliku nie ma**.

- dziesiec miejsc latania (etap 1) + cztery stuby `naked` przepisane z
  `_lexara-port-stuby.h`: pula indeksow bez globalu urzadzenia, licznik w EDI,
  granica petli `0A0h`, `bufalloc` w konwencji rejestrowej z `lea ecx,[ebp-1Ch]`;
- `CGxString`: dziura 0x14 bajtow miedzy 0x7C a 0xA0 jako `unk_7C[9]`,
  `m_geomBuffers` 0xA0, `m_timeSinceUpdate` 0xC0, `sizeof` 0xC4;
- konwencje, ktore sie ROZNIA, nie tylko adresy: `RenderGlyph` (`__fastcall`,
  ecx = face, edx = fontSize), `GetFontEffectiveWidth/Height` (`__fastcall`),
  `PoolCreate` (ecx = 1, edx = 0), `bufalloc` (ecx = &stream, edx = rozmiar),
  `GetFontFace` (`__fastcall`).

### Dwie funkcje domkniete w tym etapie

Etap 2 ich nie mial, bo `GameClient.h` Lexary wychodzi poza to, co mapowala mapa.

| rola | 3.3.5 | 1.12 | jak potwierdzone |
|---|---|---|---|
| `CGxFont::GetOrCreateGlyphEntry` | 006C3FC0 | **005CABD0** | stoi wprost przed GetBearingX w 005C6B70; wolana tez z InitializeTextLine |
| `CGxFont::GetBearingX` | 006C24F0 | **005CB080** | instrukcja w instrukcje: `fld [eax+0x50]`, `ret 0xC`, galaz else woła GetFontEffectiveHeight |
| (wolajacy obu) | 006C09A0 | **005C6B70** | jedyny obok InitializeTextLine wolajacy GetBearingX w obu buildach |
| (wolajacy RenderGlyph) | 006C2480 | **005CA160** | jedyny wolajacy w obu buildach |

Znowu zadzialala relacja wolajacy-wolany, a nie pozycja na liscie wywolan:
listy wywolan `InitializeTextLine` maja w obu buildach INNA dlugosc (24 vs 25)
i rozjezdzaja sie po czwartej pozycji.

### Co zostalo - dokladnie dwie rzeczy

1. **`D3D.cpp` - cala warstwa urzadzenia.** Adresy `CGxDevice::DeviceCreate`,
   `NotifyOnDeviceRestored`, `DeviceSetFormat`, `IDestroyD3d`, `IReleaseD3dResources`
   sa wciaz z 3.3.5, a `IShaderCreateVertex/Pixel` nie maja odpowiednika w ogole.
   Kierunek: nie mapowac ich, tylko brac urzadzenie z vtable D3D9 i wiazac wlasne
   shadery w haku `DrawIndexedPrimitive` (jedno miejsce w calym kliencie: 005A109A).
2. **Haki FreeType w `MSDF.cpp`** - zadeklarowane `__cdecl`, a 1.12 wola je
   rejestrowo. Adresy sa gotowe z etapu 2, brakuje samych konwencji:
   `FT_New_Memory_Face(ecx = library, edx = file_base, [+8] size, [+C] index, [+10] aface)`,
   `FT_Done_Face(ecx = face)`, `FT_Select_Charmap(ecx = face, edx = kod)`.

Dopoki oba nie sa zrobione, **`lexara112.dll` nie moze trafic do `dlls.txt`** -
zalatalby klienta adresami z innego builda.

# Etap 4 - warstwa D3D i FreeType przeniesione (2026-09-09)

Po tym etapie w binarce portu **nie ma juz ani jednego adresu 3.3.5**.

## Urzadzenie D3D bez adresow klienta

Lexara brala urzadzenie z globalu `*(0x00C5DF88) + 0x397C` i wieszala sie na
siedmiu funkcjach `CGxDevice`. W 1.12 nie ma ani tego globalu, ani tych funkcji.
Szukanie odpowiednika globalu **nie udalo sie i zostalo przerwane** - `00C0F464`,
ktore wyszlo z przeszukiwania lancucha wywolan wokol `DrawIndexedPrimitive`,
okazalo sie zestawem skalarow stanu viewportu, nie obiektem urzadzenia.

Zamiast szukac dalej: **`WoW.exe` 1.12 nie ma d3d9.dll w tablicy importow**
(jedyny import graficzny to `opengl32.dll`), za to ma w danych napisy `d3d9.dll`
i `Direct3DCreate9` - laduje ja wiec dynamicznie. Stad lancuch bez ani jednego
adresu klienta:

```
LoadLibraryA/W/ExW  ->  (gdy modul to d3d9.dll)  GetProcAddress("Direct3DCreate9")
                    ->  hak Direct3DCreate9  ->  vtable IDirect3D9 slot 16
                    ->  hak CreateDevice     ->  IDirect3DDevice9* zapamietane
```

W chwili wstrzykniecia DLL-a przez VanillaFixes `d3d9.dll` jeszcze nie jest
zaladowana, wiec podpiecie od razu pod eksport nie zadziala - stad haki
`LoadLibrary*`. `D3D::initialize()` **nie wola LoadLibrary samo**: leci z DllMain,
a ladowanie biblioteki pod blokada loadera to proszenie sie o zakleszczenie.

Role hakow klienta przejely metody urzadzenia:

| bylo (3.3.5) | jest (1.12) |
|---|---|
| `CGxDevice::DeviceCreate` | hak `IDirect3D9::CreateDevice` |
| `IReleaseD3dResources` | `IDirect3DDevice9::Reset`, faza przed |
| `NotifyOnDeviceRestored`, `DeviceSetFormat` | `Reset`, faza po (gdy `SUCCEEDED`) |
| `IDestroyD3d` | `D3D::shutdown()` z `DllMain(DETACH)` |
| `IShaderCreateVertex/Pixel` | **nie istnieje** - shadery wiazane samodzielnie |

## Wiazanie shaderow

`BindMsdfShaders` / `UnbindMsdfShaders` w `MSDF.cpp`. Wiazanie w `WriteGeometryHk`
(tam, gdzie i tak juz ustawiane sa atlasy i stale), zdejmowanie w
`CGxuFontRenderBatchHk` oraz na sciezce "czcionka nie jest MSDF-owa" - bez tego
drugiego calv interfejs rysowalby sie shaderem czcionek.

Poniewaz `vs_3_0` zastepuje transformacje potoku stalego, macierz
`World*View*Projection` trzeba podac w `c0..c3`; port czyta trzy macierze
z urzadzenia (`GetTransform`) i mnozy je sam, bez D3DX.

**`MSDF_WVP_TRANSPOSE` to jedyna rzecz w calym porcie, ktorej nie dalo sie
rozstrzygnac statycznie.** Przy `mul(pos, WVP)` i domyslnym pakowaniu kolumnowym
macierz podaje sie transponowana - tak jest ustawione. Objaw zlego wyboru:
tekst niewidoczny albo poza ekranem. Wtedy `false` i przebudowa.

## FreeType - osiem funkcji, wszystkie `__fastcall`

3.3.5 wolal je `__cdecl`. Sprawdzone prologiem i zgodnoscia `ret N` z liczba
argumentow (dwa pierwsze w `ecx`/`edx`, reszta na stosie, sprzata wolany):

| funkcja | 1.12 | `ret` | argumentow |
|---|---|---|---|
| `FT_New_Library` (Init) | 007CF0E0 | 0 | 2 |
| `FT_New_Memory_Face` | 007CDDE0 | 0xC | 5 |
| `FT_Done_Face` | 007CE2F0 | 0 | 1 |
| `FT_Set_Pixel_Sizes` | 007CE760 | 4 | 3 |
| `FT_Get_Char_Index` | 007CE960 | 0 | 2 |
| `FT_Load_Glyph` | 007CDB40 | 4 | 3 |
| `FT_Get_Kerning` | 007CE830 | 0xC | 5 |
| `FT_Done_FreeType` | 007CF160 | 0 | 1 |

Kolejnosc argumentow bez zmian wobec 3.3.5. Dla `FT_New_Library` potwierdza to
niezaleznie wrapper `005C17F0`: `mov edx, 0xC2B9A8` (alibrary),
`mov ecx, 0x85F4C8` (memory). `FT_New_Face` **pominieta** - ryzyko R3, zaden
z dwoch klientow jej nie wola.

## Co zostalo: uruchomic

Lista czterech rzeczy do sprawdzenia w grze i sposob wycofania sa w
`_lexara-port/README.md`. Skrot: (1) czy klient wstaje - jesli nie, podejrzany
jest R1; (2) czy tekst widac - jesli nie, `MSDF_WVP_TRANSPOSE`; (3) czy reszta
UI nie jest pomalowana shaderem czcionek; (4) pamiec tekstur wobec
`d3d9.textureMemory = 64`.

## Czego jeszcze nie ma (do etapu 4)

- `third_party/` Lexary **nie jest w repozytorium** - trzeba sciagnac FreeType 2.14.1,
  msdfgen, Detours i unordered_dense. Sam kod `src/font_exact` jest kompletny.
- Adresy 1.12 dla hakow niezaleznych od shaderow, ktorych etap 2 nie mapowal:
  `DeviceCreate` (3.3.5 `00682CB0`), `NotifyOnDeviceRestored` (`006843B0`),
  `DeviceSetFormat` (`006904D0`), `IDestroyD3d` (`006903B0`),
  `IReleaseD3dResources` (`00690150`). Alternatywa: w ogole ich nie mapowac i wziac
  urzadzenie z haka `Direct3DCreate9` / vtable, jak robi to wiekszosc addonow do 1.12 -
  wtedy zero adresow klienta w tej warstwie.
- `IShaderCreateVertex` / `IShaderCreatePixel` (`006AA0D0` / `006AA070`) - **nie szukac**,
  w 1.12 nie maja odpowiednika i nie sa portowi potrzebne.

---

# Etap 5 - pierwsze uruchomienie i debugowanie w grze (2026-09-09)

**Renderer dziala.** Tekst rysuje sie przez atlas MSDF, kropki, myslniki i litery
siedza na swoich miejscach. Ponizej to, co trzeba bylo naprawic po drodze -
kazda pozycja z pomiaru, nie z dedukcji.

## Siedem usterek znalezionych dopiero w grze

| # | objaw | prawdziwa przyczyna |
|---|---|---|
| 1 | crash przy starcie, `0xC0000005` pod `007CECA4` | klient wolal WLASNE `FT_Add_Default_Modules` na bibliotece FreeType 2.14.1 Lexary |
| 2 | shadery `vs=0 ps=0` | kompilacja w `FreeType_InitHk`, czyli ZANIM istnieje urzadzenie D3D |
| 3 | brak urzadzenia mimo zlapanego `d3d9.dll` | klient nie tworzy urzadzenia na obiekcie, ktory widzimy |
| 4 | `atlas: stron=0`, czarne prostokaty | `ProcessGeometry` generowal glify przed przechwyceniem urzadzenia |
| 5 | znikajace litery mimo poprawnego atlasu | nieudany upload zwracal "sukces", wpis zostawal z `UV (0,0)-(0,0)` na stale |
| 6 | `.`, `-`, `_` z `y ~ 1.8e7` | stub `GetGlyphYMetrics` nadpisywal EDX, ktory w 1.12 jest zywy |
| 7 | te same znaki przy GORNEJ krawedzi wiersza | moje wlasne przycinanie odejmowania - poprawka do usterki 6, ktora ja przezyla |

## Przechwycenie urzadzenia - trzy sposoby zawiodly, czwarty dziala

`WoW.exe` 1.12 **nie importuje d3d9.dll** (jedyny import graficzny to `opengl32.dll`),
laduje ja przez `LoadLibrary`. Po kolei odpadly:

1. haki `CGxDevice` - w 1.12 nie ma ani globalu `00C5DF88`, ani tych funkcji;
2. detour na ciele `Direct3DCreate9`/`CreateDevice` - **instalowal sie czysto**
   (`begin/update/attach/commit = 0`), a hak nie odpalal sie ani razu;
3. podmiana wpisu `vtbl[16]` - wpis podmieniony, nadal nie wolany.

Wniosek: klient tworzy urzadzenie poza obiektem, ktory widzimy (miedzy nim
a DXVK-iem stoi cos jeszcze - najpewniej inny mod).

**Dziala czwarty sposob:** obiekty tej samej klasy C++ WSPOLDZIELA tablice
wirtualna. Tworzymy wiec wlasne, jednorazowe urzadzenie na ukrytym oknie 8x8,
podmieniamy w jego vtable `EndScene` (slot 42) i zwalniamy swoje urzadzenie.
Od tej chwili kazde urzadzenie DXVK-a w procesie - takze to klienta - przechodzi
przez nasz hak i podaje sie w `this`.

**`commit = 0` NIE znaczy "hak dziala".** Znaczy tylko tyle, ze Detours nie
zglosil bledu. Przez trzy rundy czytalem zero jako sukces; rozstrzygnal dopiero
log z WNETRZA haka, ktory byl caly czas nieobecny.

## Stub GetGlyphYMetrics - rejestry, nie tylko adresy

```
3.3.5 (006C8C71):  mov edx, [ecx+0x54]  ->  mov ecx, [edx+0x68]   lancuch przez EDX
1.12  (005D137A):  mov ecx, [ecx+0x54]  ->  mov ecx, [ecx+0x68]   lancuch przez ECX
```

Stub przeniesiony doslownie z 3.3.5 nadpisywal EDX, ktory w 1.12 jest w tym
miejscu **zywy i celowo wyzerowany** (`005D136B mov [ebp-4],edx`,
`005D136F xor edx,edx`). Klient liczyl potem z tego smiecia wspolrzedna pionowa.

Kontrola dlugosci miejsca i adresu powrotu **przechodzila** - latka wygladala na
poprawna. Przy kazdym stubie sprawdzac takze, ktore rejestry sa zywe.

## Dwie nakladajace sie usterki - pulapka metodyczna

Usterka 6 dawala `y ~ 1.8e7`. Uznalem to za przepelnienie bez znaku w
`m_bearingY -= m_verAdv` i dodalem przycinanie do zera. Nie pomoglo, ale
**zostalo w kodzie**. Po naprawie prawdziwej przyczyny (stub) przycinanie samo
stalo sie usterka 7: sprowadzalo wyniesienie kropki i myslnika do zera, czyli
sadzalo je przy gornej krawedzi wiersza.

Rozstrzygnely dwa pomiary, nie analiza kodu:
- sonda pokazala, ze przycinanie odpalilo tylko 3 razy i to z `kod=0` -
  czyli w wywolaniach bez sensownych metryk, a dla `.` i `-` wcale;
- test "wylacz hak i zobacz" (`hook_renderglyph=0`) pokazal, ze bez odejmowania
  rozjezdzaja sie WSZYSTKIE litery, wiec jest ono konieczne, a wynik ujemny
  poprawny - klient czyta te wartosc ZE ZNAKIEM.

## Plik przelacznikow - bisekcja bez przebudowy

`lexara112.cfg` obok klienta, `nazwa=0/1`, brak pliku = wszystko wlaczone.
23 klucze: 10 miejsc latania, 6 hakow czcionek, `ft_hooks`, `shaders`.
Zmiana wymaga tylko restartu gry, nie przebudowy DLL-a - to skrocilo kazda
runde diagnozy z "przebuduj i wejdz" do samego wejscia.

**Pulapka:** hak `FT_New_Library` i `FT_Add_Default_Modules` musza byc pod TYM
SAMYM kluczem co reszta `ft_hooks`. Pierwsza wersja zostawiala je poza bramkami
"bo to infrastruktura" i przy `ft_hooks=0` klient dostawal biblioteke Lexary,
ale wolal na niej wlasne, stare funkcje - stan gorszy niz obie skrajnosci
i crash. Kontrola "wszystko wylaczone" nie byla wtedy zadna kontrola.

## Sondy - czego nie robic

- **`static bool` jako straznik ukrywa rozklad.** Sonda uploadu wypisala tylko
  PIERWSZE wystapienie kazdej przyczyny; jedynym wpisem w logu okazala sie
  spacja, czyli przypadek najmniej istotny. Liczniki na wszystkich wyjsciach
  pokazaly prawde od razu: 392 odrzucen, wszystkie z jednej galezi.
- **Nieoprzyrzadowana galez to ta, ktora zawiedzie.** `CreateAtlasPage` bylo
  jedynym `return false` bez logu i wlasnie tam siedzial blad.
- **Log w petli rysowania sam staje sie problemem.** Zrzut co 200 wywolan przy
  2,3 mln wywolan na sesje to 11 tys. otwarc pliku.
- **Kontrola o znanym wyniku rozstrzygnela sprawe znikajacych znakow.** Dopiero
  zestawienie `.` (`y = 1.8e7`) z litera `m` (`y = -31..-12`) z TEJ SAMEJ partii
  pokazalo, ze to anomalia, a nie normalna wspolrzedna tego potoku.

## Przelacznik w grze

`CTRL+ALT+F` przelacza renderer na zywo (flaga `MSDF::ENABLED`, obsluga w haku
`EndScene`, wykrywanie zbocza klawisza). Addon `LexaraCompare` (`/lexara`) daje
staly wzorzec tekstu w siedmiu rozmiarach, prawy przycisk zmienia kroj.

**Znane ograniczenie:** po przelaczeniu czesc napisow potrafi zostac w poprzedniej
postaci, bo w cache klienta zostaja wpisy glifow ze znacznikiem `u = 1 + kod`.
Zerujemy te pola przy wylaczeniu, ale napisy juz rozlozone przez klienta nie sa
przeliczane od nowa. Do domkniecia.

## Stan i co dalej

Dziala: atlas, shadery, geometria pozioma i pionowa, przechwycenie urzadzenia,
kropki, myslniki, litery. Do sprawdzenia i domkniecia:

- podkreslnik `_` - zglaszany jako zly, do potwierdzenia po ostatnich poprawkach;
- artefakty po przelaczniku (patrz wyzej);
- `metrics.pixelData` wskazuje na `storage.ownedPixelData` zmiennej LOKALNEJ
  (`MSDFFont.cpp`) - w oryginale nieszkodliwe, bo upload leci natychmiast,
  ale ponowienie uploadu z bufora czytaloby zwolniona pamiec;
- pomiar wydajnosci i pamieci wobec `d3d9.textureMemory = 64`.
