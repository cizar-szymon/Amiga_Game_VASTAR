/* Vastar port - video, see video.h. Follows MAME vastar_viddev.cpp. */
#include <string.h>
#include "video.h"

/* MAME offsets into the fg video RAM (which starts at c400), plus 0x400 for our fgram[] */
#define FG_COL       0x400      /* fg colour */
#define FG_ATTR      0x800      /* fg tile number bit 8, flip bits */
#define FG_CODE      0xc00      /* fg tile number bits 0-7 */
#define SPR_Y_COL    0x400      /* sprite y, colour */
#define SPR_ATTR     0x800      /* sprite code bit 6, double height */
#define SPR_CODE_X   0xc00      /* sprite code, flip bits, x */
#define BG0_SCROLL   0x7c0      /* 32 column scroll values of bg layer 0 */
#define BG1_SCROLL   0x7e0
/* bg RAM (8000 / 9000) */
#define BG_ATTR      0x000
#define BG_CODE      0x800
#define BG_COL       0xc00

/* 2 bit pixel x (0-3) of a byte: plane 0 = high bit */
static uint8_t pix(uint8_t b, int x)
{
    return (uint8_t)((((b >> (7 - x)) & 1) << 1) | ((b >> (3 - x)) & 1));
}

static void decode_tiles(uint8_t *out, const uint8_t *rom)
{
    int t, y, x;
    for (t = 0; t < 512; t++)
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                out[t * 64 + y * 8 + x] = pix(rom[t * 16 + (x >> 2) * 8 + y], x & 3);
}

static void decode_sprites(uint8_t *out, const uint8_t *rom)
{
    int t, y, x;
    for (t = 0; t < 256; t++)
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                out[t * 256 + y * 16 + x] =
                    pix(rom[t * 64 + (y < 8 ? y : 32 + y - 8) + (x >> 2) * 8], x & 3);
}

static void init_direct(Video *v);

void video_init(Video *v, const Vastar *m)
{
    int i;
    decode_tiles(v->fg, m->fgtiles);
    decode_tiles(v->bg0, m->bgtiles1);   /* MAME: bg tilemap n uses gfx 4 - n */
    decode_tiles(v->bg1, m->bgtiles0);
    decode_sprites(v->spr, m->sprites);
    for (i = 0; i < 256; i++) {
        v->palette[i][0] = (uint8_t)((m->proms[i] & 0x0f) * 0x11);
        v->palette[i][1] = (uint8_t)((m->proms[0x100 + i] & 0x0f) * 0x11);
        v->palette[i][2] = (uint8_t)((m->proms[0x200 + i] & 0x0f) * 0x11);
    }
    v->cache_valid = 0;
    v->prev_priority = -1;
    init_direct(v);
}

/* One tile layer: 32x32 tiles, optional scroll per column. With flip the whole layer
   is turned by 180 degrees (MAME tilemap flip). */
static void draw_layer(uint8_t *fb, const uint8_t *ram, int code_off, int attr_off, int col_off,
                       const uint8_t *tiles, const uint8_t *scroll, int opaque, int flip)
{
    int col, y, i;
    for (col = 0; col < 32; col++) {
        int sc = scroll ? scroll[col] : 0;
        for (y = VIDEO_Y0; y <= VIDEO_Y1; y++) {
            int my = (y + sc) & 0xff;
            int ti = ((my >> 3) << 5) | col;
            uint8_t attr = ram[attr_off + ti];
            int code = (ram[code_off + ti] | (attr << 8)) & 0x1ff;
            uint8_t color = (uint8_t)((ram[col_off + ti] & 0x3f) << 2);
            int row = (attr & 0x08) ? 7 - (my & 7) : (my & 7);
            const uint8_t *src = tiles + code * 64 + row * 8;
            uint8_t *dst;
            int step, sx = 1;
            if (flip) {
                dst = fb + (255 - y) * VIDEO_W + 255 - col * 8;
                step = -1;
            } else {
                dst = fb + y * VIDEO_W + col * 8;
                step = 1;
            }
            if (attr & 0x04) {   /* flip x */
                src += 7;
                sx = -1;
            }
            if (opaque) {
                for (i = 0; i < 8; i++, src += sx, dst += step)
                    *dst = (uint8_t)(color | *src);
            } else {
                for (i = 0; i < 8; i++, src += sx, dst += step)
                    if (*src)
                        *dst = (uint8_t)(color | *src);
            }
        }
    }
}

