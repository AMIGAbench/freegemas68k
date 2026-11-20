* ---------------------------------------------------------------------------
* AMMX kernels (Apollo 68080) for the freegemas compositor, version 3.
*
* Blending as the C path in render.cpp, bit for bit (self-test at start):
*     a5  = (a + 4) >> 3                       0..32  (a: 0..255)
*     out = d + ((s - d) * a5) >> 5            per channel, floored
*
* Channels without packed shifts:
*     R: x >> 11 = PMULH x,#32 & $1F ... << 11 = PMULL #2048
*     G: in place: (s & $07E0) - (d & $07E0) = dG << 5; PMUL88 with a8 = a5*8
*        gives exactly dG * a5; + (d & $07E0), & $07E0 floors to the field
*     B: s & $1F, d & $1F
* Eight pixels per pass in two interleaved chains (A, B); 4-pixel groups and
* a 1..3 pixel remainder in the same routine: the remainder lanes get
* alpha 0 (a lane mask), i.e. they store back the destination they read.
* So loads and stores may touch up to 6 bytes past a row: the buffers have
* 16 bytes of slack (render.cpp).
*
* The constants live in E registers, loaded once per frame (fg_ammx_setup);
* the task is registered for AMMX, so task switches save them, and gcc
* never uses E registers:
*     e7  0          e10 4 (x4)      e11 $FFF8      e12 32
*     e13 $001F      e14 2048        e15 $07E0      e16 mod / a8 (per call)
*     e17 255        e18 256         e19 $00FF
* Work: chain A e0 e1 e2 e3 e4 e5, chain B e6 e8 e9 e20 e21 e22, e23.
*
* Only LOAD, STORE, VPERM, PAND, POR, PADDW, PSUBW, PMULH, PMULL, PMUL88
* (marked for every 68080, also V2, in AC68080PRM's table). No STOREC, no
* B registers (those hung the V1200). VPERM from a data register (bytes 4..7
* = its long) is a form proven on a V1200.
*
* Calling: Bebbo gcc, arguments on the stack. vasm -m68080 -no-opt.
* ---------------------------------------------------------------------------

	machine	68080
	section	.text,code

*---------------------------------------------------------------------------
* the per-channel chains
*---------------------------------------------------------------------------
* one 4-pixel chain: \1 = s, \2 = d, \3 = a8, \4 \5 = temps, \6 = result
CHAIN	macro
	pmulh	e12,\1,\4		; s >> 11
	pand	e13,\4,\4		;   R(s)
	pmulh	e12,\2,\5		; d >> 11
	pand	e13,\5,\5		;   R(d)
	psubw	\5,\4,\4
	pmul88	\3,\4,\4
	paddw	\5,\4,\4
	pmull	e14,\4,\6		; R << 11
	pand	e15,\1,\4		; G(s) << 5
	pand	e15,\2,\5		; G(d) << 5
	psubw	\5,\4,\4		; dG << 5
	pmul88	\3,\4,\4		; dG * a5
	paddw	\5,\4,\4
	pand	e15,\4,\4		; floored into the field
	por	\4,\6,\6
	pand	e13,\1,\4		; B(s)
	pand	e13,\2,\5		; B(d)
	psubw	\5,\4,\4
	pmul88	\3,\4,\4
	paddw	\5,\4,\4
	por	\4,\6,\6
	endm

* two chains interleaved: A (e0 s, e1 d, e2 a8, e3 e4, e5 out),
*                         B (e6 s, e8 d, e9 a8, e20 e21, e22 out)
CHAIN2	macro
	pmulh	e12,e0,e3
	pmulh	e12,e6,e20
	pand	e13,e3,e3
	pand	e13,e20,e20
	pmulh	e12,e1,e4
	pmulh	e12,e8,e21
	pand	e13,e4,e4
	pand	e13,e21,e21
	psubw	e4,e3,e3
	psubw	e21,e20,e20
	pmul88	e2,e3,e3
	pmul88	e9,e20,e20
	paddw	e4,e3,e3
	paddw	e21,e20,e20
	pmull	e14,e3,e5
	pmull	e14,e20,e22

	pand	e15,e0,e3
	pand	e15,e6,e20
	pand	e15,e1,e4
	pand	e15,e8,e21
	psubw	e4,e3,e3
	psubw	e21,e20,e20
	pmul88	e2,e3,e3
	pmul88	e9,e20,e20
	paddw	e4,e3,e3
	paddw	e21,e20,e20
	pand	e15,e3,e3
	pand	e15,e20,e20
	por	e3,e5,e5
	por	e20,e22,e22

	pand	e13,e0,e3
	pand	e13,e6,e20
	pand	e13,e1,e4
	pand	e13,e8,e21
	psubw	e4,e3,e3
	psubw	e21,e20,e20
	pmul88	e2,e3,e3
	pmul88	e9,e20,e20
	paddw	e4,e3,e3
	paddw	e21,e20,e20
	por	e3,e5,e5
	por	e20,e22,e22
	endm

* alpha words -> a8 = (a + 4) & ~7
A8	macro
	paddw	e10,\1,\1
	pand	e11,\1,\1
	endm

* two faded alpha vectors, interleaved: a = (a * mod + 255) >> 8 -> a8
A8MOD2	macro
	pmull	e16,\1,\1		; a * mod   (low 16 bits, <= 65025)
	pmull	e16,\2,\2
	paddw	e17,\1,\1		; + 255
	paddw	e17,\2,\2
	pmulh	e18,\1,\1		; >> 8 (arithmetic) ...
	pmulh	e18,\2,\2
	pand	e19,\1,\1		; ... & $FF: the unsigned result
	pand	e19,\2,\2
	paddw	e10,\1,\1
	paddw	e10,\2,\2
	pand	e11,\1,\1
	pand	e11,\2,\2
	endm

A8MOD	macro
	pmull	e16,\1,\1
	paddw	e17,\1,\1
	pmulh	e18,\1,\1
	pand	e19,\1,\1
	paddw	e10,\1,\1
	pand	e11,\1,\1
	endm

*---------------------------------------------------------------------------
* blendn: the core. a0 dst, a1 src, a2 alpha (mode 0/1), d0 pixels (> 0),
* d5 mode: 0 per-pixel alpha, 1 per-pixel alpha * mod (e16 = mod),
*          2 constant a8 (e16 = a8). Uses d1 d2, e0..e6 e8 e9 e20..e23.
* a0/a1/a2 are advanced past the pixels.
*---------------------------------------------------------------------------
blendn:
	move.l	d0,d2
	lsr.l	#3,d2			; pairs of 4-pixel groups
	beq.w	.quad
	tst.l	d5
	beq.w	.p0
	cmp.l	#1,d5
	beq.w	.p1
	por	e16,e16,e2		; (mode 2: the constant a8, both chains)
	por	e16,e16,e9
.p2
	load	(a1)+,e0
	load	(a1)+,e6
	load	(a0),e1
	load	8(a0),e8
	CHAIN2
	store	e5,(a0)+
	store	e22,(a0)+
	subq.l	#1,d2
	bne.w	.p2
	bra.w	.quad
.p0
	load	(a2)+,e23		; 8 alpha bytes
	load	(a1)+,e0
	load	(a1)+,e6
	vperm	#$80818283,e23,e7,e2	; 00 a0 .. 00 a3
	vperm	#$84858687,e23,e7,e9	; 00 a4 .. 00 a7
	load	(a0),e1
	load	8(a0),e8
	A8	e2
	A8	e9
	CHAIN2
	store	e5,(a0)+
	store	e22,(a0)+
	subq.l	#1,d2
	bne.w	.p0
	bra.w	.quad
.p1
	load	(a2)+,e23
	load	(a1)+,e0
	load	(a1)+,e6
	vperm	#$80818283,e23,e7,e2
	vperm	#$84858687,e23,e7,e9
	load	(a0),e1
	load	8(a0),e8
	A8MOD2	e2,e9
	CHAIN2
	store	e5,(a0)+
	store	e22,(a0)+
	subq.l	#1,d2
	bne.w	.p1
.quad
	btst	#2,d0			; one more group of 4?
	beq.w	.rest
	bsr.w	alpha4			; e2 = a8 of the next 4 pixels
	addq.l	#4,a2
	load	(a1)+,e0
	load	(a0),e1
	CHAIN	e0,e1,e2,e3,e4,e5
	store	e5,(a0)+
.rest
	move.l	d0,d2
	and.l	#3,d2
	beq.w	.done
	bsr.w	alpha4			; (reads 4 alpha bytes, uses d2 of them)
	lea	fgk_lanes(pc),a3
	load	-8(a3,d2.l*8),e3	; $FFFF in the lanes that are ours
	pand	e3,e2,e2		; the others: alpha 0 -> their d stored back
	load	(a1),e0
	load	(a0),e1
	CHAIN	e0,e1,e2,e3,e4,e5
	store	e5,(a0)
	add.l	d2,d2
	add.l	d2,a0			; (past the pixels)
	add.l	d2,a1
	lsr.l	#1,d2
	add.l	d2,a2
.done
	rts

* e2 = a8 of the 4 pixels at a2 for the mode in d5 (a2 not advanced)
alpha4:
	cmp.l	#2,d5
	beq.w	.a4c
	load	(a2),e23
	vperm	#$80818283,e23,e7,e2
	tst.l	d5
	bne.w	.a4m
	A8	e2
	rts
.a4m
	A8MOD	e2
	rts
.a4c
	por	e16,e16,e2
	rts

*---------------------------------------------------------------------------
* void fg_ammx_setup(void): the constants into e7, e10..e15, e17..e19.
* Once per frame (and whenever other code may have used E registers).
*---------------------------------------------------------------------------
	xdef	_fg_ammx_setup
_fg_ammx_setup:
	lea	fgk_const(pc),a0
	load	(a0),e10		; 4
	load	8(a0),e11		; $FFF8
	load	16(a0),e12		; 32
	load	24(a0),e13		; $001F
	load	32(a0),e14		; 2048
	load	40(a0),e15		; $07E0
	load	48(a0),e7		; 0
	load	56(a0),e17		; 255
	load	64(a0),e18		; 256
	load	72(a0),e19		; $00FF
	rts

*---------------------------------------------------------------------------
* void fg_ammx_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n)
* void fg_ammx_blend_row_mod(uint16_t *dst, const uint16_t *src, const uint8_t *alpha, int n, int mod)
* void fg_ammx_blend_const(uint16_t *dst, const uint16_t *src, int n, int a8)
*     n pixels (any n)
*---------------------------------------------------------------------------
	xdef	_fg_ammx_blend_row
