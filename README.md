<p align="center">
  <img src="Useful Information/Vastar.png" width="600">
</p>

# Vastar na Amigę (68060 + RTG) i PC

*Vastar (Orca / Sesame Japan, 1983) for the Amiga with a 68060 and an RTG graphics
card, and for the PC (SDL2): an emulator of the arcade board written in C and 68k
assembler. ROMs are not included. English descriptions of the options:
[for players](port/amiga/Vastar_en.txt), [for programmers](port/amiga/Vastar_dev_en.txt).*

Port automatu **Vastar** (1983) na Amigę z procesorem 68060 i kartą graficzną RTG
(Picasso96) oraz na PC. To **emulator płyty automatu**: oryginalne ROM-y gry wykonują
dwa emulowane procesory Z80, dźwięk liczy emulowany AY-3-8910, a obraz składa renderer
kafelków i sprite'ów. Specyfikacją sprzętu były rdzeń MiSTer z tego repozytorium
(`rtl/`, autor: RodimusFVC) i sterownik MAME (`Useful Information/vastar*.cpp`).

**ROM-y nie są dołączone.** Potrzebny jest zestaw `vastar` zgodny z MAME (`vastar.zip`
albo rozpakowane pliki).

| Platforma | Wynik |
|---|---|
| A1200, Warp 1260, 68060/100 MHz, RTG 16-bit | 60,6 kl./s, każda klatka narysowana, CPU 73% (okno, `SCALE=1`); pełny ekran 640×512 (`SCALE=2`) 60 kl./s przy 83% |
| ta sama A1200 z 68060/50 MHz | gra w pełnym tempie, obraz ok. 15–18 klatek/s (automatyczne pomijanie klatek) |
| PC (Windows, SDL2) | ok. 0,5 ms na klatkę |

## Zawartość

| Katalog | Co |
|---|---|
| `port/src` | emulator: Z80 w C (`z80.c`) i w asemblerze 68k (`z80_68k.s`, generowany przez `tools/gen_z80_68k.py`), AY-3-8910, model automatu, renderer (C + `video_68k.s`), wczytywanie ROM-ów z zipów MAME, nakładki `main_sdl.c` (PC) i `main_amiga.c` (Amiga) |
| `port/tests` | testy: instrukcje Z80 (zexdoc/zexall), lockstep rdzenia asm z rdzeniem C, renderer kontra renderer wzorcowy, profil |
| `port/amiga` | ikona i opis parametrów dla Amigi |
| `releases/amiga` | wydanie na Amigę: rozpakowane `Vastar/` (program, ikona, szuflada `doc` z opisami PL/EN i historią wydań, pusta `rom/` z ReadMe) i to samo jako `Vastar-<wersja>.zip`, bez ROM-ów; robi je `tools/make_release.sh` |
| `releases/pc` | to samo dla PC: `Vastar/` (`vastar.exe`, `SDL2.dll`, `doc/`, pusta `rom/`) i `Vastar-<wersja>-pc.zip`, bez ROM-ów |
| `docs/PORTING.md` | **pełny opis portu**: architektura, pamięć, renderer, rdzeń asm, pomiary, opcje |
| `docs/Warp RTG Developer notes.pdf` | zasady pracy z VRAM na Warpie (zastosowane w porcie) |
| `rtl`, `sys`, `releases/*.rbf`, `*.mra` | oryginalny rdzeń MiSTer (opis: `docs/README_MiSTer.md`) |
| `tools` | generator rdzenia Z80, generator ikony, zdalne sterowanie Amigą (AmiAgent) i narzędzia przeniesione z portu Wizard of Wor |

## Amiga: uruchomienie

1. Weź [`releases/amiga/Vastar-0.1.zip`](releases/amiga) i rozpakuj go na Amidze:
   `UnZip Vastar-0.1.zip -d Work:Games` (powstaje szuflada `Work:Games/Vastar`).
2. Wgraj ROM-y do `Work:Games/Vastar/rom/` (`vastar.zip` z MAME albo luźne pliki).
3. Dwuklik na ikonie. Parametry w Tool Types: wpis w nawiasach jest wyłączony.
   Opisy i historia wydań są w szufladzie `doc`.