/* transparent 16 pixel wide sprite, clipped to the visible area */
static void draw_sprite(uint8_t *fb, const uint8_t *gfx, int height, uint8_t color,
                        int flipx, int flipy, int sx, int sy)
{
    int py, px;
    for (py = 0; py < height; py++) {
        int y = sy + py;
        const uint8_t *src;
        uint8_t *dst;
        if (y < VIDEO_Y0 || y > VIDEO_Y1)
            continue;
        src = gfx + (flipy ? height - 1 - py : py) * 16;
        dst = fb + y * VIDEO_W;
        for (px = 0; px < 16; px++) {
            int x = sx + px;
            uint8_t p;
            if (x < 0 || x > 255)
                continue;
            p = src[flipx ? 15 - px : px];
            if (p)
                dst[x] = (uint8_t)(color | p);
        }
    }
}

static void draw_sprites(const Video *v, const Vastar *m, uint8_t *fb, int rambase, int tilebase, int flip)
{
    const uint8_t *ram = m->fgram + rambase;
    int offs;
    for (offs = 0; offs < 0x10; offs += 2) {
        int code = ((ram[SPR_CODE_X + offs] & 0xfc) >> 2) + ((ram[SPR_ATTR + offs] & 0x01) << 6) + tilebase;
        int sx = ram[SPR_CODE_X + offs + 1];
        int sy = ram[SPR_Y_COL + offs];
        uint8_t color = (uint8_t)((ram[SPR_Y_COL + offs + 1] & 0x3f) << 2);
        int flipy = ram[SPR_CODE_X + offs] & 0x01;
        int flipx = (ram[SPR_CODE_X + offs] & 0x02) >> 1;
        if (flip) {
            flipx = !flipx;
            flipy = !flipy;
        }
        if (ram[SPR_ATTR + offs] & 0x08) {   /* 16x32 */
            const uint8_t *gfx = v->spr + (code & ~1) * 256;
            if (!flip)
                sy = 224 - sy;
            draw_sprite(fb, gfx, 32, color, flipx, flipy, sx, sy);
            draw_sprite(fb, gfx, 32, color, flipx, flipy, sx, sy + 256);
            draw_sprite(fb, gfx, 32, color, flipx, flipy, sx - 256, sy);
            draw_sprite(fb, gfx, 32, color, flipx, flipy, sx - 256, sy + 256);
        } else {
            const uint8_t *gfx = v->spr + code * 256;
            if (!flip)
                sy = 240 - sy;
            draw_sprite(fb, gfx, 16, color, flipx, flipy, sx, sy);
            draw_sprite(fb, gfx, 16, color, flipx, flipy, sx - 256, sy);
        }
    }
}

void video_render(const Video *v, const Vastar *m, uint8_t *fb)
{
    int flip = (m->latch >> 1) & 1;
    const uint8_t *bg0s = m->fgram + BG0_SCROLL, *bg1s = m->fgram + BG1_SCROLL;

#define BG0(op) draw_layer(fb, m->bg0ram, BG_CODE, BG_ATTR, BG_COL, v->bg0, bg0s, op, flip)
#define BG1(op) draw_layer(fb, m->bg1ram, BG_CODE, BG_ATTR, BG_COL, v->bg1, bg1s, op, flip)
#define FG(op)  draw_layer(fb, m->fgram, FG_CODE, FG_ATTR, FG_COL, v->fg, 0, op, flip)
#define SPRITES() do { draw_sprites(v, m, fb, 0x30, 0x80, flip); draw_sprites(v, m, fb, 0x10, 0x00, flip); } while (0)

    switch (m->priority) {
    case 1:   /* Planet Probe */
        BG0(1); BG1(0); SPRITES(); FG(0);
        break;
    case 2:
        BG0(1); SPRITES(); BG0(0); BG1(0); FG(0);
        break;
    case 3:
        BG0(1); BG1(0); FG(0); SPRITES();
        break;
    case 4:
        FG(1); SPRITES(); BG0(0); BG1(0);
        break;
    default:  /* 0 (unknown values too) */
        BG0(1); SPRITES(); BG1(0); FG(0);
        break;
    }
#undef BG0
#undef BG1
#undef FG
#undef SPRITES
}

