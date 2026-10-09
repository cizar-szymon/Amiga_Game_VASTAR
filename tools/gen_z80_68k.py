#!/usr/bin/env python3
"""Generates port/src/z80_68k.s: the Z80 core in 68k assembler (vasm, Motorola syntax).

    python tools/gen_z80_68k.py            -> port/src/z80_68k.s

Same interface and state as port/src/z80.c (see z80.h); z80.c is the reference
(the lockstep test compares both instruction by instruction).

68k registers during z80_run():
    d3  R (counts every opcode fetch)      a2  z80_tables (flag tables)
    d4  HL (upper word always 0)           a3  mem (flat 64 KB)
    d5  F                                  a4  host pointer = mem + PC
    d6  A                                  a5  the Z80 struct
    d7  cycles left                        a6  main opcode jump table
    d0 d1 d2 a0 a1 scratch
Other registers live in the Z80 struct (big endian: BC at 2, DE at 4).

Flags: S Z V C come from the 68k CCR after the matching 68k instruction (MOVE from CCR,
then a 16 entry table), H and the undocumented X/Y are computed. Not kept: WZ (MEMPTR),
so BIT n,(HL) leaves X/Y as 0 (the C core takes them from WZ).
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "port", "src", "z80_68k.s")

# struct offsets (z80.h)
O_A, O_F, O_B, O_C, O_D, O_E, O_H, O_L = range(8)
O_A2, O_F2 = 8, 9
O_IX, O_IY, O_SP, O_PC, O_WZ = 16, 18, 20, 22, 24
O_I, O_R, O_R7, O_IFF1, O_IFF2, O_IM = 26, 27, 28, 29, 30, 31
O_HALTED, O_AFTEREI, O_NMI, O_IRQ, O_IRQHOLD, O_IRQVEC, O_EIWIN = 32, 33, 34, 35, 36, 37, 38
O_ICOUNT, O_EISAVE, O_START, O_MEM = 40, 44, 48, 52
O_WRITE, O_IN, O_OUT, O_CTX, O_WTYPE, O_WMIRROR = 56, 60, 64, 68, 72, 328
# z80_tables offsets
T_SZXY, T_SZXYP, T_INC, T_DEC, T_CCRADD, T_CCRSUB = 0, 256, 512, 768, 1024, 1040

lines = []
label_no = [0]


def emit(*ls):
    for l in ls:
        lines.append(l)


def L():
    label_no[0] += 1
    return ".l%d" % label_no[0]


def ins(s):
    emit("\t" + s)


# ---------------------------------------------------------------- helpers

def write_d0_d1():
    """mem[d0.l] = d1.b through the write type; keeps d0 and d1, clobbers d2 (a0 a1 on the slow path)"""
    fast, done = L(), L()
    ins("move.w d0,d2")
    ins("lsr.w #8,d2")
    ins("tst.b (%d,a5,d2.w)" % O_WTYPE)
    ins("beq.s " + fast)
    ins("bsr wr_slow")
    ins("bra.s " + done)
    emit(fast)
    ins("move.b d1,(a3,d0.l)")
    emit(done)


def next_op(cycles):
    if cycles:
        if cycles <= 8:
            ins("subq.l #%d,d7" % cycles)
        else:
            ins("sub.l #%d,d7" % cycles)
    ins("ble exit_loop")
    ins("moveq #0,d0")
    ins("move.b (a4)+,d0")
    ins("addq.b #1,d3")
    ins("move.l (a6,d0.w*4),a0")
    ins("jmp (a0)")


def fetch16(dst):
    """little endian word at PC into dst (upper word 0)"""
    ins("moveq #0,%s" % dst)
    ins("move.b 1(a4),%s" % dst)
    ins("lsl.w #8,%s" % dst)
    ins("move.b (a4),%s" % dst)
    ins("addq.l #2,a4")


def get_pc(dst):
    ins("move.l a4,%s" % dst)
    ins("sub.l a3,%s" % dst)
    ins("and.l #$ffff,%s" % dst)


def set_pc(src):
    """src: data register with the address in the low word, upper word 0"""
    ins("lea (a3,%s.l),a4" % src)


def push_d1():
    """push d1.w; clobbers d0 d2"""
    ins("moveq #0,d0")
    ins("move.w %d(a5),d0" % O_SP)
    ins("subq.w #1,d0")
    ins("ror.w #8,d1")
    write_d0_d1()
    ins("subq.w #1,d0")
    ins("ror.w #8,d1")
    write_d0_d1()
    ins("move.w d0,%d(a5)" % O_SP)


def pop_d1():
    """pop into d1.w (upper word 0); clobbers d0"""
    ins("moveq #0,d0")
    ins("move.w %d(a5),d0" % O_SP)
    ins("moveq #0,d1")
    ins("move.b 1(a3,d0.l),d1")
    ins("lsl.w #8,d1")
    ins("move.b (a3,d0.l),d1")
    ins("addq.w #2,%d(a5)" % O_SP)


# 8 bit registers: 0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 6 (HL), 7 A
REG_MEM = {0: O_B, 1: O_C, 2: O_D, 3: O_E}


def get8(r, dst, ix=None):
    """register r into data register dst (byte); ix = struct offset of IX/IY for IXH/IXL"""
    if r in REG_MEM:
        ins("move.b %d(a5),%s" % (REG_MEM[r], dst))
    elif r == 4:
        if ix is not None:
            ins("move.b %d(a5),%s" % (ix, dst))
        else:
            ins("move.w d4,%s" % dst)
            ins("lsr.w #8,%s" % dst)
    elif r == 5:
        if ix is not None:
            ins("move.b %d(a5),%s" % (ix + 1, dst))
        else:
            ins("move.b d4,%s" % dst)
    elif r == 6:
        ins("move.b (a3,d4.l),%s" % dst)
    else:
        ins("move.b d6,%s" % dst)


def set8(r, src, ix=None):
    """data register src (byte) into register r; (HL) clobbers d0 d2 and needs src = d1"""
    if r in REG_MEM:
        ins("move.b %s,%d(a5)" % (src, REG_MEM[r]))
    elif r == 4:
        if ix is not None:
            ins("move.b %s,%d(a5)" % (src, ix))
        else:
            ins("ror.w #8,d4")
            ins("move.b %s,d4" % src)
            ins("ror.w #8,d4")
    elif r == 5:
        if ix is not None:
            ins("move.b %s,%d(a5)" % (src, ix + 1))
        else:
            ins("move.b %s,d4" % src)
    elif r == 6:
        assert src == "d1"
        ins("move.l d4,d0")
        write_d0_d1()
    else:
        ins("move.b %s,d6" % src)


def xy_from(reg):
    """F |= reg & 0x28 (clobbers reg)"""
    ins("and.b #$28,%s" % reg)
    ins("or.b %s,d5" % reg)


def alu(op):
    """A op d2.b (d2 = operand); clobbers d0 d1"""
    if op in (0, 1):                                  # ADD, ADC
        ins("move.b d6,d1")
        if op == 0:
            ins("add.b d2,d6")
        else:
            nc = L()
            ins("moveq #4,d0")
            ins("btst #0,d5")
            ins("beq.s " + nc)
            ins("moveq #$14,d0")
            emit(nc)
            ins("move.w d0,ccr")
            ins("addx.b d2,d6")
        ins("move.w ccr,d0")
        ins("and.w #15,d0")
        ins("move.b (%d,a2,d0.w),d5" % T_CCRADD)
        ins("eor.b d6,d1")
        ins("eor.b d2,d1")
        ins("and.b #$10,d1")
        ins("or.b d1,d5")
        ins("move.b d6,d1")
        xy_from("d1")
    elif op in (2, 3, 7):                             # SUB, SBC, CP
        ins("move.b d6,d1")
        if op == 3:
            nc = L()
            ins("moveq #4,d0")
            ins("btst #0,d5")
            ins("beq.s " + nc)
            ins("moveq #$14,d0")
            emit(nc)
            ins("move.w d0,ccr")
            ins("subx.b d2,d1")
        else:
            ins("sub.b d2,d1")
        ins("move.w ccr,d0")
        ins("and.w #15,d0")
        ins("move.b (%d,a2,d0.w),d5" % T_CCRSUB)
        if op != 7:
            ins("move.b d6,d0")
            ins("move.b d1,d6")                       # the result
            ins("eor.b d1,d0")
            ins("eor.b d2,d0")
            ins("and.b #$10,d0")
            ins("or.b d0,d5")
            xy_from("d1")
        else:
            ins("eor.b d6,d1")
            ins("eor.b d2,d1")
            ins("and.b #$10,d1")
            ins("or.b d1,d5")
            ins("move.b d2,d1")
            xy_from("d1")                             # CP: X Y from the operand
    else:                                             # AND, XOR, OR
        ins({4: "and.b d2,d6", 5: "eor.b d2,d6", 6: "or.b d2,d6"}[op])
        ins("moveq #0,d0")
        ins("move.b d6,d0")
        ins("move.b (%d,a2,d0.w),d5" % T_SZXYP)
        if op == 4:
            ins("or.b #$10,d5")


def incdec_flags(reg, table):
    """F = (F & C) | table[reg]; clobbers d0"""
    ins("moveq #0,d0")
    ins("move.b %s,d0" % reg)
    ins("and.b #1,d5")
    ins("or.b (%d,a2,d0.w),d5" % table)


def cond_branch_not(cc, target):
    """branch to target if condition cc (0 NZ 1 Z 2 NC 3 C 4 PO 5 PE 6 P 7 M) is false"""
    bit = {0: 6, 1: 6, 2: 0, 3: 0, 4: 2, 5: 2, 6: 7, 7: 7}[cc]
    ins("btst #%d,d5" % bit)
    ins(("bne " if cc % 2 == 0 else "beq ") + target)


def add16_code(src, dst):
    """ADD dst,src for 16 bit: dst/src are 68k operands (word); S Z P kept"""
    ins("move.w %s,d1" % src)
    ins("move.w %s,d0" % dst)
    ins("move.w d0,d2")
    ins("add.w d1,d2")
    ins("move.w ccr,-(sp)")
    ins("move.w d2,%s" % dst)
    ins("and.b #$c4,d5")
    ins("eor.w d2,d0")
    ins("eor.w d1,d0")
    ins("lsr.w #8,d0")
    ins("and.b #$10,d0")
    ins("or.b d0,d5")
    ins("move.w d2,d0")
    ins("lsr.w #8,d0")
    xy_from("d0")
    ins("move.w (sp)+,d0")
    ins("and.b #1,d0")
    ins("or.b d0,d5")


def rot_code(n):
    """CB rotation n on d1.b; result in d1, F set; clobbers d0 d2"""
    if n == 0:
        ins("rol.b #1,d1")
        ins("move.w ccr,d0")
    elif n == 1:
        ins("ror.b #1,d1")
        ins("move.w ccr,d0")
    elif n in (2, 3):
        ins("move.b d5,d0")
        ins("lsr.b #1,d0")                     # X = Z80 carry
        ins("roxl.b #1,d1" if n == 2 else "roxr.b #1,d1")
        ins("move.w ccr,d0")
    elif n == 4:
        ins("lsl.b #1,d1")
        ins("move.w ccr,d0")
    elif n == 5:
        ins("asr.b #1,d1")
        ins("move.w ccr,d0")
    elif n == 6:
        ins("lsl.b #1,d1")
        ins("move.w ccr,d0")
        ins("or.b #1,d1")
    else:
        ins("lsr.b #1,d1")
        ins("move.w ccr,d0")
    ins("and.b #1,d0")
    ins("moveq #0,d2")
    ins("move.b d1,d2")
    ins("move.b (%d,a2,d2.w),d5" % T_SZXYP)
    ins("or.b d0,d5")


def bit_code(n, val, xy):
    """BIT n,val (data register); xy = register with the X/Y source or None"""
    s, x = L(), L()
    ins("and.b #1,d5")
    ins("or.b #$10,d5")
    ins("btst #%d,%s" % (n, val))
    if n == 7:
        ins("bne.s " + s)
        ins("or.b #$44,d5")
        ins("bra.s " + x)
        emit(s)
        ins("or.b #$80,d5")
    else:
        ins("bne.s " + x)
        ins("or.b #$44,d5")
    emit(x)
    if xy:
        xy_from(xy)


# ---------------------------------------------------------------- main opcodes

MAIN = {}       # opcode -> label
CYC_MAIN = [
    4, 10, 7, 6, 4, 4, 7, 4, 4, 11, 7, 6, 4, 4, 7, 4,
    8, 10, 7, 6, 4, 4, 7, 4, 12, 11, 7, 6, 4, 4, 7, 4,
    7, 10, 16, 6, 4, 4, 7, 4, 7, 11, 16, 6, 4, 4, 7, 4,
    7, 10, 13, 6, 11, 11, 10, 4, 7, 11, 13, 6, 4, 4, 7, 4,
] + [0] * 128 + [
    5, 10, 10, 10, 10, 11, 7, 11, 5, 10, 10, 0, 10, 17, 7, 11,
    5, 10, 10, 11, 10, 11, 7, 11, 5, 4, 10, 11, 10, 0, 7, 11,
    5, 10, 10, 19, 10, 11, 7, 11, 5, 4, 10, 4, 10, 0, 7, 11,
    5, 10, 10, 4, 10, 11, 7, 11, 5, 6, 10, 4, 10, 0, 7, 11,
]
RR_EA = {0: "%d(a5)" % O_B, 1: "%d(a5)" % O_D, 2: "d4", 3: "%d(a5)" % O_SP}


def handler(name):
    emit("", name)


def gen_main(op):
    name = "op_%02X" % op
    MAIN[op] = name
    handler(name)
    cyc = CYC_MAIN[op]

    if 0x40 <= op < 0x80:
        if op == 0x76:                               # HALT
            ins("move.b #1,%d(a5)" % O_HALTED)
            ins("subq.l #4,d7")
            ins("bra exit_loop")
            return
        dst, src = (op >> 3) & 7, op & 7
        c = 7 if (dst == 6 or src == 6) else 4
        if dst in REG_MEM and src in REG_MEM:
            ins("move.b %d(a5),%d(a5)" % (REG_MEM[src], REG_MEM[dst]))
        elif dst == src:
            pass
        else:
            get8(src, "d1")
            set8(dst, "d1")
        next_op(c)
        return
    if 0x80 <= op < 0xc0:
        src = op & 7
        get8(src, "d2")
        alu((op >> 3) & 7)
        next_op(7 if src == 6 else 4)
        return

    r8 = (op >> 3) & 7
    rp = (op >> 4) & 3
    if op == 0x00:
        pass
    elif op in (0x01, 0x11):
        hi = O_B if op == 0x01 else O_D
        ins("move.b (a4)+,%d(a5)" % (hi + 1))
        ins("move.b (a4)+,%d(a5)" % hi)
    elif op == 0x21:
        fetch16("d0")
        ins("move.w d0,d4")
    elif op == 0x31:
        fetch16("d0")
        ins("move.w d0,%d(a5)" % O_SP)
    elif op in (0x02, 0x12):
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % (O_B if op == 0x02 else O_D))
        ins("move.b d6,d1")
        write_d0_d1()
    elif op in (0x0A, 0x1A):
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % (O_B if op == 0x0A else O_D))
        ins("move.b (a3,d0.l),d6")
    elif op in (0x03, 0x13, 0x23, 0x33):
        ins("addq.w #1,%s" % RR_EA[rp])
    elif op in (0x0B, 0x1B, 0x2B, 0x3B):
        ins("subq.w #1,%s" % RR_EA[rp])
    elif (op & 0xc7) == 0x04:                        # INC r
        if r8 in REG_MEM:
            ins("addq.b #1,%d(a5)" % REG_MEM[r8])
            incdec_flags("%d(a5)" % REG_MEM[r8], T_INC)
        else:
            get8(r8, "d1")
            ins("addq.b #1,d1")
            incdec_flags("d1", T_INC)
            set8(r8, "d1")
    elif (op & 0xc7) == 0x05:                        # DEC r
        if r8 in REG_MEM:
            ins("subq.b #1,%d(a5)" % REG_MEM[r8])
            incdec_flags("%d(a5)" % REG_MEM[r8], T_DEC)
        else:
            get8(r8, "d1")
            ins("subq.b #1,d1")
            incdec_flags("d1", T_DEC)
            set8(r8, "d1")
    elif (op & 0xc7) == 0x06:                        # LD r,n
        if r8 in REG_MEM:
            ins("move.b (a4)+,%d(a5)" % REG_MEM[r8])
        else:
            ins("move.b (a4)+,d1")
            set8(r8, "d1")
    elif op == 0x07:                                 # RLCA
        ins("rol.b #1,d6")
        ins("and.b #$c4,d5")
        ins("move.b d6,d0")
        ins("and.b #$29,d0")
        ins("or.b d0,d5")
    elif op == 0x0F:                                 # RRCA
        ins("ror.b #1,d6")
        ins("and.b #$c4,d5")
        ins("move.b d6,d0")
        ins("and.b #$28,d0")
        ins("or.b d0,d5")
        ins("move.b d6,d0")
        ins("rol.b #1,d0")
        ins("and.b #1,d0")
        ins("or.b d0,d5")
    elif op in (0x17, 0x1F):                         # RLA, RRA
        ins("move.b d5,d0")
        ins("lsr.b #1,d0")
        ins("roxl.b #1,d6" if op == 0x17 else "roxr.b #1,d6")
        ins("move.w ccr,d0")
        ins("and.b #1,d0")
        ins("and.b #$c4,d5")
        ins("or.b d0,d5")
        ins("move.b d6,d0")
        xy_from("d0")
    elif op == 0x08:                                 # EX AF,AF'
        ins("move.b %d(a5),d0" % O_A2)
        ins("move.b d6,%d(a5)" % O_A2)
        ins("move.b d0,d6")
        ins("move.b %d(a5),d0" % O_F2)
        ins("move.b d5,%d(a5)" % O_F2)
        ins("move.b d0,d5")
    elif op in (0x09, 0x19, 0x29, 0x39):             # ADD HL,rr
        add16_code(RR_EA[rp], "d4")
    elif op == 0x10:                                 # DJNZ
        nt = L()
        ins("move.b (a4)+,d0")
        ins("subq.b #1,%d(a5)" % O_B)
        ins("beq.s " + nt)
        ins("ext.w d0")
        ins("add.w d0,a4")
        next_op(13)
        emit(nt)
    elif op == 0x18:                                 # JR
        ins("move.b (a4)+,d0")
        ins("ext.w d0")
        ins("add.w d0,a4")
    elif op in (0x20, 0x28, 0x30, 0x38):             # JR cc
        nt = L()
        ins("move.b (a4)+,d0")
        cond_branch_not((op >> 3) & 3, nt)
        ins("ext.w d0")
        ins("add.w d0,a4")
        next_op(12)
        emit(nt)
    elif op == 0x22:                                 # LD (nn),HL
        fetch16("d0")
        ins("move.b d4,d1")
        write_d0_d1()
        ins("addq.w #1,d0")
        ins("move.w d4,d1")
        ins("lsr.w #8,d1")
        write_d0_d1()
    elif op == 0x2A:                                 # LD HL,(nn)
        fetch16("d0")
        ins("move.b 1(a3,d0.l),d1")
        ins("lsl.w #8,d1")
        ins("move.b (a3,d0.l),d1")
        ins("move.w d1,d4")
    elif op == 0x32:
        fetch16("d0")
        ins("move.b d6,d1")
        write_d0_d1()
    elif op == 0x3A:
        fetch16("d0")
        ins("move.b (a3,d0.l),d6")
    elif op == 0x27:                                 # DAA
        ins("moveq #0,d0")
        ins("move.b d5,d0")
        ins("and.b #$13,d0")
        ins("lsl.w #8,d0")
        ins("move.b d6,d0")
        ins("lea _z80_daa,a0")
        ins("move.w (a0,d0.l*2),d0")
        ins("move.b d0,d5")
        ins("lsr.w #8,d0")
        ins("move.b d0,d6")
    elif op == 0x2F:                                 # CPL
        ins("not.b d6")
        ins("and.b #$c5,d5")
        ins("or.b #$12,d5")
        ins("move.b d6,d0")
        xy_from("d0")
    elif op == 0x37:                                 # SCF
        ins("and.b #$c4,d5")
        ins("or.b #1,d5")
        ins("move.b d6,d0")
        xy_from("d0")
    elif op == 0x3F:                                 # CCF
        a, b = L(), L()
        ins("move.b d5,d0")
        ins("and.b #$c4,d5")
        ins("btst #0,d0")
        ins("beq.s " + a)
        ins("or.b #$10,d5")
        ins("bra.s " + b)
        emit(a)
        ins("or.b #1,d5")
        emit(b)
        ins("move.b d6,d0")
        xy_from("d0")
    elif op == 0x36:                                 # LD (HL),n
        ins("move.b (a4)+,d1")
        ins("move.l d4,d0")
        write_d0_d1()
    elif (op & 0xc7) == 0xc0:                        # RET cc
        nt = L()
        cond_branch_not((op >> 3) & 7, nt)
        pop_d1()
        set_pc("d1")
        next_op(11)
        emit(nt)
    elif op == 0xC9:
        pop_d1()
        set_pc("d1")
    elif (op & 0xcf) == 0xc1:                        # POP
        pop_d1()
        if rp == 0:
            ins("move.w d1,%d(a5)" % O_B)
        elif rp == 1:
            ins("move.w d1,%d(a5)" % O_D)
        elif rp == 2:
            ins("move.w d1,d4")
        else:
            ins("move.b d1,d5")
            ins("lsr.w #8,d1")
            ins("move.b d1,d6")
    elif (op & 0xcf) == 0xc5:                        # PUSH
        if rp == 0:
            ins("move.w %d(a5),d1" % O_B)
        elif rp == 1:
            ins("move.w %d(a5),d1" % O_D)
        elif rp == 2:
            ins("move.w d4,d1")
        else:
            ins("move.b d6,d1")
            ins("lsl.w #8,d1")
            ins("move.b d5,d1")
        push_d1()
    elif (op & 0xc7) == 0xc2:                        # JP cc
        nt = L()
        cond_branch_not((op >> 3) & 7, nt)
        fetch16("d0")
        set_pc("d0")
        next_op(10)
        emit(nt)
        ins("addq.l #2,a4")
    elif op == 0xC3:
        fetch16("d0")
        set_pc("d0")
    elif (op & 0xc7) == 0xc4:                        # CALL cc
        nt = L()
        cond_branch_not((op >> 3) & 7, nt)
        get_pc("d1")
        ins("addq.w #2,d1")
        push_d1()
        fetch16("d0")
        set_pc("d0")
        next_op(17)
        emit(nt)
        ins("addq.l #2,a4")
    elif op == 0xCD:
        get_pc("d1")
        ins("addq.w #2,d1")
        push_d1()
        fetch16("d0")
        set_pc("d0")
    elif (op & 0xc7) == 0xc6:                        # ALU n
        ins("move.b (a4)+,d2")
        alu((op >> 3) & 7)
    elif (op & 0xc7) == 0xc7:                        # RST
        get_pc("d1")
        push_d1()
        ins("moveq #$%02x,d0" % (op & 0x38))
        set_pc("d0")
    elif op == 0xCB:
        ins("addq.b #1,d3")
        ins("moveq #0,d0")
        ins("move.b (a4)+,d0")
        ins("lea cbtab,a0")
        ins("move.l (a0,d0.w*4),a0")
        ins("jmp (a0)")
        return
    elif op in (0xDD, 0xFD):
        ins("addq.b #1,d3")
        ins("moveq #0,d0")
        ins("move.b (a4)+,d0")
        ins("lea %s,a0" % ("ddtab" if op == 0xDD else "fdtab"))
        ins("move.l (a0,d0.w*4),a0")
        ins("jmp (a0)")
        return
    elif op == 0xED:
        ins("addq.b #1,d3")
        ins("moveq #0,d0")
        ins("move.b (a4)+,d0")
        ins("lea edtab,a0")
        ins("move.l (a0,d0.w*4),a0")
        ins("jmp (a0)")
        return
    elif op == 0xD3:                                 # OUT (n),A
        ins("moveq #0,d0")
        ins("move.b d6,d0")
        ins("lsl.w #8,d0")
        ins("move.b (a4)+,d0")
        ins("move.b d6,d1")
        ins("bsr call_out")
    elif op == 0xDB:                                 # IN A,(n)
        ins("moveq #0,d0")
        ins("move.b d6,d0")
        ins("lsl.w #8,d0")
        ins("move.b (a4)+,d0")
        ins("bsr call_in")
        ins("move.b d0,d6")
    elif op == 0xD9:                                 # EXX
        ins("move.l %d(a5),d0" % O_B)
        ins("move.l %d(a5),%d(a5)" % (O_B + 8, O_B))
        ins("move.l d0,%d(a5)" % (O_B + 8))
        ins("move.w %d(a5),d0" % (O_H + 8))
        ins("move.w d4,%d(a5)" % (O_H + 8))
        ins("move.w d0,d4")
    elif op == 0xE3:                                 # EX (SP),HL
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % O_SP)
        ins("moveq #0,d1")
        ins("move.b 1(a3,d0.l),d1")
        ins("lsl.w #8,d1")
        ins("move.b (a3,d0.l),d1")
        ins("move.w d1,-(sp)")
        ins("move.b d4,d1")
        write_d0_d1()
        ins("addq.w #1,d0")
        ins("move.w d4,d1")
        ins("lsr.w #8,d1")
        write_d0_d1()
        ins("move.w (sp)+,d4")
    elif op == 0xE9:
        set_pc("d4")
    elif op == 0xEB:
        ins("move.w %d(a5),d0" % O_D)
        ins("move.w d4,%d(a5)" % O_D)
        ins("move.w d0,d4")
    elif op == 0xF3:
        ins("clr.b %d(a5)" % O_IFF1)
        ins("clr.b %d(a5)" % O_IFF2)
    elif op == 0xF9:
        ins("move.w d4,%d(a5)" % O_SP)
    elif op == 0xFB:                                 # EI
        a, b = L(), L()
        ins("tst.b %d(a5)" % O_EIWIN)
        ins("beq.s " + a)
        ins("clr.b %d(a5)" % O_EIWIN)
        ins("add.l %d(a5),d7" % O_EISAVE)
        ins("subq.l #1,d7")
        emit(a)
        ins("move.b #1,%d(a5)" % O_IFF1)
        ins("move.b #1,%d(a5)" % O_IFF2)
        ins("subq.l #4,d7")
        ins("ble.s " + b)
        # time left: run the next instruction, then look at the interrupts
        ins("move.l d7,%d(a5)" % O_EISAVE)
        ins("moveq #1,d7")
        ins("move.b #1,%d(a5)" % O_EIWIN)
        ins("bra dispatch")
        emit(b)
        ins("move.b #1,%d(a5)" % O_AFTEREI)
        ins("bra done")
        return
    else:
        raise ValueError("main %02x" % op)
    next_op(cyc)


# ---------------------------------------------------------------- CB

def gen_cb(op):
    name = "cb_%02X" % op
    handler(name)
    r, n, grp = op & 7, (op >> 3) & 7, op >> 6
    if grp == 1:                                     # BIT
        get8(r, "d1")
        if r == 6:
            bit_code(n, "d1", None)
            next_op(12)
        else:
            ins("move.b d1,d2")
            bit_code(n, "d1", "d2")
            next_op(8)
        return
    if grp in (2, 3):                                # RES / SET
        opc = "bclr" if grp == 2 else "bset"
        if r in REG_MEM:
            ins("%s #%d,%d(a5)" % (opc, n, REG_MEM[r]))
        elif r == 4:
            ins("%s #%d,d4" % (opc, n + 8))
        elif r == 5:
            ins("%s #%d,d4" % (opc, n))
        elif r == 7:
            ins("%s #%d,d6" % (opc, n))
        else:
            ins("move.b (a3,d4.l),d1")
            ins("%s #%d,d1" % (opc, n))
            ins("move.l d4,d0")
            write_d0_d1()
        next_op(15 if r == 6 else 8)
        return
    get8(r, "d1")
    rot_code(n)
    set8(r, "d1")
    next_op(15 if r == 6 else 8)


# ---------------------------------------------------------------- ED

def gen_ed(op):
    name = "ed_%02X" % op
    handler(name)
    if 0x40 <= op < 0x80:
        r = (op >> 3) & 7
        rp = (op >> 4) & 3
        k = op & 7
        if k == 0:                                   # IN r,(C)
            ins("moveq #0,d0")
            ins("move.w %d(a5),d0" % O_B)
            ins("bsr call_in")
            ins("move.b d0,d1")
            ins("moveq #0,d0")
            ins("move.b d1,d0")
            ins("and.b #1,d5")
            ins("or.b (%d,a2,d0.w),d5" % T_SZXYP)
            if r != 6:
                set8(r, "d1")
            next_op(12)
            return
        if k == 1:                                   # OUT (C),r
            if r == 6:
                ins("moveq #0,d1")
            else:
                get8(r, "d1")
            ins("moveq #0,d0")
            ins("move.w %d(a5),d0" % O_B)
            ins("bsr call_out")
            next_op(12)
            return
        if k == 2:                                   # SBC / ADC HL,rr
            nc = L()
            ins("move.w d4,d1")                      # old
            ins("move.w %s,d2" % RR_EA[rp])
            ins("moveq #4,d0")
            ins("btst #0,d5")
            ins("beq.s " + nc)
            ins("moveq #$14,d0")
            emit(nc)
            ins("move.w d0,ccr")
            ins(("addx.w d2,d4" if op & 8 else "subx.w d2,d4"))
            ins("move.w ccr,d0")
            ins("and.w #15,d0")
            ins("move.b (%d,a2,d0.w),d5" % (T_CCRADD if op & 8 else T_CCRSUB))
            ins("eor.w d4,d1")
            ins("eor.w d2,d1")
            ins("lsr.w #8,d1")
            ins("and.b #$10,d1")
            ins("or.b d1,d5")
            ins("move.w d4,d1")
            ins("lsr.w #8,d1")
            xy_from("d1")
            next_op(15)
            return
        if k == 3:                                   # LD (nn),rr / LD rr,(nn)
            fetch16("d0")
            if op & 8:
                ins("move.b 1(a3,d0.l),d1")
                ins("lsl.w #8,d1")
                ins("move.b (a3,d0.l),d1")
                ins("move.w d1,%s" % RR_EA[rp])
            else:
                ins("move.w %s,d1" % RR_EA[rp])
                write_d0_d1()
                ins("addq.w #1,d0")
                ins("lsr.w #8,d1")
                write_d0_d1()
            next_op(20)
            return
        if k == 4:                                   # NEG
            ins("move.b d6,d2")
            ins("moveq #0,d6")
            alu(2)
            next_op(8)
            return
        if k == 5:                                   # RETN / RETI
            ins("move.b %d(a5),%d(a5)" % (O_IFF2, O_IFF1))
            pop_d1()
            set_pc("d1")
            ins("sub.l #14,d7")
            ins("bra exit_loop")                     # IFF1 may allow a pending interrupt now
            return
        if k == 6:
            im = 0 if not op & 0x10 else (2 if op & 8 else 1)
            ins("move.b #%d,%d(a5)" % (im, O_IM))
            next_op(8)
            return
        if op == 0x47:
            ins("move.b d6,%d(a5)" % O_I)
            next_op(9)
            return
        if op == 0x4F:
            ins("move.b d6,d3")
            ins("move.b d6,d0")
            ins("and.b #$80,d0")
            ins("move.b d0,%d(a5)" % O_R7)
            next_op(9)
            return
        if op in (0x57, 0x5F):
            nz = L()
            if op == 0x57:
                ins("move.b %d(a5),d6" % O_I)
            else:
                ins("move.b d3,d6")
                ins("and.b #$7f,d6")
                ins("or.b %d(a5),d6" % O_R7)
            ins("moveq #0,d0")
            ins("move.b d6,d0")
            ins("and.b #1,d5")
            ins("or.b (%d,a2,d0.w),d5" % T_SZXY)
            ins("tst.b %d(a5)" % O_IFF2)
            ins("beq.s " + nz)
            ins("or.b #4,d5")
            emit(nz)
            next_op(9)
            return
        if op in (0x67, 0x6F):                       # RRD, RLD
            ins("move.b (a3,d4.l),d0")
            ins("moveq #0,d1")
            ins("moveq #0,d2")
            ins("move.b d0,d1")
            ins("move.b d6,d2")
            if op == 0x67:
                # mem = A low << 4 | mem >> 4; A = A high | mem low
                ins("lsr.b #4,d1")
                ins("lsl.b #4,d2")
                ins("or.b d2,d1")
                ins("and.b #$0f,d0")
            else:
                # mem = mem << 4 | A low; A = A high | mem >> 4
                ins("lsl.b #4,d1")
                ins("and.b #$0f,d2")
                ins("or.b d2,d1")
                ins("lsr.b #4,d0")
            ins("and.b #$f0,d6")
            ins("or.b d0,d6")
            ins("move.l d4,d0")
            write_d0_d1()
            ins("moveq #0,d0")
            ins("move.b d6,d0")
            ins("and.b #1,d5")
            ins("or.b (%d,a2,d0.w),d5" % T_SZXYP)
            next_op(18)
            return
        next_op(8)                                   # ED 77, ED 7F
        return

    if op in (0xA0, 0xA8, 0xB0, 0xB8):               # LDI LDD LDIR LDDR
        step = "addq.w #1," if op in (0xA0, 0xB0) else "subq.w #1,"
        rep = op >= 0xB0
        loop, last, fl = L(), L(), L()
        emit(loop)
        ins("move.b (a3,d4.l),d1")
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % O_D)
        write_d0_d1()
        ins(step + "d4")
        ins(step + "%d(a5)" % O_D)
        ins("subq.w #1,%d(a5)" % O_B)
        if rep:
            ins("beq.s " + last)
            ins("addq.b #2,d3")                       # the next round fetches ED xx again
            ins("sub.l #21,d7")
            ins("bgt.s " + loop)
            ins("subq.b #2,d3")
            ins("subq.l #2,a4")                       # out of time: continue later
            ins("bsr ld_flags")
            ins("bra exit_loop")
            emit(last)
            ins("bsr ld_flags")
            next_op(16)
        else:
            ins("bsr ld_flags")
            next_op(16)
        return
    if op in (0xA1, 0xA9, 0xB1, 0xB9):               # CPI CPD CPIR CPDR
        ins("move.b (a3,d4.l),d2")
        ins(("addq.w #1,d4" if op in (0xA1, 0xB1) else "subq.w #1,d4"))
        ins("subq.w #1,%d(a5)" % O_B)
        ins("bsr cp_flags")
        if op >= 0xB0:
            nr = L()
            ins("tst.w %d(a5)" % O_B)
            ins("beq.s " + nr)
            ins("btst #6,d5")
            ins("bne.s " + nr)
            ins("subq.l #2,a4")
            ins("subq.l #5,d7")
            emit(nr)
        next_op(16)
        return
    if op in (0xA2, 0xAA, 0xB2, 0xBA):               # INI IND INIR INDR
        inc = op in (0xA2, 0xB2)
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % O_B)
        ins("bsr call_in")
        ins("move.b d0,d1")
        ins("move.l d4,d0")
        write_d0_d1()
        ins("subq.b #1,%d(a5)" % O_B)
        ins(("addq.w #1,d4" if inc else "subq.w #1,d4"))
        ins("move.b %d(a5),d2" % O_C)
        ins(("addq.b #1,d2" if inc else "subq.b #1,d2"))
        ins("bsr io_flags")
        if op >= 0xB0:
            nr = L()
            ins("tst.b %d(a5)" % O_B)
            ins("beq.s " + nr)
            ins("subq.l #2,a4")
            ins("subq.l #5,d7")
            emit(nr)
        next_op(16)
        return
    if op in (0xA3, 0xAB, 0xB3, 0xBB):               # OUTI OUTD OTIR OTDR
        inc = op in (0xA3, 0xB3)
        ins("move.b (a3,d4.l),d1")
        ins("subq.b #1,%d(a5)" % O_B)
        ins("moveq #0,d0")
        ins("move.w %d(a5),d0" % O_B)
        ins("move.w d1,-(sp)")
        ins("bsr call_out")
        ins("move.w (sp)+,d1")
        ins(("addq.w #1,d4" if inc else "subq.w #1,d4"))
        ins("move.b d4,d2")
        ins("bsr io_flags")
        if op >= 0xB0:
            nr = L()
            ins("tst.b %d(a5)" % O_B)
            ins("beq.s " + nr)
            ins("subq.l #2,a4")
            ins("subq.l #5,d7")
            emit(nr)
        next_op(16)
        return
    next_op(8)                                       # undefined: 8 cycle NOP


# ---------------------------------------------------------------- DD / FD

def gen_index(prefix, ix):
    """handlers for DD (ix = O_IX) or FD (ix = O_IY); returns the 256 entry table"""
    pre = "dd" if ix == O_IX else "fd"
    table = []
    IXEA = "%d(a5)" % ix
    rr_ea = {0: "%d(a5)" % O_B, 1: "%d(a5)" % O_D, 2: IXEA, 3: "%d(a5)" % O_SP}

    def addr():
        """d0.l = IX + d"""
        ins("moveq #0,d0")
        ins("move.w %s,d0" % IXEA)
        ins("move.b (a4)+,d1")
        ins("ext.w d1")
        ins("add.w d1,d0")

    for op in range(256):
        name = "%s_%02X" % (pre, op)
        src, dst = op & 7, (op >> 3) & 7
        uses = False
        if 0x40 <= op < 0xc0 and op != 0x76:
            uses = src in (4, 5, 6) or (op < 0x80 and dst in (4, 5, 6))
        if op in (0x09, 0x19, 0x29, 0x39, 0x21, 0x22, 0x2A, 0x23, 0x2B, 0x24, 0x25, 0x26,
                  0x2C, 0x2D, 0x2E, 0x34, 0x35, 0x36, 0xCB, 0xE1, 0xE3, 0xE5, 0xE9, 0xF9):
            uses = True
        if not uses:
            table.append("%s_plain_%02X" % (pre, op))
            handler("%s_plain_%02X" % (pre, op))
            ins("subq.l #4,d7")
            ins("bra " + MAIN[op])
            continue
        table.append(name)
        handler(name)
        if 0x40 <= op < 0xc0:
            if src == 6 or (op < 0x80 and dst == 6):
                addr()
                if op >= 0x80:
                    ins("move.b (a3,d0.l),d2")
                    alu(dst)
                elif src == 6:
                    ins("move.b (a3,d0.l),d1")
                    set8(dst, "d1")
                else:
                    get8(src, "d1")
                    write_d0_d1()
                next_op(19)
            else:
                get8(src, "d1" if op < 0x80 else "d2", ix)
                if op >= 0x80:
                    alu(dst)
                else:
                    set8(dst, "d1", ix)
                next_op(8)
            continue
        if op in (0x09, 0x19, 0x29, 0x39):
            add16_code(rr_ea[(op >> 4) & 3], IXEA)
            next_op(15)
        elif op == 0x21:
            ins("move.b (a4)+,%d(a5)" % (ix + 1))
            ins("move.b (a4)+,%d(a5)" % ix)
            next_op(14)
        elif op == 0x22:
            fetch16("d0")
            ins("move.w %s,d1" % IXEA)
            write_d0_d1()
            ins("addq.w #1,d0")
            ins("lsr.w #8,d1")
            write_d0_d1()
            next_op(20)
        elif op == 0x2A:
            fetch16("d0")
            ins("move.b (a3,d0.l),%d(a5)" % (ix + 1))
            ins("move.b 1(a3,d0.l),%d(a5)" % ix)
            next_op(20)
        elif op == 0x23:
            ins("addq.w #1,%s" % IXEA)
            next_op(10)
        elif op == 0x2B:
            ins("subq.w #1,%s" % IXEA)
            next_op(10)
        elif op in (0x24, 0x2C, 0x25, 0x2D):
            off = ix if op in (0x24, 0x25) else ix + 1
            if op in (0x24, 0x2C):
                ins("addq.b #1,%d(a5)" % off)
                incdec_flags("%d(a5)" % off, T_INC)
            else:
                ins("subq.b #1,%d(a5)" % off)
                incdec_flags("%d(a5)" % off, T_DEC)
            next_op(8)
        elif op in (0x26, 0x2E):
            ins("move.b (a4)+,%d(a5)" % (ix if op == 0x26 else ix + 1))
            next_op(11)
        elif op in (0x34, 0x35):
            addr()
            ins("move.b (a3,d0.l),d1")
            ins("addq.b #1,d1" if op == 0x34 else "subq.b #1,d1")
            ins("move.l d0,a1")
            incdec_flags("d1", T_INC if op == 0x34 else T_DEC)
            ins("move.l a1,d0")
            write_d0_d1()
            next_op(23)
        elif op == 0x36:
            addr()
            ins("move.b (a4)+,d1")
            write_d0_d1()
            next_op(19)
        elif op == 0xCB:
            addr()
            ins("moveq #0,d1")
            ins("move.b (a4)+,d1")
            ins("lea xcbtab,a0")
            ins("move.l (a0,d1.w*4),a0")
            ins("jmp (a0)")
        elif op == 0xE1:
            pop_d1()
            ins("move.w d1,%s" % IXEA)
            next_op(14)
        elif op == 0xE5:
            ins("move.w %s,d1" % IXEA)
            push_d1()
            next_op(15)
        elif op == 0xE3:
            ins("moveq #0,d0")
            ins("move.w %d(a5),d0" % O_SP)
            ins("moveq #0,d1")
            ins("move.b 1(a3,d0.l),d1")
            ins("lsl.w #8,d1")
            ins("move.b (a3,d0.l),d1")
            ins("move.w d1,-(sp)")
            ins("move.b %d(a5),d1" % (ix + 1))
            write_d0_d1()
            ins("addq.w #1,d0")
            ins("move.b %d(a5),d1" % ix)
            write_d0_d1()
            ins("move.w (sp)+,%s" % IXEA)
            next_op(23)
        elif op == 0xE9:
            ins("moveq #0,d0")
            ins("move.w %s,d0" % IXEA)
            set_pc("d0")
            next_op(8)
        elif op == 0xF9:
            ins("move.w %s,%d(a5)" % (IXEA, O_SP))
            next_op(10)
        else:
            raise ValueError("%s %02x" % (pre, op))
    return table


def gen_xcb(op):
    """DDCB / FDCB: the address is in d0.l"""
    name = "xcb_%02X" % op
    handler(name)
    r, n, grp = op & 7, (op >> 3) & 7, op >> 6
    ins("move.b (a3,d0.l),d1")
    if grp == 1:
        ins("move.w d0,d2")
        ins("lsr.w #8,d2")                           # X Y from the high byte of the address
        bit_code(n, "d1", "d2")
        next_op(20)
        return
    if grp == 0:
        ins("move.l d0,a1")
        rot_code(n)
        ins("move.l a1,d0")
    else:
        ins("%s #%d,d1" % ("bclr" if grp == 2 else "bset", n))
    write_d0_d1()
    if r != 6:
        set8(r, "d1")
    next_op(23)


# ---------------------------------------------------------------- frame code

def gen_frame():
    emit("""