_fg_ammx_blend_row:
	movem.l	d2/d5/a2-a3,-(sp)	; 16 bytes
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),a2
	move.l	32(sp),d0
	ble.w	.r0
	moveq	#0,d5
	bsr.w	blendn
.r0
	movem.l	(sp)+,d2/d5/a2-a3
	rts

	xdef	_fg_ammx_blend_row_mod
_fg_ammx_blend_row_mod:
	movem.l	d2/d5/a2-a3,-(sp)
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),a2
	move.l	32(sp),d0
	ble.w	.m0
	move.l	36(sp),d1
	vperm	#$67676767,d1,d1,e16	; 4 x mod
	moveq	#1,d5
	bsr.w	blendn
.m0
	movem.l	(sp)+,d2/d5/a2-a3
	rts

	xdef	_fg_ammx_blend_const
_fg_ammx_blend_const:
	movem.l	d2/d5/a2-a3,-(sp)
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),d0
	ble.w	.c0
	move.l	32(sp),d1
	vperm	#$67676767,d1,d1,e16	; 4 x a8
	moveq	#2,d5
	bsr.w	blendn
.c0
	movem.l	(sp)+,d2/d5/a2-a3
	rts

*---------------------------------------------------------------------------
* void fg_ammx_spans(uint16_t *drow, const uint16_t *gs, const uint8_t *ga,
*                    const int16_t *spans, const int16_t *spansEnd, int mod)
*   the gathered pixels of one scaled row (fg_k_scaled_spans): per span
*   (offset, kind, count) a copy (kind 2, mod 255) or a blend -- one call
*   per row instead of one per span
*---------------------------------------------------------------------------
	xdef	_fg_ammx_spans
