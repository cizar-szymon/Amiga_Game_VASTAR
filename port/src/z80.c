/* Vastar port - Z80 CPU emulator, see z80.h */
#include <stddef.h>
#include <string.h>
#include "z80.h"

#ifdef Z80_ASM
#define z80_run z80c_run        /* z80_68k.s provides z80_run */
/* the offsets z80_68k.s relies on */
typedef char check_ix[offsetof(Z80, ix) == 16 ? 1 : -1];
typedef char check_icount[offsetof(Z80, icount) == 40 ? 1 : -1];
typedef char check_mem[offsetof(Z80, mem) == 52 ? 1 : -1];
typedef char check_wtype[offsetof(Z80, wtype) == 72 ? 1 : -1];
typedef char check_wmirror[offsetof(Z80, wmirror) == 328 ? 1 : -1];
#endif

#define CF 0x01
#define NF 0x02
#define PF 0x04
#define XF 0x08
#define HF 0x10
#define YF 0x20
#define ZF 0x40
#define SF 0x80
#define XYF (XF | YF)

/* tables (also used by z80_68k.s): S Z X Y of a value, the same with parity,
   INC / DEC flags by result, 68k CCR (N Z V C) -> Z80 flags after add / sub */
uint8_t z80_tables[1056];
uint16_t z80_daa[0x1400];     /* (F & 0x13) << 8 | A -> A << 8 | F after DAA */
#define szxy (z80_tables)
#define szxyp (z80_tables + 256)
#define incf (z80_tables + 512)
#define decf (z80_tables + 768)
#define ccr_add (z80_tables + 1024)
#define ccr_sub (z80_tables + 1040)
static int tables_ready;

/* B C D E H L - A as offsets into the register file */
static const uint8_t reg_off[8] = { 2, 3, 4, 5, 6, 7, 0, 0 };
#define REG(z, i) (*((uint8_t *)(z) + reg_off[i]))

static const uint8_t cyc_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4,  4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4, 12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4,  7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4,  7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11,  5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11,  5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11,  5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11,  5, 6,10, 4,10, 0, 7,11
};

static void daa(struct Z80 *z);

static void init_tables(void)
{
    int i, j, p;
    for (i = 0; i < 256; i++) {
        p = 0;
        for (j = 0; j < 8; j++)
            p ^= (i >> j) & 1;
        szxy[i] = (uint8_t)((i & (SF | XYF)) | (i == 0 ? ZF : 0));
        szxyp[i] = (uint8_t)(szxy[i] | (p ? 0 : PF));
        incf[i] = (uint8_t)(szxy[i] | ((i & 0x0f) == 0 ? HF : 0) | (i == 0x80 ? PF : 0));
        decf[i] = (uint8_t)(szxy[i] | NF | ((i & 0x0f) == 0x0f ? HF : 0) | (i == 0x7f ? PF : 0));
    }
    for (i = 0; i < 16; i++) {          /* 68k CCR: N = 8, Z = 4, V = 2, C = 1 */
        uint8_t f = (uint8_t)(((i & 8) ? SF : 0) | ((i & 4) ? ZF : 0) | ((i & 2) ? PF : 0) | ((i & 1) ? CF : 0));
        ccr_add[i] = f;
        ccr_sub[i] = (uint8_t)(f | NF);
    }
    for (i = 0; i < 0x1400; i++) {
        Z80 t;
        if ((i >> 8) & ~0x13)
            continue;
        t.a = (uint8_t)i;
        t.f = (uint8_t)(i >> 8);
        daa(&t);
        z80_daa[i] = (uint16_t)((t.a << 8) | t.f);
    }
    tables_ready = 1;
}

/* ---- memory ---- */

static uint8_t rd(Z80 *z, uint16_t a)
{
    return z->mem[a];
}

static void wr(Z80 *z, uint16_t a, uint8_t v)
{
    switch (z->wtype[a >> 8]) {
    case Z80_RAM:
        z->mem[a] = v;
        break;
    case Z80_ROM:
        break;
    case Z80_MIRROR:
        z->mem[a] = v;
        z->wmirror[a >> 8][a & 0xff] = v;
        break;
    default:
        z->write(z->ctx, a, v);
        break;
    }
}

static uint8_t fetch(Z80 *z)
{
    return rd(z, z->pc++);
}

static uint16_t fetch16(Z80 *z)
{
    uint16_t lo = fetch(z);
    return (uint16_t)(lo | (fetch(z) << 8));
}

static uint16_t rd16(Z80 *z, uint16_t a)
{
    uint16_t lo = rd(z, a);
    return (uint16_t)(lo | (rd(z, (uint16_t)(a + 1)) << 8));
}