; int z80_run(Z80 *z, int cycles)
	public	_z80_run
	cnop	0,4
_z80_run:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a5
	move.l	52(sp),d0
	add.l	d0,%(ICOUNT)d(a5)
	move.l	%(ICOUNT)d(a5),d7
	move.l	d7,%(START)d(a5)
	move.l	%(MEM)d(a5),a3
	lea	_z80_tables,a2
	lea	optab,a6
	moveq	#0,d4
	move.w	%(H)d(a5),d4
	move.b	%(F)d(a5),d5
	move.b	%(A)d(a5),d6
	move.b	%(R)d(a5),d3
	moveq	#0,d0
	move.w	%(PC)d(a5),d0
	lea	(a3,d0.l),a4
	tst.l	d7
	ble	done
	bra	check_ints

; the time ran out (or HALT, RETN, the instruction after EI)
exit_loop:
	tst.b	%(EIWIN)d(a5)
	beq.s	.1
	clr.b	%(EIWIN)d(a5)
	add.l	%(EISAVE)d(a5),d7
	subq.l	#1,d7
.1:	tst.l	d7
	ble	done
check_ints:
	tst.b	%(NMI)d(a5)
	bne	take_nmi
	tst.b	%(AFTEREI)d(a5)
	beq.s	.2
	clr.b	%(AFTEREI)d(a5)
	bra	halt_check
