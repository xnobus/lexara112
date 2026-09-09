# Latka `site_texnullfill` - crash 0x00448955 przy nieistniejacej teksturze

Dotyczy klienta **Turtle WoW 1.12 / `WoW_Modernized.exe` (build 5875)**.
Nie ma zwiazku z rendererem MSDF - zaklada sie niezaleznie od `msdf_enabled`.

## Objaw

`ERROR #132`, `0xC0000005 (ACCESS_VIOLATION)` przy `0x00448955`, zapis pod
`0x00000000`. Rejestry sa **zawsze takie same**, bo sciezka jest deterministyczna:

```
EAX=FFFFFFFF EBX=00000008 ECX=00000002 EDX=00000001 ESI=00000002 EDI=00000000
```

Stos: `EvtSched.cpp` -> `CSimpleTop.cpp` -> `CGxDevice.cpp` -> `CGxD3dTexture.cpp`
-> `Texture.cpp`. **Zadnej ramki Lua**, wiec wyglada jak crash renderera - a jest
to sciezka do pliku, ktorego nie ma, podana wczesniej z Lua przez `SetTexture`.
Ladowanie jest odlozone do rysowania, wiec crash pada klatke po tym, kto go
spowodowal, i winowajcy nie widac na stosie.

## Przyczyna

Callback ladowania tekstury `0x0044A260`:

```
0044A2AF  mov eax, [ebp+14h]      ; obiekt tekstury
0044A2B2  mov edx, [0B05D14h]     ; globalna tablica bitow zastepczych
0044A2C0  lea ecx, [eax+120h]
0044A2C8  mov [ecx], edx          ; obj->mipBits = tablica zastepcza
0044A2D2  lea esi, [eax+0Ch]      ; obj->filename
0044A2D9  mov ecx, esi
0044A2DB  call 004491F0           ; wczytaj plik
0044A2E0  test eax, eax
0044A2E2  jne 0044A306            ; sukces
0044A2E4  mov edx, [ebp+8]
0044A2E7  push 1
0044A2E9  mov ecx, edi
0044A2EB  call 00448920           ; PORAZKA -> "wypelnij bialym"
```

`0x004491F0` zwraca 0 na dwa sposoby, oba przez `0x005A3660`:

- pusta sciezka - `cmp byte ptr [edi],0`, `SetLastError(57h)`, `xor eax,eax`;
- nieudane otwarcie pliku - `0x005A369D`: `test eax,eax` / `jne`, galaz bledu
  wychodzi z `eax` juz wyzerowanym.

**Otwarta zagadka.** Funkcja tworzaca teksture (`0x0044A140`) otwiera plik juz na
wejsciu (`0x0044A164 call 005A3660`) i przy porazce zwraca 0, nie tworzac obiektu -
wiec z tego kodu wynika, ze nieistniejacy plik NIE powinien w ogole dojsc do
callbacku. A dochodzi: reprodukcja nizej opiera sie wlasnie na sciezkach do plikow,
ktorych nie ma. Czyli albo obiekt powstaje jeszcze gdzies indziej, albo nazwa jest
rozwiazywana inaczej przy tworzeniu niz przy ladowaniu. Nie ustalone.

Rejestry w chwili crasha mowia, ze pekl poziom mipmapy **2x1** (`ESI=2`, `EDX=1`,
`EBX=8` = 2*1*4). Lancuch 256x32 (paski) i 256x128 (`logo.tga`) schodzi wlasnie do
2x1, a oba to `.tga`, czyli pliki bez mipmap. Poszlaka, nie ustalenie.

`0x00448920` chodzi po lancuchu mipmap tablicy zastepczej i wypelnia ja
wartoscia `0FFFFFFFFh`. Tablica istnieje, ale **jej wpisy sa NULL** - bufor
zastepczy nie jest w tym kliencie nigdy alokowany:

