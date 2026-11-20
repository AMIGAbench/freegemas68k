* ---------------------------------------------------------------------------
* The hot loops of freegemas for AmigaOS, plain 68k (68060 and 68080).
*
* Every kernel gives exactly the result of its C reference in render.cpp /
* display.cpp / audio.cpp; the self-test at start (fg_kernels_init) checks
* that and keeps the C path for any kernel that differs.
*
* Blending, as the C path (blend565):
*     x   = (p | p << 16) & $07E0F81F      (the three fields spread)
*     out = d + (((s - d) * a5) >> 5)      (32-bit, masked again)
*     a5  = (a + 4) >> 3, a = 0..255
*
* Calling: Bebbo gcc, arguments on the stack; d0-d1/a0-a1 are scratch.
* vasm: -no-opt (all branch sizes written out).
* ---------------------------------------------------------------------------

	section	.text,code

MASK	equ	$07E0F81F

* d (zero-extended word in dreg) -> spread in the same register; uses tmp
SPREAD	macro
	move.l	\1,\2
	swap	\2
	or.l	\2,\1
	and.l	d7,\1
	endm

* \1 = spread d, \2 = spread s (destroyed: the result), \3 = a5, \4 = tmp
* result packed in the low word of \2
BLENDX	macro
	sub.l	\1,\2
	mulu.l	\3,\2
	lsr.l	#5,\2
	add.l	\1,\2
	and.l	d7,\2
	move.l	\2,\4
	swap	\4
	or.w	\4,\2
	endm

*---------------------------------------------------------------------------
* void fg_k_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n)
*   a == 0: unchanged, a == 255: copy, else blend
*---------------------------------------------------------------------------
	xdef	_fg_k_blend_row
_fg_k_blend_row:
	movem.l	d2-d7/a2,-(sp)		; 28 bytes
	move.l	32(sp),a0
	move.l	36(sp),a1
	move.l	40(sp),a2
	move.l	44(sp),d0
	subq.l	#1,d0
	bmi.w	.done
	move.l	#MASK,d7
.loop
	moveq	#0,d1
	move.b	(a2)+,d1
	beq.w	.skip
	cmp.w	#255,d1
	beq.w	.copy
	addq.w	#4,d1
	lsr.w	#3,d1			; a5
	moveq	#0,d2
	move.w	(a0),d2
	SPREAD	d2,d3
	moveq	#0,d4
	move.w	(a1)+,d4
	SPREAD	d4,d5
	BLENDX	d2,d4,d1,d5
	move.w	d4,(a0)+
	subq.l	#1,d0
	bpl.w	.loop
	bra.w	.done
.skip
	addq.l	#2,a0
	addq.l	#2,a1
	subq.l	#1,d0
	bpl.w	.loop
	bra.w	.done
.copy
	move.w	(a1)+,(a0)+
	subq.l	#1,d0
	bpl.w	.loop
.done
	movem.l	(sp)+,d2-d7/a2
	rts

*---------------------------------------------------------------------------
* void fg_k_blend_row_mod(uint16_t *dst, const uint16_t *src,
*                         const uint8_t *alpha /* NULL: 255 */, int n, int mod)
*   a = (a * mod + 255) >> 8; a == 0: unchanged, else blend
*---------------------------------------------------------------------------
	xdef	_fg_k_blend_row_mod
_fg_k_blend_row_mod:
	movem.l	d2-d7/a2,-(sp)
	move.l	32(sp),a0
	move.l	36(sp),a1
	move.l	40(sp),a2
	move.l	44(sp),d0
	move.l	48(sp),d6		; mod
	subq.l	#1,d0
	bmi.w	.mdone
	move.l	#MASK,d7
.mloop
	move.l	#255,d1
	tst.l	a2
	beq.w	.mconst
	moveq	#0,d1
	move.b	(a2)+,d1
.mconst
	mulu.w	d6,d1
	add.l	#255,d1
	lsr.l	#8,d1
	beq.w	.mskip
	addq.w	#4,d1
	lsr.w	#3,d1			; a5
	moveq	#0,d2
	move.w	(a0),d2
	SPREAD	d2,d3
	moveq	#0,d4
	move.w	(a1)+,d4
	SPREAD	d4,d5
	BLENDX	d2,d4,d1,d5
	move.w	d4,(a0)+
	subq.l	#1,d0
	bpl.w	.mloop
	bra.w	.mdone