.2:	tst.b	%(IRQ)d(a5)
	beq	halt_check
	tst.b	%(IFF1)d(a5)
	bne	take_irq
halt_check:
	tst.b	%(HALTED)d(a5)
	bne	burn
dispatch:
	moveq	#0,d0
	move.b	(a4)+,d0
	addq.b	#1,d3
	move.l	(a6,d0.w*4),a0
	jmp	(a0)

; halted: nothing happens before the next interrupt, which only comes from outside
burn:
	move.l	d7,d0
	addq.l	#3,d0
	asr.l	#2,d0
	add.b	d0,d3
	lsl.l	#2,d0
	sub.l	d0,d7
	bra	done

take_nmi:
	clr.b	%(NMI)d(a5)
	clr.b	%(HALTED)d(a5)
	clr.b	%(IFF1)d(a5)
	addq.b	#1,d3
""" % dict(ICOUNT=O_ICOUNT, START=O_START, MEM=O_MEM, H=O_H, F=O_F, A=O_A, R=O_R, PC=O_PC,
           EIWIN=O_EIWIN, EISAVE=O_EISAVE, NMI=O_NMI, AFTEREI=O_AFTEREI, IRQ=O_IRQ, IFF1=O_IFF1,
           HALTED=O_HALTED))
    get_pc("d1")
    push_d1()
    ins("moveq #$66,d0")
    set_pc("d0")
    ins("sub.l #11,d7")
    ins("bra halt_check")
    emit("""
