# Vastar: port na PC i Amigę (68060 + RTG)

Stan na 2026-10-09. Cel: Vastar (Orca, 1983) i Planet Probe na Amidze z 68060/50 MHz, Picasso96 i AHI (WinUAE lub prawdziwa Amiga). Rdzeń FPGA z tego repozytorium (`rtl/`) i sterownik MAME (`Useful Information/vastar*.cpp`) służą jako specyfikacja sprzętu.

## 1. Stan prac

| Krok | Zakres | Stan |
|---|---|---|
| 1 | Emulator Z80 w C (`port/src/z80.c`) | ✅ zexdoc 67/67, zexall 67/67 |
| 1a | Emulator Z80 w asemblerze 68k (`z80_68k.s`, generowany) | ✅ prelim; lockstep z rdzeniem C: 2600 klatek, 25,3 mln instrukcji identycznych |
| 2 | AY-3-8910 (`ay8910.c`) | ✅ dźwięk generowany; do odsłuchu przez użytkownika |
| 3 | Model automatu: 2× Z80, mapa pamięci, przerwania, zatrzask LS259 (`vastar.c`) | ✅ |
| 4 | Obraz: 3 warstwy kafelków, 16 sprite'ów, priorytety, paleta z PROM (`video.c`) | ✅ zgodny z wzorcem (patrz 5) |
| 5 | ROM-y: zip MAME (własny inflate) lub katalog z plikami (`romload.c`) | ✅ |
| 6 | Wersja na PC (SDL2, `main_sdl.c`) | ✅ tytuł, demo, rozgrywka |
| 7 | Wersja na Amigę (`main_amiga.c`: okno Workbencha, P96, AHI/Paula, joystick) | ✅ A1200 060/50: pełne tempo gry (60 kl./s) z Paulą, obraz ok. 15 kl./s (pomijanie klatek); ⏳ płynność obrazu (patrz 6) |

## 2. Architektura

To **emulator sprzętu**, nie przepisana gra: ROM-y z automatu wykonuje emulowany Z80.

```
 vastar.zip / rom\ ──romload.c──► Vastar (vastar.c)
                                   ├─ Z80 główny 3,072 MHz: 0000-7FFF ROM, 8000/9000 RAM tła,
                                   │     C000 priorytet, C400-CFFF RAM pierwszego planu + sprite'y,
                                   │     F000-F7FF RAM wspólny, porty 0-7 = LS259 (NMI, flip, reset Z80 #2)
                                   ├─ Z80 dźwiękowy 3,072 MHz: 0000-1FFF ROM, 4000-47FF RAM wspólny,
                                   │     8000/8040/8080 wejścia, porty 0-2 = AY-3-8910 (DSW na portach A/B)
                                   └─ AY-3-8910 1,536 MHz (ay8910.c)
                                   ▼
                                 video.c: bufor 256×256 indeksów palety → obrót ROT90 → 224×256
                                   ▼
                       PC: main_sdl.c          Amiga: main_amiga.c
```

Kod wspólny (`z80.c`, `ay8910.c`, `vastar.c`, `video.c`, `romload.c`) jest w C99, bez FPU i bez arytmetyki 64-bit. Kompiluje się vbcc dla 68060.

**Pamięć Z80:** każdy procesor ma płaską tablicę 64 KB (`mmem`, `smem` w `Vastar`), czytaną wprost. Zapisy idą przez typ strony 256 B (`wtype`): RAM, ROM (ignorowany), lustro (zapis także w drugim miejscu: RAM wspólny F000/4000 obu procesorów, odbicia RAM tła 8000↔A000, 9000↔B000) albo funkcja C (C000 priorytet, E000 watchdog). Wejścia (8000/8040/8080 drugiego Z80) wpisywane są do tablicy raz na klatkę.

**Obraz (`video_render_screen`, przyrostowy, bez bitmap warstw):**
1. bajty RAM kafelków porównywane są z kopią (`vscan`, po 4 bajty); zmienione kafelki są zaznaczane,
2. wyznaczane są bloki 8×8 do sprawdzenia: wiersze bloków, którym zmieniło się przewinięcie, bloki pod zmienionymi kafelkami (z uwzględnieniem przewinięcia) oraz poprzednie i obecne prostokąty sprite'ów,
3. każdy taki blok jest składany w 64-bajtowym buforze **wprost z kafelków**: wiersz ekranu w kolumnie kafelków to najwyżej dwa kafelki, a grafika kafelków jest wstępnie obrócona i spakowana po 2 bity na piksel (`ptile`, z wariantem odbitym w pionie, 48 KB), więc wiersz bloku to sklejenie dwóch 16-bitowych słów, przesunięcie o przewinięcie i rozwinięcie przez tablicę (`xpix`, `xmask`); kolory dwóch kafelków łączy maska `split`. Sprite'y rysowane są tylko w blokach, które mają sprite'a,
4. blok jest porównywany z ekranem i zapisywany tylko wtedy, gdy się różni; `v->blk` mówi wersji na Amigę, które bloki wysłać.