.mskip
	addq.l	#2,a0
	addq.l	#2,a1
	subq.l	#1,d0
	bpl.w	.mloop
.mdone
	movem.l	(sp)+,d2-d7/a2
	rts

*---------------------------------------------------------------------------
* void fg_k_copy_row(uint16_t *dst, const uint16_t *src, int n)   (n pixels)
*   longword copy, 16 bytes per pass; the destination is aligned to 4 first
*---------------------------------------------------------------------------
	xdef	_fg_k_copy_row
_fg_k_copy_row:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d0
	ble.w	.cdone
	move.l	a0,d1
	btst	#1,d1
	beq.w	.caligned
	move.w	(a1)+,(a0)+
	subq.l	#1,d0
.caligned
	move.l	d0,d1
	lsr.l	#3,d1			; blocks of 8 pixels
	beq.w	.ctail
	subq.l	#1,d1
.cblock
	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	subq.l	#1,d1
	bpl.w	.cblock
.ctail
	and.l	#7,d0
	beq.w	.cdone
	subq.l	#1,d0
.cword
	move.w	(a1)+,(a0)+
	subq.l	#1,d0
	bpl.w	.cword
.cdone
	rts

*---------------------------------------------------------------------------
* void fg_k_scaled_row(uint16_t *dst, const uint16_t *srow, const uint8_t *arow,
*                      uint32_t fx, uint32_t step, int n, int mod)
*   the scaled texture row (16.16 source position fx, step per pixel);
*   arow NULL: alpha 255. a = mod == 255 ? a : (a * mod + 255) >> 8;
*   a == 0: unchanged, a == 255: copy, else blend
*---------------------------------------------------------------------------
	xdef	_fg_k_scaled_row
_fg_k_scaled_row:
	movem.l	d2-d7/a2-a4,-(sp)	; 36 bytes
	move.l	40(sp),a0		; dst
	move.l	44(sp),a1		; srow
	move.l	48(sp),a2		; arow
	move.l	52(sp),d0		; fx
	move.l	56(sp),a3		; step
	move.l	60(sp),d6		; n
	move.l	64(sp),a4		; mod
	move.l	#MASK,d7
	subq.l	#1,d6
	bmi.w	.sdone
.sloop
	move.l	d0,d2
	clr.w	d2
	swap	d2			; xs = fx >> 16
	add.l	a3,d0
	move.l	#255,d1
	tst.l	a2
	beq.w	.sha
	moveq	#0,d1
	move.b	(a2,d2.l),d1
.sha
	cmpa.l	#255,a4
	beq.w	.snomod
	move.l	a4,d3
	mulu.w	d3,d1
	add.l	#255,d1
	lsr.l	#8,d1
.snomod
	tst.l	d1
	beq.w	.sskip
	cmp.w	#255,d1
	beq.w	.scopy
	addq.w	#4,d1
	lsr.w	#3,d1			; a5
	moveq	#0,d4
	move.w	(a1,d2.l*2),d4		; s
	SPREAD	d4,d5
	moveq	#0,d3
	move.w	(a0),d3			; d
	SPREAD	d3,d5
	BLENDX	d3,d4,d1,d5
	move.w	d4,(a0)+
	subq.l	#1,d6
	bpl.w	.sloop
	bra.w	.sdone
.scopy
	move.w	(a1,d2.l*2),(a0)+
	subq.l	#1,d6
	bpl.w	.sloop
	bra.w	.sdone
.sskip
	addq.l	#2,a0
	subq.l	#1,d6
	bpl.w	.sloop
.sdone
	movem.l	(sp)+,d2-d7/a2-a4
	rts

*---------------------------------------------------------------------------
* int16_t *fg_k_scaled_spans(const uint16_t *srow, const uint8_t *arow,
*         const uint16_t *runs, const uint16_t *runsEnd, uint32_t fx, uint32_t step,
*         int n, uint16_t *gs, uint8_t *ga, int16_t *spans)
*   One row of a scaled texture: walks the source row's runs itself (no
*   division), jumps over transparent runs and gathers the visible pixels
*   (gs, ga: arow NULL = 255) one after the other. Each visible stretch is
*   recorded in spans as (offset, kind, count); returns the end of spans.
*---------------------------------------------------------------------------
	xdef	_fg_k_scaled_spans