void video_rotate(const uint8_t *fb, uint8_t *screen, int pitch)
{
    /* ROT90 (clockwise): screen (x, y) = fb column y, row 239 - x */
    int x, y;
    for (y = 0; y < SCREEN_H; y++) {
        uint8_t *dst = screen + y * pitch;
        const uint8_t *src = fb + VIDEO_Y1 * VIDEO_W + y;
        for (x = 0; x < SCREEN_W; x++, src -= VIDEO_W)
            dst[x] = *src;
    }
}

/* ---- fast renderer: 8x8 blocks composed straight from the tiles ----
 *
 * Screen (sx, sy) shows the arcade pixel x = sy, y = 239 - sx. A screen block row
 * (8 screen rows) is one tilemap column, and the 8 pixels of a block row come from at
 * most 2 tiles of that column (tilemap rows ty = (239 - sx + scroll) & 255, going down
 * as sx goes right). With the tiles packed 2 bits per pixel in that order, a block row
 * is two 16 bit tile rows joined, shifted and expanded through a table. The working set
 * is small (packed tiles 48 KB, tables 2 KB, tile RAM), which matters on machines whose
 * L2 cache is smaller than a bitmap per layer (Warp: 96 KB).
 */

unsigned long video_stat_tiles;   /* tiles changed (statistics) */

/* 4 pixels at a time; unaligned 32 bit accesses are fine on the 68020+ */
#if defined(__VBCC__) || defined(__m68k__) || defined(__mc68000__)
#define LOAD32(p) (*(const uint32_t *)(const void *)(p))
#define STORE32(p, v) (*(uint32_t *)(void *)(p) = (v))
#else
static uint32_t LOAD32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void STORE32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
#endif

/* the tables for video_render_screen() (called by video_init) */
static void init_direct(Video *v)
{
    const uint8_t *sets[3];
    int k, t, f, px, j, i;
    uint8_t b[8];
    sets[0] = v->bg0;
    sets[1] = v->bg1;
    sets[2] = v->fg;
    for (k = 0; k < 3; k++)
        for (t = 0; t < 512; t++) {
            const uint8_t *src = sets[k] + t * 64;
            uint8_t any = 0;
            for (f = 0; f < 2; f++)
                for (px = 0; px < 8; px++) {
                    uint16_t w = 0;
                    for (j = 0; j < 8; j++) {      /* j = screen order: tile row 7 - j */
                        uint8_t p = src[(f ? j : 7 - j) * 8 + px];
                        w = (uint16_t)((w << 2) | p);
                        any |= p;
                    }
                    v->ptile[k][t][f][px] = w;
                }
            v->tile_any[k][t] = any;
        }
    for (i = 0; i < 256; i++) {
        for (j = 0; j < 4; j++)
            b[j] = (uint8_t)((i >> (6 - 2 * j)) & 3);
        v->xpix[i] = LOAD32(b);
        for (j = 0; j < 4; j++)
            b[j] = (uint8_t)(b[j] ? 0xff : 0);
        v->xmask[i] = LOAD32(b);
    }
    for (i = 0; i <= 8; i++) {
        for (j = 0; j < 8; j++)
            b[j] = (uint8_t)(j < i ? 0xff : 0);
        v->split[i][0] = LOAD32(b);
        v->split[i][1] = LOAD32(b + 4);
    }
}

/* the first i >= from (multiple of 4) where one of the 3 tile arrays differs from its
   copy, 1024 if none (video_68k.s has it for the Amiga) */
#ifdef VIDEO_ASM
int vscan(const uint8_t *r0, const uint8_t *s0, const uint8_t *r1, const uint8_t *s1,
          const uint8_t *r2, const uint8_t *s2, int from);
#else
static int vscan(const uint8_t *r0, const uint8_t *s0, const uint8_t *r1, const uint8_t *s1,
                 const uint8_t *r2, const uint8_t *s2, int from)
{
    for (; from < 1024; from += 4)
        if (LOAD32(r0 + from) != LOAD32(s0 + from) || LOAD32(r1 + from) != LOAD32(s1 + from) ||
            LOAD32(r2 + from) != LOAD32(s2 + from))
            break;
    return from;
}
#endif