take_irq:
	clr.b	%(HALTED)d(a5)
	clr.b	%(IFF1)d(a5)
	clr.b	%(IFF2)d(a5)
	addq.b	#1,d3
	tst.b	%(IRQHOLD)d(a5)
	beq.s	.1
	clr.b	%(IRQ)d(a5)
.1:""" % dict(HALTED=O_HALTED, IFF1=O_IFF1, IFF2=O_IFF2, IRQHOLD=O_IRQHOLD, IRQ=O_IRQ))
    get_pc("d1")
    push_d1()
    emit("""	move.b	%(IM)d(a5),d0
	cmp.b	#2,d0
	beq.s	.im2
	cmp.b	#1,d0
	beq.s	.im1
	moveq	#0,d0
	move.b	%(IRQVEC)d(a5),d0
	and.b	#$38,d0
	bra.s	.go13
.im1:	moveq	#$38,d0
.go13:	lea	(a3,d0.l),a4
	sub.l	#13,d7
	bra	halt_check
.im2:	moveq	#0,d0
	move.b	%(I)d(a5),d0
	lsl.w	#8,d0
	move.b	%(IRQVEC)d(a5),d0
	moveq	#0,d1
	move.b	1(a3,d0.l),d1
	lsl.w	#8,d1
	move.b	(a3,d0.l),d1
	lea	(a3,d1.l),a4
	sub.l	#19,d7
	bra	halt_check

