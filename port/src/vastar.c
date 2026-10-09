/* Vastar port - the arcade machine, see vastar.h */
#include <stdio.h>
#include <string.h>
#include "vastar.h"
#include "romload.h"

/* ---- ROM sets (MAME 0.27x names and CRCs) ---- */

#define VASTAR_GFX \
    { "c_c9.rom",    0x2000, 0x34f067b6UL, RGN_FGTILES,  0x0000 }, \
    { "c_f7.rom",    0x2000, 0xedbf3b13UL, RGN_SPRITES,  0x0000 }, \
    { "c_f9.rom",    0x2000, 0x8f309e22UL, RGN_SPRITES,  0x2000 }, \
    { "c_n4.rom",    0x2000, 0xb5f9c866UL, RGN_BGTILES0, 0x0000 }, \
    { "c_s4.rom",    0x2000, 0xc9fbbfc9UL, RGN_BGTILES1, 0x0000 }, \
    { "tbp24s10.6p", 0x0100, 0xa712d73aUL, RGN_PROMS,    0x0000 }, \
    { "tbp24s10.6s", 0x0100, 0x0a7d48ecUL, RGN_PROMS,    0x0100 }, \
    { "tbp24s10.6m", 0x0100, 0x4c3db907UL, RGN_PROMS,    0x0200 }, \
    { "tbp24s10.8n", 0x0100, 0xb5297a3bUL, RGN_UNKPROM,  0x0000 }

#define VASTAR_SUB \
    { "e_f2.rom",    0x1000, 0x713478d8UL, RGN_SUB,      0x0000 }, \
    { "e_j2.rom",    0x1000, 0xe4535442UL, RGN_SUB,      0x1000 }

static const RomDef roms_vastar[] = {
    { "e_f4.rom", 0x1000, 0x45fa5075UL, RGN_MAIN, 0x0000 },
    { "e_k4.rom", 0x1000, 0x84531982UL, RGN_MAIN, 0x1000 },
    { "e_h4.rom", 0x1000, 0x94a4f778UL, RGN_MAIN, 0x2000 },
    { "e_l4.rom", 0x1000, 0x40e4d57bUL, RGN_MAIN, 0x3000 },
    { "e_j4.rom", 0x1000, 0xbd607651UL, RGN_MAIN, 0x4000 },
    { "e_n4.rom", 0x1000, 0x7a3779a4UL, RGN_MAIN, 0x5000 },
    { "e_n7.rom", 0x1000, 0x31b6be39UL, RGN_MAIN, 0x6000 },
    { "e_n5.rom", 0x1000, 0xf63f0e78UL, RGN_MAIN, 0x7000 },
    VASTAR_SUB,
    VASTAR_GFX
};

static const RomDef roms_vastar2[] = {
    { "3.4f",  0x1000, 0x6741ff9cUL, RGN_MAIN, 0x0000 },
    { "6.4k",  0x1000, 0x5027619bUL, RGN_MAIN, 0x1000 },
    { "4.4h",  0x1000, 0xfdaa44e6UL, RGN_MAIN, 0x2000 },
    { "7.4l",  0x1000, 0x29bef91cUL, RGN_MAIN, 0x3000 },
    { "5.4j",  0x1000, 0xc17c2458UL, RGN_MAIN, 0x4000 },
    { "8.4n",  0x1000, 0x8ca25c37UL, RGN_MAIN, 0x5000 },
    { "10.6n", 0x1000, 0x80df74baUL, RGN_MAIN, 0x6000 },
    { "9.5n",  0x1000, 0x239ec84eUL, RGN_MAIN, 0x7000 },
    VASTAR_SUB,
    VASTAR_GFX
};

static const RomDef roms_vastar3[] = {
    { "vst_2.4f", 0x2000, 0xad4e512aUL, RGN_MAIN, 0x0000 },
    { "vst_3.4h", 0x2000, 0x2276c5d0UL, RGN_MAIN, 0x2000 },
    { "vst_4.4j", 0x2000, 0xdeca2aa1UL, RGN_MAIN, 0x4000 },
    { "vst_5.6n", 0x2000, 0x743ed1c7UL, RGN_MAIN, 0x6000 },
    { "vst_0.2f", 0x1000, 0x713478d8UL, RGN_SUB,  0x0000 },
    { "vst_1.2j", 0x1000, 0xe4535442UL, RGN_SUB,  0x1000 },
    VASTAR_GFX
};