static void wr16(Z80 *z, uint16_t a, uint16_t v)
{
    wr(z, a, (uint8_t)v);
    wr(z, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static void push(Z80 *z, uint16_t v)
{
    z->sp--;
    wr(z, z->sp, (uint8_t)(v >> 8));
    z->sp--;
    wr(z, z->sp, (uint8_t)v);
}

static uint16_t pop(Z80 *z)
{
    uint16_t lo = rd(z, z->sp++);
    return (uint16_t)(lo | (rd(z, z->sp++) << 8));
}

static uint8_t def_read(void *ctx, uint16_t a) { (void)ctx; (void)a; return 0xff; }
static void def_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; (void)a; (void)v; }

#define BC ((uint16_t)((z->b << 8) | z->c))
#define DE ((uint16_t)((z->d << 8) | z->e))
#define HL ((uint16_t)((z->h << 8) | z->l))
#define SET_BC(v) do { uint16_t v_ = (uint16_t)(v); z->b = (uint8_t)(v_ >> 8); z->c = (uint8_t)v_; } while (0)
#define SET_DE(v) do { uint16_t v_ = (uint16_t)(v); z->d = (uint8_t)(v_ >> 8); z->e = (uint8_t)v_; } while (0)
#define SET_HL(v) do { uint16_t v_ = (uint16_t)(v); z->h = (uint8_t)(v_ >> 8); z->l = (uint8_t)v_; } while (0)

/* ---- ALU ---- */

static void add8(Z80 *z, uint8_t v, int carry)
{
    unsigned r = (unsigned)z->a + v + (unsigned)carry;
    z->f = (uint8_t)(szxy[r & 0xff] | ((z->a ^ v ^ r) & HF) |
                     (((z->a ^ ~v) & (z->a ^ r) & 0x80) >> 5) | ((r >> 8) & CF));
    z->a = (uint8_t)r;
}

static uint8_t sub8(Z80 *z, uint8_t v, int carry)
{
    unsigned r = (unsigned)z->a - v - (unsigned)carry;
    z->f = (uint8_t)((szxy[r & 0xff] & ~XYF) | (r & XYF) | ((z->a ^ v ^ r) & HF) |
                     (((z->a ^ v) & (z->a ^ r) & 0x80) >> 5) | NF | ((r >> 8) & CF));
    return (uint8_t)r;
}

static void alu(Z80 *z, int op, uint8_t v)
{
    switch (op) {
    case 0: add8(z, v, 0); break;
    case 1: add8(z, v, z->f & CF); break;
    case 2: z->a = sub8(z, v, 0); break;
    case 3: z->a = sub8(z, v, z->f & CF); break;
    case 4: z->a &= v; z->f = (uint8_t)(szxyp[z->a] | HF); break;
    case 5: z->a ^= v; z->f = szxyp[z->a]; break;
    case 6: z->a |= v; z->f = szxyp[z->a]; break;
    default:  /* CP: X and Y come from the operand */
        sub8(z, v, 0);
        z->f = (uint8_t)((z->f & ~XYF) | (v & XYF));
        break;
    }
}

static uint8_t inc8(Z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    z->f = (uint8_t)((z->f & CF) | szxy[r] | ((r & 0x0f) == 0 ? HF : 0) | (r == 0x80 ? PF : 0));
    return r;
}

static uint8_t dec8(Z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    z->f = (uint8_t)((z->f & CF) | NF | szxy[r] | ((v & 0x0f) == 0 ? HF : 0) | (r == 0x7f ? PF : 0));
    return r;
}

static uint16_t add16(Z80 *z, uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    z->wz = (uint16_t)(a + 1);
    z->f = (uint8_t)((z->f & (SF | ZF | PF)) | ((r >> 8) & XYF) | (((a ^ b ^ r) >> 8) & HF) | (r >> 16));
    return (uint16_t)r;
}

static void adc16(Z80 *z, uint16_t b)
{
    uint16_t a = HL;
    uint32_t r = (uint32_t)a + b + (z->f & CF);
    z->wz = (uint16_t)(a + 1);
    z->f = (uint8_t)(((r >> 8) & (SF | XYF)) | ((r & 0xffff) ? 0 : ZF) | (((a ^ b ^ r) >> 8) & HF) |
                     (((a ^ ~b) & (a ^ r) & 0x8000) >> 13) | ((r >> 16) & CF));
    SET_HL(r);
}

static void sbc16(Z80 *z, uint16_t b)
{
    uint16_t a = HL;
    uint32_t r = (uint32_t)a - b - (z->f & CF);
    z->wz = (uint16_t)(a + 1);
    z->f = (uint8_t)(((r >> 8) & (SF | XYF)) | ((r & 0xffff) ? 0 : ZF) | (((a ^ b ^ r) >> 8) & HF) |
                     (((a ^ b) & (a ^ r) & 0x8000) >> 13) | NF | ((r >> 16) & CF));
    SET_HL(r);
}

static void daa(struct Z80 *z)
{
    uint8_t a = z->a, diff = 0, carry = z->f & CF;
    if ((z->f & HF) || (a & 0x0f) > 9)
        diff = 0x06;
    if (carry || a > 0x99) {
        diff |= 0x60;
        carry = CF;
    }
    if (z->f & NF) {
        z->a = (uint8_t)(a - diff);
        z->f = (uint8_t)(szxyp[z->a] | NF | carry | (((z->f & HF) && (a & 0x0f) < 6) ? HF : 0));
    } else {
        z->a = (uint8_t)(a + diff);
        z->f = (uint8_t)(szxyp[z->a] | carry | ((a & 0x0f) > 9 ? HF : 0));
    }
}

static int cond(Z80 *z, int cc)
{
    switch (cc) {
    case 0: return !(z->f & ZF);
    case 1: return z->f & ZF;
    case 2: return !(z->f & CF);
    case 3: return z->f & CF;
    case 4: return !(z->f & PF);
    case 5: return z->f & PF;
    case 6: return !(z->f & SF);
    default: return z->f & SF;
    }
}

/* CB rotations and shifts */
static uint8_t rot(Z80 *z, int op, uint8_t v)
{
    uint8_t r, c;
    switch (op) {
    case 0: c = v >> 7; r = (uint8_t)((v << 1) | c); break;                 /* RLC */
    case 1: c = v & 1; r = (uint8_t)((v >> 1) | (c << 7)); break;            /* RRC */
    case 2: c = v >> 7; r = (uint8_t)((v << 1) | (z->f & CF)); break;        /* RL */
    case 3: c = v & 1; r = (uint8_t)((v >> 1) | ((z->f & CF) << 7)); break;  /* RR */
    case 4: c = v >> 7; r = (uint8_t)(v << 1); break;                        /* SLA */
    case 5: c = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;          /* SRA */
    case 6: c = v >> 7; r = (uint8_t)((v << 1) | 1); break;                  /* SLL */
    default: c = v & 1; r = (uint8_t)(v >> 1); break;                        /* SRL */
    }
    z->f = (uint8_t)(szxyp[r] | c);
    return r;
}

/* BIT n,v; xy = source of the X and Y flags */
static void bit(Z80 *z, int n, uint8_t v, uint8_t xy)
{
    uint8_t f = (uint8_t)((z->f & CF) | HF | (xy & XYF));
    if (v & (1 << n)) {
        if (n == 7)
            f |= SF;
    } else {
        f |= ZF | PF;
    }
    z->f = f;
}

/* ---- prefixed instructions ---- */

static void exec_cb(Z80 *z)
{
    uint8_t op = fetch(z), v;
    int idx = op & 7, n = (op >> 3) & 7;
    z->r++;
    if (idx == 6) {
        uint16_t a = HL;
        v = rd(z, a);
        switch (op >> 6) {
        case 0: wr(z, a, rot(z, n, v)); z->icount -= 15; break;
        case 1: bit(z, n, v, (uint8_t)(z->wz >> 8)); z->icount -= 12; break;
        case 2: wr(z, a, (uint8_t)(v & ~(1 << n))); z->icount -= 15; break;
        default: wr(z, a, (uint8_t)(v | (1 << n))); z->icount -= 15; break;
        }
    } else {
        uint8_t *p = &REG(z, idx);
        z->icount -= 8;
        switch (op >> 6) {
        case 0: *p = rot(z, n, *p); break;
        case 1: bit(z, n, *p, *p); break;
        case 2: *p = (uint8_t)(*p & ~(1 << n)); break;
        default: *p = (uint8_t)(*p | (1 << n)); break;
        }
    }
}

/* DDCB / FDCB: the result is also copied to a register (undocumented) unless idx == 6 */
static void exec_xycb(Z80 *z, uint16_t a)
{
    uint8_t op = fetch(z), v = rd(z, a), r;
    int idx = op & 7, n = (op >> 3) & 7;
    z->wz = a;
    switch (op >> 6) {
    case 0: r = rot(z, n, v); break;
    case 1: bit(z, n, v, (uint8_t)(a >> 8)); z->icount -= 20; return;
    case 2: r = (uint8_t)(v & ~(1 << n)); break;
    default: r = (uint8_t)(v | (1 << n)); break;
    }
    wr(z, a, r);
    if (idx != 6)
        REG(z, idx) = r;
    z->icount -= 23;
}

static void block_ld(Z80 *z, int dir)
{
    uint8_t v = rd(z, HL), n;
    uint16_t bc = (uint16_t)(BC - 1);
    wr(z, DE, v);
    SET_HL(HL + dir);
    SET_DE(DE + dir);
    SET_BC(bc);
    n = (uint8_t)(v + z->a);
    z->f = (uint8_t)((z->f & (SF | ZF | CF)) | (bc ? PF : 0) | (n & XF) | ((n & 0x02) << 4));
}

static void block_cp(Z80 *z, int dir)
{
    uint8_t v = rd(z, HL), r = (uint8_t)(z->a - v), n;
    uint16_t bc = (uint16_t)(BC - 1);
    SET_HL(HL + dir);
    SET_BC(bc);
    z->wz = (uint16_t)(z->wz + dir);
    z->f = (uint8_t)((z->f & CF) | NF | (szxy[r] & (SF | ZF)) | ((z->a ^ v ^ r) & HF) | (bc ? PF : 0));
    n = (uint8_t)(r - ((z->f & HF) ? 1 : 0));
    z->f |= (uint8_t)((n & XF) | ((n & 0x02) << 4));
}

static void block_io_flags(Z80 *z, uint8_t v, unsigned k)
{
    z->f = (uint8_t)(szxy[z->b] | ((v & 0x80) ? NF : 0) | (k > 0xff ? (HF | CF) : 0) |
                     (szxyp[(uint8_t)((k & 7) ^ z->b)] & PF));
}

static void block_in(Z80 *z, int dir)
{
    uint8_t v = z->in(z->ctx, BC);
    z->wz = (uint16_t)(BC + dir);
    wr(z, HL, v);
    z->b--;
    SET_HL(HL + dir);
    block_io_flags(z, v, (unsigned)v + (uint8_t)(z->c + dir));
}

static void block_out(Z80 *z, int dir)
{
    uint8_t v = rd(z, HL);
    z->b--;
    z->wz = (uint16_t)(BC + dir);
    z->out(z->ctx, BC, v);
    SET_HL(HL + dir);
    block_io_flags(z, v, (unsigned)v + z->l);
}

static uint16_t get_rr(Z80 *z, int n)   /* BC DE HL SP */
{
    switch (n) {
    case 0: return BC;
    case 1: return DE;
    case 2: return HL;
    default: return z->sp;
    }
}

static void set_rr(Z80 *z, int n, uint16_t v)
{
    switch (n) {
    case 0: SET_BC(v); break;
    case 1: SET_DE(v); break;
    case 2: SET_HL(v); break;
    default: z->sp = v; break;
    }
}

static void exec_ed(Z80 *z)
{
    uint8_t op = fetch(z), v;
    uint16_t w;
    int n = (op >> 4) & 3;
    z->r++;
    if (op >= 0x40 && op < 0x80) {
        switch (op & 7) {
        case 0:  /* IN r,(C) */
            v = z->in(z->ctx, BC);
            z->wz = (uint16_t)(BC + 1);
            z->f = (uint8_t)((z->f & CF) | szxyp[v]);
            if (op != 0x70)
                REG(z, (op >> 3) & 7) = v;
            z->icount -= 12;
            return;
        case 1:  /* OUT (C),r */
            z->out(z->ctx, BC, op == 0x71 ? 0 : REG(z, (op >> 3) & 7));
            z->wz = (uint16_t)(BC + 1);
            z->icount -= 12;
            return;
        case 2:
            if (op & 8)
                adc16(z, get_rr(z, n));
            else
                sbc16(z, get_rr(z, n));
            z->icount -= 15;
            return;
        case 3:
            w = fetch16(z);
            if (op & 8)
                set_rr(z, n, rd16(z, w));
            else
                wr16(z, w, get_rr(z, n));
            z->wz = (uint16_t)(w + 1);
            z->icount -= 20;
            return;
        case 4:  /* NEG */
            v = z->a;
            z->a = 0;
            z->a = sub8(z, v, 0);
            z->icount -= 8;
            return;
        case 5:  /* RETN / RETI */
            z->iff1 = z->iff2;
            z->pc = pop(z);
            z->wz = z->pc;
            z->icount -= 14;
            return;
        case 6:
            z->im = (uint8_t)((op & 0x10) ? ((op & 8) ? 2 : 1) : 0);
            z->icount -= 8;
            return;
        default:
            switch (op) {
            case 0x47: z->i = z->a; z->icount -= 9; return;
            case 0x4f: z->r = z->a; z->r7 = z->a & 0x80; z->icount -= 9; return;
            case 0x57:
                z->a = z->i;
                z->f = (uint8_t)((z->f & CF) | szxy[z->a] | (z->iff2 ? PF : 0));
                z->icount -= 9;
                return;
            case 0x5f:
                z->a = (uint8_t)((z->r & 0x7f) | z->r7);
                z->f = (uint8_t)((z->f & CF) | szxy[z->a] | (z->iff2 ? PF : 0));
                z->icount -= 9;
                return;
            case 0x67:  /* RRD */
                v = rd(z, HL);
                wr(z, HL, (uint8_t)((z->a << 4) | (v >> 4)));
                z->a = (uint8_t)((z->a & 0xf0) | (v & 0x0f));
                z->f = (uint8_t)((z->f & CF) | szxyp[z->a]);
                z->wz = (uint16_t)(HL + 1);
                z->icount -= 18;
                return;
            case 0x6f:  /* RLD */
                v = rd(z, HL);
                wr(z, HL, (uint8_t)((v << 4) | (z->a & 0x0f)));
                z->a = (uint8_t)((z->a & 0xf0) | (v >> 4));
                z->f = (uint8_t)((z->f & CF) | szxyp[z->a]);
                z->wz = (uint16_t)(HL + 1);
                z->icount -= 18;
                return;
            default:   /* ED 77, ED 7F */
                z->icount -= 8;
                return;
            }
        }
    }
    switch (op) {
    case 0xa0: block_ld(z, 1); z->icount -= 16; return;
    case 0xa8: block_ld(z, -1); z->icount -= 16; return;
    case 0xb0:
    case 0xb8:
        block_ld(z, op == 0xb0 ? 1 : -1);
        z->icount -= 16;
        if (z->b | z->c) {
            z->pc -= 2;
            z->wz = (uint16_t)(z->pc + 1);
            z->icount -= 5;
        }
        return;
    case 0xa1: block_cp(z, 1); z->icount -= 16; return;
    case 0xa9: block_cp(z, -1); z->icount -= 16; return;
    case 0xb1:
    case 0xb9:
        block_cp(z, op == 0xb1 ? 1 : -1);
        z->icount -= 16;
        if ((z->b | z->c) && !(z->f & ZF)) {
            z->pc -= 2;
            z->wz = (uint16_t)(z->pc + 1);
            z->icount -= 5;
        }
        return;
    case 0xa2: block_in(z, 1); z->icount -= 16; return;
    case 0xaa: block_in(z, -1); z->icount -= 16; return;
    case 0xb2:
    case 0xba:
        block_in(z, op == 0xb2 ? 1 : -1);
        z->icount -= 16;
        if (z->b) {
            z->pc -= 2;
            z->icount -= 5;
        }
        return;
    case 0xa3: block_out(z, 1); z->icount -= 16; return;
    case 0xab: block_out(z, -1); z->icount -= 16; return;
    case 0xb3:
    case 0xbb:
        block_out(z, op == 0xb3 ? 1 : -1);
        z->icount -= 16;
        if (z->b) {
            z->pc -= 2;
            z->icount -= 5;
        }
        return;
    default:   /* undefined ED xx: 8 cycle NOP */
        z->icount -= 8;
        return;
    }
}

static void exec_main(Z80 *z, uint8_t op);

/* DD / FD prefix: HL -> IX/IY, H/L -> IXH/IXL, (HL) -> (IX+d) */
static void exec_index(Z80 *z, uint16_t *ix)
{
    uint8_t op = fetch(z), v;
    uint16_t a, w;
    z->r++;

    if (op >= 0x40 && op < 0xc0 && op != 0x76) {
        int src = op & 7, dst = (op >> 3) & 7;
        if (src == 6 || (op < 0x80 && dst == 6)) {
            /* (IX+d) operand, the other register is the real H/L */
            a = (uint16_t)(*ix + (int8_t)fetch(z));
            z->wz = a;
            z->icount -= 19;
            if (op >= 0x80)
                alu(z, dst, rd(z, a));
            else if (src == 6)
                REG(z, dst) = rd(z, a);
            else
                wr(z, a, REG(z, src));
            return;
        }
        z->icount -= 8;
        if (src == 4) v = (uint8_t)(*ix >> 8);
        else if (src == 5) v = (uint8_t)*ix;
        else v = REG(z, src);
        if (op >= 0x80)
            alu(z, dst, v);
        else if (dst == 4)
            *ix = (uint16_t)((*ix & 0x00ff) | (v << 8));
        else if (dst == 5)
            *ix = (uint16_t)((*ix & 0xff00) | v);
        else
            REG(z, dst) = v;
        return;
    }

    switch (op) {
    case 0x09: *ix = add16(z, *ix, BC); z->icount -= 15; return;
    case 0x19: *ix = add16(z, *ix, DE); z->icount -= 15; return;
    case 0x29: *ix = add16(z, *ix, *ix); z->icount -= 15; return;
    case 0x39: *ix = add16(z, *ix, z->sp); z->icount -= 15; return;
    case 0x21: *ix = fetch16(z); z->icount -= 14; return;
    case 0x22: w = fetch16(z); wr16(z, w, *ix); z->wz = (uint16_t)(w + 1); z->icount -= 20; return;
    case 0x2a: w = fetch16(z); *ix = rd16(z, w); z->wz = (uint16_t)(w + 1); z->icount -= 20; return;
    case 0x23: (*ix)++; z->icount -= 10; return;
    case 0x2b: (*ix)--; z->icount -= 10; return;
    case 0x24: *ix = (uint16_t)((*ix & 0xff) | (inc8(z, (uint8_t)(*ix >> 8)) << 8)); z->icount -= 8; return;
    case 0x25: *ix = (uint16_t)((*ix & 0xff) | (dec8(z, (uint8_t)(*ix >> 8)) << 8)); z->icount -= 8; return;
    case 0x26: *ix = (uint16_t)((*ix & 0xff) | (fetch(z) << 8)); z->icount -= 11; return;
    case 0x2c: *ix = (uint16_t)((*ix & 0xff00) | inc8(z, (uint8_t)*ix)); z->icount -= 8; return;
    case 0x2d: *ix = (uint16_t)((*ix & 0xff00) | dec8(z, (uint8_t)*ix)); z->icount -= 8; return;
    case 0x2e: *ix = (uint16_t)((*ix & 0xff00) | fetch(z)); z->icount -= 11; return;
    case 0x34:
    case 0x35:
        a = (uint16_t)(*ix + (int8_t)fetch(z));
        z->wz = a;
        v = rd(z, a);
        wr(z, a, op == 0x34 ? inc8(z, v) : dec8(z, v));
        z->icount -= 23;
        return;
    case 0x36:
        a = (uint16_t)(*ix + (int8_t)fetch(z));
        z->wz = a;
        wr(z, a, fetch(z));
        z->icount -= 19;
        return;
    case 0xcb:
        a = (uint16_t)(*ix + (int8_t)fetch(z));
        exec_xycb(z, a);
        return;
    case 0xe1: *ix = pop(z); z->icount -= 14; return;
    case 0xe5: push(z, *ix); z->icount -= 15; return;
    case 0xe3:
        w = rd16(z, z->sp);
        wr16(z, z->sp, *ix);
        *ix = w;
        z->wz = w;
        z->icount -= 23;
        return;
    case 0xe9: z->pc = *ix; z->icount -= 8; return;
    case 0xf9: z->sp = *ix; z->icount -= 10; return;
    default:
        /* the prefix has no effect: run the plain instruction, 4 cycles more */
        z->icount -= 4;
        exec_main(z, op);
        return;
    }
}

static void exec_main(Z80 *z, uint8_t op)
{
    uint8_t v;
    uint16_t w;

    z->icount -= cyc_main[op];

    if (op >= 0x40 && op < 0xc0) {
        int src = op & 7;
        if (op == 0x76) {
            z->halted = 1;
            return;
        }
        v = src == 6 ? rd(z, HL) : REG(z, src);
        if (op >= 0x80)
            alu(z, (op >> 3) & 7, v);
        else if (((op >> 3) & 7) == 6)
            wr(z, HL, v);
        else
            REG(z, (op >> 3) & 7) = v;
        return;
    }

    switch (op) {
    case 0x00: return;
    case 0x01: z->c = fetch(z); z->b = fetch(z); return;
    case 0x11: z->e = fetch(z); z->d = fetch(z); return;
    case 0x21: z->l = fetch(z); z->h = fetch(z); return;
    case 0x31: z->sp = fetch16(z); return;
    case 0x02: wr(z, BC, z->a); z->wz = (uint16_t)(((BC + 1) & 0xff) | (z->a << 8)); return;
    case 0x12: wr(z, DE, z->a); z->wz = (uint16_t)(((DE + 1) & 0xff) | (z->a << 8)); return;
    case 0x0a: z->a = rd(z, BC); z->wz = (uint16_t)(BC + 1); return;
    case 0x1a: z->a = rd(z, DE); z->wz = (uint16_t)(DE + 1); return;
    case 0x03: SET_BC(BC + 1); return;
    case 0x13: SET_DE(DE + 1); return;
    case 0x23: SET_HL(HL + 1); return;
    case 0x33: z->sp++; return;
    case 0x0b: SET_BC(BC - 1); return;
    case 0x1b: SET_DE(DE - 1); return;
    case 0x2b: SET_HL(HL - 1); return;
    case 0x3b: z->sp--; return;
    case 0x04: z->b = inc8(z, z->b); return;
    case 0x0c: z->c = inc8(z, z->c); return;
    case 0x14: z->d = inc8(z, z->d); return;
    case 0x1c: z->e = inc8(z, z->e); return;
    case 0x24: z->h = inc8(z, z->h); return;
    case 0x2c: z->l = inc8(z, z->l); return;
    case 0x3c: z->a = inc8(z, z->a); return;
    case 0x34: wr(z, HL, inc8(z, rd(z, HL))); return;
    case 0x05: z->b = dec8(z, z->b); return;
    case 0x0d: z->c = dec8(z, z->c); return;
    case 0x15: z->d = dec8(z, z->d); return;
    case 0x1d: z->e = dec8(z, z->e); return;
    case 0x25: z->h = dec8(z, z->h); return;
    case 0x2d: z->l = dec8(z, z->l); return;
    case 0x3d: z->a = dec8(z, z->a); return;
    case 0x35: wr(z, HL, dec8(z, rd(z, HL))); return;
    case 0x06: z->b = fetch(z); return;
    case 0x0e: z->c = fetch(z); return;
    case 0x16: z->d = fetch(z); return;
    case 0x1e: z->e = fetch(z); return;
    case 0x26: z->h = fetch(z); return;
    case 0x2e: z->l = fetch(z); return;
    case 0x3e: z->a = fetch(z); return;
    case 0x36: v = fetch(z); wr(z, HL, v); return;
    case 0x07:
        z->a = (uint8_t)((z->a << 1) | (z->a >> 7));
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & (XYF | CF)));
        return;
    case 0x0f:
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & CF));
        z->a = (uint8_t)((z->a >> 1) | (z->a << 7));
        z->f |= (uint8_t)(z->a & XYF);
        return;
    case 0x17:
        v = z->a >> 7;
        z->a = (uint8_t)((z->a << 1) | (z->f & CF));
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & XYF) | v);
        return;
    case 0x1f:
        v = z->a & 1;
        z->a = (uint8_t)((z->a >> 1) | ((z->f & CF) << 7));
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & XYF) | v);
        return;
    case 0x08:
        v = z->a; z->a = z->a2; z->a2 = v;
        v = z->f; z->f = z->f2; z->f2 = v;
        return;
    case 0x09: SET_HL(add16(z, HL, BC)); return;
    case 0x19: SET_HL(add16(z, HL, DE)); return;
    case 0x29: SET_HL(add16(z, HL, HL)); return;
    case 0x39: SET_HL(add16(z, HL, z->sp)); return;
    case 0x10:
        v = fetch(z);
        if (--z->b) {
            z->pc = (uint16_t)(z->pc + (int8_t)v);
            z->wz = z->pc;
            z->icount -= 5;
        }
        return;
    case 0x18:
        v = fetch(z);
        z->pc = (uint16_t)(z->pc + (int8_t)v);
        z->wz = z->pc;
        return;
    case 0x20: case 0x28: case 0x30: case 0x38:
        v = fetch(z);
        if (cond(z, (op >> 3) & 3)) {
            z->pc = (uint16_t)(z->pc + (int8_t)v);
            z->wz = z->pc;
            z->icount -= 5;
        }
        return;
    case 0x22: w = fetch16(z); wr(z, w, z->l); wr(z, (uint16_t)(w + 1), z->h); z->wz = (uint16_t)(w + 1); return;
    case 0x2a: w = fetch16(z); z->l = rd(z, w); z->h = rd(z, (uint16_t)(w + 1)); z->wz = (uint16_t)(w + 1); return;
    case 0x32: w = fetch16(z); wr(z, w, z->a); z->wz = (uint16_t)(((w + 1) & 0xff) | (z->a << 8)); return;
    case 0x3a: w = fetch16(z); z->a = rd(z, w); z->wz = (uint16_t)(w + 1); return;
    case 0x27: daa(z); return;
    case 0x2f:
        z->a = (uint8_t)~z->a;
        z->f = (uint8_t)((z->f & (SF | ZF | PF | CF)) | HF | NF | (z->a & XYF));
        return;
    case 0x37:
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | CF | (z->a & XYF));
        return;
    case 0x3f:
        z->f = (uint8_t)((z->f & (SF | ZF | PF)) | ((z->f & CF) ? HF : CF) | (z->a & XYF));
        return;

    case 0xc0: case 0xc8: case 0xd0: case 0xd8: case 0xe0: case 0xe8: case 0xf0: case 0xf8:
        if (cond(z, (op >> 3) & 7)) {
            z->pc = pop(z);
            z->wz = z->pc;
            z->icount -= 6;
        }
        return;
    case 0xc9: z->pc = pop(z); z->wz = z->pc; return;
    case 0xc1: w = pop(z); SET_BC(w); return;
    case 0xd1: w = pop(z); SET_DE(w); return;
    case 0xe1: w = pop(z); SET_HL(w); return;
    case 0xf1: w = pop(z); z->a = (uint8_t)(w >> 8); z->f = (uint8_t)w; return;
    case 0xc5: push(z, BC); return;
    case 0xd5: push(z, DE); return;
    case 0xe5: push(z, HL); return;
    case 0xf5: push(z, (uint16_t)((z->a << 8) | z->f)); return;
    case 0xc2: case 0xca: case 0xd2: case 0xda: case 0xe2: case 0xea: case 0xf2: case 0xfa:
        w = fetch16(z);
        z->wz = w;
        if (cond(z, (op >> 3) & 7))
            z->pc = w;
        return;
    case 0xc3: z->pc = fetch16(z); z->wz = z->pc; return;
    case 0xc4: case 0xcc: case 0xd4: case 0xdc: case 0xe4: case 0xec: case 0xf4: case 0xfc:
        w = fetch16(z);
        z->wz = w;
        if (cond(z, (op >> 3) & 7)) {
            push(z, z->pc);
            z->pc = w;
            z->icount -= 7;
        }
        return;
    case 0xcd: w = fetch16(z); push(z, z->pc); z->pc = w; z->wz = w; return;
    case 0xc6: case 0xce: case 0xd6: case 0xde: case 0xe6: case 0xee: case 0xf6: case 0xfe:
        alu(z, (op >> 3) & 7, fetch(z));
        return;
    case 0xc7: case 0xcf: case 0xd7: case 0xdf: case 0xe7: case 0xef: case 0xf7: case 0xff:
        push(z, z->pc);
        z->pc = op & 0x38;
        z->wz = z->pc;
        return;
    case 0xcb: exec_cb(z); return;
    case 0xdd: exec_index(z, &z->ix); return;
    case 0xfd: exec_index(z, &z->iy); return;
    case 0xed: exec_ed(z); return;
    case 0xd3:
        v = fetch(z);
        z->out(z->ctx, (uint16_t)(v | (z->a << 8)), z->a);
        z->wz = (uint16_t)(((v + 1) & 0xff) | (z->a << 8));
        return;
    case 0xdb:
        w = (uint16_t)(fetch(z) | (z->a << 8));
        z->a = z->in(z->ctx, w);
        z->wz = (uint16_t)(w + 1);
        return;
    case 0xd9:
        v = z->b; z->b = z->b2; z->b2 = v;
        v = z->c; z->c = z->c2; z->c2 = v;
        v = z->d; z->d = z->d2; z->d2 = v;
        v = z->e; z->e = z->e2; z->e2 = v;
        v = z->h; z->h = z->h2; z->h2 = v;
        v = z->l; z->l = z->l2; z->l2 = v;
        return;
    case 0xe3:
        w = rd16(z, z->sp);
        wr16(z, z->sp, HL);
        SET_HL(w);
        z->wz = w;
        return;
    case 0xe9: z->pc = HL; return;
    case 0xeb:
        v = z->d; z->d = z->h; z->h = v;
        v = z->e; z->e = z->l; z->l = v;
        return;
    case 0xf3: z->iff1 = z->iff2 = 0; return;
    case 0xfb: z->iff1 = z->iff2 = 1; z->after_ei = 1; return;
    case 0xf9: z->sp = HL; return;
    default: return;
    }
}

