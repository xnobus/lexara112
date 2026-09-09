# Wspolny atlas MSDF - jeden na proces zamiast jednego na kroj

## Objaw

`Errors\2026-09-09 21.56.59 Crash.txt`: `ACCESS_VIOLATION` w `ucrtbase!memcpy+78`,
wolanym z `d3d9.dll` (DXVK 2.6.1 **x86**). Ramka nizej to `0x005A10A0`, czyli
powrot z `call [ebx+0x148]` pod `0x005A109A` - `IDirect3DDevice9::DrawIndexedPrimitive`
(indeks 82 w vtable, siedem pushy zgadza sie z sygnaturą). Rejestry:
`ECX=EDX=0x10000` (64 KiB), zrodlo `ESI=EBX=0x000CCDC0` - adres niezmapowany.

Dwie ostatnie linie `lexara112.log` przed smiercia procesu:

```
[MSDF] CreateTexture 2048x2048 fmt=21 pool=1 hr=0x8876017C wolne=4048 MB
[MSDF] CreateAtlasPage: CreateTexture 2048x2048 ODMOWIL (stron dotad=0, max=4)
```

`0x8876017C` to `D3DERR_OUTOFVIDEOMEMORY`, ale `GetAvailableTextureMem()` melduje
4048 MB wolnego. **To nie VRAM - to 32-bitowa przestrzen adresowa procesu.**
`D3DPOOL_MANAGED` trzyma w DXVK pelna kopie tekstury w pamieci procesu.

## Przyczyna

`MSDFFont::CreateAtlasPage` alokowal strony **na kroj**: kazdy `MSDFFont`
dostawal wlasne 2048x2048 A8R8G8B8 w `D3DPOOL_MANAGED`, czyli 16 MiB na strone
i do `MAX_ATLAS_PAGES` = 4 stron, a wiec do **64 MiB na jeden kroj**. Gorna
granica calosci nie istniala - rosla z liczba faktycznie rysowanych krojow.

W feralnej sesji powstalo 10 atlasow (160 MiB), jedenasty zostal odrzucony
dwukrotnie, a chwile pozniej DXVK zginal na kopiowaniu bufora przy rysowaniu.
Sam ciag "brak przestrzeni adresowej -> crash w memcpy" jest wnioskiem
z kolejnosci zdarzen, nie z dowodu w zrzucie - ale `ODMOWIL` wystepuje
w calym logu **tylko 2 razy i tylko w tej sesji**.

## Zmiana

`s_atlasPages`, `s_oldestPage` i `s_evictionCount` sa teraz **statyczne**:
jeden atlas na proces, dzielony przez wszystkie kroje. Gorna granica to
`MAX_ATLAS_PAGES * ATLAS_SIZE^2 * 4 B` = **64 MiB na caly proces**, niezaleznie
od liczby krojow.

**Kodowanie strony sie nie zmienia.** Indeks strony jedzie znakami UV
(`MSDF.cpp`: `uSign`/`vSign`), a shader wybiera nim sampler `s12`-`s15`
(`MSDFShaders.h`). To dwa bity, wiec 4 strony to sufit narzucony przez format
wierzcholka klienta (`CGxFontVertex` ma tylko `pos`, `u`, `v` - `pos.z` niesie
glebokosc tekstu 3D, wiec nie da sie go zabrac). Wspolny atlas nie rusza tego
kodowania - zmienia tylko to, czyje glify leza na tych czterech stronach.

### Eksmisja

Strona miesza teraz kroje, wiec `AtlasPage::codepoints` (`vector<uint32_t>`)
stalo sie `entries` (`vector<pair<MSDFFont*, uint32_t>>`) - sam kod znaku nie
identyfikuje juz glifu.

