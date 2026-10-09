; Vastar port - inner loops of video.c in 68k assembler (vasm, Motorola syntax)

	machine	68060
	section	"CODE",code

; int vscan(const uint8_t *r0, const uint8_t *s0, const uint8_t *r1, const uint8_t *s1,
;           const uint8_t *r2, const uint8_t *s2, int from)
; The first i >= from (a multiple of 4) where the longs at i of r0/s0, r1/s1 or r2/s2
; differ; 1024 if there is none (the tile RAM against its copy, see update_layer).
	public	_vscan
	cnop	0,4
_vscan:
	movem.l	a2-a5,-(sp)
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),a2
	move.l	32(sp),a3
	move.l	36(sp),a4
	move.l	40(sp),a5
	move.l	44(sp),d0
.l:	cmp.l	#1024,d0
	bge.s	.done
	move.l	(a0,d0.l),d1
	cmp.l	(a1,d0.l),d1
	bne.s	.done
	move.l	(a2,d0.l),d1
	cmp.l	(a3,d0.l),d1
	bne.s	.done
	move.l	(a4,d0.l),d1
	cmp.l	(a5,d0.l),d1
	bne.s	.done
	addq.l	#4,d0
	bra.s	.l
.done:	movem.l	(sp)+,a2-a5
	rts

; void comp_layer_asm(UBYTE *t, const UWORD *ptile, const UBYTE *tile_any,
;                     const UBYTE *code, const UBYTE *attr, const UBYTE *col,
;                     ULONG scroll, ULONG by, ULONG bx, ULONG opaque, const ULONG *tabs)
;
; One tile layer into the 8x8 block t (8 bytes per row) - comp_layer() in video.c.
; ptile = Video.ptile[k] (512 tiles x 2 flips x 8 UWORDs), tile_any = Video.tile_any[k],
; code / attr / col = the tile RAM arrays, tabs = Video.xpix (followed by xmask and split).
	public	_comp_layer_asm
	cnop	0,4
_comp_layer_asm:
	movem.l	d2-d7/a2-a6,-(sp)
	; t0 = (239 - bx*8 + scroll) & 255
	move.l	80(sp),d0		; bx
	lsl.l	#3,d0
	move.l	#239,d1
	sub.l	d0,d1
	add.l	72(sp),d1		; + scroll
	and.l	#255,d1			; t0
	move.l	d1,d7
	and.l	#7,d7			; fa
	lsr.l	#3,d1			; ra
	move.l	d1,d2
	subq.l	#1,d2
	and.l	#31,d2			; rb
	move.l	76(sp),d3		; by
	lsl.l	#5,d1
	or.l	d3,d1			; ia
	lsl.l	#5,d2
	or.l	d3,d2			; ib
	move.l	64(sp),a0		; attr
	moveq	#0,d3
	move.b	(a0,d1.l),d3		; aa
	moveq	#0,d4
	move.b	(a0,d2.l),d4		; ab
	move.l	60(sp),a0		; code
	moveq	#0,d5
	move.b	d3,d5
	lsl.w	#8,d5
	move.b	(a0,d1.l),d5
	and.w	#$1ff,d5		; ca
	moveq	#0,d6
	move.b	d4,d6
	lsl.w	#8,d6
	move.b	(a0,d2.l),d6
	and.w	#$1ff,d6		; cb
	; transparent layer over two empty tiles: nothing to do
	tst.l	84(sp)
	bne.s	.go
	move.l	56(sp),a0		; tile_any
	tst.b	(a0,d5.l)
	bne.s	.go
	cmp.w	#7,d7
	beq	.ret
	tst.b	(a0,d6.l)
	beq	.ret