/* remember the tile RAM and mark the tiles that changed */
static void update_layer(Video *v, int k, const uint8_t *ram, int code_off, int attr_off, int col_off)
{
    uint8_t *sc = v->shadow[k][0], *sa = v->shadow[k][1], *sl = v->shadow[k][2];
    const uint8_t *rc = ram + code_off, *ra = ram + attr_off, *rl = ram + col_off;
    int i, j;
    if (!v->cache_valid) {
        memcpy(sc, rc, 1024);
        memcpy(sa, ra, 1024);
        memcpy(sl, rl, 1024);
        return;
    }
    for (i = 0; (i = vscan(rc, sc, ra, sa, rl, sl, i)) < 1024; i += 4)
        for (j = i; j < i + 4; j++) {
            if (rc[j] == sc[j] && ra[j] == sa[j] && rl[j] == sl[j])
                continue;
            sc[j] = rc[j];
            sa[j] = ra[j];
            sl[j] = rl[j];
            v->tile_dirty[k][j] = 1;
            video_stat_tiles++;
        }
}

/* the blocks a changed tile of layer k covers on screen, with its column's scroll */
static void mark_tiles(Video *v, int k, const uint8_t *scroll)
{
    uint8_t *td = v->tile_dirty[k];
    int i, t;
    for (i = 0; i < 1024; i += 4) {
        if (!LOAD32(td + i))
            continue;
        for (t = i; t < i + 4; t++) {
            int c = t & 31, r = t >> 5, s = scroll ? scroll[c] : 0, ty;
            if (!td[t])
                continue;
            td[t] = 0;
            for (ty = r * 8; ty < r * 8 + 8; ty++) {
                int y = (ty - s) & 0xff;                     /* arcade y of this tilemap row */
                if (y >= VIDEO_Y0 && y <= VIDEO_Y1)
                    v->blk[c][(VIDEO_Y1 - y) >> 3] = 1;      /* screen x = 239 - y */
            }
        }
    }
}

/* one visible sprite copy: into the list and its blocks into map */
static void add_sprite(Video *v, uint8_t map[32][28], const uint8_t *gfx, int height, uint8_t color,
                       int flipx, int flipy, int sx, int sy)
{
    int x0 = sx < 0 ? 0 : sx, x1 = sx + 15 > 255 ? 255 : sx + 15;
    int y0 = sy < VIDEO_Y0 ? VIDEO_Y0 : sy, y1 = sy + height - 1 > VIDEO_Y1 ? VIDEO_Y1 : sy + height - 1;
    struct SprCopy *s;
    int by, bx;
    if (x0 > x1 || y0 > y1 || v->nsprites >= 64)
        return;
    s = &v->sprites[v->nsprites++];
    s->gfx = gfx;
    s->ox = (short)sx;
    s->oy = (short)sy;
    s->h = (short)height;
    s->sy0 = (short)x0;                  /* screen row = arcade x */
    s->sy1 = (short)x1;
    s->sx0 = (short)(VIDEO_Y1 - y1);     /* screen x = 239 - arcade y */
    s->sx1 = (short)(VIDEO_Y1 - y0);
    s->color = color;
    s->flipx = (uint8_t)flipx;
    s->flipy = (uint8_t)flipy;
    for (by = x0 >> 3; by <= x1 >> 3; by++)
        for (bx = s->sx0 >> 3; bx <= s->sx1 >> 3; bx++)
            map[by][bx] = 1;
}

/* the sprite list in MAME's drawing order (bank 0x30, then 0x10; later ones on top) */
static void build_sprites(Video *v, const Vastar *m, uint8_t map[32][28])
{
    static const int bases[2] = { 0x30, 0x10 }, tilebases[2] = { 0x80, 0x00 };
    int b, offs;
    v->nsprites = 0;
    for (b = 0; b < 2; b++) {
        const uint8_t *ram = m->fgram + bases[b];
        for (offs = 0; offs < 0x10; offs += 2) {
            int code = ((ram[SPR_CODE_X + offs] & 0xfc) >> 2) + ((ram[SPR_ATTR + offs] & 0x01) << 6) + tilebases[b];
            int sx = ram[SPR_CODE_X + offs + 1], sy = ram[SPR_Y_COL + offs];
            uint8_t color = (uint8_t)((ram[SPR_Y_COL + offs + 1] & 0x3f) << 2);
            int flipy = ram[SPR_CODE_X + offs] & 0x01;
            int flipx = (ram[SPR_CODE_X + offs] & 0x02) >> 1;
            if (ram[SPR_ATTR + offs] & 0x08) {
                const uint8_t *gfx = v->spr + (code & ~1) * 256;
                sy = 224 - sy;
                add_sprite(v, map, gfx, 32, color, flipx, flipy, sx, sy);
                add_sprite(v, map, gfx, 32, color, flipx, flipy, sx, sy + 256);
                add_sprite(v, map, gfx, 32, color, flipx, flipy, sx - 256, sy);
                add_sprite(v, map, gfx, 32, color, flipx, flipy, sx - 256, sy + 256);
            } else {
                const uint8_t *gfx = v->spr + code * 256;
                sy = 240 - sy;
                add_sprite(v, map, gfx, 16, color, flipx, flipy, sx, sy);
                add_sprite(v, map, gfx, 16, color, flipx, flipy, sx - 256, sy);
            }
        }
    }
}

