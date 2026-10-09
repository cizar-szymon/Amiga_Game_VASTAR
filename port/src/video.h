/* Vastar port - video: three tile layers and 16 sprites (Orca OVG-46C)
 *
 * The picture is rendered in the arcade's own orientation into a 256x256 buffer of
 * palette indices (0-255); the visible part is rows 16-239. The monitor is rotated:
 * the frontend shows it turned 90 degrees clockwise (MAME ROT90) as 224x256.
 */
#ifndef VASTAR_VIDEO_H
#define VASTAR_VIDEO_H

#include <stdint.h>
#include "vastar.h"

#define VIDEO_W 256
#define VIDEO_H 256
#define VIDEO_Y0 16          /* first visible row */
#define VIDEO_Y1 239         /* last visible row */
#define SCREEN_W 224         /* rotated screen */
#define SCREEN_H 256

typedef struct Video {
    uint8_t fg[512 * 64];        /* decoded 8x8 tiles, one pixel (0-3) per byte */
    uint8_t bg0[512 * 64];       /* tiles of bg layer 0 (from the bgtiles1 ROM, as in MAME) */
    uint8_t bg1[512 * 64];       /* tiles of bg layer 1 (from the bgtiles0 ROM) */
    uint8_t spr[256 * 256];      /* decoded 16x16 sprites; a 16x32 sprite n = sprites 2n, 2n+1 */
    uint8_t palette[256][3];     /* RGB 0-255 */

    /* video_render_screen(): every 8x8 block is composed straight from the tiles, so
       that the working set fits the caches (no layer bitmaps). Tiles packed 2 bits per
       pixel in the screen orientation: ptile[layer][tile][flip y][arcade x within the
       tile] = 8 pixels from screen left to right, the first in bits 15-14. */
    uint16_t ptile[3][512][2][8];
    uint8_t tile_any[3][512];    /* tile has non-transparent pixels */
    uint32_t xpix[256];          /* 4 packed pixels -> 4 bytes (memory order) */
    uint32_t xmask[256];         /* 4 packed pixels -> 0xff where not transparent */
    uint32_t split[9][2];        /* first n bytes of 8 = 0xff */
    uint8_t shadow[3][3][1024];  /* code, attribute, colour of each tile last frame */
    uint8_t tile_dirty[3][1024]; /* tile changed since the last frame */
    int cache_valid;

    /* the visible sprite copies of this frame, in drawing order */
    struct SprCopy {
        const uint8_t *gfx;
        short ox, oy, h;         /* arcade origin, height */
        short sy0, sy1, sx0, sx1;/* screen rectangle (inclusive) */
        uint8_t color, flipx, flipy;
    } sprites[64];
    int nsprites;

    /* blocks of 8x8 screen pixels: BLK_ROWS rows (= tilemap columns) of BLK_COLS */
    uint8_t blk[32][28];         /* video_render_screen(): the blocks that changed */
    uint8_t spr_blk[32][28];     /* blocks covered by sprites last frame */
    uint8_t prev_scroll[2][32];
    int prev_priority;           /* -1: everything has to be drawn */
    uint8_t fb[VIDEO_W * VIDEO_H];   /* for the slow path (flipped screen) */
} Video;

#define BLK_ROWS 32
#define BLK_COLS 28

void video_init(Video *v, const Vastar *m);

/* the reference renderer: the arcade orientation into fb (256x256) */
void video_render(const Video *v, const Vastar *m, uint8_t *fb);

/* rotate the visible part of fb to a 224x256 screen of palette indices */
void video_rotate(const uint8_t *fb, uint8_t *screen, int pitch);

/* the fast renderer: updates the rotated 224x256 screen (which must keep the previous
   frame) to the same result as video_render + video_rotate. Only the 8x8 blocks that
   can have changed are composed again, and written only if they differ: v->blk marks
   the blocks that changed, the result is their number. */
int video_render_screen(Video *v, const Vastar *m, uint8_t *screen);

/* the next video_render_screen() draws the whole screen */
void video_invalidate(Video *v);

/* profiling: called after each stage of video_render_screen() (NULL = off) */
extern void (*video_prof)(int stage);

#endif
