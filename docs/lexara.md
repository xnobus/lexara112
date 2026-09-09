# Lexara (HD MSDF) na 1.12 - komplet wiedzy

Port renderera czcionek przez atlas MSDF z 3.3.5a na Turtle WoW 1.12.
Oryginal: https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5

**Stan na 2026-09-09: DZIALA w grze.** Tekst rysuje sie przez atlas MSDF,
litery, cyfry, kropki, myslniki i podkreslniki na swoich miejscach.

## Gdzie co lezy

| sciezka | co to |
|---|---|
| `_wiedza/lexara-port-mapa.md` | **komplet adresow i etapow** - offsety struktur, konwencje, wszystkie ustalenia z binarki |
| `_lexara-port/` | budowalny projekt (`build.bat` -> `lexara112.dll`), wlasny README |
| `_lexara-narzedzia/` | skrypty do porownywania binarek 1.12 vs 3.3.5a |
| `_lexara-probe/` | sonda zdolnosci urzadzenia (kompiluje i rysuje shadery Lexary) |
| `lexara112.dll` | wgrany DLL, wstrzykiwany przez VanillaFixes (wpis w `dlls.txt`) |
| `lexara112.cfg` | przelaczniki latek - patrz nizej |
| `lexara112.log` | log DLL-a (domyslnie oszczedny) |
| `Interface/AddOns/LexaraCompare/` | addon `/lexara` - panel porownawczy |
| katalog `LEXARA` w `%TEMP%` | cache wygenerowanych glifow, mozna kasowac |

**Wycofanie:** usunac `lexara112.dll` z `dlls.txt` (kopia `dlls.txt.bak-przed-lexara`).
DLL nie zapisuje niczego w plikach klienta - lata pamiec procesu.

## Obsluga

- **`lexara112.cfg`** - 24 przelaczniki, `nazwa=0/1`, brak pliku = wszystko wlaczone.
  10 miejsc latania (`site_*`), 6 hakow czcionek (`hook_*`), `ft_hooks`, `shaders`,
  `msdf_enabled`. **Zmiana wymaga tylko restartu gry, nie przebudowy DLL-a** -
  to narzedzie pierwszego wyboru przy kazdej regresji.
- **`CTRL+ALT+F11`** - zapisuje `msdf_enabled` do cfg; dziala od nastepnego startu.
- **`/lexara`** (albo `/lex`) - panel z tym samym tekstem w 9 rozmiarach (8-72 px),
  pelny zestaw znakow specjalnych, prawy przycisk zmienia kroj.

## Architektura - czym port rozni sie od oryginalu

**Klient 1.12 nie ma shaderow czcionek w ogole.** 3.3.5 (`006BE230`) tworzy obiekt
shadera wierzcholkow i pikseli, zanim wywola `InitFontIndexBuffer`; 1.12
(`005C17F0`) zaczyna sie od tego wywolania. `SetVertexShader` nie jest w tej
binarce wolane ani razu, a `RenderBatch` ustawia wylacznie stany potoku stalego.
Cala warstwa `D3D.cpp` Lexary (haki `IShaderCreate*`, podmiana bajtkodu) nie ma
tu odpowiednika - port kompiluje wlasne `vs_3_0`/`ps_3_0` i wiaze je sam.

**`ps_3_0` dziala pod DXVK tego klienta** - zmierzone sonda `_lexara-probe`, nie
zalozone: caps SM 3.0/3.0, `fwidth` liczy, FVF wystarcza za deklaracje
wierzcholkow, cztery atlasy 2048x2048 A8R8G8B8 wchodza. Napisy `ps_2_0` w `.data`
klienta (tablica `0085C608`) to **martwy kod** - nikt w `.text` ich nie dotyka.

**Urzadzenie D3D przechwytujemy przez wspoldzielona tablice wirtualna.** Tworzymy
wlasne, jednorazowe urzadzenie na ukrytym oknie 8x8, podmieniamy w jego vtable
`EndScene` (slot 42) i zwalniamy swoje. Obiekty tej samej klasy C++ dziela vtable,
wiec od tej chwili urzadzenie klienta samo sie przedstawia w `this`.

## Siedem usterek znalezionych dopiero w grze

| objaw | prawdziwa przyczyna |
|---|---|
| crash `0xC0000005` pod `007CECA4` | klient wolal WLASNE `FT_Add_Default_Modules` na bibliotece FreeType 2.14.1 Lexary |
| `vs=0 ps=0` | shadery kompilowane w `FreeType_InitHk`, czyli zanim istnieje urzadzenie |
| brak urzadzenia mimo zlapanego `d3d9.dll` | klient nie tworzy go na obiekcie, ktory widzimy |
| `atlas: stron=0`, czarne prostokaty | `ProcessGeometry` generowal glify przed przechwyceniem urzadzenia |
| znikajace litery | nieudany upload zwracal "sukces", wpis zostawal z `UV (0,0)` na stale |
| `.` `-` `_` z `y ~ 1.8e7` | stub `GetGlyphYMetrics` nadpisywal EDX, ktory w 1.12 jest zywy |
| te znaki przy gornej krawedzi | wlasna poprawka do poprzedniej usterki, ktora ja przezyla |