_fg_k_scaled_spans:
	movem.l	d2-d7/a2-a6,-(sp)	; 44 bytes
	move.l	48(sp),a0		; srow
	move.l	52(sp),a1		; arow (0: hard mask, alpha 255)
	move.l	56(sp),a2		; runs
	move.l	60(sp),a3		; runsEnd
	move.l	64(sp),d0		; fx
	move.l	68(sp),d1		; step
	move.l	72(sp),d2		; n (pixels left)
	move.l	76(sp),a4		; gs
	move.l	80(sp),a5		; ga
	move.l	84(sp),a6		; spans
	moveq	#0,d3			; lim = end of the current run, 16.16
	moveq	#0,d4			; kind of the current run
	moveq	#-1,d7			; gs address where the open span began (-1: none)
	tst.l	d2
	ble.w	.pend
.ploop
	cmp.l	d3,d0
	bcs.w	.pinrun			; fx < lim
	cmpa.l	a3,a2
	bcc.w	.pend			; no runs left
	tst.l	d7			; (a new run: the open span ends)
	bmi.w	.pnoclose
	move.l	a4,d5
	sub.l	d7,d5
	lsr.l	#1,d5
	move.w	d5,(a6)+		; count
	moveq	#-1,d7
.pnoclose
	moveq	#0,d6
	move.w	(a2)+,d6
	move.l	d6,d4
	moveq	#14,d5
	lsr.l	d5,d4			; kind
	and.l	#$3FFF,d6
	swap	d6			; length << 16
	add.l	d6,d3
	bra.w	.ploop
.pinrun
	tst.w	d4
	beq.w	.ptrans
	tst.l	d7
	bpl.w	.phave
	move.l	72(sp),d5		; open: offset = n - pixels left
	sub.l	d2,d5
	move.w	d5,(a6)+
	move.w	d4,(a6)+
	move.l	a4,d7
.phave
	move.l	d0,d6
	swap	d6			; xs in the low word (one pixel ahead from here)
	tst.l	a1
	beq.w	.vn
.va
	move.w	(a0,d6.w*2),(a4)+
	move.b	(a1,d6.w),(a5)+
	add.l	d1,d0
	move.l	d0,d6
	swap	d6			; (swap sets the flags: the count test comes after it)
	subq.l	#1,d2
	beq.w	.pend
	cmp.l	d3,d0
	bcs.w	.va
	bra.w	.ploop
.vn
	move.w	(a0,d6.w*2),(a4)+
	st	(a5)+
	add.l	d1,d0
	move.l	d0,d6
	swap	d6			; (swap sets the flags: the count test comes after it)
	subq.l	#1,d2
	beq.w	.pend
	cmp.l	d3,d0
	bcs.w	.vn
	bra.w	.ploop
.ptrans
	add.l	d1,d0			; transparent: just walk on
	subq.l	#1,d2
	beq.w	.pend
	cmp.l	d3,d0
	bcs.w	.ptrans
	bra.w	.ploop
.pend
	tst.l	d7
	bmi.w	.pfin
	move.l	a4,d5
	sub.l	d7,d5
	lsr.l	#1,d5
	move.w	d5,(a6)+
.pfin
	move.l	a6,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

*---------------------------------------------------------------------------
* void fg_k_gather(uint16_t *gs, uint8_t *ga, const uint16_t *srow, const uint8_t *arow,
*                  uint32_t fx, uint32_t step, int n)
*   gs[k] = srow[fx >> 16], ga[k] = arow ? arow[fx >> 16] : 255
*---------------------------------------------------------------------------
	xdef	_fg_k_gather
_fg_k_gather:
	movem.l	d2-d4/a2-a3,-(sp)	; 20 bytes
	move.l	24(sp),a0		; gs
	move.l	28(sp),a1		; ga
	move.l	32(sp),a2		; srow
	move.l	36(sp),a3		; arow
	move.l	40(sp),d0		; fx
	move.l	44(sp),d1		; step
	move.l	48(sp),d3		; n
	subq.l	#1,d3
	bmi.w	.gdone
	tst.l	a3
	beq.w	.gnoalpha
