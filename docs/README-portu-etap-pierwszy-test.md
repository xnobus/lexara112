# Lexara -> Turtle WoW 1.12 (twmoa_1171)

Port renderera czcionek MSDF z https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5.
Mapa adresow i ustalenia: `..\_wiedza\lexara-port-mapa.md`.
Skrot calej wiedzy o porcie: `..\_wiedza\lexara.md`. Sonda zdolnosci: `..\_lexara-probe\`.

## Stan: gotowy do PIERWSZEGO testu w grze

`lexara112.dll` ma juz komplet adresow 1.12. Kontrola po buildzie: **zaden adres
3.3.5 nie zostal w binarce** (sprawdzane m.in. `00C5DF88`, `00C7D2CC/D0`,
`00991320`, `00993370`, `00682CB0`, `006AA0D0`), a wszystkie nowe sa obecne.

Nie znaczy to, ze zadziala za pierwszym razem - znaczy, ze nie ma juz nic,
co dalo sie rozstrzygnac bez uruchomienia. Cztery rzeczy do sprawdzenia
pomiarem, w tej kolejnosci:

1. **Czy klient wstaje.** Jesli nie - najpewniej R1: miejsce `ProcessBatch`
   (`005C91A8`) ma dokladnie 5 bajtow i jest CELEM skoku `je 005C91A9`
   z `005C8FF3`. Wejscie w ten skok po zalataniu to skok w srodek instrukcji.
2. **Czy tekst jest w ogole widoczny.** Jesli tekst znika albo laduje poza
   ekranem, to macierz: `MSDF_WVP_TRANSPOSE` w `src/font_exact/MSDF.cpp`.
   Zmienic na `false`, zbudowac ponownie. To jedyne miejsce w porcie, ktorego
   nie dalo sie ustalic statycznie.
3. **Czy interfejs poza tekstem wyglada normalnie.** Jesli cale UI jest
   pomalowane shaderem czcionek, to znaczy, ze `UnbindMsdfShaders` nie lapie
   wszystkich wyjsc z partii.
4. **Pamiec tekstur.** Cztery atlasy 2048x2048 to 64 MB. Sonda potwierdzila,
   ze wchodza, ale klient ma `d3d9.textureMemory = 64` i historie crashy przy
   duzej liczbie ZYWYCH MAPOWAN (nie przy ilosci pamieci) - patrz
   `_wiedza/crash-i-dxvk.md`.

**Wycofanie:** usunac wpis z `dlls.txt`. DLL niczego nie zapisuje na dysku
i nie zmienia plikow klienta - lata pamiec procesu, wiec restart bez wpisu
wraca do stanu sprzed.

## Budowanie

```
build.bat
```

Wymaga VS 2022 **BuildTools** (Community na tej maszynie nie ma toolsetu) i CMake
z pipa (`python -m pip install --user cmake`) - sciezka wpisana w `build.bat`.
Wynik: `build\out\Release\lexara112.dll`.

## Ladowanie (docelowo)

3.3.5 ladowal Lexare jako proxy `dinput8.dll`. Tutaj klient ma VanillaFixes,
wiec wystarczy skopiowac DLL do katalogu klienta i dopisac nazwe do `dlls.txt`.
Stad `src/dllmain112.cpp` zamiast `dllmain.cpp` + `Proxy.cpp`.

Kopia jest juz w katalogu klienta jako `lexara112.dll`. Do wlaczenia brakuje
tylko linii `lexara112.dll` w `dlls.txt` - **swiadomie niedopisanej**, zeby
nie uruchomic nieprzetestowanego kodu przy najblizszym starcie gry.

## Zaleznosci

`third_party/` sciagniete osobno (Lexara ich nie dolacza):

| katalog | zrodlo | uwagi |
|---|---|---|
| `freetype-2.14.1` | github.com/freetype/freetype, tag `VER-2-14-1` | wersja, ktorej uzywa Lexara |
| `msdfgen` | github.com/Chlumsky/msdfgen | SVG i PNG **wylaczone** (ciagna tinyxml2/libpng) |
| `Detours` | github.com/microsoft/Detours | dodane: `CMakeLists.txt` i forwarder `detours.h` |
| `unordered_dense_src` | github.com/martinus/unordered_dense | tylko naglowek |

**msdfgen bez Skii** nie ma `resolveShapeGeometry`; zastapione w
`src/font_exact/MSDFCompat.h` tym samym, czym zastepuje to sam msdfgen
(`shape.orientContours()`, `main.cpp:1155`). Szczegoly w naglowku pliku.

## Zmienione pliki Lexary

Kopie oryginalow obok, jako `.bak-335`:

- `src/font_exact/GameClient.h` - adresy i konwencje 1.12, uklad `CGxString`,
  FreeType na `__fastcall`.
- `src/font_exact/MSDF.cpp` - dziesiec miejsc latania, cztery stuby `naked`,
  `BindMsdfShaders`/`UnbindMsdfShaders`, haki FreeType na `__fastcall`.
- `src/font_exact/D3D.cpp` - warstwa urzadzenia przepisana: zamiast siedmiu
  hakow `CGxDevice` lancuch `LoadLibrary -> Direct3DCreate9 -> CreateDevice`.
- `src/font_exact/MSDF.h` - usuniete globale shaderow czcionek.
- `src/font_exact/MSDFValidator.h`, `MSDFFont.cpp` - `MSDFCompat::ResolveShapeGeometry`.

Kazda zmiana merytoryczna (nie sam adres) ma w kodzie komentarz `[1.12]`.
