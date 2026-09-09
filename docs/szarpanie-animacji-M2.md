# Szarpanie animacji M2 — przyczyna i naprawa

**Status: ZAMKNIETE.** Objaw ustapil, czcionki dzialaja.

## Objaw

Przy chodzeniu szarpaly sie animacje wszystkich modeli M2 — postaci graczy i NPC.
Na postoju, z samym obracaniem kamery, bylo dobrze. Swiat, teren i interfejs
zachowywaly sie normalnie; szarpaly wylacznie modele animowane.

Kluczowe: **to nigdy nie byl spadek plynnosci.** Zmierzony czas klatki przez caly
czas wynosil 16,67 ms przy maksimum 18–19 ms i zerze klatek powyzej 20 ms.

## Przyczyna

Zeby zalozyc hak na `IDirect3DDevice9::EndScene`, port potrzebowal adresu tablicy
wirtualnej urzadzenia. Zdobywal go tworzac **wlasne, jednorazowe urzadzenie D3D9
8x8 wraz z tymczasowym oknem**, odczytujac z niego wskaznik tablicy i zwalniajac je.

To wystarczalo, zeby rozstroic animacje w calym kliencie.

Powstawalo drugie urzadzenie D3D9 na DXVK, zanim klient utworzyl swoje. Klient
dostawal potem urzadzenie w innym stanie sterownika niz normalnie. Czas klatki
zostawal rowny, wiec to nigdy nie byl koszt — to byla zmiana warunkow.

## Jak to zostalo ustalone

Bisekcja przez `lexara112.cfg` — ta sama, ktora w innych sprawach dziala —
**musiala tu dawac same falszywe negatywy.** Kazdy config, ktory dotykal sciezki
czcionek, wolal `D3D::GetDevice()`, a `GetDevice()` tworzylo urzadzenie zastepcze.
Objaw szedl wiec za samym FAKTEM dotkniecia sciezki, nie za tym, co ta sciezka
robila. Config N nie byl czysty dlatego, ze wylaczal szesc hakow geometrii tekstu,
tylko dlatego, ze przy nim nikt nie wolal `GetDevice()`. Serie N, S, T i W
mierzyly wiec przez trzydziesci wejsc do gry jedno i to samo zdarzenie.

Rozstrzygnela dopiero drabinka schodzaca do zera, kazdy szczebel osobnym wejsciem:

| stan | wynik |
|---|---|
| bez `lexara112.dll` w `dlls.txt` | czysto |
| DLL wczytany, zero kodu na klatke | czysto |
| renderer MSDF wlaczony | szarpie |
| renderer wylaczony, sam przechwyt rysowania i shaderow | szarpie |
| renderer wylaczony, bez przechwytow, tylko `hkEndSceneShared` raz na klatke | szarpie |
| **jak wyzej, ale bez urzadzenia zastepczego** | **czysto** |

Ostatni wiersz izoluje przyczyne: w tym stanie w klatce nie leci NIC naszego,
a jedyna roznica wzgledem wiersza wyzej jest jednorazowe utworzenie i zwolnienie
urzadzenia zastepczego na starcie.

## Naprawa

Urzadzenie zastepcze **nie powstaje**. Zostalo w kodzie za `shared_vtable=1`,
wylacznie do diagnostyki (`AcquireSharedDeviceVtable` w `D3D.cpp`).

Urzadzenie klienta lapiemy przez podmieniony wpis `CreateDevice` w tablicy
wirtualnej `IDirect3D9`. Wymagalo to dwoch rzeczy:

1. **Straz nad wpisem `vtbl[16]`** (`VtableGuardThread` w `D3D.cpp`). Cos przywraca
   pierwotny wpis DXVK zaraz po naszej podmianie, a klient tworzy urzadzenie
   wlasnie w tej dziurze — dlatego `hkCreateDevice` nie odpalal sie dla niego ani
   razu. Straz odzyskuje wpis w gestej petli przez pierwsze sekundy i **konczy sie,
   gdy tylko urzadzenie zostanie zlapane**. Gdy cudzy wpis zostanie zastany, staje
   sie on naszym oryginalem, wiec lancuch hakow tamtego moda dziala dalej.
2. **Obsadzone `Direct3DCreate9Ex`** — drugie wejscie do d3d9, dotad nieobsadzone.

