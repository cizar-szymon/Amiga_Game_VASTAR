# Wizard of Wor (C64): dokumentacja portowania

Stan na 2026-10-09. Opis tego, jak gra z C64 (cartridge 16 KB, 1983, Jeff Bruette) została przeniesiona do C. Wersja na PC (Windows, SDL2) jest grywalna, z grafiką i dźwiękiem. Wersja na Amigę jest kolejnym krokiem.

Szczegółowa mapa wszystkich procedur asemblera i dziennik postępu: [`AMIGA_PORT_MAP.md`](../AMIGA_PORT_MAP.md).

---

## 1. Stan prac

| Krok | Zakres | Stan |
|---|---|---|
| 0 | Kompilacja disassembly (64tass), obraz `.crt`, uruchomienie w VICE | ✅ |
| 1 | Eksport grafiki i danych gry do C (narzędzia w Pythonie) | ✅ zgodne z RAM w VICE bajt w bajt |
| 2 | Model maszyny C64 + renderer VIC-II na PC | ✅ obraz zgodny z VICE piksel w piksel |
| 3 | Ekrany: tytuł, plansza, teksty, wyniki, „GET READY”, „GAME OVER” | ✅ |
| 4 | Rozgrywka: pętla, ruch, kolizje, pociski, trafienia, potwory | ✅ |
| 5 | Tempo gry (synchronizacja z ramkami) | ✅ skalibrowane na loch 1 |
| 6 | Rundy Worluka i Wizarda, rekordy, pauza, easter egg | ✅ przeniesione; brak migawek z VICE dla Worluka/Wizarda |
| 8 | Dźwięk: muzyka, melodyjki, 12 efektów (model SID) | ✅ |
| 7 | **Backend Amigi** (bitplany, sprite'y/BOB-y, Paula, joysticki) | ⏳ następny |
| – | Mowa Magic Voice | ✗ pominięta |

**Testy:** 129 testów różnicowych z VICE, 0 błędów (sekcja 7).

### Czy gra jest przepisana na C?

**Tak, cała logika gry jest w C.** To nie emulator 6502: każda procedura asemblera (ok. 3100 instrukcji 6502) ma swój odpowiednik w C, przepisany instrukcja po instrukcji z numerem linii oryginału w komentarzu. Po stronie C64 zostały tylko **dane**: obraz ROM cartridge'a (tablice, teksty, układy lochów, dane sprite'ów), dostępny w C jako tablica.

Ok. 3700 linii C:

| Plik | Linie | Zawartość |
|---|---|---|
| `port/src/gameplay.c` | 1496 | pętle rozgrywki, ruch, kolizje, pociski, trafienia, potwory, radar, Worluk, Wizard, rekordy |
| `port/src/screens.c` | 1140 | ekrany, plansza, teksty, wyniki, życia, duże litery |
| `port/src/irq.c` | 365 | przerwanie co ramkę, melodyjki, efekty dźwiękowe |
| `port/src/sid.c` | 247 | model układu dźwiękowego SID |
| `port/src/main_flow.c` | 215 | inicjalizacja, ekrany tytułowe, start lochu |
| `port/src/render.c` | 96 | renderer VIC-II (tylko PC) |
| `port/src/main_sdl.c` | 276 | okno, klawiatura, dźwięk (tylko PC) |
| `port/src/c64.c/.h`, `hal.h`, `game.h`, `sid.h`, `render.h` | ~330 | model maszyny, interfejsy |

Część przenośna (logika + SID) nie zależy od platformy. Platformę obsługuje tylko HAL: `main_sdl.c` + `render.c` na PC, a w przyszłości backend Amigi.

---

## 2. Architektura portu

```
 wizard_of_wor.asm ──64tass──► wow.bin (ROM 16 KB) + wow.symbols + wow.lst
        │                              │
        │ tłumaczenie 1:1               │ tools/export_data.py, export_gfx.py
        ▼                              ▼
 port/src/*.c  ◄──────────── build/data/wow_rom.c, wow_symbols.h, build/gfx/gfx_data.c
 (logika gry)
        │  czyta/pisze
        ▼
 mem[65536]  ── model przestrzeni adresowej C64:
   $0000-$03FF zmienne gry      $0400 ekran (mapa znaków = mapa kolizji)
   $0800 zestaw znaków          $2000 sprite'y          $8000 ROM gry
   $D000 rejestry VIC (wirtualny VIC)   $D400 rejestry SID   $D800 color RAM
        │
        ▼
 HAL (hal.h): hal_wait_frame, hal_joystick, hal_keyboard_rows, hal_audio_push
        │
  ┌─────┴───────────────┐
  PC: main_sdl.c          Amiga (krok 7): copper, bitplany,
      + render.c (VIC)    BOB-y lub sprite'y, Paula, joysticki
      + SDL2 audio
```

### 2.1 Konwencje tłumaczenia

- **Pamięć:** jedna tablica `mem[]`. Makro `M(adres)` to odczyt i zapis, `PTR(zp)` to 16-bitowy wskaźnik w strefie zerowej, `IND_Y(zp)` to tryb `(zp),Y`. Dzięki temu wskaźniki zapisane w strefie zerowej działają jak w oryginale, nawet gdy wskazują ekran, color RAM czy rejestry VIC.
- **Nazwy:** adresy z disassembly mają prefiks `A_` (np. `A_actual_actor`, `A_draw_dungeon__block_offsets`), generowany w `wow_symbols.h`. Funkcje mają nazwy procedur asemblera (`big_letters.print_word` → `big_letters_print_word`).
- **Rejestry 6502:** globalne `rA`, `rX`, `rY`, bo oryginał przekazuje nimi parametry i wyniki.
- **Skoki między pętlami:** oryginał przeskakuje przez `JMP` i `JMP (gameplay_ptr)` między pętlą zwykłą, rundą Worluka, rundą Wizarda, nowym lochem i ekranem rekordów. W C jest to automat stanów (`gs_t` w `game.h`, `game_run()` w `gameplay.c`): każdy krok zwraca stan, w którym gra ma kontynuować.
- **Błędy oryginału** (opisane w komentarzach disassembly jako `BUG`) są zachowane, żeby gra zachowywała się identycznie.