/* layer k of block (by, bx) into the 8x8 block t (8 bytes per row) */
static void comp_layer_c(const Video *v, int k, const uint8_t *ram, int code_off, int attr_off, int col_off,
                       int scroll, int by, int bx, uint8_t *t, int opaque)
{
    const int t0 = (VIDEO_Y1 - bx * 8 + scroll) & 0xff;   /* tilemap row of the left pixel */
    const int ra = t0 >> 3, fa = t0 & 7, rb = (ra - 1) & 31;
    const int ia = (ra << 5) | by, ib = (rb << 5) | by;
    const uint8_t aa = ram[attr_off + ia], ab = ram[attr_off + ib];
    const int ca = ((aa << 8) | ram[code_off + ia]) & 0x1ff, cb = ((ab << 8) | ram[code_off + ib]) & 0x1ff;
    const uint16_t *wa = v->ptile[k][ca][(aa >> 3) & 1], *wb = v->ptile[k][cb][(ab >> 3) & 1];
    const int xa = (aa & 0x04) ? 7 : 0, xb = (ab & 0x04) ? 7 : 0;  /* flip x: rows in reverse */
    const int shift = 2 * (7 - fa);          /* the pixels of tile a left of the block */
    uint32_t cola = (uint32_t)((ram[col_off + ia] & 0x3f) << 2) * 0x01010101UL;
    uint32_t colb = (uint32_t)((ram[col_off + ib] & 0x3f) << 2) * 0x01010101UL;
    uint32_t col0 = (cola & v->split[fa + 1][0]) | (colb & ~v->split[fa + 1][0]);
    uint32_t col1 = (cola & v->split[fa + 1][1]) | (colb & ~v->split[fa + 1][1]);
    int px;
    if (!opaque && !v->tile_any[k][ca] && (fa == 7 || !v->tile_any[k][cb]))
        return;                              /* nothing to draw */
    for (px = 0; px < 8; px++, t += 8) {
        uint32_t w = (((uint32_t)wa[px ^ xa] << 16) | wb[px ^ xb]) << shift;
        unsigned hi = (unsigned)(w >> 24), lo = (unsigned)(w >> 16) & 0xff;
        if (opaque) {
            STORE32(t, v->xpix[hi] | col0);
            STORE32(t + 4, v->xpix[lo] | col1);
        } else {
            uint32_t m0 = v->xmask[hi], m1 = v->xmask[lo];
            if (m0)
                STORE32(t, (LOAD32(t) & ~m0) | ((v->xpix[hi] | col0) & m0));
            if (m1)
                STORE32(t + 4, (LOAD32(t + 4) & ~m1) | ((v->xpix[lo] | col1) & m1));
        }
    }
}

#ifdef VIDEO_ASM
/* video_68k.s */
void comp_layer_asm(uint8_t *t, const uint16_t *ptile, const uint8_t *tile_any, const uint8_t *code,
                    const uint8_t *attr, const uint8_t *col, uint32_t scroll, uint32_t by, uint32_t bx,
                    uint32_t opaque, const uint32_t *tabs);
#endif

static void comp_layer(const Video *v, int k, const uint8_t *ram, int code_off, int attr_off, int col_off,
                       int scroll, int by, int bx, uint8_t *t, int opaque)
{
#ifdef VIDEO_ASM
    comp_layer_asm(t, v->ptile[k][0][0], v->tile_any[k], ram + code_off, ram + attr_off, ram + col_off,
                   (uint32_t)scroll, (uint32_t)by, (uint32_t)bx, (uint32_t)opaque, v->xpix);
#else
    comp_layer_c(v, k, ram, code_off, attr_off, col_off, scroll, by, bx, t, opaque);
#endif
}