.go:	; colours: cola / colb replicated, then split at fa + 1
	move.l	68(sp),a0		; col
	moveq	#0,d0
	move.b	(a0,d1.l),d0
	and.b	#$3f,d0
	lsl.b	#2,d0
	mulu.l	#$01010101,d0		; cola
	moveq	#0,d1
	move.b	(a0,d2.l),d1
	and.b	#$3f,d1
	lsl.b	#2,d1
	mulu.l	#$01010101,d1		; colb
	move.l	88(sp),a6		; tabs: xpix, xmask (+1024), split (+2048)
	move.l	d7,d2
	addq.l	#1,d2
	lsl.l	#3,d2
	lea	2048(a6,d2.l),a0	; split[fa + 1]
	move.l	(a0),d2			; sel0
	move.l	d0,a1
	and.l	d2,d0
	not.l	d2
	and.l	d1,d2
	or.l	d2,d0			; col0 (left 4 pixels)
	move.l	4(a0),d2		; sel1
	exg	a1,d0			; d0 = cola, a1 = col0
	and.l	d2,d0
	not.l	d2
	and.l	d2,d1
	or.l	d1,d0
	move.l	d0,a5			; col1 (right 4 pixels)
	; tile row pointers and steps: flip x reads the rows backwards
	move.l	52(sp),a0		; ptile
	lsl.l	#5,d5			; 32 bytes per tile
	btst	#3,d3
	beq.s	.fa0
	add.l	#16,d5			; flip y variant
.fa0:	lea	(a0,d5.l),a2		; wa
	moveq	#2,d5			; step a
	btst	#2,d3
	beq.s	.xa0
	lea	14(a2),a2
	moveq	#-2,d5
.xa0:	lsl.l	#5,d6
	btst	#3,d4
	beq.s	.fb0
	add.l	#16,d6
.fb0:	lea	(a0,d6.l),a3		; wb
	moveq	#2,d6			; step b
	btst	#2,d4
	beq.s	.xb0
	lea	14(a3),a3
	moveq	#-2,d6
.xb0:	move.l	d7,d4
	neg.l	d4
	addq.l	#7,d4
	add.l	d4,d4			; shift = 2 * (7 - fa)
	move.l	48(sp),a4		; t
	moveq	#7,d7			; rows
	tst.l	84(sp)
	beq.s	.trans
.op:	move.w	(a2),d0
	swap	d0
	move.w	(a3),d0
	add.l	d5,a2
	add.l	d6,a3
	lsl.l	d4,d0
	swap	d0			; the 8 pixels in the low word
	moveq	#0,d1
	move.w	d0,d1
	lsr.w	#8,d1			; left 4
	and.l	#$ff,d0			; right 4
	move.l	(a6,d1.l*4),d2
	move.l	a1,d3
	or.l	d3,d2
	move.l	d2,(a4)+
	move.l	(a6,d0.l*4),d2
	move.l	a5,d3
	or.l	d3,d2
	move.l	d2,(a4)+
	dbf	d7,.op
	bra.s	.ret
.trans:	move.w	(a2),d0
	swap	d0
	move.w	(a3),d0
	add.l	d5,a2
	add.l	d6,a3
	lsl.l	d4,d0
	swap	d0
	moveq	#0,d1
	move.w	d0,d1
	lsr.w	#8,d1			; left 4
	and.l	#$ff,d0			; right 4
	move.l	1024(a6,d1.l*4),d2	; mask
	beq.s	.t1
	move.l	(a6,d1.l*4),d3
	move.l	a1,d1
	or.l	d1,d3
	and.l	d2,d3			; new pixels
	not.l	d2
	and.l	(a4),d2
	or.l	d3,d2
	move.l	d2,(a4)
.t1:	move.l	1024(a6,d0.l*4),d2
	beq.s	.t2
	move.l	(a6,d0.l*4),d3
	move.l	a5,d1
	or.l	d1,d3
	and.l	d2,d3
	not.l	d2
	and.l	4(a4),d2
	or.l	d3,d2
	move.l	d2,4(a4)
.t2:	addq.l	#8,a4
	dbf	d7,.trans
.ret:	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void strip_layer_asm(UBYTE *strip, const UWORD *ptile, const UBYTE *tile_any,
;                      const UBYTE *code, const UBYTE *attr, const UBYTE *col,
;                      ULONG scroll, ULONG by, ULONG opaque, const ULONG *tabs)
;
; One tile layer into the strip of a block row (8 rows of 240 bytes, screen x 0 at
; index 8): the tiles of the column one after the other - strip_layer_c() in video.c.
	public	_strip_layer_asm
	cnop	0,4