done:
	move.l	a4,d0
	sub.l	a3,d0
	move.w	d0,%(PC)d(a5)
	move.w	d4,%(H)d(a5)
	move.b	d5,%(F)d(a5)
	move.b	d6,%(A)d(a5)
	move.b	d3,%(R)d(a5)
	move.l	d7,%(ICOUNT)d(a5)
	move.l	%(START)d(a5),d0
	sub.l	d7,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; slow writes: d0.l address, d1.b value, d2 page; keeps d0 d1
wr_slow:
	move.l	d2,a1
	move.b	(%(WTYPE)d,a5,d2.w),d2
	subq.b	#2,d2
	bmi.s	.rom
	bne.s	.io
	move.b	d1,(a3,d0.l)
	move.l	a1,d2
	lsl.w	#2,d2
	move.l	(%(WMIRROR)d,a5,d2.w),a0
	move.w	d0,d2
	and.w	#$ff,d2
	move.b	d1,(a0,d2.w)
.rom:	rts
.io:	movem.l	d0-d1/a0-a1,-(sp)
	moveq	#0,d2
	move.b	d1,d2
	move.l	d2,-(sp)
	moveq	#0,d2
	move.w	d0,d2
	move.l	d2,-(sp)
	move.l	%(CTX)d(a5),-(sp)
	move.l	%(WRITE)d(a5),a0
	jsr	(a0)
	lea	12(sp),sp
	movem.l	(sp)+,d0-d1/a0-a1
	rts