/* the sprites into block (by, bx) */
static void comp_sprites(const Video *v, int by, int bx, uint8_t *t)
{
    const int r0 = by * 8, c0 = bx * 8;
    int i;
    for (i = 0; i < v->nsprites; i++) {
        const struct SprCopy *s = &v->sprites[i];
        int ya = s->sy0 > r0 ? s->sy0 : r0, yb = s->sy1 < r0 + 7 ? s->sy1 : r0 + 7;
        int xa = s->sx0 > c0 ? s->sx0 : c0, xb = s->sx1 < c0 + 7 ? s->sx1 : c0 + 7;
        int y, x;
        if (ya > yb || xa > xb)
            continue;
        for (y = ya; y <= yb; y++) {           /* screen row = arcade x */
            int px = y - s->ox;
            if (s->flipx)
                px = 15 - px;
            for (x = xa; x <= xb; x++) {       /* screen x = 239 - arcade y */
                int py = VIDEO_Y1 - x - s->oy;
                uint8_t p = s->gfx[(s->flipy ? s->h - 1 - py : py) * 16 + px];
                if (p)
                    t[(y - r0) * 8 + (x - c0)] = (uint8_t)(s->color | p);
            }
        }
    }
}

/* ---- whole rows of blocks ----
 *
 * When most blocks of a block row have to be composed (after a scroll change), the row
 * is built as one strip of 8 x 224 pixels, walking the tiles of the column once each
 * instead of twice per block. The strip has 8 pixels of margin on both sides, so that
 * the partly visible first and last tiles can be written whole.
 */
#define STRIP_W 240                      /* 8 + 224 + 8 */
#define ROW_MIN 12                       /* blocks in a row from which the strip is cheaper */

#ifdef VIDEO_ASM
/* video_68k.s */
void strip_layer_asm(uint8_t *strip, const uint16_t *ptile, const uint8_t *tile_any, const uint8_t *code,
                     const uint8_t *attr, const uint8_t *col, uint32_t scroll, uint32_t by, uint32_t opaque,
                     const uint32_t *tabs);
#else
static void strip_layer_c(const Video *v, int k, const uint8_t *code, const uint8_t *attr,
                          const uint8_t *col, int scroll, int by, uint8_t *strip, int opaque)
{
    const int t0 = (VIDEO_Y1 + scroll) & 0xff;     /* tilemap row of screen x 0 */
    int r = t0 >> 3, off = (t0 & 7) + 1;          /* strip index of the first tile's left pixel */
    for (; off < 8 + SCREEN_W; off += 8, r = (r - 1) & 31) {
        const int i = (r << 5) | by;
        const uint8_t a = attr[i];
        const int c = ((a << 8) | code[i]) & 0x1ff;
        const uint16_t *w = v->ptile[k][c][(a >> 3) & 1];
        const int fx = (a & 0x04) ? 7 : 0;
        const uint32_t cl = (uint32_t)((col[i] & 0x3f) << 2) * 0x01010101UL;
        uint8_t *d = strip + off;
        int px;
        if (!opaque && !v->tile_any[k][c])
            continue;
        for (px = 0; px < 8; px++, d += STRIP_W) {
            unsigned hi = w[px ^ fx] >> 8, lo = w[px ^ fx] & 0xff;
            if (opaque) {
                STORE32(d, v->xpix[hi] | cl);
                STORE32(d + 4, v->xpix[lo] | cl);
            } else {
                uint32_t m0 = v->xmask[hi], m1 = v->xmask[lo];
                if (m0)
                    STORE32(d, (LOAD32(d) & ~m0) | ((v->xpix[hi] | cl) & m0));
                if (m1)
                    STORE32(d + 4, (LOAD32(d + 4) & ~m1) | ((v->xpix[lo] | cl) & m1));
            }
        }
    }
}
#endif

