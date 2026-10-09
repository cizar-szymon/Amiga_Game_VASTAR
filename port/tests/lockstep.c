/* Vastar port - lockstep test of the 68k assembler Z80 core against the C core (Amiga)
 *
 *   lockstep ROMDIR [FRAMES]
 *
 * Two machines run the same game (attract mode, coin at 1200, start at 1260, then
 * moving and firing): one with z80_run (z80_68k.s), one with z80c_run (z80.c).
 * After every instruction the CPU states are compared, after every frame the memory
 * of both CPUs and the AY registers. The first difference is printed.
 * The X/Y flags are compared too, except after BIT n,(HL) (the assembler core does
 * not keep MEMPTR).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vastar.h"

static Vastar ma, mb;
static unsigned long steps, xy_skips;

static void dump(const char *name, const Z80 *z)
{
    printf("  %s: PC %04x SP %04x A %02x F %02x BC %02x%02x DE %02x%02x HL %02x%02x IX %04x IY %04x\n"
           "      AF' %02x%02x BC' %02x%02x DE' %02x%02x HL' %02x%02x I %02x R %02x/%02x IFF %d%d IM %d"
           " halt %d irq %d nmi %d ei %d icount %ld\n",
           name, z->pc, z->sp, z->a, z->f, z->b, z->c, z->d, z->e, z->h, z->l, z->ix, z->iy,
           z->a2, z->f2, z->b2, z->c2, z->d2, z->e2, z->h2, z->l2, z->i, z->r & 0x7f, z->r7,
           z->iff1, z->iff2, z->im, z->halted, z->irq_line, z->nmi_pending, z->after_ei, (long)z->icount);
}

/* 1 = same */
static int same(const Z80 *a, const Z80 *b, int bit_hl)
{
    uint8_t fmask = (uint8_t)(bit_hl ? 0xd7 : 0xff);
    if (bit_hl && ((a->f ^ b->f) & 0x28))
        xy_skips++;
    return a->a == b->a && ((a->f ^ b->f) & fmask) == 0 && a->b == b->b && a->c == b->c &&
           a->d == b->d && a->e == b->e && a->h == b->h && a->l == b->l &&
           a->a2 == b->a2 && a->f2 == b->f2 && a->b2 == b->b2 && a->c2 == b->c2 &&
           a->d2 == b->d2 && a->e2 == b->e2 && a->h2 == b->h2 && a->l2 == b->l2 &&
           a->ix == b->ix && a->iy == b->iy && a->sp == b->sp && a->pc == b->pc &&
           a->i == b->i && (a->r & 0x7f) == (b->r & 0x7f) && a->r7 == b->r7 &&
           a->iff1 == b->iff1 && a->iff2 == b->iff2 && a->im == b->im && a->halted == b->halted &&
           a->irq_line == b->irq_line && a->nmi_pending == b->nmi_pending &&
           a->after_ei == b->after_ei;
}

/* run one CPU of both machines for `cycles`, instruction by instruction */
static int step_cpu(Z80 *za, Z80 *zb, int cycles, long frame, const char *name)
{
    int used = 0;
    while (used < cycles) {
        uint16_t pc = za->pc;
        const uint8_t *m = za->mem;
        uint8_t op0 = m[pc], op1 = m[(uint16_t)(pc + 1)], op2 = m[(uint16_t)(pc + 2)], op3 = m[(uint16_t)(pc + 3)];
        int bit_hl = op0 == 0xcb && (op1 & 0xc7) == 0x46;
        int ca, cb;
        static Z80 before;
        memcpy(&before, zb, 48);   /* the registers only (the struct is big) */
        za->icount = 0;
        zb->icount = 0;
        ca = z80_run(za, 1);
        cb = z80c_run(zb, 1);
        steps++;
        if (ca != cb || !same(za, zb, bit_hl)) {
            printf("DIFFERENCE after step %lu, frame %ld, %s CPU, instruction at %04x: %02x %02x %02x %02x\n"
                   "  cycles asm %d, C %d\n", steps, frame, name, pc, op0, op1, op2, op3, ca, cb);
            dump("before", &before);
            dump("asm   ", za);
            dump("C     ", zb);
            return -1;
        }
        if (bit_hl)
            za->f = zb->f;   /* carry the C core's X/Y on, they are not compared here */
        if (ca <= 0)
            ca = 1;   /* halted with nothing to do: the C core reports the burnt cycles, so does asm */
        used += ca;
    }
    return 0;
}

