/* Vastar port - the fast renderer against the reference renderer, frame by frame
 *
 *   video_test ROMDIR [FRAMES]
 *
 * Plays the attract mode and a game (coin at 1200, start at 1260, moving and firing)
 * and compares video_render_screen() (on the Amiga with video_68k.s) with
 * video_render() + video_rotate() after every frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vastar.h"
#include "video.h"

static Vastar m;
static Video v;
static uint8_t fb[VIDEO_W * VIDEO_H];
static uint8_t fast[SCREEN_W * SCREEN_H], ref[SCREEN_W * SCREEN_H];
static int16_t audio[1024];

int main(int argc, char **argv)
{
    const char *src[1];
    long frames = argc > 2 ? atol(argv[2]) : 4000, f, bad = 0;
    unsigned long blocks = 0;

    if (argc < 2) {
        printf("usage: video_test ROMDIR [FRAMES]\n");
        return 20;
    }
    src[0] = argv[1];
    if (vastar_load_roms(&m, vastar_find_game("vastar"), src, 1))
        return 20;
    vastar_init(&m, 22050);
    video_init(&v, &m);

    for (f = 0; f < frames; f++) {
        m.in_system = (uint8_t)((f >= 1200 && f < 1206 ? IN_COIN1 : 0) | (f >= 1260 && f < 1266 ? IN_START1 : 0));
        m.in_p1 = f >= 1400 ? (uint8_t)(((f / 7) % 3 ? IN_BUTTON1 : 0) | ((f / 60) % 4 == 0 ? IN_UP :
                                (f / 60) % 4 == 1 ? IN_RIGHT : (f / 60) % 4 == 2 ? IN_DOWN : IN_LEFT)) : 0;
        vastar_run_frame(&m, audio);
        blocks += (unsigned long)video_render_screen(&v, &m, fast);
        video_render(&v, &m, fb);
        video_rotate(fb, ref, SCREEN_W);
        if (memcmp(fast, ref, sizeof fast)) {
            if (bad < 5) {
                int i;
                for (i = 0; i < SCREEN_W * SCREEN_H && fast[i] == ref[i]; i++)
                    ;
                printf("frame %ld differs, first at x %d y %d: fast %02x ref %02x\n", f,
                       i % SCREEN_W, i / SCREEN_W, fast[i], ref[i]);
            }
            bad++;
        }
        if (f % 500 == 0) {
            printf("frame %ld, %ld bad so far\n", f, bad);
            fflush(stdout);
        }
    }
    printf("%s: %ld frames, %ld with differences, %lu blocks redrawn per frame\n", bad ? "FAILED" : "OK",
           frames, bad, blocks / (unsigned long)frames);
    return bad ? 10 : 0;
}