Zestaw roboczy jest mały (spakowane kafelki 48 KB, tablice 2 KB, RAM kafelków), zgodnie z „Warp RTG Developer notes” (L2 96 KB). Na Amidze `comp_layer` jest w asemblerze (`video_68k.s`). Przy odwróconym ekranie (koktajl) działa renderer wzorcowy (`video_render` + `video_rotate`). Zgodność: `vastar -check-render` (PC) i `VideoTest` (Amiga) porównują oba renderery w każdej klatce: 0 różnic.

## 3. Budowanie i uruchamianie (PC)

MSYS2 UCRT64 (gcc, SDL2, make):

```
cd port
make                      # ..\build\port\vastar.exe, z80_test.exe
```

```
build\port\vastar.exe                    # vastar z rom\, roms\ lub . (zip, podkatalog albo luźne pliki)
build\port\vastar.exe pprobe -rompath D:\MAME\roms
```

Klawisze: strzałki, lewy Ctrl / Z / spacja = ogień, lewy Alt / X = przycisk 2, 5 = moneta, 1 / 2 = start, P = pauza, F3 = reset, Esc = wyjście. Pad: A, B, Back = moneta, Start.

Opcje testowe: `-headless -frames N -shots KATALOG 100,200 -wav PLIK -script "1200:+coin1,1206:-coin1,..." -stats`.

## 4. Testy

- `z80_test.exe zexdoc.cim` / `zexall.cim`: testy instrukcji Z80 (pliki z repozytorium superzazu/z80, nie są tu dołączone). Wynik: wszystkie OK, ok. 70 s na PC.
- Przebieg automatyczny: `-headless -frames 3000 -script ...` (moneta 1200, start 1260, ruch i strzały) daje zrzuty: test pamięci, tytuł z animacją logo, tekst fabuły, rozgrywkę z przewijaniem i punktami.
- Wydajność na PC: ok. 0,5 ms na klatkę (rdzeń Z80: ok. 680 MHz emulowanego zegara).

## 5. Znane różnice i otwarte sprawy

- **Kolejność nakładania sprite'ów:** według MAME (późniejszy na wierzchu). RTL z MiSTera rysuje odwrotnie (pierwszy wygrywa). Do sprawdzenia na grze, jeśli sprite'y się nakładają.
- **Kolory:** liczone jak w MAME (`RGB_444_PROMS`, wartość × 17). `Useful Information/Vastar.png` ma inne odcienie (np. różowawe logo), ale jego wartości nie dają się odtworzyć z PROM-ów żadnym spójnym przeliczeniem, więc to raczej obróbka zrzutu. Warto porównać z MiSTerem lub MAME.
- **Dźwięk:** demo jest ciche (dźwięk pojawia się po wrzuceniu monety); odsłuch do potwierdzenia.
- **Rekordy (hiscore)** nie są jeszcze zapisywane.

## 6. Amiga

**Budowanie:** `make -f Makefile.amiga` w `port/` (MSYS2, vbcc + NDK 3.2 + AHI + P96 w `C:miga-dev`, jak dla Wizard of Wor) daje w `build/amiga/`:
- `Vastar`: gra (68060, rdzeń Z80 w asemblerze),
- `Lockstep`: test rdzenia asm przeciw rdzeniowi C,
- `Z80Test`: prelim/zexdoc/zexall z rdzeniem asm.

`src/z80_68k.s` generuje `python tools/gen_z80_68k.py` (Makefile robi to sam, gdy generator się zmieni).

**Uruchamianie:** `Vastar [SET] [SCALE=1|2|3] [ROMPATH=dir] [NOSOUND] [SOUND=AHI|PAULA] [VIDEO=BLIT|WPA|WCP] [DSW1=hh] [DSW2=hh] [STATS|STATS=FULL]`, to samo jako Tool Types ikony. ROM-y: `SET.zip`, szuflada `SET` albo luźne pliki w `ROMPATH`, `PROGDIR:`, `PROGDIR:rom`, `PROGDIR:roms`. Sterowanie jak na PC (strzałki, Ctrl/Spacja/Z ogień, Alt/X przycisk 2, 5 moneta, 1/2 start, P pauza, F3 reset, Esc) plus joystick w porcie 2 (CD32: niebieski = przycisk 2, Play = start).

**`main_amiga.c`** (na wzór Wizard of Wor): okno Workbencha, obraz do okna tylko w zmienionych blokach 8×8 (P96 `BLIT`: zapis zmienionych bloków do bitmapy na karcie i jeden blit prostokąta wokół nich; `WPA`; bez P96 `WriteChunkyPixels`), AHI przez `AHI_AllocAudio` z dwoma buforami (Paula jako zapas), 22 050 Hz, tempo z `timer.device` (60,606 Hz), joystick czytany raz na klatkę, strażnik zawieszeń (`RAM:Vastar-hang.txt`).