/* ---- interface ---- */

void z80_init(Z80 *z, uint8_t *mem)
{
    int i;
    if (!tables_ready)
        init_tables();
    memset(z, 0, sizeof *z);
    z->mem = mem;
    z->in = def_read;
    z->write = z->out = def_write;
    z->irq_vector = 0xff;
    for (i = 0; i < 256; i++) {
        z->wtype[i] = Z80_RAM;
        z->wmirror[i] = 0;
    }
    z80_reset(z);
}

void z80_reset(Z80 *z)
{
    z->pc = 0;
    z->i = z->r = z->r7 = 0;
    z->iff1 = z->iff2 = 0;
    z->im = 0;
    z->halted = z->after_ei = z->ei_window = 0;
    z->nmi_pending = 0;
    z->a = z->f = 0xff;
    z->sp = 0xffff;
    z->wz = 0;
}

void z80_set_wtype(Z80 *z, uint16_t start, uint16_t end, int type, uint8_t *mirror)
{
    int p;
    for (p = start >> 8; p <= end >> 8; p++) {
        z->wtype[p] = (uint8_t)type;
        z->wmirror[p] = mirror ? mirror + ((p - (start >> 8)) << 8) : 0;
    }
}

void z80_nmi(Z80 *z)
{
    z->nmi_pending = 1;
}