_strip_layer_asm:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	72(sp),d0		; scroll
	add.l	#239,d0
	and.l	#255,d0			; t0 = tilemap row of screen x 0
	move.l	d0,d7
	and.l	#7,d7
	addq.l	#1,d7			; off = index of the first tile's left pixel
	lsr.l	#3,d0
	lsl.l	#5,d0
	or.l	76(sp),d0
	move.l	d0,d6			; i = r << 5 | by
	move.l	64(sp),a0		; attr
	move.l	60(sp),a1		; code
	move.l	68(sp),a2		; col
	move.l	52(sp),a5		; ptile
	move.l	84(sp),a6		; tabs: xpix, xmask (+1024)
	tst.l	80(sp)
	beq	.ttile

.otile:	cmp.l	#232,d7
	bge	.ret
	moveq	#0,d1
	move.b	(a0,d6.l),d1		; attribute
	moveq	#0,d2
	move.b	d1,d2
	lsl.w	#8,d2
	move.b	(a1,d6.l),d2
	and.w	#$1ff,d2		; tile
	moveq	#0,d3
	move.b	(a2,d6.l),d3
	and.b	#$3f,d3
	lsl.b	#2,d3
	mulu.l	#$01010101,d3		; colour
	lsl.l	#5,d2
	btst	#3,d1
	beq.s	.of
	add.l	#16,d2			; flip y variant
.of:	lea	(a5,d2.l),a3
	moveq	#2,d5
	btst	#2,d1
	beq.s	.ox
	lea	14(a3),a3		; flip x: rows backwards
	moveq	#-2,d5
.ox:	move.l	48(sp),a4
	add.l	d7,a4
	moveq	#7,d2
.orow:	move.w	(a3),d4
	add.l	d5,a3
	moveq	#0,d1
	move.b	d4,d1			; right 4 pixels
	moveq	#0,d0
	move.w	d4,d0
	lsr.w	#8,d0			; left 4 pixels
	move.l	(a6,d0.l*4),d0
	or.l	d3,d0
	move.l	d0,(a4)
	move.l	(a6,d1.l*4),d1
	or.l	d3,d1
	move.l	d1,4(a4)
	lea	240(a4),a4
	dbf	d2,.orow
	addq.l	#8,d7
	sub.l	#32,d6
	and.l	#$3ff,d6
	bra	.otile

.ttile:	cmp.l	#232,d7
	bge	.ret
	moveq	#0,d1
	move.b	(a0,d6.l),d1
	moveq	#0,d2
	move.b	d1,d2
	lsl.w	#8,d2
	move.b	(a1,d6.l),d2
	and.w	#$1ff,d2
	move.l	56(sp),a3		; tile_any
	tst.b	(a3,d2.l)
	beq	.tnext			; empty tile
	moveq	#0,d3
	move.b	(a2,d6.l),d3
	and.b	#$3f,d3
	lsl.b	#2,d3
	mulu.l	#$01010101,d3
	lsl.l	#5,d2
	btst	#3,d1
	beq.s	.tf
	add.l	#16,d2
.tf:	lea	(a5,d2.l),a3
	moveq	#2,d5
	btst	#2,d1
	beq.s	.tx
	lea	14(a3),a3
	moveq	#-2,d5
.tx:	move.l	48(sp),a4
	add.l	d7,a4
	moveq	#7,d2
.trow:	move.w	(a3),d4
	add.l	d5,a3
	moveq	#0,d1
	move.b	d4,d1
	moveq	#0,d0
	move.w	d4,d0
	lsr.w	#8,d0
	move.l	1024(a6,d0.l*4),d4	; mask of the left 4
	beq.s	.t1
	move.l	(a6,d0.l*4),d0
	or.l	d3,d0
	and.l	d4,d0
	not.l	d4
	and.l	(a4),d4
	or.l	d4,d0
	move.l	d0,(a4)
.t1:	move.l	1024(a6,d1.l*4),d4	; mask of the right 4
	beq.s	.t2
	move.l	(a6,d1.l*4),d1
	or.l	d3,d1
	and.l	d4,d1
	not.l	d4
	and.l	4(a4),d4
	or.l	d4,d1
	move.l	d1,4(a4)
.t2:	lea	240(a4),a4
	dbf	d2,.trow
.tnext:	addq.l	#8,d7
	sub.l	#32,d6
	and.l	#$3ff,d6
	bra	.ttile

.ret:	movem.l	(sp)+,d2-d7/a2-a6
	rts