## Pulapki, ktore po drodze kosztowaly wejscia do gry

- **Podmiana wpisu vtable lapie urzadzenie klienta tylko wtedy, gdy zrobi sie ja
  PO jego utworzeniu.** Zalozona wczesnie nie zlapala go ani razu. Dzialalo to
  wczesniej przypadkiem, bo przechwyt byl leniwy i szedl ze sciezki czcionek.
- **Detour na CIALO funkcji urzadzenia DXVK instaluje sie czysto i nie odpala sie
  ani razu**, jesli zalozyc go wczesnie. `commit=0`, poprawne adresy, zero wywolan.
  Te same detoury zalozone pozno, z watku rysujacego, dzialaja.
- **`DetourTransactionCommit` bez sprawdzenia statusu** — cicha porazka zalozenia
  hakow wyglada identycznie jak brak wywolan. Status jest teraz logowany.
- **Po `Hooks::Detour` zmienna z oryginalem trzyma TRAMPOLINE**, nie adres eksportu.
  Porownywanie jej z `GetProcAddress` zawsze wypada negatywnie i produkuje falszywe
  alarmy o „drugim module".
- **Metryka „poza modeli sie nie zmienila" nie opisuje tego objawu.** Klient
  wysyla macierze kosci (`vs c10..c135`) przed kazdym rysowaniem modelu, wiec ich
  tresc jest bezposrednim odczytem pozy. Przy wylaczonym rendererze wychodzilo
  srednio 49% zmienionych zapisow, przy wlaczonym 33–42% — zakresy zachodza na
  siebie. Poza stojaca co druga klatke jest normalnym zachowaniem tego klienta,
  a nie objawem.
- **Przyrzad pomiarowy sam wywolywal objaw.** Sonda, ktora mierzyla te liczby,
  potrzebowala do dzialania urzadzenia zastepczego - czyli wlasnie tego, co bylo
  przyczyna. Kilka „przebiegow odniesienia" bylo przez to skazonych i nie
  nadawalo sie do porownan. Po ustaleniu przyczyny sonda zostala usunieta.

## Poprawki zrobione przy okazji

- **Rejestry stalych przeniesione ponad zakres klienta.** Pomiar wykazal, ze
  klient pisze po `vs c2..c198`. Nasz hak pisal po `c0..c3` (WorldViewProj,
  przydzielone domyslnie przez kompilator HLSL) i `c23` (control), czyli kolidowal
  na `c2`, `c3` i `c23` — okolo 190 nadpisan na klatke. Teraz `control` siedzi
  w `c220`, a `WorldViewProj` w `c240`; wartosci musza sie zgadzac z `register(cNN)`
  w `MSDFShaders.h`. Zapas nad `c198` jest niewielki, a `ps_3_0` konczy sie na
  `c223` — jesli klient kiedys siegnie wyzej, trzeba wyjac `control` z rejestrow
  stalych, a nie przesuwac go dalej.
- **`vtbl[20]` ruszany tylko po potwierdzeniu `IDirect3D9Ex`.** DXVK zwraca
  z `Direct3DCreate9` obiekt Ex i tam bylo bezpiecznie, ale `d3d9.dll` z Windows
  zwraca zwykly `IDirect3D9`, ktorego tablica konczy sie na slocie 16 — zapis pod
  `vtbl[20]` byl tam zapisem poza tablice, po cudzych danych w `.rdata`.
- **`IsD3D9Name` rozpoznaje tez ukosnik w przod**, bo wpis `dxvk` w `dlls.txt`
  potrafi podac sciezke jako `dxvk/d3d9.dll`.

## Znane konsekwencje

- **Skrot CTRL+ALT+F nie dziala.** Obsluga siedzi w `hkEndSceneShared`, ktory jest
  zakladany przez podmiane `vtbl[42]` — a ta wymaga urzadzenia zastepczego.
  Przelaczanie renderera zostaje przez `msdf_enabled` w `lexara112.cfg`.
- **`shared_vtable=1` przywraca urzadzenie zastepcze**, a wraz z nim szarpanie.
  Zostawione wylacznie na wypadek, gdyby przechwyt przez `CreateDevice` gdzies
  zawiodl i trzeba bylo porownac.