_fg_ammx_spans:
	movem.l	d2-d7/a2-a6,-(sp)	; 44 bytes
	move.l	48(sp),a6		; drow
	move.l	52(sp),a1		; gs
	move.l	56(sp),a2		; ga
	move.l	60(sp),a4		; spans
	move.l	64(sp),a5		; spansEnd
	move.l	68(sp),d6		; mod
	moveq	#0,d5			; mode 0
	cmp.l	#255,d6
	beq.w	.sloop
	move.l	d6,d1
	vperm	#$67676767,d1,d1,e16
	moveq	#1,d5
.sloop
	cmpa.l	a5,a4
	bcc.w	.sdone
	move.w	(a4)+,d3		; offset
	ext.l	d3
	move.w	(a4)+,d4		; kind
	move.w	(a4)+,d0		; count
	ext.l	d0
	lea	(a6,d3.l*2),a0
	move.l	d0,d7
	cmp.w	#2,d4
	bne.w	.sblend
	tst.l	d5
	bne.w	.sblend
	subq.l	#1,d7			; opaque, no fade: a copy
.scopy
	move.w	(a1)+,(a0)+
	subq.l	#1,d7
	bpl.w	.scopy
	add.l	d0,a2
	bra.w	.sloop
.sblend
	bsr.w	blendn
	bra.w	.sloop