static int compare_memory(long frame)
{
    int i;
    for (i = 0; i < 0x10000; i++)
        if (ma.mmem[i] != mb.mmem[i]) {
            printf("DIFFERENCE in main memory at %04x after frame %ld: asm %02x C %02x\n", i, frame,
                   ma.mmem[i], mb.mmem[i]);
            return -1;
        }
    for (i = 0; i < 0x10000; i++)
        if (ma.smem[i] != mb.smem[i]) {
            printf("DIFFERENCE in sub memory at %04x after frame %ld: asm %02x C %02x\n", i, frame,
                   ma.smem[i], mb.smem[i]);
            return -1;
        }
    if (memcmp(ma.ay.regs, mb.ay.regs, 16) || ma.latch != mb.latch || ma.priority != mb.priority) {
        printf("DIFFERENCE in AY / latch / priority after frame %ld\n", frame);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *src[1];
    long frames = argc > 2 ? atol(argv[2]) : 3000, f;
    int slice;

    if (argc < 2) {
        printf("usage: lockstep ROMDIR [FRAMES]\n");
        return 20;
    }
    src[0] = argv[1];
    if (vastar_load_roms(&ma, vastar_find_game("vastar"), src, 1) ||
        vastar_load_roms(&mb, vastar_find_game("vastar"), src, 1))
        return 20;
    vastar_init(&ma, 22050);
    vastar_init(&mb, 22050);

    for (f = 0; f < frames; f++) {
        Vastar *m[2];
        int k;
        m[0] = &ma;
        m[1] = &mb;
        for (k = 0; k < 2; k++) {
            m[k]->in_system = (uint8_t)((f >= 1200 && f < 1206 ? IN_COIN1 : 0) | (f >= 1260 && f < 1266 ? IN_START1 : 0));
            m[k]->in_p1 = f >= 1400 ? (uint8_t)(((f / 7) % 3 ? IN_BUTTON1 : 0) | ((f / 60) % 4 == 0 ? IN_UP :
                                      (f / 60) % 4 == 1 ? IN_RIGHT : (f / 60) % 4 == 2 ? IN_DOWN : IN_LEFT)) : 0;
            m[k]->smem[0x8000] = m[k]->in_p2;
            m[k]->smem[0x8040] = m[k]->in_p1;
            m[k]->smem[0x8080] = m[k]->in_system;
        }
        for (slice = 0; slice < VASTAR_LINES / 6; slice++) {
            int line = (240 + slice * 6) % VASTAR_LINES;
            for (k = 0; k < 2; k++) {
                if (line == 240 && (m[k]->latch & 1))
                    z80_nmi(&m[k]->main);
                if (line % 66 == 0 && (m[k]->latch & 4))
                    z80_set_irq(&m[k]->sub, 1, 1);
            }
            if (step_cpu(&ma.main, &mb.main, 6 * 192, f, "main"))
                return 10;
            if ((ma.latch & 4) != (mb.latch & 4)) {
                printf("DIFFERENCE: sub CPU reset line, frame %ld\n", f);
                return 10;
            }
            if ((ma.latch & 4) && step_cpu(&ma.sub, &mb.sub, 6 * 192, f, "sub"))
                return 10;
        }
        if (compare_memory(f))
            return 10;
        if (f % 250 == 0) {
            printf("frame %ld ok, %lu instructions\n", f, steps);
            fflush(stdout);
        }
    }
    printf("OK: %ld frames, %lu instructions identical (%lu X/Y differences after BIT n,(HL) ignored)\n",
           frames, steps, xy_skips);
    return 0;
}