`EvictOldestPage()` **uniewaznia wpisy zamiast je usuwac**, i to jest
poprawka, nie kosmetyka: `m_glyphPool` to `ankerl::unordered_dense` - mapa
gesta, ktorej `erase` przestawia inne elementy. `UploadGlyphToAtlas` dostaje
`GlyphMetrics&` **z tej wlasnie mapy** i trzyma te referencje przez cala
eksmisje, wiec kazdy `erase` w trakcie moze ja uniewaznic. Stary kod robil
dokladnie to (na wpisach wlasnego kroju) i uchodzilo mu to plazem tylko
dlatego, ze eksmisje byly rzadkie. Zerowanie `u0/v0/u1/v1` nie rusza ukladu
mapy, a `GetGlyph` i tak rozpoznaje wpis o zerowym `u1`/`v1` jako "nigdy nie
wgrany" i wysyla go do atlasu ponownie - ta sciezka ponowienia juz istnieje
i jest przechodzona przy kazdym starcie, przed pierwsza klatka.

Licznik eksmisji jest globalny, wiec `CheckGeometryHk` przebuduje geometrie
**kazdego** napisu, nie tylko tego kroju, ktory wywolal eksmisje. Tak ma byc:
strona miesza kroje.

### Odmowa alokacji nie jest juz koncem glifu

Wczesniej `CreateAtlasPage() == false` konczylo sie `return false` - glif
przepadal na dobre (te 392 odrzucone glify z komentarza w kodzie). Teraz
odmowa alokacji spada na te sama sciezke co osiagniecie `MAX_ATLAS_PAGES`:
eksmituj najstarsza strone i uloz glif tam. Skoro strony sa wspolne, jest
sie czym podzielic.

### Cykl zycia

- `~MSDFFont` **nie zwalnia stron** - zabralby je krojom, ktore dalej rysuja.
  Wymiata za to wpisy tego kroju (`ForgetFontEntries`), bo trzymaja `this`
  i eksmisja siegnelaby po martwy obiekt.
- Straznik `m_atlasEntryCount != 0` przed tym wymiataniem jest **konieczny
  ze wzgledu na watki**: `MSDFPregen` tworzy `MSDFFont` na watkach roboczych
  i te obiekty nigdy nic nie ukladaja w atlasie (ida prosto do `GenerateMSDF`).
  Bez straznika ich destruktory chodzilyby po stanie dzielonym z watkiem
  rysujacym.
- `ClearAllCache()` (z `D3D::RegisterOnDestroy`) zwalnia strony i zeruje
  pule glifow wszystkich krojow - urzadzenie znika, tekstury musza pojsc.
- `Shutdown()` czysci `s_fontHandles` i `s_atlasPages`.

## Czego to NIE naprawia

Pojemnosc. Wczesniej kazdy kroj mial 4 strony dla siebie; teraz dzieli 4 strony
z pozostalymi. Przy `SDF_RENDER_SIZE = 96` i `SDF_SPREAD = 12` typowy glif
lacinski zajmuje ok. 74x94 px, z odstepem `ATLAS_GUTTER = 14` daje to ok. 23x18
= ~410 glifow na strone, czyli **~1650 na caly atlas**. Dziesiec krojow po
ok. 120 uzywanych znakow to ~1200 - miesci sie, ale bez zapasu.

Jesli w logu zacznie sie mnozyc eksmisja (`s_evictionCount` rosnie, tekst
migocze przy przebudowie geometrii), nastepna dzwignia to **`SDF_RENDER_SIZE`
96 -> 64**: powierzchnia glifu spada wtedy ok. 2,2x, czyli pojemnosc atlasu
rosnie do ~3600 glifow. Kosztem jest ostrosc bardzo duzego tekstu.

## Sprawdzenie

Zbudowane 2026-09-09 22:14 (`build.bat` wymaga cmake; tu poszlo bezposrednio
MSBuild-em na wygenerowanym `build\lexara112.vcxproj`, bo w systemie nie ma
cmake w PATH). W grze **niesprawdzone** - do potwierdzenia potrzeba sesji,
w ktorej narysuje sie kilkanascie roznych krojow.

Czego szukac w `lexara112.log`:

| linia | znaczenie |
|---|---|
| `strona N utworzona (..., atlas wspolny)` | maksymalnie 4 razy na sesje, nie 4 razy na kroj |
| `ODMOWIL` | nie powinno sie juz pojawiac po zbudowaniu 4 stron |

Kopia poprzedniego DLL-a: `lexara112.dll.bak-perfont-atlas` w katalogu gry.
Kopie zrodel: `MSDFFont.h.bak-sharedatlas`, `MSDFFont.cpp.bak-sharedatlas`.