static const RomDef roms_vastar4[] = {
    { "3.bin",    0x1000, 0xd2b8f177UL, RGN_MAIN, 0x0000 },
    { "e_k4.rom", 0x1000, 0x84531982UL, RGN_MAIN, 0x1000 },
    { "e_h4.rom", 0x1000, 0x94a4f778UL, RGN_MAIN, 0x2000 },
    { "e_l4.rom", 0x1000, 0x40e4d57bUL, RGN_MAIN, 0x3000 },
    { "e_j4.rom", 0x1000, 0xbd607651UL, RGN_MAIN, 0x4000 },
    { "e_n4.rom", 0x1000, 0x7a3779a4UL, RGN_MAIN, 0x5000 },
    { "e_n7.rom", 0x1000, 0x31b6be39UL, RGN_MAIN, 0x6000 },
    { "e_n5.rom", 0x1000, 0xf63f0e78UL, RGN_MAIN, 0x7000 },
    VASTAR_SUB,
    VASTAR_GFX
};

static const RomDef roms_pprobe[] = {
    { "pb2.bin",       0x2000, 0xa88592aaUL, RGN_MAIN,     0x0000 },
    { "pb3.bin",       0x2000, 0xe4e20f74UL, RGN_MAIN,     0x2000 },
    { "pb4.bin",       0x2000, 0x4e40e3feUL, RGN_MAIN,     0x4000 },
    { "pb5.bin",       0x2000, 0xb26ff0fdUL, RGN_MAIN,     0x6000 },
    { "pb1.bin",       0x2000, 0xcd624df9UL, RGN_SUB,      0x0000 },
    { "pb9.bin",       0x2000, 0x82294dd6UL, RGN_FGTILES,  0x0000 },
    { "pb8.bin",       0x2000, 0x8d809e45UL, RGN_SPRITES,  0x0000 },
    { "pb10.bin",      0x2000, 0x895f9dd3UL, RGN_SPRITES,  0x2000 },
    { "pb6.bin",       0x2000, 0xff309239UL, RGN_BGTILES0, 0x0000 },
    { "pb7.bin",       0x2000, 0x439978f7UL, RGN_BGTILES1, 0x0000 },
    { "n82s129.3",     0x0100, 0xdfb6b97cUL, RGN_PROMS,    0x0000 },
    { "n82s129.1",     0x0100, 0x3cc696a2UL, RGN_PROMS,    0x0100 },
    { "dm74s287.2",    0x0100, 0x64fea033UL, RGN_PROMS,    0x0200 },
    { "mmi6301-1.bin", 0x0100, 0xb5297a3bUL, RGN_UNKPROM,  0x0000 }
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* DSW1 defaults: 3 lives, upright cabinet (bit 6 = 0); DSW2: 1 coin 1 credit */
const GameDef vastar_games[] = {
    { "vastar",  0,        "Vastar (set 1)",            roms_vastar,  COUNT(roms_vastar),  0xbf, 0xff },
    { "vastar2", "vastar", "Vastar (set 2)",            roms_vastar2, COUNT(roms_vastar2), 0xbf, 0xff },
    { "vastar3", "vastar", "Vastar (set 3)",            roms_vastar3, COUNT(roms_vastar3), 0xbf, 0xff },
    { "vastar4", "vastar", "Vastar (set 4)",            roms_vastar4, COUNT(roms_vastar4), 0xbf, 0xff },
    { "pprobe",  0,        "Planet Probe (prototype?)", roms_pprobe,  COUNT(roms_pprobe),  0xbd, 0xff }
};
const int vastar_game_count = COUNT(vastar_games);

const GameDef *vastar_find_game(const char *name)
{
    int i;
    for (i = 0; i < vastar_game_count; i++)
        if (!strcmp(vastar_games[i].name, name))
            return &vastar_games[i];
    return 0;
}

static uint8_t *region_ptr(Vastar *m, int region)
{
    switch (region) {
    case RGN_MAIN: return m->main_rom;
    case RGN_SUB: return m->sub_rom;
    case RGN_FGTILES: return m->fgtiles;
    case RGN_SPRITES: return m->sprites;
    case RGN_BGTILES0: return m->bgtiles0;
    case RGN_BGTILES1: return m->bgtiles1;
    case RGN_PROMS: return m->proms;
    default: return m->unkprom;
    }
}

int vastar_load_roms(Vastar *m, const GameDef *game, const char *const *sources, int nsources)
{
    RomRequest req[32];
    int i;
    m->game = game;
    for (i = 0; i < game->nroms && i < 32; i++) {
        const RomDef *r = &game->roms[i];
        req[i].name = r->name;
        req[i].size = r->size;
        req[i].crc = r->crc;
        req[i].dest = region_ptr(m, r->region) + r->offset;
        req[i].found = 0;
    }
    return rom_load(sources, nsources, req, i);
}

/* ---- main CPU ---- */

/* c000-c3ff (c000 = layer priority) and e000-efff (watchdog) */
static void main_write(void *ctx, uint16_t a, uint8_t v)
{
    Vastar *m = (Vastar *)ctx;
    if (a == 0xc000)
        m->priority = v;
}

static void set_sub_reset(Vastar *m, int held)
{
    if (held) {
        z80_reset(&m->sub);
        z80_set_irq(&m->sub, 0, 0);
    }
}

static void main_out(void *ctx, uint16_t port, uint8_t v)
{
    Vastar *m = (Vastar *)ctx;
    int bit = port & 0x0f;
    uint8_t old = m->latch;
    if (bit > 7)
        return;
    if (v & 1)
        m->latch |= (uint8_t)(1 << bit);
    else
        m->latch &= (uint8_t)~(1 << bit);
    if ((old ^ m->latch) & 0x04)
        set_sub_reset(m, !(m->latch & 0x04));
}

/* ---- sub CPU ---- */

static uint8_t sub_in(void *ctx, uint16_t port)
{
    Vastar *m = (Vastar *)ctx;
    if ((port & 0x0f) == 0x02) {
        m->ay.port_a = m->dsw1;
        m->ay.port_b = m->dsw2;
        return ay_data_r(&m->ay);
    }
    return 0xff;
}

static void sub_out(void *ctx, uint16_t port, uint8_t v)
{
    Vastar *m = (Vastar *)ctx;
    switch (port & 0x0f) {
    case 0x00: ay_address_w(&m->ay, v); break;
    case 0x01: ay_data_w(&m->ay, v); break;
    default: break;
    }
}

void vastar_init(Vastar *m, uint32_t sample_rate)
{
    Z80 *z = &m->main;

    m->bg1ram = m->mmem + 0x8000;
    m->bg0ram = m->mmem + 0x9000;
    m->fgram = m->mmem + 0xc000;
    m->shared = m->mmem + 0xf000;

    /* main CPU: ROM, bg RAM 8000/9000 mirrored at a000/b000, registers at c000,
       fg RAM c400-cfff, watchdog e000, RAM f000-f7ff shared with the sub CPU */
    z80_init(z, m->mmem);
    z->ctx = m;
    z->write = main_write;
    z->out = main_out;
    z80_set_wtype(z, 0x0000, 0xffff, Z80_ROM, 0);
    z80_set_wtype(z, 0x8000, 0x8fff, Z80_MIRROR, m->mmem + 0xa000);
    z80_set_wtype(z, 0xa000, 0xafff, Z80_MIRROR, m->mmem + 0x8000);
    z80_set_wtype(z, 0x9000, 0x9fff, Z80_MIRROR, m->mmem + 0xb000);
    z80_set_wtype(z, 0xb000, 0xbfff, Z80_MIRROR, m->mmem + 0x9000);
    z80_set_wtype(z, 0xc000, 0xc3ff, Z80_IO, 0);
    z80_set_wtype(z, 0xc400, 0xcfff, Z80_RAM, 0);
    z80_set_wtype(z, 0xf000, 0xf7ff, Z80_MIRROR, m->smem + 0x4000);

    /* sub CPU: ROM, RAM 4000-47ff shared, inputs at 8000/8040/8080 */
    z = &m->sub;
    z80_init(z, m->smem);
    z->ctx = m;
    z->in = sub_in;
    z->out = sub_out;
    z80_set_wtype(z, 0x0000, 0xffff, Z80_ROM, 0);
    z80_set_wtype(z, 0x4000, 0x47ff, Z80_MIRROR, m->mmem + 0xf000);

    m->sample_rate = sample_rate;
    ay_init(&m->ay, VASTAR_AY_CLOCK, sample_rate);
    m->dsw1 = m->game ? m->game->dsw1 : 0xbf;
    m->dsw2 = m->game ? m->game->dsw2 : 0xff;
    m->p2_from_p1 = 1;
    vastar_reset(m);
}

void vastar_reset(Vastar *m)
{
    /* unmapped addresses read 0xff, RAM starts cleared */
    memset(m->mmem, 0xff, sizeof m->mmem);
    memcpy(m->mmem, m->main_rom, sizeof m->main_rom);
    memset(m->mmem + 0x8000, 0, 0x4000);         /* bg RAM and its mirrors */
    memset(m->mmem + 0xc400, 0, 0x0c00);
    memset(m->mmem + 0xf000, 0, 0x0800);
    memset(m->smem, 0xff, sizeof m->smem);
    memcpy(m->smem, m->sub_rom, sizeof m->sub_rom);
    memset(m->smem + 0x4000, 0, 0x0800);
    m->priority = 0;
    m->latch = 0;              /* the sub CPU stays in reset until the main CPU releases it */
    z80_reset(&m->main);
    set_sub_reset(m, 1);
    ay_reset(&m->ay);
    m->audio_acc = 0;
    m->frame = 0;
}

/* ---- frame ---- */

#define SLICE_LINES 6
#define SLICE_CYCLES (SLICE_LINES * VASTAR_LINE_CYCLES)
#define VBLANK_LINE 240

int vastar_run_frame(Vastar *m, int16_t *audio)
{
    int slice, samples = 0;
    /* extra lives (Vastar sets only): f101 is the game's copy of the lives setting,
       which it reads when a game starts; f110 the lives left (besides the one in play) */
    if (m->game && strncmp(m->game->name, "vastar", 6) == 0) {
        if (m->lives) {
            m->mmem[0xf101] = m->lives;
            m->smem[0x4101] = m->lives;          /* the sub CPU's copy of the shared RAM */
        }
        if (m->infinite_lives && m->mmem[0xf110] < 2) {
            m->mmem[0xf110] = 2;
            m->smem[0x4110] = 2;
        }
    }

    /* the inputs as the sub CPU reads them */
    m->smem[0x8000] = (uint8_t)(m->in_p2 | (m->p2_from_p1 ? m->in_p1 : 0));
    m->smem[0x8040] = m->in_p1;
    m->smem[0x8080] = m->in_system;
    for (slice = 0; slice < VASTAR_LINES / SLICE_LINES; slice++) {
        int line = (VBLANK_LINE + slice * SLICE_LINES) % VASTAR_LINES;
        int n;

        /* main CPU: NMI at the start of vblank, if enabled by the latch */
        if (line == VBLANK_LINE && (m->latch & 0x01))
            z80_nmi(&m->main);
        /* sub CPU: IRQ 4 times per frame (242 Hz), held until acknowledged */
        if (line % (VASTAR_LINES / 4) == 0 && (m->latch & 0x04))
            z80_set_irq(&m->sub, 1, 1);

        z80_run(&m->main, SLICE_CYCLES);
        if (m->latch & 0x04)
            z80_run(&m->sub, SLICE_CYCLES);

        m->audio_acc += m->sample_rate * SLICE_CYCLES;
        n = (int)(m->audio_acc / VASTAR_CPU_CLOCK);
        m->audio_acc -= (uint32_t)n * VASTAR_CPU_CLOCK;
        if (audio && n > 0) {
            ay_render(&m->ay, audio + samples, n);
            samples += n;
        }
    }
    m->frame++;
    return samples;
}
