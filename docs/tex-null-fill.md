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

## Sprawdzenie

W logu `lexara112.log` przy starcie:

```
[MSDF] site_texnullfill: latka na crash 0x00448955 zalozona
```

`commit = 0` z Detours znaczy tylko "brak bledu", NIE "hak dziala". Dowodem jest
dopiero sonda w grze - bez latki wywala klienta, z latka rysuje pusto
(miesci sie w limicie 255 znakow pola czatu 1.12):

```
/run local f=CreateFrame("Frame",nil,UIParent) f:SetWidth(64) f:SetHeight(64) f:SetPoint("CENTER") local t=f:CreateTexture() t:SetAllPoints() t:SetTexture("Interface\\AddOns\\Nic\\Takiego\\Nie\\Ma")
```

## Wylaczenie

`lexara112.cfg` obok klienta:

```
site_texnullfill=0
```