### 2.2 Sprzęt C64 → model w porcie

| C64 | Jak gra go używa | Port |
|---|---|---|
| VIC-II, rejestry sprite'ów `$D000-$D010` | **pozycje aktorów** (gra czyta je jako zmienne) | zwykła pamięć w `mem[]` |
| `$D015` (włączenie sprite'ów) | niewidzialność garworów i thorworów | zwykła pamięć |
| `$D027+` (kolory sprite'ów) | rozpoznawanie burworów po kolorze | zwykła pamięć |
| `$07F8+` (wskaźniki sprite'ów) | klatka animacji, wybuch | zwykła pamięć |
| Pamięć ekranu `$0400` | **mapa kolizji**: ściany, pociski, ukryte znaczniki potworów | zwykła pamięć |
| Color RAM `$D800` | radar (rysowany samymi kolorami) | zwykła pamięć |
| `$D012` (raster) | źródło losowości | wyliczany z licznika czasu ramki (`raster_line()`) |
| `$D011` (czekanie na ramkę) | odliczanie sekund | `wait_frame()` |
| Przerwanie rastra | timery, drzwi, niewidzialność, dźwięk | `irq_handler()` raz na ramkę |
| CIA1 `$DC00/$DC01` | joysticki, klawiatura (pauza, easter egg) | `cia1_read()` → `hal_joystick()`, `hal_keyboard_rows()` |
| SID `$D400` | muzyka i efekty; gra **odczytuje** też `$D41B/$D41C` | model SID (`sid.c`), zapisy przez `sid_write()` |
| Magic Voice | mowa | pominięty (zachowuje się jak brak cartridge'a) |

---

## 3. Mechanika gry i gdzie jest w kodzie

### 3.1 Przebieg

`game_run()`: start → ekrany tytułowe (rekordy ↔ tabela wrogów, `title_screens`) → `start_game` → `start_dungeon` (wybór układu, „GET READY … GO”, „DOUBLE SCORE”, bonusowe życie, rysowanie planszy) → pętla rozgrywki → (po wybiciu potworów) runda Worluka → (losowo) runda Wizarda → następny loch. Po game over: zapis rekordu i powrót do tytułu.

### 3.2 Pętla rozgrywki (`normal_gameplay_loop`)

Każdy krok obsługuje **jednego** z 8 aktorów (0 = gracz 2, 1 = gracz 1, 2–7 = potwory):

1. klawiatura (pauza, easter egg), co 8. krok radar, ruch pocisków, sprawdzenie końca gry,
2. aktor martwy → nic; umierający → animacja wybuchu (`actor_is_dying`) i ewentualnie nowy potwór (`monster_is_dead`),
3. sprawdzenie trafienia (`check_hitbox_of_current_actor`),
4. potwór: co `speed+1` kroków ruch; 50% szans na strzał, jeśli jest w rzędzie lub kolumnie „swojego” gracza (nieparzyste potwory celują w gracza 1, parzyste w gracza 2); co jakiś czas skręt w stronę losowego gracza,
5. gracz: ruch, gdy licznik zmniejszany w przerwaniu dojdzie do zera; joystick, strzał, wyjście z klatki,
6. drzwi teleportacyjne, koniec niewidzialności, ponowne sprawdzenie trafienia.

### 3.3 Kolizje: ekran jako mapa

Nie ma osobnej mapy kolizji. Gra sprawdza **znaki na ekranie**:
- `FLOOR` ($0F) = wolne pole, inne kody znaków ścian = blokada (`check_actor_collision`: 5 punktów wokół sprite'a),
- pociski to znaki ($40/$41 gracza, $42/$43 potworów), przesuwane co 2. (gracze) lub co 4. (potwory) ramkę (`move_bullets`),
- każdy potwór zostawia niewidoczny znak „środka” (`MONSTER_CENTER`) i „głowy” przed sobą (`MONSTER_HEAD`). Gracz ginie od dotknięcia głowy potwora, a pocisk trafia w środek,
- trafienie: obszar 3×3 znaki wokół środka sprite'a, właściciela pocisku znajduje się po jego pozycji (`check_hitbox_of_current_actor`).

### 3.4 Potwory

- Burwor (niebieski, 100 pkt), garwor (żółty, 200), thorwor (czerwony, 500), Worluk (1000), Wizard of Wor (2500). Punkty z gracza: 1000.
- Po zabiciu burwora pojawia się garwor, jeśli numer lochu ≥ liczby pozostałych burworów. Po garworze zawsze pojawia się thorwor. Thorworów nie zastępuje nic (`monster_is_dead`).
- Niewidzialność: przerwanie co 10 s wyłącza sprite garworów i thorworów i przyspiesza je. Widać je znowu w rzędzie lub kolumnie gracza (`check_if_monster_can_be_invisible`).
- Pozycje startowe: stałe 8 × 8 punktów, z dala od rzędów i kolumn graczy (`spawn_monster`).

### 3.5 Rundy specjalne

- **Worluk** (`enter_worluk`, `worluk_gameplay_loop`): pojawia się po wybiciu potworów (od lochu 2), ściany migają na czerwono i czarno, radar jest wyłączony, Worluk kieruje się do drzwi. Zabity: podwójne punkty w następnym lochu i 1/4 szansy na Wizarda. Uciekł: 1/8 szansy.
- **Wizard** (`enter_wizard_of_wor`, `wizard_gameplay_loop`): teleportuje się co 160 ramek, jego pociski lecą tak szybko jak pociski graczy, drzwi są stale otwarte. Kończy się po trafieniu Wizarda albo gracza.

### 3.6 Pozostałe

- Radar: kolory w color RAM pod znakami bloków (`update_radar`). Migotanie radaru w oryginale wynika z braku synchronizacji z obrazem.
- Drzwi teleportacyjne: otwierane przez przerwanie po 10 s (4 s po pojawieniu się Worluka), każde otwarcie przyspiesza bas w grze.
- Lochy: 25 układów (6×6 bloków 3×3 znaki, lustrzanych do 11×6). Loch 4 to „Arena”, 13, 19, 25… to „Pit”. Dodatkowe życie dostaje się w Arenie i w pierwszym Picie.
- Pauza: Run/Stop. Easter egg: Run/Stop + C= + Ctrl (albo L.Shift + S + A) pokazuje na ekranie tytułowym „authored by jeff bruette / dedicated to mom and dad”.

---

## 4. Tempo gry

**Problem:** pętla rozgrywki oryginału nie czeka na ramkę obrazu. Potwory ruszają się co określoną liczbę *przebiegów pętli*, więc ich prędkość zależy od szybkości procesora 6502 (1 MHz). Szybszy procesor bez kontroli tempa dałby kilka razy szybsze potwory. Gracze, pociski i timery liczą natomiast ramki w przerwaniu.

**Rozwiązanie:** każdy dostęp do pamięci przez `M()` zwiększa licznik. Gdy licznik przekroczy budżet ramki (`frame_budget = 2840`), port czeka na następną ramkę i wykonuje przerwanie (`frame_sync()` w `c64.c`). Krótkie pętle czekające z oryginału doliczają swój czas przez `add_cycles()`.

**Kalibracja:** pomiar w VICE (`tools/measure_loop_timing.py`, breakpoint na pętli i zegar emulatora) dał średnio 12,1 przebiegu pętli na ramkę (11,1–13,8) w lochu 1 z 6 burworami. Port przy budżecie 2840 wykonuje 12,2 (`wowport --fast --stats --fire-at 450`).

**Uwagi:** gra zakłada NTSC 60 Hz. Na Amidze PAL (50 Hz) trzeba wykonywać 6 „ramek” logiki na 5 ramek obrazu albo przeskalować timery. Kalibracja w dalszych lochach jest jeszcze do zrobienia.

---

## 5. Grafika

### 5.1 Ekran

- Tryb znakowy 40×25, multicolor. Kolor znaku w color RAM: bit 3 = multicolor (pary bitów: tło `$D021`, `$D022`, `$D023`, kolor znaku), bez bitu 3 = hires.
- Ściany to znaki multicolor w kolorach wspólnych `$D022/$D023`. Dlatego migotanie ścian w rundzie Worluka i czarno-biały szum po zabiciu Wizarda to tylko zmiana dwóch rejestrów, a na Amidze zmiana dwóch wpisów palety.
- **Zestaw znaków** (`$0800`): gra składa go przy starcie z 3 źródeł (własne znaki, znaki „spakowane” 5/8 linii, litery i cyfry z ROM-u znaków C64). Eksport odtwarza to dokładnie (`tools/export_gfx.py`). Znaki `$9D–$FF` nie są inicjalizowane i nieużywane.
- **Duże litery** („GET READY”, „GAME OVER”, „DOUBLE SCORE”) to bloki 4×5 znaków.
- **Napisy 2×1** (wyniki, teksty): każda litera to 2 znaki w pionie.

### 5.2 Sprite'y

- 36 sprite'ów w źródłach (`inc.sprites_*.asm`), 24×18 pikseli zapisanych (3 ostatnie linie puste), **jednokolorowe (hires)**, faktycznie do **18 pikseli szerokości**.
- Przy starcie gra tworzy z nich 86 klatek (sprite'y `$80–$D5`, `$D6` = pusty): 40 kopii, 18 odbić pionowych, 28 poziomych (`create_sprites`). Kilka „klatek” to bajty kodu skopiowane razem z danymi (nieużywane).
- Animacja: 3 klatki na kierunek (0-1-2-1), tablice `horizontal/vertical_animation_ptr_tbl` według typu aktora. Kolor według typu (`type_color_tbl`).
- Gra **nie używa** sprzętowych kolizji sprite'ów ani powiększania. Jedynie w tabeli wrogów Worluk i Wizard są multicolor.
- Eksport (`build/gfx/gfx_data.c`): `sprite_frames[87][21]` (jedna linia = `uint32_t`, bit 31 = lewy piksel, gotowe pod blitter), `c64_sprites_ram` (układ RAM C64), podgląd `sprites.png`.

### 5.3 Renderer na PC

`render.c` rysuje całą ramkę z `mem[]`: tryb znakowy (hires/multicolor, przesuw), sprite'y (hires/multicolor, powiększenie, priorytet, kolejność), ramkę, wygaszanie. Obraz 384×247 jak w VICE NTSC. Zgodność z VICE: ekran tytułowy i tabela wrogów piksel w piksel (`tools/compare_render.py`, paleta `colodore.vpl`).

### 5.4 Grafika na Amidze: sprite'y sprzętowe czy BOB-y?

Gra ma do **8 aktorów jednocześnie** (2 graczy + 6 potworów), często w tych samych liniach ekranu. Każdy aktor ma 1 kolor i do **18 pikseli** szerokości.

| Rozwiązanie | OCS/ECS (A500, A600) | AGA (A1200, A4000) |
|---|---|---|
| **Sprite'y sprzętowe** | 8 sprite'ów po **16 px**: postać 18 px nie mieści się w jednym. Dwa sprite'y na aktora dają 16, a jest tylko 8. Multipleksowanie nie pomoże, bo aktorzy stoją w tych samych liniach. Przycięcie do 16 px zmieniłoby grafikę. **Nie nadaje się.** | 8 sprite'ów o szerokości **32 px**: **1 aktor = 1 sprite**, zero pracy CPU i blittera, bez odświeżania tła. **Najlepsze rozwiązanie.** |
| **BOB-y (blitter)** | Działa: 8 obiektów 32×21, każdy jednokolorowy (maska + kolor przez minterm na bitplan), podwójne buforowanie, odtwarzanie tła. Koszt blittera ok. 8 × (zapis tła + wycięcie) na ramkę: do przyjęcia. **Rozwiązanie dla OCS.** | Działa, ale niepotrzebne. |

**Rekomendacja:** jeśli docelowa jest Amiga 1200 (AGA), **zostawić postacie jako sprite'y sprzętowe** (32 px), najprostsze i najwierniejsze. Jeśli docelowa jest A500 (OCS), **BOB-y**. Można przygotować oba warianty za tym samym interfejsem HAL, bo logika gry ustawia tylko pozycję, klatkę, kolor i widoczność aktora (`$D000–$D010`, `$07F8`, `$D027`, `$D015`). Backend czyta te wartości raz na ramkę.

Tło (znaki) na Amidze: bitplany 320×200, przerysowanie tylko zmienionych znaków (mapa brudnych komórek), 4 bitplany = 16 kolorów C64. Kolory dynamiczne (`$D020–$D023`) to wpisy palety zmieniane przez copper.

**Wydajność (szacunek do sprawdzenia w WinUAE):** logika wykonuje ok. 2800 dostępów do `mem[]` na ramkę. W C na 68000 to szacunkowo 100–170 tys. cykli na ramkę, a A500 (7 MHz) ma ok. 118 tys. przy 60 Hz i 142 tys. przy 50 Hz. **Na A500 może być za mało czasu** bez optymalizacji (zmienne gry w rejestrach, wyłączenie licznika dostępów na rzecz stałych kosztów, wstawki w asemblerze). **A1200 (68020, 14 MHz) ma zapas.**

---

## 6. Dźwięk

### 6.1 Jak gra używa SID-a

| Głos | Użycie |
|---|---|
| 1 | bas w grze (3 nuty grane „od tyłu” 2-1-3, prostokąt, tempo zależne od `music_speed`, przyspiesza przy otwarciu drzwi); 1. głos melodyjek |
| 2 | efekty dwukanałowe; 2. głos melodyjek |
| 3 | efekty (wszystkie); 3. głos melodyjek; **jego obwiednia i oscylator są odczytywane** |

- **Melodyjki** (`jingles.play`): „get ready”, „double score”, „game over” na 3 głosach z vibrato.
- **Efekty** (`sfx.play` + `play_sfx`): 20 opisów po 20 lub 27 bajtów. Bajt 0 = czas, przełącznik efektu, przesunięcie modulacji częstotliwości, liczba kanałów. Potem początkowe wartości rejestrów SID, drugi kształt fali głosu 3 i numer następnego efektu (łańcuchy). 12 efektów gry (`SFX_*`) z priorytetami.
- **Logika czasu efektów:** gdy obwiednia głosu 3 (`$D41C`) spadnie do zera, efekt startuje ponownie z drugim kształtem fali i zmniejsza licznik powtórzeń. Częstotliwość jest modulowana wartością oscylatora 3 (`$D41B`) i obwiedni, a filtr jest ustawiany z częstotliwości.

### 6.2 Model SID w porcie (`sid.c`)

Odtwarzacz efektów odczytuje wyjścia SID-a, więc sample nagrane z VICE nie wystarczą do wiernego odtworzenia logiki. Port ma model SID:
- 3 oscylatory 24-bit: trójkąt, piła, prostokąt, szum (LFSR), kombinacje fal (AND), synchronizacja, modulacja kołowa, bit test,
- ADSR z tabelą czasów SID-a i wykładniczym opadaniem,
- filtr stanowo-zmienny (LP/BP/HP, rezonans, routing), wyłączenie głosu 3, głośność,
- `sid_write()` łapie przełączenia bramki (gra wyłącza i włącza głos w tym samym przerwaniu, żeby zacząć nową nutę), a `sid_update_readback()` wpisuje wyjścia głosu 3 do `$D41B/$D41C` przed każdym przerwaniem.

Na PC dźwięk jest generowany po przerwaniu każdej ramki (735–737 próbek, 44,1 kHz) i podawany do SDL2. `wowport --wav PLIK` zapisuje go do pliku.

**Zgodność z VICE** (nagranie tego samego startu gry): te same wysokości dźwięków melodyjki (291–355 Hz) i jej długość, ten sam rytm basu (0,47 s), poziom wyrównany. Barwa filtrowanych efektów jest przybliżona, bo analogowy filtr 6581 jest trudny do odwzorowania.

### 6.3 Dźwięk na Amidze (plan)

`sid_voice_state()` podaje co ramkę stan każdego głosu: częstotliwość, kształt fali, wypełnienie, obwiednię. Backend Pauli:
- kanał 0–2 = głos SID 1–3: zapętlony jednookresowy przebieg (prostokąt z wypełnieniem, piła, trójkąt) albo bufor szumu,
- okres Pauli = `zegar_Pauli / (f_SID × długość_przebiegu)`, gdzie f_SID = rejestr × 1 022 727 / 2²⁴,
- głośność (0–64) = obwiednia × głośność główna,
- bez filtra; kanał 3 wolny (np. na mowę).

Model SID (ADSR, oscylator 3) liczy się dalej w C, bo jest potrzebny logice efektów. Kosztowna synteza próbek odpada, robi ją Paula.

---

## 7. Narzędzia i testy

Wszystko buduje `build.bat` (64tass, cartconv i eksporty). Port buduje `make` w `port/` (MSYS2 UCRT64: gcc, SDL2).

| Narzędzie | Rola |
|---|---|
| `tools/export_gfx.py` | odtwarza `init_cset` / `create_sprites` z obrazu ROM → `build/gfx/` (C, PNG) |
| `tools/verify_gfx_vice.py` | porównuje eksport z RAM w VICE |
| `tools/export_data.py` | obraz ROM jako C + `wow_symbols.h` (757 symboli) + `data_map.md` |
| `tools/vice_monitor.py` | klient zdalnego monitora VICE |
| `tools/measure_loop_timing.py` | pomiar tempa pętli w VICE |
| `tools/capture_snapshots.py` | migawki pamięci przy wejściu i wyjściu z procedury w VICE (`--skip`, `--when-changed`, `--event`) |
| `port/tests/proc_test.c` (`make test`) | uruchamia procedury C od stanu wejściowego i porównuje ze stanem wyjściowym |
| `tools/compare_render.py` | porównuje renderer z zrzutami ekranu VICE |

**Testy różnicowe:** dla każdej procedury VICE zapisuje pełną pamięć 64 KB i rejestry przy wejściu i przy powrocie. Wersja C startuje od stanu wejściowego i musi dojść do stanu wyjściowego. Porównanie pomija tylko to, co zmienia równolegle przerwanie (timery, licznik losowy) oraz rejestry, których nie da się odczytać. Dla procedur dźwiękowych porównywane są też rejestry SID. Rzadkie zdarzenia (np. trafienie gracza) zbiera tryb `--event`: pierwszy przebieg VICE znajduje chwilę zdarzenia, drugi, identyczny (VICE jest deterministyczny), zaczyna zapis tuż przed nią. **Wynik: 129 migawek, 0 błędów.**

**Uruchamianie portu:** `build\port\wowport.exe`. Sterowanie: strzałki + spacja = joystick 2, WASD + lewy Ctrl = joystick 1, P = pauza (wznowienie: Enter, 1 lub 2), P + lewy Alt + prawy Ctrl = easter egg, Esc = wyjście. Opcje testowe: `--fire-at N`, `--demo`, `--fast`, `--stats`, `--budget N`, `--frames N`, `--shots DIR LISTA`, `--wav PLIK`, `--render ZRZUT OUT.bmp`.

---

## 8. Znane różnice i ograniczenia

- **Brak migawek VICE dla:** rund Worluka i Wizarda, zabicia potwora przez gracza (VICE nie przyjmuje joysticka przez monitor; ścieżka sprawdzona w porcie trybem `--demo`), gałęzi efektów zależnych od niezerowych `$D41B/$D41C` (monitor VICE pokazuje 0).
- **Rejestr A** po powrocie z części procedur różni się od oryginału. Każde miejsce wywołania od razu ładuje A na nowo, więc nie ma to wpływu na grę.
- **Losowość** zależy od rastra, więc przebieg gry różni się od VICE w szczegółach (jak w oryginale przy innym momencie startu).
- **Migotanie radaru** nie występuje, bo renderer rysuje całą ramkę naraz.
- **Filtr SID** jest przybliżony.
- **Magic Voice:** brak mowy.
- **Easter egg na PC:** P + lewy Alt + prawy Ctrl (kombinacja L.Shift + S + A koliduje z joystickiem na WASD).

---

## 8a. Wersja na Amigę: okno na Workbenchu

**Docelowa konfiguracja (decyzja z 2026-10-09):** Amiga 1200, 68060/50 MHz, 250 MB Fast RAM, karta RTG, AmigaOS 3.2, AHI. Gra działa **w oknie na Workbenchu**, zgodnie z systemem, bez przejmowania sprzętu. Sekcja 5.4 (sprite'y sprzętowe vs BOB-y) dotyczy tylko wariantu przejmującego sprzęt. Okno Workbencha używa tego samego renderera co PC.

| Element | Rozwiązanie (`port/src/main_amiga.c`) |
|---|---|
| Okno | Intuition, `OpenWindowTags` na ekranie Workbencha, 384×247 × skala 1–3 (domyślnie 2) |
| Grafika | `render.c` (wspólny z PC) rysuje numery kolorów C64; 16 piór z `ObtainBestPen`; do okna trafiają tylko zmienione wiersze, przez `WriteChunkyPixels` (graphics.library V40, na RTG obsługiwane przez Picasso96) |
| Tempo | `timer.device`, zegar E (`ReadEClock`), 59,826 Hz |
| Dźwięk | `ahi.device` (AHI 4+), 16 bit mono, 22 050 Hz, 4 bufory połączone `ahir_Link`; bez AHI gra działa bez dźwięku |
| Joystick | `lowlevel.library` `ReadJoyPort(1)` → joystick 1 z C64 (gracz 1, start gry jednoosobowej) |
| Klawiatura | IDCMP RAWKEY: strzałki + spacja = joystick 2, WASD + Ctrl = joystick 1, P = pauza, Return/1/2 = wznowienie, P + lewy Alt + prawy Alt = easter egg, Esc = wyjście |
| Opcje (Shell) | `WizardOfWor SCALE=1\|2\|3 NOSOUND` |

Kod wspólny jest przenośny: model SID i licznik czasu działają na liczbach całkowitych (bez FPU, bez arytmetyki 64-bit), więc gra pójdzie też na 68LC060 i 68020.

**Kompilacja (Windows):** vbcc 0.9h + NDK 3.2 R4 + AHI dev 4.18 w `C:\amiga-dev\` (konfiguracja vbcc `aos68k_win` ze ścieżkami bezwzględnymi), potem `make -f Makefile.amiga` w `port/` (MSYS2) → `build/amiga/WizardOfWor` (ok. 100 KB, plik wykonywalny AmigaOS).

**Stan:** ✅ działa w WinUAE na AmigaOS 3.2 z kartą RTG uaegfx: ekran tytułowy, „GET READY … GO” i rozgrywka w oknie Workbencha (sprawdzone zrzutami ekranu emulatora). Dźwięk przez AHI do odsłuchania przez użytkownika.

**Wydajność** (WinUAE: 68060 + MMU bez JIT, rozgrywka, opcja `STATS` pokazuje pomiar w tytule okna):

| Wersja | kl./s | CPU | grafika | dźwięk | logika |
|---|---|---|---|---|---|
| pierwsza (pełny renderer co klatkę) | 13,6 | 95% | 85% | – | – |
| renderer przyrostowy | 59,7 | 82–86% | 22–28% | 39–50% | 10–12% |
| + optymalizacje modelu SID, `-O3 -speed` | 59,7–59,9 | 42–50% | 18–21% | 16–21% | 8% |
| to samo, `SCALE=1` | 59,8 | 36% | 15% | 14% | ~7% |

Na czym polegają optymalizacje:
- **`render_update()`** (render.c) zapamiętuje stan ekranu C64 (znaki, kolory, rejestry, sprite'y) i przerysowuje tylko bloki 8×8, w których coś się zmieniło, albo w których sprite był lub jest. Średnio 22 z 1488 bloków na klatkę. Okno dostaje tylko zmienione ciągi bloków. Wynik jest identyczny z pełnym renderem: `wowport --check-render` porównuje oba w każdej klatce (9001 klatek, 0 różnic).
- **Model SID:** w ciszy próbki są zerami, a oscylatory przesuwają się hurtem. Obwiednie liczone są partiami (tylko faktyczne zmiany poziomu), co 4 próbki. Szum jest taktowany tylko, gdy słychać go na wyjściu, a synchronizacja i filtr liczone tylko, gdy są używane. Wysokości dźwięków, rytm i głośność bez zmian (porównanie WAV).

**Po pierwszym teście na prawdziwej A1200 (68060, karta Warp z RTG):** 60 kl./s przy 80% CPU. Dźwięk z ikony nie grał, a z Shella grał tylko chwilami. Poprawki:
- **Dźwięk przez bibliotekę AHI** (`AHI_AllocAudio`) zamiast `CMD_WRITE` do `ahi.device`. Dwa bufory po 768 próbek grane na zmianę; funkcja zwrotna `AHIA_SoundFunc` kolejkuje następny i daje sygnał, gra dopełnia zwolniony bufor z bufora pierścieniowego. Wcześniej krótkie (16 ms) zapytania w kolejce 4 buforów nie mieściły się w opóźnieniu miksera AHI i kolejne klatki dźwięku były odrzucane. `STATS` pokazuje liczbę zamian buforów (WinUAE: 57 na 2 s, zgodnie z oczekiwaniem).
- **Grafika według [Warp RTG Developer notes]:** na ekranie Picasso96 piksele powstają w Fast RAM od razu w formacie ekranu (CLUT, R5G6B5, R5G5B5, ich wersje PC, 32-bit ARGB/BGRA itd.) i są zapisywane długimi słowami (`CopyMemQuick`, tylko zapis, bez odczytu VRAM) do bitmapy na karcie (`AllocBitMap` z bitmapą ekranu jako „friend”, `p96LockBitMap`). Do okna trafiają blitem systemowym `BltBitMapRastPort`, który na Warpie wykonuje sprzęt, a obcinanie do widocznej części okna robią warstwy. W innych przypadkach zostaje `WriteChunkyPixels`. Opcja `NORTG` wymusza stary sposób do porównań.
- Nagłówki: `libraries/Picasso96.h` i `clib/Picasso96_protos.h` z Aminet `dev/misc/P96Develop.lha` w `C:\amiga-dev\p96inc`.
- **„bad load hunk” na prawdziwej A1200:** vlink z opcją `-Rshort` (domyślnie w konfiguracji vbcc) zapisywał część relokacji jako krótkie (blok `0x3F7`, 16-bitowe przesunięcia), obok zwykłych `RELOC32`. AmigaOS 3.2 w WinUAE to wczytywał, system na prawdziwej Amidze nie. W `C:\amiga-dev\vbcc\config\aos68k_win` usunięto `-Rshort`: plik ma teraz tylko standardowe bloki `CODE/DATA/BSS/RELOC32/END` (150 692 B).

**Konfiguracja WinUAE** (`C:\amiga-dev\winuae\WizardOfWor_A1200_OS32.uae`): A1200 AGA, 68060 + FPU + MMU, 2 MB Chip, 256 MB Z3 Fast, RTG UAE Zorro III (uaegfx) 32 MB, Kickstart 3.2 (47.96, `C:\amiga-dev\roms\kicka1200.rom`). Dyski: `DH0:` = **kopia** systemu użytkownika z `D:\AMIGA\System3.2_SD0` w `C:\amiga-dev\winuae\System` (oryginał nienaruszony), `WOW:` = `build\amiga`.

Zmiany w kopii systemu (tylko dla emulatora):
- `S:User-Startup`: na końcu blok „WizardOfWor test”, który uruchamia `WOW:autorun` (czeka 10 s i startuje grę), jeśli ten plik istnieje. Usunięcie `build\amiga\autorun` wyłącza autostart.
- `Devs:AudioModes/EMU10KX` przeniesiony do `Devs/AudioModes-disabled-for-WinUAE`: sterownik karty PCI emu10kx bez `openpci.library` w emulatorze pokazywał komunikat przy każdym otwarciu AHI, który blokował grę.

Uruchomienie: `winuae64.exe -f C:\amiga-dev\winuae\WizardOfWor_A1200_OS32.uae` (z `-s use_gui=no` bez okna ustawień).

## 8b. Testy na prawdziwej Amidze przez sieć: AmiAgent

`amiagent/AmiAgent.c` (vbcc, `bsdsocket.library` Roadshow; `make` w `amiagent/` → `build/amiga/AmiAgent`, 20 KB) to mały serwer TCP (port 6800) na Amigę. Startuje z `S:User-Startup`, hasło czyta z `ENVARC:AmiAgent.key`. Protokół tekstowy, pierwsza linia `AUTH <hasło>`:

| Polecenie | Działanie |
|---|---|
| `PUT <rozmiar> <ścieżka>` + dane | zapis pliku na Amidze |
| `GET <ścieżka>` | odczyt pliku |
| `RUN <polecenie>` | uruchomienie i zwrot kodu wyjścia oraz wyjścia (przez `T:`) |
| `START <polecenie>` | uruchomienie w tle (`SystemTags` asynchronicznie) |
| `STOP <nazwa>` | Ctrl-C do procesów CLI wykonujących program o tej nazwie |
| `TITLES` | tytuły ekranów i okien (np. pomiar `STATS` gry) |
| `SHOT [x y w h]` | zrzut przedniego ekranu w RGB (`p96ReadPixelArray` na RTG, `ReadPixel` na AGA) |
| `KEY <kod> [ms]` | naciśnięcie klawisza przez `input.device` (trafia do aktywnego okna) |

Po stronie PC: `tools/amiga_remote.py` (`ping`, `put`, `get`, `run`, `start`, `stop`, `titles`, `shot`, `key`, `deploy`, `stats`). Konfiguracja w `C:\amiga-dev\amiagent.cfg` (host, port, hasło), poza repozytorium. Gra kończy się na Ctrl-C, więc `deploy` może ją zatrzymać przed wgraniem nowej wersji.

Sprawdzone w WinUAE (`WizardOfWor_A1200_OS32_net.uae` = ta sama konfiguracja + `bsdsocket_emu=true`, konfiguracja klienta `C:\amiga-dev\amiagent-winuae.cfg`, połączenie przez 127.0.0.1): `deploy` → `key ctrl` (start gry) → `stats` („WoW 59.8 fps CPU 36% … AHI 58”) → `shot` (1024×768) → `stop`.

**Spadek do 20 kl./s w rundzie z Worlukiem (prawdziwa A1200):** gra miga tłem, zmieniając w każdej klatce `$D022/$D023` (kolory wielokolorowe). `render_update()` traktował każdą zmianę tych rejestrów jak zmianę całego ekranu i przerysowywał 1488 bloków. Teraz każdy piksel ma zapamiętaną klasę (tło, MC1, MC2, znak), a zmiana samych kolorów `$D020–$D023` przemalowuje tylko bloki zawierające piksele danej klasy (`recolor()`): ok. 122 bloki zamiast 1488. Zgodność dalej sprawdza `--check-render`.

Do tego tryby `VIDEO=BLIT|WPA|WCP`: według notatek Warp kopiowanie VRAM→VRAM w 32 bitach robi CPU, więc na ekranie 32-bit domyślnie jest `WPA` (obraz w Fast RAM, `p96WritePixelArray` prosto do okna), na 8/16-bit `BLIT`. Opcja testowa `WORLUK=N` wymusza rundę z Worlukiem od klatki N. AmiAgent 1.1: `ACTIVATE <początek tytułu>` (aktywuje okno; `amiga_remote.py key` robi to sam), `TITLES` oznacza aktywne okno `*`.

WinUAE przez agenta, `STATS WORLUK=700`: `VIDEO=BLIT` i `VIDEO=WPA` cały czas ok. 60 kl./s (CPU 25–80%).

**Pomiar na prawdziwej A1200 przez agenta** pokazał drugą przyczynę: „game” zajmowało ok. 50% klatki (w WinUAE 6%). Gra czyta joystick ok. 24 razy na klatkę, a `ReadJoyPort()` na prawdziwym sprzęcie jest wolne (dostępy do CIA, rozpoznawanie typu kontrolera). Teraz port jest czytany raz na klatkę (`read_joy_port()`, odświeżane w `hal_wait_frame`): „game” 5–6%, runda z Worlukiem 60 kl./s przy CPU 65–70% (`VIDEO=BLIT`, wybierane automatycznie na ekranie użytkownika). `VIDEO=WPA` jest tam wolniejsze (w rundzie z Worlukiem do 45 kl./s).

**Ustawienia w ikonie:** przy starcie z Workbencha (`argc == 0`) gra czyta Tool Types ikony (`icon.library`, `GetDiskObject`) i traktuje je jak opcje Shella (`parse_option()`): `SCALE=1|2|3`, `NOSOUND`, `STATS`, `NORTG`, `VIDEO=…`; wpis w nawiasach jest pominięty. Ikona wydania ma `SCALE=2`, `(NOSOUND)`, `(STATS)`, `(VIDEO=BLIT)` (ustawione `tools/icon_tooltypes.py`). Sprawdzone na A1200: ikona z `SCALE=1 STATS` otwarta przez ARexx Workbencha (`ICON WINDOW … OPEN`) dała małe okno z pomiarem.

**Dźwięk przez Paulę** (`SOUND=PAULA`, też automatycznie, gdy nie ma AHI): `audio.device`, jeden kanał lewy i jeden prawy (mapa przydziału `{3, 5, 10, 12}`, `ADIOF_NOWAIT`), oba grają te same 8-bitowe bufory w Chip RAM. Trzy bufory po dwa `CMD_WRITE` (`ADIOF_PERVOL`, okres = zegar PAL/NTSC / 22050). `hal_audio_push()` odbiera wrócone zapytania (`GetMsg`, bez czekania) i wypełnia je tym, co jest w buforze pierścieniowym, o zmiennej długości: różnica między częstotliwością Pauli (22 168 Hz przy okresie 160) a 22 050 Hz nie wymaga dopełniania ciszą. Sprawdzone na A1200 (użytkownik potwierdził dźwięk), 60 kl./s przy ok. 30% CPU. `STATS` pokazuje teraz tylko fps, CPU i rodzaj dźwięku; `STATS=FULL` dawny podział czasu.

**Optymalizacja pod 68060/50 MHz** (wcześniejsze pomiary były przy 100 MHz). `STATS=FULL` pokazuje podział czasu obrazu: `r` budowanie (`render_update`), `w` zapis do karty, `b` blity, `runs`, `blocks zapisane/zmienione`. Przed: runda z Worlukiem 32–48 kl./s (obraz do 75%, SID do 30%). Zmiany:
- `render.c`: `text_rect()` liczy całą komórkę znaku naraz (zamiast `text_pixel()` na piksel), `recolor()` bez testów ramki dla bloków wewnątrz obrazu, odbudowa bloku bez `memcpy` na 8 bajtów, sprite'y tylko dla nakładających się bloków, przesunięcia zamiast dzieleń w `sprite_draw()`, porównanie ekranu i mapy bloków po 4 bajty (`LOAD32`). Stały narzut klatki spadł z 7–20% do 1–2%. `--check-render`: 0 różnic.
- `sid.c`: wartości zależne od rejestrów liczone raz na wywołanie, faza 24.8 (jedno dodawanie na próbkę), bez dzielenia przy miksowaniu; bez synchronizacji i modulacji pierścieniowej głosy liczone po kolei przez cały bufor (wynik bit w bit jak pętla po próbkach).
- BLIT: dwie bitmapy na karcie z kopią ich zawartości w Fast RAM (`shadow`); zmienione bloki, które bitmapa już ma (migające ściany), tylko się blituje. Przy Worluku zapisywanych jest ok. 10 ze 100–160 zmienionych bloków. Pętle po mapie bloków przeskakują 4 puste bloki naraz.
- Wynik na A1200 (060/50, Paula): runda z Worlukiem 59–60 kl./s przy CPU 60–74%.

**Ślady po Worluku (dwie bitmapy):** blity łączyły zmienione bloki z maks. 3 niezmienionymi między nimi, a taki niezmieniony blok mógł być w bieżącej bitmapie nieaktualny (np. stara pozycja Worluka) i trafiał do okna. Do tego bloki dołączane przy zapisie nie trafiały do `shadow`. Teraz blit dołącza tylko bloki, które `shadow` bieżącej bitmapy potwierdza, a zapisane bloki dołączone trafiają do `shadow`. Opcja testowa `VERIFY` (ze `STATS`) co 30 klatek czyta okno z karty (`p96ReadPixelArray`) i porównuje każdy blok z `indexed`: wersja z błędem 220 złych bloków w 23 sprawdzeniach rundy z Worlukiem, poprawiona 0 w 31.

**Zawieszenie wyjaśnione** dzięki strażnikowi: `RAM:WoW-hang.txt` miał stan `TS_READY` (pętla bez końca) i adres w `select_dungeon_layout` (mapa z tej samej kompilacji: `vc -k`, potem `vlink … -M`, plik identyczny). Gdy wylosowany układ lochu był taki sam jak poprzedni, port losował ponownie bez upływu klatki, a `random_number` zmienia tylko przerwanie: pętla bez końca (ok. 1 na 15 startów lochu). Oryginał kręci się, aż przerwanie zmieni liczbę; port czeka teraz klatkę (`wait_frame()`).

**Pełny ekran PAL** (`SCREEN=FULL`, domyślnie gdy Workbench nie jest na RTG; `SCREEN=WINDOW` = okno, domyślnie na RTG): własny ekran 640×256 `PAL_MONITOR_ID|HIRES_KEY` (bez sterownika PAL: `HIRES_KEY`), 4 płaszczyzny, paleta C64 (`SA_Colors32`), okno backdrop bez ramki (klawisze, bez wskaźnika myszy, tytuł tylko ze `STATS`). Punkt C64 to dwa punkty HighRes, więc 320 punktów obrazu C64 wypełnia 640 (boczna ramka C64 jest pomijana), w pionie 247 linii wyśrodkowane. Blok 8×8 to jedno słowo na płaszczyznę i linię: `present_planar()` zamienia 8 punktów na 4 słowa tablicami `expand01/expand23` (po 2 bity na punkt w dwóch 16-bitowych połówkach) i pisze je wprost do płaszczyzn ekranu. A1200 060/50: runda z Worlukiem 57–60 kl./s, CPU do 77%.

**Zasady: `FRIENDLYFIRE=OFF`** (PC: `--no-friendly-fire`), poza oryginałem: w `check_hitbox_of_current_actor()` pocisk jednego gracza trafiający drugiego gracza jest traktowany jak własny pocisk (przelatuje, bez punktów i bez śmierci). Domyślnie zasada z oryginału: zestrzelenie drugiego gracza daje punkty. Testy porównawcze bez zmian (opcja wyłączona).

**Zasady: `LIVES=UNLIMITED`** (PC: `--unlimited-lives`): po skończonej animacji śmierci gracz nie traci życia (pominięte `lives_player--` w obsłudze śmierci). Odejmowania przy ucieczce Worluka i Wizarda zostały: trafiają w inny adres (błąd oryginału). Sprawdzone w trybie demo na PC: bez opcji 2 → 1 → 0 i koniec gry, z opcją stale 2 życia przez 20 000 klatek.

**Ikona:** `tools/make_icon.py OUT.info --tooltypes …` składa obrazek 64×50 ze sprite'ów gry (`build/gfx/c64_sprites.bin`: Burwor $85, Worluk $A8, Worrior strzelający w prawo $BA, niebieskie ściany i czerwone drzwi) i zapisuje ikonę z obrazem 4-kolorowym (dowolny Workbench) oraz ColorIcon OS 3.5+ (`FORM ICON`: `FACE` + `IMAG`, RLE na strumieniu bitów, paleta C64). Sprawdzona w WinUAE (start dwuklikiem, Tool Types) i na A1200 z OS 3.2.

**Strażnik zawieszeń:** jedno uruchomienie zawisło na ekranie „GET READY” (bez reakcji na Ctrl-C, potrzebny reset); 14 kolejnych prób go nie powtórzyło. Gra startuje proces „WoW watchdog” (priorytet 1, budzi się co 0,5 s). Gdy przez 3 s nie ma nowej klatki, zapisuje do `RAM:WoW-hang.txt` stan zadania gry (`tc_State`, sygnały, adresy segmentów kodu, 160 długich słów stosu od `tc_SPReg`). Adresy powrotu odejmuje się od początku segmentu 0 i szuka w mapie linkera (`vlink -M`). Plik w `buildmiga-release` jest zlinkowany ręcznie z mapą `buildmigaWizardOfWor-release.map` (146 980 B), żeby adresy z raportu pasowały do mapy. Sprawdzone w WinUAE na celowo zawieszonej wersji: zapisany PC wskazywał pętlę w `hal_wait_frame`.

Bezpieczeństwo: kto zna hasło, uruchamia na Amidze dowolne polecenia. Tylko w domowej sieci, bez przekierowania portu na routerze.

## 9. Następne kroki i decyzje

1. **Wybór docelowej Amigi:** A500/OCS (BOB-y, ryzyko wydajności) czy A1200/AGA (sprite'y 32 px, zapas mocy). Od tego zależy backend grafiki.
2. Backend Amigi: przejęcie systemu, copper i paleta, bitplany z przerysowaniem zmienionych znaków, postacie (sprite'y lub BOB-y), Paula, joysticki, klawiatura, tempo 50/60 Hz.
3. Kompilator dla Amigi (Bebbo amiga-gcc lub vbcc) i testy w WinUAE.
4. Uzupełnienie migawek: Worluk, Wizard, kalibracja tempa w dalszych lochach.
5. Opcjonalnie: mowa Magic Voice jako sample na 4. kanale Pauli.