```
00448936  cmp edx, 1              ; petla po poziomach
00448939  ja  00448940
0044893B  cmp esi, 1
0044893E  jbe 00448985            ; wyjscie
00448940  mov edi, [ebp-4]        ; kolejny wpis tablicy
00448943  mov edi, [edi]          ; <-- NULL
00448945  mov ecx, esi
00448947  imul ecx, edx
0044894A  shl  ecx, 2
0044894D  mov ebx, ecx            ; rozmiar w bajtach
0044894F  shr  ecx, 2             ; rozmiar w dwordach
00448952  or  eax, 0FFFFFFFFh     ; <-- MIEJSCE LATANIA (5 bajtow)
00448955  rep stosd               ; <-- CRASH
00448957  mov ecx, ebx            ; <-- powrot przy edi != 0
00448959  and ecx, 3
0044895C  rep stosb               ; drugi zapis, tez pod NULL
0044895E  mov edi, [ebp-4]        ; <-- powrot przy edi == 0
```

## Latka

`src/font_exact/TexNullFill.cpp`. Detour na `0x00448952`, zajmuje dokladnie
5 bajtow (`83 C8 FF` + `F3 AB`), obie instrukcje kompletne, granica instrukcji
z obu stron.

```asm
or   eax, 0FFFFFFFFh
test edi, edi
jz   no_buffer
rep  stosd
jmp  00448957          ; normalna droga - reszta oryginalu dokancza stosb
no_buffer:
jmp  0044895E          ; pomija OBA zapisy
```

**Dwa adresy powrotu, nie jeden.** Drugi zapis (`rep stosb` pod `0044895C`)
wywalilby sie tak samo, wiec przy NULL trzeba przeskoczyc od razu na `0044895E`.

Pominiecie poziomu jest bezpieczne: petla przelicza `ecx` (`00448945`) i `ebx`
od nowa, a `edi` bierze ponownie z `[ebp-4]` (`0044895E`). Zaden stan nie
przechodzi miedzy obrotami przez rejestry, ktore latka zostawia.

Przed zalozeniem latka porownuje bajty pod `0x00448952` z oczekiwanymi
`83 C8 FF F3 AB` i przy niezgodnosci **odmawia** - inaczej po podmianie
`WoW_Modernized.exe` wpisalaby skok w srodek czegos innego.

## Wynik

Nieudane ladowanie tekstury konczy sie brakiem grafiki zamiast smiercia procesu.
**To naprawa obslugi bledu, nie przyczyny** - sciezka do nieistniejacego pliku
dalej jest bledem po stronie addona.

## Sprawdzenie - POTWIERDZONE W GRZE 2026-09-09

W logu `lexara112.log` przy starcie:

```
[MSDF] site_texnullfill: latka na crash 0x00448955 zalozona
```

`commit = 0` z Detours znaczy tylko "brak bledu", NIE "hak dziala", a sam log
mowi tylko tyle, ze latka sie zalozyla. Rozstrzyga dopiero test z kontrola:

| `site_texnullfill` | wynik |
|---|---|
| `0` | crash `0x00448955`, rejestry identyczne jak w sesjach 19:33 i 19:41 (`Errors\2026-09-09 20.31.18 Crash.txt`) |
| `1` | brak crasha, ta sama akcja |

**Sonda `/run` z `SetTexture` na martwa sciezke NIE reprodukuje tego crasha** -
sprawdzone, klient przezywa nawet przy wylaczonej latce. Reprodukcja wymaga
sciezki zapisanej w bazie addona i przemalowania wiersza, ktory ja rysuje:
w WeakestAuras jest to `groupIcon` grupy i **zwiniecie tej grupy** w liscie aur
(`Regions.lua`, `groupModifyThumbnail`). Szczegoly: `_wiedza\weakauras.md`
w katalogu gry.

Wniosek metodyczny: sonda bez kontroli o znanym wyniku nie jest sonda. Pierwsza
wersja tej sondy nie crashowala klienta i wygladalo to na dzialajaca latke,
a byl to po prostu kod, ktory nie dotyka lataneho miejsca.

## Wylaczenie

`lexara112.cfg` obok klienta:

```
site_texnullfill=0
```