static void strip_layer(const Video *v, int k, const uint8_t *ram, int code_off, int attr_off, int col_off,
                        int scroll, int by, uint8_t *strip, int opaque)
{
#ifdef VIDEO_ASM
    strip_layer_asm(strip, v->ptile[k][0][0], v->tile_any[k], ram + code_off, ram + attr_off, ram + col_off,
                    (uint32_t)scroll, (uint32_t)by, (uint32_t)opaque, v->xpix);
#else
    strip_layer_c(v, k, ram + code_off, ram + attr_off, ram + col_off, scroll, by, strip, opaque);
#endif
}

/* the sprites into the strip of block row by */
static void strip_sprites(const Video *v, int by, uint8_t *strip)
{
    const int r0 = by * 8;
    int i;
    for (i = 0; i < v->nsprites; i++) {
        const struct SprCopy *s = &v->sprites[i];
        int ya = s->sy0 > r0 ? s->sy0 : r0, yb = s->sy1 < r0 + 7 ? s->sy1 : r0 + 7;
        int y, x;
        if (ya > yb)
            continue;
        for (y = ya; y <= yb; y++) {           /* screen row = arcade x */
            int px = y - s->ox;
            uint8_t *d = strip + (y - r0) * STRIP_W + 8;
            if (s->flipx)
                px = 15 - px;
            for (x = s->sx0; x <= s->sx1; x++) {   /* screen x = 239 - arcade y */
                int py = VIDEO_Y1 - x - s->oy;
                uint8_t p = s->gfx[(s->flipy ? s->h - 1 - py : py) * 16 + px];
                if (p)
                    d[x] = (uint8_t)(s->color | p);
            }
        }
    }
}

/* profiling hook (STATS=FULL on the Amiga): called after each stage of
   video_render_screen() with 0 = tiles checked, 1 = blocks marked, 2 = blocks composed */
void (*video_prof)(int stage);
#define PROF(n) do { if (video_prof) video_prof(n); } while (0)

void video_invalidate(Video *v)
{
    v->prev_priority = -1;
}