void z80_set_irq(Z80 *z, int level, int hold)
{
    z->irq_line = (uint8_t)(level != 0);
    z->irq_hold = (uint8_t)(hold != 0);
}

static void take_nmi(Z80 *z)
{
    z->nmi_pending = 0;
    z->halted = 0;
    z->iff1 = 0;
    z->r++;
    push(z, z->pc);
    z->pc = 0x0066;
    z->wz = z->pc;
    z->icount -= 11;
}

static void take_irq(Z80 *z)
{
    z->halted = 0;
    z->iff1 = z->iff2 = 0;
    z->r++;
    if (z->irq_hold)
        z->irq_line = 0;
    push(z, z->pc);
    switch (z->im) {
    case 2:
        z->pc = rd16(z, (uint16_t)((z->i << 8) | z->irq_vector));
        z->icount -= 19;
        break;
    case 1:
        z->pc = 0x0038;
        z->icount -= 13;
        break;
    default:   /* IM 0: only RST n on the data bus is supported */
        z->pc = z->irq_vector & 0x38;
        z->icount -= 13;
        break;
    }
    z->wz = z->pc;
}

int z80_run(Z80 *z, int cycles)
{
    int start;
    z->icount += cycles;
    start = z->icount;
    while (z->icount > 0) {
        if (z->nmi_pending)
            take_nmi(z);
        else if (z->after_ei)
            z->after_ei = 0;
        else if (z->irq_line && z->iff1)
            take_irq(z);
        if (z->halted) {
            /* nothing happens until the next interrupt, which can only come from
               outside this call: burn the rest of the time slice */
            int n = (z->icount + 3) >> 2;
            z->r = (uint8_t)(z->r + n);
            z->icount -= n << 2;
            break;
        }
        z->r++;
        exec_main(z, fetch(z));
    }
    return start - z->icount;
}