; d0.l port -> d0.b value
call_in:
	move.l	d0,-(sp)
	move.l	%(CTX)d(a5),-(sp)
	move.l	%(INFN)d(a5),a0
	jsr	(a0)
	addq.l	#8,sp
	rts

; d0.l port, d1.b value
call_out:
	moveq	#0,d2
	move.b	d1,d2
	move.l	d2,-(sp)
	move.l	d0,-(sp)
	move.l	%(CTX)d(a5),-(sp)
	move.l	%(OUTFN)d(a5),a0
	jsr	(a0)
	lea	12(sp),sp
	rts

; LDI/LDD flags: d1 = the byte moved
ld_flags:
	and.b	#$c1,d5
	tst.w	%(B)d(a5)
	beq.s	.1
	or.b	#4,d5
.1:	add.b	d6,d1
	move.b	d1,d0
	and.b	#$08,d0
	or.b	d0,d5
	and.b	#$02,d1
	lsl.b	#4,d1
	or.b	d1,d5
	rts

; CPI/CPD flags: d2 = the byte compared (B C already decremented)
cp_flags:
	move.b	d6,d1
	sub.b	d2,d1			; r = A - v
	and.b	#1,d5
	or.b	#2,d5
	moveq	#0,d0
	move.b	d1,d0
	move.b	(%(SZXY)d,a2,d0.w),d0
	and.b	#$c0,d0
	or.b	d0,d5
	move.b	d6,d0
	eor.b	d2,d0
	eor.b	d1,d0
	and.b	#$10,d0
	or.b	d0,d5
	tst.w	%(B)d(a5)
	beq.s	.1
	or.b	#4,d5