## Pulapki warte zapamietania poza tym projektem

- **`commit = 0` z Detours znaczy tylko "brak bledu", NIE "hak dziala".** Trzy
  sposoby przechwycenia tworzenia urzadzenia instalowaly sie czysto i nie byly
  wolane ani razu. Jedyna sonda jest log z WNETRZA samego haka.
- **Przy stubie asemblerowym sprawdzac ZYWE REJESTRY, nie tylko dlugosc miejsca
  i adres powrotu.** `GetGlyphYMetrics`: 3.3.5 prowadzi lancuch przez EDX
  (`mov edx,[ecx+0x54]` / `mov ecx,[edx+0x68]`), 1.12 przez ECX
  (`mov ecx,[ecx+0x54]` / `mov ecx,[ecx+0x68]`). Doslowne przeniesienie
  nadpisywalo zywy, celowo wyzerowany EDX. Kontrola dlugosci przy tym przechodzila.
- **Dwie nakladajace sie usterki.** Leczylem objaw cudzego bledu; poprawka nie
  pomogla, ale ZOSTALA w kodzie i po naprawie prawdziwej przyczyny sama stala sie
  usterka. Po kazdej pojedynczej zmianie mierzyc, a stare "poprawki" wycofywac.
- **`static bool` jako straznik sondy ukrywa rozklad.** Sonda uploadu wypisala
  tylko PIERWSZE wystapienie kazdej przyczyny; jedynym wpisem okazala sie spacja,
  czyli przypadek najmniej istotny. Liczniki na wszystkich wyjsciach pokazaly
  prawde od razu: 392 odrzucenia, wszystkie z jednej galezi.
- **Nieoprzyrzadowana galez to ta, ktora zawiedzie.** `CreateAtlasPage` bylo
  jedynym `return false` bez logu i wlasnie tam siedzial blad.
- **Kontrola o znanym wyniku rozstrzyga.** Zestawienie znikajacej kropki
  (`y = 1.8e7`) z widoczna litera `m` (`y = -31..-12`) Z TEJ SAMEJ partii pokazalo,
  ze to anomalia, a nie normalna wspolrzedna tego potoku.
- **Log w petli rysowania sam staje sie problemem.** Zrzut co 200 wywolan przy
  2,3 mln wywolan `GetGlyph` na sesje to 11 tys. otwarc pliku.
- **Plik przelacznikow oplacil sie wielokrotnie.** Bisekcja bez przebudowy skracala
  runde z "przebuduj i wejdz" do samego wejscia. Warunek: pod bramkami musi byc
  KOMPLET hakow danej warstwy - zostawienie `FT_New_Library` poza `ft_hooks`
  dawalo stan gorszy niz obie skrajnosci (klient z obca biblioteka, ale wlasnymi
  funkcjami) i crash.

## Zaleznosci projektu

`third_party/` sciagane osobno (Lexara ich nie dolacza): FreeType `VER-2-14-1`,
msdfgen (SVG i PNG **wylaczone** - ciagna tinyxml2/libpng), Detours (dopisany
`CMakeLists.txt` i forwarder `detours.h`), unordered_dense.

**msdfgen bez Skii nie ma `resolveShapeGeometry`** - zastapione w `MSDFCompat.h`
tym samym, czym zastepuje to sam msdfgen (`shape.orientContours()`,
`main.cpp:1155`). Cena: glify o zachodzacych konturach moga miec artefakt
na przecieciu. Nie zaobserwowano, ale nie mierzone celowo.

**Toolchain:** brak CMake w systemie i w VS (Community 2022 jest bez toolsetu -
toolset ma **BuildTools**). CMake z pipa. `.bat` z PowerShella uruchamiac przez
`& cmd.exe /c '<pelna sciezka>'`.

## Co zostalo niedokonczone

- **Zywe przelaczanie renderera.** `GetOrCreateGlyphEntry` (`005CABD0`) to czysty
  odczyt z tablicy haszujacej - zwraca wpis BEZ sprawdzania pol, wiec zerowanie
  `m_cellIndexMin/Max` i `m_texturePageIndex` (tak robi Lexara) NIE wymusza
  ponownego wygenerowania glifu. Przy wlaczonym rendererze klient nigdy nie
  wyrenderowal tych glifow u siebie, wiec nie ma czego przywracac. Zywe
  przelaczanie wymaga USUNIECIA wpisu z tablicy klienta - do zrobienia, gdy
  znajdzie sie funkcja usuwajaca. Stad przelacznik dziala od restartu.
- `metrics.pixelData` wskazuje na `ownedPixelData` zmiennej LOKALNEJ
  (`MSDFFont.cpp`) - w oryginale nieszkodliwe, bo upload leci natychmiast, ale
  kazde ponowienie uploadu z bufora czytaloby zwolniona pamiec.
- Pomiar wydajnosci i pamieci wobec `d3d9.textureMemory = 64` (patrz
  `_wiedza/crash-i-dxvk.md` - ograniczeniem jest liczba zywych mapowan).
- Jakosc krojow ozdobnych bez `resolveShapeGeometry` (patrz wyzej).