.sdone
	movem.l	(sp)+,d2-d7/a2-a6
	rts

*---------------------------------------------------------------------------
* void fg_ammx_runs(uint16_t *d, const uint16_t *s, const uint8_t *a,
*                   const uint16_t *runs, const uint16_t *runsEnd, int sx, int n, int mod)
*   one unscaled texture row: the pixels sx .. sx+n-1 of a row whose runs
*   are given; d/s/a point at pixel sx. Skip runs are jumped over, copy runs
*   copied (no fade), the rest blended. a NULL (hard mask): no blend runs;
*   with a fade they blend with the constant alpha of mod.
*---------------------------------------------------------------------------
	xdef	_fg_ammx_runs
_fg_ammx_runs:
	movem.l	d2-d7/a2-a6,-(sp)	; 44 bytes
	move.l	48(sp),a0		; d
	move.l	52(sp),a1		; s
	move.l	56(sp),a2		; a
	move.l	60(sp),a4		; runs
	move.l	64(sp),a5		; runsEnd
	move.l	68(sp),d3		; sx  (position of the walk: run start)
	move.l	72(sp),d4		; n
	move.l	76(sp),d6		; mod
	add.l	d3,d4			; d4 = end = sx + n, d3 = sx
	moveq	#0,d7			; d7 = pos: start of the next run in the row
	moveq	#0,d5			; mode 0
	cmp.l	#255,d6
	beq.w	.rinit
	tst.l	a2
	beq.w	.rconst
	move.l	d6,d1
	vperm	#$67676767,d1,d1,e16	; mod
	moveq	#1,d5
	bra.w	.rinit
.rconst
	mulu.w	#255,d6			; a = (255 * mod + 255) >> 8, a8 = (a + 4) & ~7
	add.l	#255,d6
	lsr.l	#8,d6
	addq.l	#4,d6
	and.l	#$FFF8,d6
	move.l	d6,d1
	vperm	#$67676767,d1,d1,e16
	moveq	#2,d5
.rinit
	move.l	a0,a6			; (a0 at sx: the pixel of pos = sx)
.rloop
	cmpa.l	a5,a4
	bcc.w	.rdone
	cmp.l	d4,d7
	bge.w	.rdone			; past the end
	moveq	#0,d1
	move.w	(a4)+,d1
	move.l	d1,d2
	and.l	#$3FFF,d2		; length
	moveq	#14,d0
	lsr.l	d0,d1			; kind
	move.l	d7,d0			; run [pos, pos+len) clipped to [sx, end)
	add.l	d2,d7
	cmp.l	d3,d0
	bge.w	.rs0
	move.l	d3,d0