.gloop
	move.l	d0,d2
	clr.w	d2
	swap	d2
	add.l	d1,d0
	move.w	(a2,d2.l*2),(a0)+
	move.b	(a3,d2.l),(a1)+
	subq.l	#1,d3
	bpl.w	.gloop
	bra.w	.gdone
.gnoalpha
	move.b	#255,d4
.gnloop
	move.l	d0,d2
	clr.w	d2
	swap	d2
	add.l	d1,d0
	move.w	(a2,d2.l*2),(a0)+
	move.b	d4,(a1)+
	subq.l	#1,d3
	bpl.w	.gnloop
.gdone
	movem.l	(sp)+,d2-d4/a2-a3
	rts

*---------------------------------------------------------------------------
* void fg_k_conv16(uint32_t *dst, const uint32_t *src, int pairs,
*                  const uint32_t *hi, const uint32_t *lo)
*   two RGB565 pixels per longword -> the screen's 16-bit format:
*   p -> hi[p >> 8] | lo[p & 255]
*---------------------------------------------------------------------------
	xdef	_fg_k_conv16
_fg_k_conv16:
	movem.l	d2-d5/a2-a3,-(sp)	; 24 bytes
	move.l	28(sp),a0
	move.l	32(sp),a1
	move.l	36(sp),d0
	move.l	40(sp),a2		; hi
	move.l	44(sp),a3		; lo
	subq.l	#1,d0
	bmi.w	.vdone
.vloop
	move.l	(a1)+,d1		; p0 << 16 | p1
	moveq	#0,d2
	move.w	d1,d2			; p1
	move.l	d2,d3
	lsr.l	#8,d3
	and.l	#255,d2
	move.l	(a3,d2.l*4),d4
	or.l	(a2,d3.l*4),d4		; conv(p1)
	swap	d1
	moveq	#0,d2
	move.w	d1,d2			; p0
	move.l	d2,d3
	lsr.l	#8,d3
	and.l	#255,d2
	move.l	(a3,d2.l*4),d5
	or.l	(a2,d3.l*4),d5		; conv(p0)
	swap	d5
	move.w	d4,d5
	move.l	d5,(a0)+
	subq.l	#1,d0
	bpl.w	.vloop
.vdone
	movem.l	(sp)+,d2-d5/a2-a3
	rts

*---------------------------------------------------------------------------
* void fg_k_mix_add(int16_t *acc, const int8_t *src, int n, int vol)
*   acc[i] += vol == 256 ? s[i] : (s[i] * vol) >> 8     (arithmetic shift)
*---------------------------------------------------------------------------
	xdef	_fg_k_mix_add
_fg_k_mix_add:
	move.l	d2,-(sp)
	move.l	8(sp),a0
	move.l	12(sp),a1
	move.l	16(sp),d0
	move.l	20(sp),d2
	subq.l	#1,d0
	bmi.w	.adone
	cmp.l	#256,d2
	beq.w	.afull
.aloop
	move.b	(a1)+,d1
	ext.w	d1
	muls.w	d2,d1
	asr.l	#8,d1
	add.w	d1,(a0)+
	subq.l	#1,d0
	bpl.w	.aloop
	bra.w	.adone
.afull
	move.b	(a1)+,d1
	ext.w	d1
	add.w	d1,(a0)+
	subq.l	#1,d0
	bpl.w	.afull
.adone
	move.l	(sp)+,d2
	rts

*---------------------------------------------------------------------------
* void fg_k_mix_out(int8_t *out, const int16_t *acc, int n)
*   out[i] = clamp(acc[i], -128, 127)
*---------------------------------------------------------------------------
	xdef	_fg_k_mix_out
_fg_k_mix_out:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d0
	subq.l	#1,d0
	bmi.w	.odone
.oloop
	move.w	(a1)+,d1
	cmp.w	#127,d1
	ble.w	.olow
	moveq	#127,d1
	bra.w	.ostore
.olow
	cmp.w	#-128,d1
	bge.w	.ostore
	moveq	#-128,d1
.ostore
	move.b	d1,(a0)+
	subq.l	#1,d0
	bpl.w	.oloop
.odone
	rts

	end
