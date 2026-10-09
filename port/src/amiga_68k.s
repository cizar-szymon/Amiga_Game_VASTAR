; Vastar port - Amiga frontend helpers in 68k assembler (vasm, Motorola syntax)
;
; void copy_rows(const UBYTE *src, UBYTE *dst, ULONG bytes, ULONG rows,
;                ULONG src_mod, ULONG dst_mod, ULONG move16)
;
; Copies `rows` rows of `bytes` bytes from fast RAM to VRAM, only writing VRAM
; (see "Warp RTG Developer notes": never mix VRAM reads and writes, write wide).
; move16 != 0: src, dst, both modulos and bytes are multiples of 16, MOVE16 is used;
; otherwise long moves (bytes a multiple of 4).

	machine	68060
	section	"CODE",code

	public	_copy_rows
	cnop	0,4
_copy_rows:
	movem.l	d2-d6/a2-a3,-(sp)
	move.l	32(sp),a0		; src
	move.l	36(sp),a1		; dst
	move.l	40(sp),d0		; bytes
	move.l	44(sp),d1		; rows
	move.l	48(sp),d2		; src_mod
	move.l	52(sp),d3		; dst_mod
	tst.l	56(sp)
	beq.s	.longs
	tst.l	d1
	beq.s	.done
	lsr.l	#4,d0			; 16 byte lines per row
.r16:	move.l	a0,a2
	move.l	a1,a3
	move.l	d0,d4
.l16:	move16	(a2)+,(a3)+
	subq.l	#1,d4
	bne.s	.l16
	add.l	d2,a0
	add.l	d3,a1
	subq.l	#1,d1
	bne.s	.r16
	bra.s	.done
.longs:	tst.l	d1
	beq.s	.done
	lsr.l	#2,d0			; longs per row
.rl:	move.l	a0,a2
	move.l	a1,a3
	move.l	d0,d4
.ll:	move.l	(a2)+,(a3)+
	subq.l	#1,d4
	bne.s	.ll
	add.l	d2,a0
	add.l	d3,a1
	subq.l	#1,d1
	bne.s	.rl
.done:	movem.l	(sp)+,d2-d6/a2-a3
	rts

; void convert16(const UBYTE *src, UBYTE *dst, const UBYTE *flags, const void *lut,
;                ULONG dst_bpr, ULONG scale)
;
; One row of 8x8 blocks of palette indices (src = first of 8 rows of 224 bytes) into
; the 16 bit frame in fast RAM (dst = first row; scale 1: 16 bytes per block row,
; lut = UWORD[256]; scale 2: 32 bytes per block row and two rows per source row,
; lut = ULONG[256], the pixel twice). Blocks where flags[bx] == 0 are skipped.
	public	_convert16
	cnop	0,4
_convert16:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a2		; src
	move.l	52(sp),a3		; dst
	move.l	56(sp),a4		; flags
	move.l	60(sp),a5		; lut
	move.l	64(sp),d6		; dst_bpr
	moveq	#0,d7			; bx
	moveq	#0,d0
	cmp.l	#2,68(sp)
	beq	.s2
.b1:	tst.b	(a4,d7.l)
	beq.s	.n1
	move.l	d7,d1
	lsl.l	#3,d1
	lea	(a2,d1.l),a0		; src block
	add.l	d1,d1
	lea	(a3,d1.l),a1		; dst block (16 bytes per block row)
	moveq	#7,d2
.y1:	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	move.b	(a0)+,d0
	move.w	(a5,d0.w*2),(a1)+
	lea	224-8(a0),a0
	lea	-16(a1,d6.l),a1
	dbf	d2,.y1
.n1:	addq.l	#1,d7
	cmp.l	#28,d7
	blt.s	.b1
	bra	.done

.s2:	tst.b	(a4,d7.l)
	beq.s	.n2
	move.l	d7,d1
	lsl.l	#3,d1
	lea	(a2,d1.l),a0		; src block
	lsl.l	#2,d1
	lea	(a3,d1.l),a1		; dst block (32 bytes per block row)
	moveq	#7,d2
.y2:	lea	(a1,d6.l),a6		; the second row
	moveq	#7,d3
.x2:	move.b	(a0)+,d0
	move.l	(a5,d0.w*4),d4
	move.l	d4,(a1)+
	move.l	d4,(a6)+
	dbf	d3,.x2
	lea	224-8(a0),a0
	lea	-32(a1,d6.l),a1
	add.l	d6,a1			; two rows down
	dbf	d2,.y2
.n2:	addq.l	#1,d7
	cmp.l	#28,d7
	blt.s	.s2
.done:	movem.l	(sp)+,d2-d7/a2-a6
	rts