.rs0
	move.l	d7,d2
	cmp.l	d4,d2
	ble.w	.rs1
	move.l	d4,d2
.rs1
	sub.l	d0,d2			; count
	ble.w	.rloop
	tst.l	d1
	beq.w	.rloop			; transparent
	sub.l	d3,d0			; offset from sx
	lea	(a6,d0.l*2),a0
	move.l	52(sp),a1
	lea	(a1,d0.l*2),a1
	cmp.l	#2,d1
	bne.w	.rblend
	tst.l	d5
	bne.w	.rblend
	subq.l	#1,d2			; opaque, no fade: a copy
.rcopy
	move.w	(a1)+,(a0)+
	subq.l	#1,d2
	bpl.w	.rcopy
	bra.w	.rloop
.rblend
	move.l	56(sp),a2
	tst.l	a2
	beq.w	.rnoa
	add.l	d0,a2
.rnoa
	move.l	d2,d0
	move.l	d3,-(sp)		; (blendn uses d2; d3/d4/d7 kept)
	bsr.w	blendn
	move.l	(sp)+,d3
	bra.w	.rloop
.rdone
	movem.l	(sp)+,d2-d7/a2-a6
	rts

*---------------------------------------------------------------------------
* void fg_ammx_copy_row(uint16_t *dst, const uint16_t *src, int n)   (n pixels)
*---------------------------------------------------------------------------
	xdef	_fg_ammx_copy_row
_fg_ammx_copy_row:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d0
	ble.w	.pdone
	move.l	d0,d1
	lsr.l	#4,d1
	beq.w	.prest
.pblock
	load	(a1)+,e0
	load	(a1)+,e1
	load	(a1)+,e2
	load	(a1)+,e3
	store	e0,(a0)+
	store	e1,(a0)+
	store	e2,(a0)+
	store	e3,(a0)+
	subq.l	#1,d1
	bne.w	.pblock
.prest
	and.l	#15,d0
	beq.w	.pdone
	subq.l	#1,d0
.pword
	move.w	(a1)+,(a0)+
	subq.l	#1,d0
	bpl.w	.pword
.pdone
	rts

*---------------------------------------------------------------------------
* uint32_t fg_ccc(void): the 68080 cycle counter (movec ccc,d0; readable in
* user mode on current 68080 cores). Measuring builds only.
*---------------------------------------------------------------------------
	xdef	_fg_ccc
_fg_ccc:
	dc.w	$4e7a,$0809		; movec ccc,d0
	rts

*---------------------------------------------------------------------------
* void fg_ammx_ctx_on(void) -- only via exec/Supervisor(): SR bit 11 in the
* stacked SR registers the task for AMMX. Plain 68000 code.
*---------------------------------------------------------------------------
	xdef	_fg_ammx_ctx_on
_fg_ammx_ctx_on:
	ori.w	#$0800,(sp)
	rte

	xdef	_fg_ammx_register
_fg_ammx_register:
	movem.l	a5-a6,-(sp)
	move.l	4.w,a6
	lea	_fg_ammx_ctx_on(pc),a5
	jsr	-30(a6)			; Supervisor
	movem.l	(sp)+,a5-a6
	rts

	cnop	0,8
fgk_const:
	dc.w	4,4,4,4
	dc.w	$FFF8,$FFF8,$FFF8,$FFF8
	dc.w	32,32,32,32
	dc.w	$001F,$001F,$001F,$001F
	dc.w	2048,2048,2048,2048
	dc.w	$07E0,$07E0,$07E0,$07E0
	dc.w	0,0,0,0
	dc.w	255,255,255,255
	dc.w	256,256,256,256
	dc.w	$00FF,$00FF,$00FF,$00FF
* lane masks for 1, 2, 3 remaining pixels
fgk_lanes:
	dc.w	$FFFF,0,0,0
	dc.w	$FFFF,$FFFF,0,0
	dc.w	$FFFF,$FFFF,$FFFF,0

	end
