/* Vastar port - where do the two Z80s spend their time?
 *
 *   profile ROMDIR [FRAMES]
 *
 * Runs the attract mode and a game (coin at 1200, start at 1260, then fire and
 * movement) and samples the PC of both CPUs every 64 cycles. Prints the hottest
 * addresses and the instructions executed per frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vastar.h"

static Vastar m;
static unsigned long hits[2][65536];

static int cmp_desc(const void *a, const void *b)
{
    unsigned long x = *(const unsigned long *)a >> 16, y = *(const unsigned long *)b >> 16;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void top(const char *name, unsigned long *h, unsigned long total)
{
    static unsigned long list[65536];
    int i;
    for (i = 0; i < 65536; i++)
        list[i] = (h[i] << 16) | (unsigned long)i;
    qsort(list, 65536, sizeof list[0], cmp_desc);
    printf("%s: hottest addresses (%% of samples)\n", name);
    for (i = 0; i < 24; i++)
        printf("  %04lx %5.1f%%\n", list[i] & 0xffff, 100.0 * (double)(list[i] >> 16) / (double)total);
}

int main(int argc, char **argv)
{
    const char *src[1];
    long frames = argc > 2 ? atol(argv[2]) : 4000, f;
    unsigned long total = 0;
    int slice;

    if (argc < 2) {
        fprintf(stderr, "usage: profile ROMDIR [FRAMES]\n");
        return 2;
    }
    src[0] = argv[1];
    if (vastar_load_roms(&m, vastar_find_game("vastar"), src, 1))
        return 1;
    vastar_init(&m, 22050);

    for (f = 0; f < frames; f++) {
        m.in_system = (f >= 1200 && f < 1206 ? IN_COIN1 : 0) | (f >= 1260 && f < 1266 ? IN_START1 : 0);
        m.in_p1 = f >= 1400 ? (uint8_t)(IN_BUTTON1 | ((f / 60) % 2 ? IN_UP : IN_DOWN)) : 0;
        m.smem[0x8040] = m.in_p1;
        m.smem[0x8080] = m.in_system;
        /* like vastar_run_frame, but sampling the PCs */
        for (slice = 0; slice < VASTAR_LINES / 6; slice++) {
            int line = (240 + slice * 6) % VASTAR_LINES, c;
            if (line == 240 && (m.latch & 1))
                z80_nmi(&m.main);
            if (line % 66 == 0 && (m.latch & 4))
                z80_set_irq(&m.sub, 1, 1);
            for (c = 0; c < 6 * 192; c += 64) {
                z80_run(&m.main, 64);
                if (m.latch & 4)
                    z80_run(&m.sub, 64);
                if (f >= 1300) {
                    hits[0][m.main.pc]++;
                    hits[1][m.sub.pc]++;
                    total++;
                }
            }
        }
    }
    top("main CPU", hits[0], total);
    top("sub CPU", hits[1], total);
    return 0;
}