PC: rozpakuj [`releases/pc/Vastar-0.1-pc.zip`](releases/pc), ROM-y wgraj do `Vastar\rom\`,
uruchom `vastar.exe`.

Domyślnie: okno na Workbenchu, dźwięk przez Paulę, Free Play (bez monet), joystick
w lewo / w prawo = start 1 / 2 graczy, gracz 2 gra tym samym joystickiem.
Opisy parametrów: dla graczy [`port/amiga/Vastar.txt`](port/amiga/Vastar.txt)
(najważniejsze: `SCALE`, `SCREEN=FULL`, `LIVES`, `COINS`, `JOYSTART=OFF`, ...),
dla programistów [`port/amiga/Vastar_dev.txt`](port/amiga/Vastar_dev.txt)
(wszystkie parametry, tryby obrazu i dźwięku, pomiary `STATS`, testy, wersja PC).
Po angielsku: [`Vastar_en.txt`](port/amiga/Vastar_en.txt) (players),
[`Vastar_dev_en.txt`](port/amiga/Vastar_dev_en.txt) (programmers).

Wymagania: AmigaOS 3.1+, 68060 (budowane dla 68060), Picasso96 zalecane.

## Budowanie

**PC** (MSYS2 UCRT64: gcc, SDL2, make): `cd port && make` → `build/port/vastar.exe`.
Klawisze: strzałki, Ctrl/Z/Spacja ogień, Alt/X tarcza, 5 moneta, 1/2 start, P pauza,
F3 reset, Esc. Opcje: `-freeplay`, `-joystart`, `-lives N|inf`, `-scale N`, testowe
`-headless -frames N -shots DIR F1,F2 -script ... -check-render -wav`.

**Amiga** (krzyżowo na Windows: vbcc 0.9h, NDK 3.2, nagłówki AHI i Picasso96 w
`C:\amiga-dev`, konfiguracja vbcc bez `-Rshort`): `cd port && make -f Makefile.amiga`
→ `build/amiga/Vastar` (+ `Lockstep`, `Z80Test`, `VideoTest`).

## Co jest sprawdzone

| Co | Jak | Wynik |
|---|---|---|
| Rdzeń Z80 w C | zexdoc i zexall na PC | 67/67 i 67/67 |
| Rdzeń Z80 w asemblerze 68k | prelim; **lockstep** z rdzeniem C na kodzie gry (demo, moneta, start, rozgrywka), instrukcja po instrukcji, w WinUAE | prelim OK; 2600 klatek, 25,3 mln instrukcji identycznych (rejestry, cykle, pamięć obu Z80, rejestry AY) |
| Renderer przyrostowy | porównanie z rendererem wzorcowym w każdej klatce: `-check-render` (PC), `VideoTest` (A1200, z asemblerem) | 0 różnic: 5000 klatek PC, 2500 klatek A1200 |
| Obraz w oknie na karcie | `VERIFY`: odczyt okna z VRAM co 30 klatek | 0 złych bloków (okno i pełny ekran, Warp 16-bit) |
| AY-3-8910 (szybka wersja) | porównanie WAV z wersją wcześniejszą | identyczne bit w bit |
| Gra na PC | demo, ekrany tytułowe, rozgrywka ze skryptem wejść, zrzuty | działa |
| Gra na A1200 (Warp, 68060/50 i /100) | uruchomienia z pomiarem `STATS`, zrzuty, start z ikony | działa, wyniki w tabeli wyżej |
| Gra na 2 graczy jednym joystickiem | PC: gra 2-osobowa, wynik 2UP | gracz 2 strzela i zdobywa punkty |
| Free Play, start joystickiem lewo/prawo | PC: start, wciskanie startu w trakcie gry, start 2 graczy | działa, start w trakcie gry ignorowany |
| `LIVES=n` | PC: 9 i 20 żyć, licznik w RAM (`F101`, `F110`) | działa; powyżej 9 ikony się nie mieszczą (limit 9) |
| Pełny ekran `SCREEN=FULL` | A1200, `SCALE=2` | tryb 640×512, obraz wyśrodkowany, 60 kl./s |
| Ikona | A1200: widoczna w szufladzie, start dwuklikiem (ARexx Workbencha) | działa, Tool Types czytane |

## Czego jeszcze nie sprawdzono

- **Fizyczny joystick, pad CD32 i `JOYPORT=BOTH`** na Amidze: zdalnie testowane tylko
  klawiaturą (logika wejść sprawdzona na PC).
- **Pełne zexdoc/zexall z rdzeniem asemblerowym** (trwałyby ok. godziny w WinUAE);
  rdzeń asm sprawdzony lockstepem na kodzie gry i prelim.
- **Uproszczenie rdzenia asm:** po `BIT n,(HL)` nieudokumentowane flagi X/Y = 0 (brak
  rejestru MEMPTR). Gra z tego nie korzysta.
- **`LIVES=INFINITE`**: logika prosta, ale nie grana długo.
- **Inne zestawy** `vastar2`, `vastar3`, `vastar4` i **Planet Probe** (`pprobe`): są
  zdefiniowane (sumy kontrolne z MAME), nie uruchamiane z braku ROM-ów.
- **Odwrócony ekran (koktajl)**: działa przez renderer wzorcowy, nie testowany w grze.
- **Kolory i kolejność sprite'ów** według MAME; nie porównane z prawdziwym automatem
  (zrzut `Vastar.png` ma inne odcienie, ale nie dają się odtworzyć z PROM-ów żadnym
  spójnym przeliczeniem; rdzeń MiSTer rysuje nakładające się sprite'y w odwrotnej
  kolejności niż MAME).
- **Dźwięk** nie porównany z MAME ani z MiSTerem (tylko pomiary i odsłuch).
- **Inne karty i tryby**: ekrany 8- i 32-bit na prawdziwej Amidze, karty inne niż Warp,
  Amiga bez Picasso96 (droga `WriteChunkyPixels`), AGA bez RTG (brak trybu natywnego).
- **AHI**: działa, ale miksuje programowo (ok. 5% 68060/50); domyślna jest Paula.
- **`MOVE16=ON`**: na 68060 trzeba najpierw opróżniać pamięć podręczną danych
  (`CacheClearE`), zysk znikomy, dlatego domyślnie wyłączone.

## Brakuje

- zapisu rekordów (hiscore) na dysk,
- trybu natywnego AGA/OCS bez karty graficznej,
- sprawdzenia Planet Probe.

## Licencje

Kod portu (`port/`, `tools/gen_z80_68k.py`, `tools/make_icon.py`): napisany do tego
projektu. Rdzeń MiSTer (`rtl/`, `sys/`) ma własne licencje w nagłówkach plików
(m.in. jt49: GPL, T80, szablon MiSTer). Gra i ROM-y są własnością ich właścicieli i
nie są tu dołączone.