int video_render_screen(Video *v, const Vastar *m, uint8_t *screen)
{
    const uint8_t *bg0s = m->fgram + BG0_SCROLL, *bg1s = m->fgram + BG1_SCROLL;
    static uint8_t spr_now[32][28];
    uint32_t tb[16];                         /* the block being composed */
    uint8_t *t = (uint8_t *)tb;
    int by, bx, n = 0;

    if (m->latch & 0x02) {   /* flipped screen (cocktail): the reference renderer */
        video_render(v, m, v->fb);
        video_rotate(v->fb, screen, SCREEN_W);
        memset(v->blk, 1, sizeof v->blk);
        v->prev_priority = -1;   /* everything again after the flip */
        return BLK_ROWS * BLK_COLS;
    }
    update_layer(v, 0, m->bg0ram, BG_CODE, BG_ATTR, BG_COL);
    update_layer(v, 1, m->bg1ram, BG_CODE, BG_ATTR, BG_COL);
    update_layer(v, 2, m->fgram, FG_CODE, FG_ATTR, FG_COL);
    v->cache_valid = 1;
    PROF(0);

    /* the blocks that can have changed */
    memset(spr_now, 0, sizeof spr_now);
    build_sprites(v, m, spr_now);
    if (v->prev_priority != m->priority) {
        memset(v->blk, 1, sizeof v->blk);
        memset(v->tile_dirty, 0, sizeof v->tile_dirty);
    } else {
        for (by = 0; by < BLK_ROWS; by++) {
            int full = bg0s[by] != v->prev_scroll[0][by] || bg1s[by] != v->prev_scroll[1][by];
            for (bx = 0; bx < BLK_COLS; bx++)
                v->blk[by][bx] = (uint8_t)(full | v->spr_blk[by][bx] | spr_now[by][bx]);
        }
        mark_tiles(v, 0, bg0s);
        mark_tiles(v, 1, bg1s);
        mark_tiles(v, 2, 0);
    }
    v->prev_priority = m->priority;
    memcpy(v->prev_scroll[0], bg0s, 32);
    memcpy(v->prev_scroll[1], bg1s, 32);
    memcpy(v->spr_blk, spr_now, sizeof spr_now);
    PROF(1);

    /* compose them; write and keep marked only those that differ from the screen */
#define BG0(op) comp_layer(v, 0, m->bg0ram, BG_CODE, BG_ATTR, BG_COL, bg0s[by], by, bx, t, op)
#define BG1(op) comp_layer(v, 1, m->bg1ram, BG_CODE, BG_ATTR, BG_COL, bg1s[by], by, bx, t, op)
#define FG(op)  comp_layer(v, 2, m->fgram, FG_CODE, FG_ATTR, FG_COL, 0, by, bx, t, op)
#define SPRITES() do { if (spr_now[by][bx]) comp_sprites(v, by, bx, t); } while (0)
    for (by = 0; by < BLK_ROWS; by++) {
        int count = 0, sprites = 0;
        for (bx = 0; bx < BLK_COLS; bx++) {
            count += v->blk[by][bx];
            sprites |= spr_now[by][bx];
        }
        if (count >= ROW_MIN) {
            /* the whole row as one strip */
            static uint32_t strip_l[8 * STRIP_W / 4];
            uint8_t *strip = (uint8_t *)strip_l;
#define SBG0(op) strip_layer(v, 0, m->bg0ram, BG_CODE, BG_ATTR, BG_COL, bg0s[by], by, strip, op)
#define SBG1(op) strip_layer(v, 1, m->bg1ram, BG_CODE, BG_ATTR, BG_COL, bg1s[by], by, strip, op)
#define SFG(op)  strip_layer(v, 2, m->fgram, FG_CODE, FG_ATTR, FG_COL, 0, by, strip, op)
#define SSPR() do { if (sprites) strip_sprites(v, by, strip); } while (0)
            switch (m->priority) {
            case 1: SBG0(1); SBG1(0); SSPR(); SFG(0); break;
            case 2: SBG0(1); SSPR(); SBG0(0); SBG1(0); SFG(0); break;
            case 3: SBG0(1); SBG1(0); SFG(0); SSPR(); break;
            case 4: SFG(1); SSPR(); SBG0(0); SBG1(0); break;
            default: SBG0(1); SSPR(); SBG1(0); SFG(0); break;
            }
#undef SBG0
#undef SBG1
#undef SFG
#undef SSPR
            for (bx = 0; bx < BLK_COLS; bx++) {
                const uint8_t *src = strip + 8 + bx * 8;
                uint8_t *d = screen + by * 8 * SCREEN_W + bx * 8;
                int y, same = 1;
                if (!v->blk[by][bx])
                    continue;
                for (y = 0; y < 8; y++)
                    if (LOAD32(d + y * SCREEN_W) != LOAD32(src + y * STRIP_W) ||
                        LOAD32(d + y * SCREEN_W + 4) != LOAD32(src + y * STRIP_W + 4)) {
                        same = 0;
                        break;
                    }
                if (same) {
                    v->blk[by][bx] = 0;
                    continue;
                }
                for (y = 0; y < 8; y++) {
                    STORE32(d + y * SCREEN_W, LOAD32(src + y * STRIP_W));
                    STORE32(d + y * SCREEN_W + 4, LOAD32(src + y * STRIP_W + 4));
                }
                n++;
            }
            continue;
        }
        for (bx = 0; bx < BLK_COLS; bx++) {
            uint8_t *d;
            int y, same = 1;
            if (!v->blk[by][bx])
                continue;
            switch (m->priority) {
            case 1: BG0(1); BG1(0); SPRITES(); FG(0); break;
            case 2: BG0(1); SPRITES(); BG0(0); BG1(0); FG(0); break;
            case 3: BG0(1); BG1(0); FG(0); SPRITES(); break;
            case 4: FG(1); SPRITES(); BG0(0); BG1(0); break;
            default: BG0(1); SPRITES(); BG1(0); FG(0); break;
            }
            d = screen + by * 8 * SCREEN_W + bx * 8;
            for (y = 0; y < 8; y++, d += SCREEN_W)
                if (LOAD32(d) != tb[y * 2] || LOAD32(d + 4) != tb[y * 2 + 1]) {
                    same = 0;
                    break;
                }
            if (same) {
                v->blk[by][bx] = 0;
                continue;
            }
            d = screen + by * 8 * SCREEN_W + bx * 8;
            for (y = 0; y < 8; y++, d += SCREEN_W) {
                STORE32(d, tb[y * 2]);
                STORE32(d + 4, tb[y * 2 + 1]);
            }
            n++;
        }
    }
#undef BG0
#undef BG1
#undef FG
#undef SPRITES
    PROF(2);
    return n;
}