**Wysyłanie do karty (tryb `BLIT`, według „Warp RTG Developer notes”):** zmienione bloki wiersza bloków są przeliczane na format ekranu (`convert16` w asemblerze dla 16 bitów) do małego bufora w Fast RAM (jeden wiersz bloków: mieści się w pamięciach podręcznych; bufor całej klatki był wolniejszy, bo nie mieścił się w L2) i kopiowane do bitmapy na karcie wyłącznie zapisami (`copy_rows`: długie słowa; `MOVE16=ON` używa MOVE16, ale wtedy trzeba najpierw opróżnić pamięć podręczną danych, `CacheClearE`, bo MOVE16 czyta pamięć z pominięciem pamięci podręcznej w trybie copyback: bez tego robot zostawiał smugi). Potem jeden blit prostokąta wokół zmian (na 16 bitach robi go sprzęt Warpa). `VERIFY` co 30 klatek odczytuje okno z karty i porównuje każdy blok: 0 złych bloków.

**Pomijanie klatek (`FRAMESKIP=AUTO`, domyślnie):** emulacja (logika gry i dźwięk) zawsze liczy 60,606 klatki na sekundę; gdy poprzednia klatka skończyła się za późno, obraz tej klatki nie jest składany ani wysyłany (najwyżej 3 pod rząd). `FRAMESKIP=n` rysuje stale co (n+1). klatkę. `STATS` pokazuje tempo gry i liczbę narysowanych obrazów na sekundę.

**Rdzeń Z80 w asemblerze (`z80_68k.s`):** HL, A, F, R i licznik cykli w rejestrach 68k (d3–d7), PC jako wskaźnik do pamięci (a4), pozostałe rejestry w strukturze `Z80` (wielki endian: pary BC/DE czytane jednym `move.w`). Każda instrukcja kończy się własnym `jmp` przez tablicę 256 adresów (osobne tablice CB, ED, DD, FD, DDCB/FDCB). Flagi S Z V C z rejestru CCR 68k po odpowiedniej instrukcji 68k (`move ccr` + tablica 16 wpisów), H oraz X/Y liczone. Przerwania sprawdzane tylko na granicach przebiegu (linie przerwań zmieniają się tylko między wywołaniami `z80_run`) i po EI/RETN, więc pętla instrukcji nie ma żadnych testów poza licznikiem cykli. Uproszczenie: brak rejestru WZ (MEMPTR), więc `BIT n,(HL)` daje X/Y = 0.

**Test lockstep:** dwa automaty, jeden z rdzeniem asm, drugi z C, wykonują grę (demo, moneta, start, rozgrywka) instrukcja po instrukcji; po każdej instrukcji porównywane są wszystkie rejestry i liczba cykli, po każdej klatce cała pamięć obu procesorów i rejestry AY.

**Wydajność** (`STATS=FULL`, % czasu; „emu” = oba Z80 + AY, „video” = renderer, „present” = okno; demo w 32.–44. sekundzie):

| Wersja | WinUAE 060 | A1200 060/50 (ekran 16-bit, Warp) |
|---|---|---|
| rdzeń C, pełny renderer co klatkę | 18,2 kl./s | – |
| rdzeń C, renderer z pamięcią warstw | 21,8 kl./s | 11–15 kl./s |
| rdzeń asm | 35,9 kl./s | 19–26 kl./s (`SCALE=2`) |
| + renderer przyrostowy, AY bez dzieleń, pętle obrazu w asm | 33–36 kl./s | 29 kl./s (`SCALE=1`; emu 13 ms, z tego AY 6 ms) |
| + AY generator po generatorze, 1 blit, szybsze sprite'y, `vscan` | – | 32 kl./s bez pomijania |
| + pomijanie klatek (`SCALE=1`) | – | AHI: 58–59 kl./s gry, 14 obrazów/s; **Paula: 60 kl./s gry, 15 obrazów/s**; bez dźwięku: 60 kl./s, 20 obrazów/s |

Po „Warp RTG Developer notes” (A1200, Paula; bez pomijania / z pomijaniem):

| Wersja | `SCALE=1` | `SCALE=2` |
|---|---|---|
| bitmapy warstw, porównanie w C, zapis do VRAM w C | 38–40 / 60 (15 obrazów/s) | 26–27 / 37–41 |
| + przeliczanie i kopiowanie w asm przez mały bufor, porównanie w asm | 39,5 / 61 (15) | 32 / 50 (12–13) |
| renderer bez bitmap warstw (mieści się w L2), porównanie w rendererze | 36,7 / 60,7 (15) | 31,3 / 48,5 (13) |

Renderer mieszczący się w L2 nie przyspieszył: teraz ogranicza go liczba instrukcji (ok. 500 bloków na klatkę, gdy przewija się tło, po 3 warstwy z obliczeniem kafelków dla każdego bloku), a nie pamięć. Zysk to ok. 250 KB mniej pamięci (bez bitmap warstw i bez kopii ekranu do porównań).

**Dalsze możliwości:** AY w asemblerze, składanie trzech warstw w jednym przejściu na blok, mniej bloków przy zmianie przewinięcia (przesunięcie zamiast składania), pomijanie pętli oczekiwania głównego Z80 (ok. 12% jego czasu).