.1:	btst	#4,d5
	beq.s	.2
	subq.b	#1,d1			; n = r - H
.2:	move.b	d1,d0
	and.b	#$08,d0
	or.b	d0,d5
	and.b	#$02,d1
	lsl.b	#4,d1
	or.b	d1,d5
	rts

; INI/OUTI flags: d1 = the byte, d2 = the byte added to it (C+-1 or L), B already decremented
io_flags:
	moveq	#0,d0
	move.b	%(B)d(a5),d0
	move.b	(%(SZXY)d,a2,d0.w),d5
	tst.b	d1
	bpl.s	.1
	or.b	#2,d5
.1:	and.w	#$ff,d1
	and.w	#$ff,d2
	add.w	d2,d1			; k
	cmp.w	#$ff,d1
	bls.s	.2
	or.b	#$11,d5
.2:	and.b	#7,d1
	eor.b	d0,d1
	and.w	#$ff,d1
	move.b	(%(SZXYP)d,a2,d1.w),d1
	and.b	#4,d1
	or.b	d1,d5
	rts
""" % dict(IM=O_IM, IRQVEC=O_IRQVEC, I=O_I, PC=O_PC, H=O_H, F=O_F, A=O_A, R=O_R, ICOUNT=O_ICOUNT,
           START=O_START, WTYPE=O_WTYPE, WMIRROR=O_WMIRROR, CTX=O_CTX, WRITE=O_WRITE, INFN=O_IN,
           OUTFN=O_OUT, B=O_B, SZXY=T_SZXY, SZXYP=T_SZXYP))


def main():
    emit("; Z80 core for the 68k - generated by tools/gen_z80_68k.py, do not edit",
         "\tmachine\t68060", "\tsection\t\"CODE\",code", "\txref\t_z80_tables", "\txref\t_z80_daa")
    gen_frame()
    for op in range(256):
        gen_main(op)
    for op in range(256):
        gen_cb(op)
    for op in range(256):
        gen_ed(op)
    ddt = gen_index("dd", O_IX)
    fdt = gen_index("fd", O_IY)
    for op in range(256):
        gen_xcb(op)

    def table(name, entries):
        emit("", "\tcnop\t0,4", name)
        for i in range(0, 256, 8):
            ins("dc.l " + ",".join(entries[i:i + 8]))

    table("optab", ["op_%02X" % i for i in range(256)])
    table("cbtab", ["cb_%02X" % i for i in range(256)])
    table("edtab", ["ed_%02X" % i for i in range(256)])
    table("ddtab", ddt)
    table("fdtab", fdt)
    table("xcbtab", ["xcb_%02X" % i for i in range(256)])
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d lines" % (OUT, len(lines)))


if __name__ == "__main__":
    main()
