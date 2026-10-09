/* Vastar port - the arcade machine (Orca, 1983): two Z80s, AY-3-8910, video RAM.
 *
 * Reference: MAME orca/vastar.cpp + vastar_viddev.cpp and the MiSTer core
 * (rtl/Vastar_CPU.sv) in this repository.
 */
#ifndef VASTAR_H
#define VASTAR_H

#include <stdint.h>
#include "z80.h"
#include "ay8910.h"

#define VASTAR_CPU_CLOCK   3072000     /* 18.432 MHz / 6, both Z80s */
#define VASTAR_AY_CLOCK    1536000     /* 18.432 MHz / 12 */
#define VASTAR_LINE_CYCLES 192         /* 384 pixels at 6.144 MHz = 192 CPU cycles */
#define VASTAR_LINES       264
#define VASTAR_FRAME_CYCLES (VASTAR_LINE_CYCLES * VASTAR_LINES)   /* 50688 -> 60.606 Hz */
#define VASTAR_FPS_X1000   60606

/* inputs, active high (MAME port layout) */
#define IN_LEFT    0x01
#define IN_RIGHT   0x02
#define IN_UP      0x04
#define IN_DOWN    0x08
#define IN_BUTTON1 0x10
#define IN_BUTTON2 0x20
#define IN_COIN1   0x01
#define IN_COIN2   0x02
#define IN_SERVICE 0x04
#define IN_START1  0x08
#define IN_START2  0x10

enum { RGN_MAIN, RGN_SUB, RGN_FGTILES, RGN_SPRITES, RGN_BGTILES0, RGN_BGTILES1, RGN_PROMS, RGN_UNKPROM };

typedef struct RomDef {
    const char *name;
    uint32_t size;
    uint32_t crc;
    uint8_t region;
    uint16_t offset;
} RomDef;

typedef struct GameDef {
    const char *name;       /* MAME set name */
    const char *parent;     /* MAME parent set (its zip may hold shared ROMs), or NULL */
    const char *title;
    const RomDef *roms;
    int nroms;
    uint8_t dsw1, dsw2;     /* default DIP switches */
} GameDef;

extern const GameDef vastar_games[];
extern const int vastar_game_count;
const GameDef *vastar_find_game(const char *name);

typedef struct Vastar {
    const GameDef *game;

    /* ROMs */
    uint8_t main_rom[0x8000];
    uint8_t sub_rom[0x2000];
    uint8_t fgtiles[0x2000];
    uint8_t sprites[0x4000];
    uint8_t bgtiles0[0x2000];
    uint8_t bgtiles1[0x2000];
    uint8_t proms[0x300];        /* red, green, blue */
    uint8_t unkprom[0x100];

    /* the address spaces of the two CPUs (see z80.h); the RAM regions are
       indexed by CPU address & 0xfff */
    uint8_t mmem[0x10000 + 4];   /* main CPU */
    uint8_t smem[0x10000 + 4];   /* sub CPU */
    uint8_t *bg1ram;             /* 8000-8fff (mirror a000) */
    uint8_t *bg0ram;             /* 9000-9fff (mirror b000) */
    uint8_t *fgram;              /* c000-cfff, RAM from c400: fg layer, sprites, scroll */
    uint8_t *shared;             /* main f000-f7ff = sub 4000-47ff */

    uint8_t priority;            /* c000: layer order */
    uint8_t latch;               /* LS259: bit 0 NMI enable, 1 flip screen, 2 sub CPU run */

    uint8_t in_p1, in_p2, in_system, dsw1, dsw2;

    /* not in the original: lives at the start of a game (0 = the DIP switches, which
       allow 3 to 6), and lives that do not run out */
    uint8_t lives, infinite_lives;

    /* player 2 (alternating) plays with player 1's controls; the arcade reads a
       second set of controls for player 2 (P2). On by default (vastar_init). */
    uint8_t p2_from_p1;

    Z80 main, sub;
    AY8910 ay;

    uint32_t sample_rate;
    uint32_t audio_acc;
    unsigned long frame;
} Vastar;

/* load the ROMs of `game` from the given sources (zip files / directories);
   returns the number of missing ROMs */
int vastar_load_roms(Vastar *m, const GameDef *game, const char *const *sources, int nsources);

void vastar_init(Vastar *m, uint32_t sample_rate);   /* after vastar_load_roms */
void vastar_reset(Vastar *m);

/* run one video frame (264 lines, starting at the beginning of vblank).
   `audio` receives the samples of the frame (at most sample_rate / 60 + 2), the
   return value is their number. Render the picture after this call. */
int vastar_run_frame(Vastar *m, int16_t *audio);

#endif
