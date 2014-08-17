
/*******************************************************************************
#             uvccapture: USB UVC Video Class Snapshot Software                #
#This package work with the Logitech UVC based webcams with the mjpeg feature  #
#All the decoding is in user space with the embedded jpeg decoder              #
#.                                                                             #
# 	Orginally Copyright (C) 2005 2006 Laurent Pinchart &&  Michel Xhaard   #
#       Modifications Copyright (C) 2006  Gabriel A. Devenyi                   #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "utils.h"

#define ISHIFT 11

#define IFIX(a) ((int)((a) * (1 << ISHIFT) + .5))
#define IMULT(a, b) (((a) * (b)) >> ISHIFT)
#define ITOINT(a) ((a) >> ISHIFT)

#ifndef __P
# define __P(x) x
#endif

/* special markers */
#define M_BADHUFF	-1
#define M_EOF		0x80

struct jpeg_decdata {
  int dcts[6 * 64 + 16];
  int out[64 * 6];
  int dquant[3][64];
};

struct in {
  unsigned char *p;
  unsigned int bits;
  int left;
  int marker;

  int (*func) __P ((void *));
  void *data;
};

/*********************************/
struct dec_hufftbl;
struct enc_hufftbl;

union hufftblp {
  struct dec_hufftbl *dhuff;
  struct enc_hufftbl *ehuff;
};

struct scan {
  int dc;			/* old dc value */

  union hufftblp hudc;
  union hufftblp huac;
  int next;			/* when to switch to next scan */

  int cid;			/* component id */
  int hv;			/* horiz/vert, copied from comp */
  int tq;			/* quant tbl, copied from comp */
};

/*********************************/

#define DECBITS 10		/* seems to be the optimum */

struct dec_hufftbl {
  int maxcode[17];
  int valptr[16];
  unsigned char vals[256];
  unsigned int llvals[1 << DECBITS];
};

static void decode_mcus __P ((struct in *, int *, int, struct scan *, int *));
static int dec_readmarker __P ((struct in *));
static void dec_makehuff __P ((struct dec_hufftbl *, int *, unsigned char *));

static void setinput __P ((struct in *, unsigned char *));

/*********************************/

#undef PREC
#define PREC int

static void idctqtab __P ((unsigned char *, PREC *));
static void idct __P ((int *, int *, PREC *, PREC, int));
static void scaleidctqtab __P ((PREC *, PREC));

/*********************************/

static void initcol __P ((PREC[][64]));

static void col221111 __P ((int *, unsigned char *, int));
static void yuyvtopict __P ((int *, unsigned char *, int));

/*********************************/

#define M_SOI	0xd8
#define M_APP0	0xe0
#define M_DQT	0xdb
#define M_SOF0	0xc0
#define M_DHT   0xc4
#define M_DRI	0xdd
#define M_SOS	0xda
#define M_RST0	0xd0
#define M_EOI	0xd9
#define M_COM	0xfe

static unsigned char *datap;

static int
getbyte (void)
{
  return *datap++;
}

static int
getword (void)
{
  int c1, c2;

  c1 = *datap++;
  c2 = *datap++;
  return c1 << 8 | c2;
}

struct comp {
  int cid;
  int hv;
  int tq;
};

#define MAXCOMP 4
struct jpginfo {
  int nc;			/* number of components */
  int ns;			/* number of scans */
  int dri;			/* restart interval */
  int nm;			/* mcus til next marker */
  int rm;			/* next restart marker */
};

static struct jpginfo info;
static struct comp comps[MAXCOMP];

static struct scan dscans[MAXCOMP];

static unsigned char quant[4][64];

static struct dec_hufftbl dhuff[4];

#define dec_huffdc (dhuff + 0)
#define dec_huffac (dhuff + 2)

static struct in in;

static int
readtables (int till)
{
  int m, l, i, j, lq, pq, tq;
  int tc, th, tt;

  for (;;) {
    if (getbyte () != 0xff)
      return -1;
    if ((m = getbyte ()) == till)
      break;

    switch (m) {
    case 0xc2:
      return 0;

    case M_DQT:
      lq = getword ();
      while (lq > 2) {
	pq = getbyte ();
	tq = pq & 15;
	if (tq > 3)
	  return -1;
	pq >>= 4;
	if (pq != 0)
	  return -1;
	for (i = 0; i < 64; i++)
	  quant[tq][i] = getbyte ();
	lq -= 64 + 1;
      }
      break;

    case M_DHT:
      l = getword ();
      while (l > 2) {
	int hufflen[16], k;
	unsigned char huffvals[256];

	tc = getbyte ();
	th = tc & 15;
	tc >>= 4;
	tt = tc * 2 + th;
	if (tc > 1 || th > 1)
	  return -1;
	for (i = 0; i < 16; i++)
	  hufflen[i] = getbyte ();
	l -= 1 + 16;
	k = 0;
	for (i = 0; i < 16; i++) {
	  for (j = 0; j < hufflen[i]; j++)
	    huffvals[k++] = getbyte ();
	  l -= hufflen[i];
	}
	dec_makehuff (dhuff + tt, hufflen, huffvals);
      }
      break;

    case M_DRI:
      l = getword ();
      info.dri = getword ();
      break;

    default:
      l = getword ();
      while (l-- > 2)
	getbyte ();
      break;
    }
  }
  return 0;
}

static void
dec_initscans (void)
{
  int i;

  info.nm = info.dri + 1;
  info.rm = M_RST0;
  for (i = 0; i < info.ns; i++)
    dscans[i].dc = 0;
}

static int
dec_checkmarker (void)
{
  int i;

  if (dec_readmarker (&in) != info.rm)
    return -1;
  info.nm = info.dri;
  info.rm = (info.rm + 1) & ~0x08;
  for (i = 0; i < info.ns; i++)
    dscans[i].dc = 0;
  return 0;
}

int
jpeg_decode (unsigned char **pic, unsigned char *buf, int *width, int *height)
{
  struct jpeg_decdata *decdata;
  int i, j, m, tac, tdc;
  int intwidth, intheight;
  int mcusx, mcusy, mx, my;
  int max[6];
  int err = 0;
  int enc411 = 1;
  decdata = (struct jpeg_decdata *) malloc (sizeof (struct jpeg_decdata));
  if (!decdata) {
    err = -1;
    goto error;
  }
  if (buf == NULL) {
    err = -1;
    goto error;
  }
  datap = buf;
  if (getbyte () != 0xff) {
    err = ERR_NO_SOI;
    goto error;
  }
  if (getbyte () != M_SOI) {
    err = ERR_NO_SOI;
    goto error;
  }
  if (readtables (M_SOF0)) {
    err = ERR_BAD_TABLES;
    goto error;
  }
  getword ();
  i = getbyte ();
  if (i != 8) {
    err = ERR_NOT_8BIT;
    goto error;
  }
  intheight = getword ();
  intwidth = getword ();

  //if ((intheight & 15) || (intwidth & 15)){
  if ((intheight & 7) || (intwidth & 15)) {
    err = ERR_BAD_WIDTH_OR_HEIGHT;
    goto error;
  }
  info.nc = getbyte ();
  if (info.nc > MAXCOMP) {
    err = ERR_TOO_MANY_COMPPS;
    goto error;
  }
  for (i = 0; i < info.nc; i++) {
    int h, v;

    comps[i].cid = getbyte ();
    comps[i].hv = getbyte ();
    v = comps[i].hv & 15;
    h = comps[i].hv >> 4;
    comps[i].tq = getbyte ();
    if (h > 3 || v > 3) {
      err = ERR_ILLEGAL_HV;
      goto error;
    }
    if (comps[i].tq > 3) {
      err = ERR_QUANT_TABLE_SELECTOR;
      goto error;
    }
  }
  if (readtables (M_SOS)) {
    err = ERR_BAD_TABLES;
    goto error;
  }
  getword ();
  info.ns = getbyte ();
  if (info.ns != 3) {
    err = ERR_NOT_YCBCR_221111;
    goto error;
  }
  for (i = 0; i < 3; i++) {
    dscans[i].cid = getbyte ();
    tdc = getbyte ();
    tac = tdc & 15;
    tdc >>= 4;
    if (tdc > 1 || tac > 1) {
      err = ERR_QUANT_TABLE_SELECTOR;
      goto error;
    }
    for (j = 0; j < info.nc; j++)
      if (comps[j].cid == dscans[i].cid)
	break;
    if (j == info.nc) {
      err = ERR_UNKNOWN_CID_IN_SCAN;
      goto error;
    }
    dscans[i].hv = comps[j].hv;
    dscans[i].tq = comps[j].tq;
    dscans[i].hudc.dhuff = dec_huffdc + tdc;
    dscans[i].huac.dhuff = dec_huffac + tac;
  }

  i = getbyte ();
  j = getbyte ();
  m = getbyte ();

  if (i != 0 || j != 63 || m != 0) {
    err = ERR_NOT_SEQUENTIAL_DCT;
    goto error;
  }

  if (dscans[0].cid != 1 || dscans[1].cid != 2 || dscans[2].cid != 3) {
    err = ERR_NOT_YCBCR_221111;
    goto error;
  }

  if (dscans[1].hv != 0x11 || dscans[2].hv != 0x11) {
    err = ERR_NOT_YCBCR_221111;
    goto error;
  }
  /* if internal width and external are not the same or heigth too
     and pic not allocated realloc the good size and mark the change
     need 1 macroblock line more ?? */
  if (intwidth != *width || intheight != *height || *pic == NULL) {
    *width = intwidth;
    *height = intheight;
    // BytesperPixel 2 yuyv , 3 rgb24
    *pic =
      (unsigned char *) realloc ((unsigned char *) *pic,
				 (size_t) intwidth * (intheight + 8) * 2);
  }

  switch (dscans[0].hv) {
  case 0x22:
    mcusx = *width >> 4;
    mcusy = *height >> 4;
    enc411 = 1;
    break;
  case 0x21:
    mcusx = *width >> 4;
    mcusy = *height >> 3;
    enc411 = 0;
    break;
  default:
    err = ERR_NOT_YCBCR_221111;
    goto error;
    break;
  }

  idctqtab (quant[dscans[0].tq], decdata->dquant[0]);
  idctqtab (quant[dscans[1].tq], decdata->dquant[1]);
  idctqtab (quant[dscans[2].tq], decdata->dquant[2]);
  //initcol(decdata->dquant);
  setinput (&in, datap);

  dec_initscans ();

  dscans[0].next = 6 - 4;
  dscans[1].next = 6 - 4 - 1;
  dscans[2].next = 6 - 4 - 1 - 1;	/* 411 encoding */
  for (my = 0; my < mcusy; my++) {
    for (mx = 0; mx < mcusx; mx++) {
      if (info.dri && !--info.nm)
	if (dec_checkmarker ()) {
	  err = ERR_WRONG_MARKER;
	  goto error;
	}
      if (enc411) {
	decode_mcus (&in, decdata->dcts, 6, dscans, max);
	idct (decdata->dcts, decdata->out, decdata->dquant[0],
	      IFIX (128.5), max[0]);
	idct (decdata->dcts + 64, decdata->out + 64,
	      decdata->dquant[0], IFIX (128.5), max[1]);
	idct (decdata->dcts + 128, decdata->out + 128,
	      decdata->dquant[0], IFIX (128.5), max[2]);
	idct (decdata->dcts + 192, decdata->out + 192,
	      decdata->dquant[0], IFIX (128.5), max[3]);
	idct (decdata->dcts + 256, decdata->out + 256,
	      decdata->dquant[1], IFIX (0.5), max[4]);
	idct (decdata->dcts + 320, decdata->out + 320,
	      decdata->dquant[2], IFIX (0.5), max[5]);

	//col221111(decdata->out, *pic + (my * 16 * mcusx + mx) * 16 * 3, mcusx * 16 * 3);
	yuyvtopict (decdata->out,
		    *pic + (my * 16 * mcusx + mx) * 16 * 2, mcusx * 16 * 2);
      } else {
	decode_mcus (&in, decdata->dcts, 4, dscans, max);
	idct (decdata->dcts, decdata->out, decdata->dquant[0],
	      IFIX (128.5), max[0]);
	idct (decdata->dcts + 64, decdata->out + 64,
	      decdata->dquant[0], IFIX (128.5), max[1]);
	idct (decdata->dcts + 128, decdata->out + 256,
	      decdata->dquant[1], IFIX (0.5), max[4]);
	idct (decdata->dcts + 192, decdata->out + 320,
	      decdata->dquant[2], IFIX (0.5), max[5]);

	//col221111(decdata->out, *pic + (my * 8 * mcusx + mx) * 16 * 3, mcusx * 16 * 3);
	yuyvtopict (decdata->out,
		    *pic + (my * 8 * mcusx + mx) * 16 * 2, mcusx * 16 * 2);
      }

    }
  }

  m = dec_readmarker (&in);
  if (m != M_EOI) {
    err = ERR_NO_EOI;
    goto error;
  }
  if (decdata)
    free (decdata);
  return 0;
error:
  if (decdata)
    free (decdata);
  return err;
}

/****************************************************************/

/**************       huffman decoder             ***************/

/****************************************************************/

static int fillbits __P ((struct in *, int, unsigned int));
static int dec_rec2
__P ((struct in *, struct dec_hufftbl *, int *, int, int));

static void
setinput (in, p)
     struct in *in;
     unsigned char *p;
{
  in->p = p;
  in->left = 0;
  in->bits = 0;
  in->marker = 0;
}

static int
fillbits (in, le, bi)
     struct in *in;
     int le;
     unsigned int bi;
{
  int b, m;

  if (in->marker) {
    if (le <= 16)
      in->bits = bi << 16, le += 16;
    return le;
  }
  while (le <= 24) {
    b = *in->p++;
    if (b == 0xff && (m = *in->p++) != 0) {
      if (m == M_EOF) {
	if (in->func && (m = in->func (in->data)) == 0)
	  continue;
      }
      in->marker = m;
      if (le <= 16)
	bi = bi << 16, le += 16;
      break;
    }
    bi = bi << 8 | b;
    le += 8;
  }
  in->bits = bi;		/* tmp... 2 return values needed */
  return le;
}

static int
dec_readmarker (in)
     struct in *in;
{
  int m;

  in->left = fillbits (in, in->left, in->bits);
  if ((m = in->marker) == 0)
    return 0;
  in->left = 0;
  in->marker = 0;
  return m;
}

#define LEBI_DCL	int le, bi
#define LEBI_GET(in)	(le = in->left, bi = in->bits)
#define LEBI_PUT(in)	(in->left = le, in->bits = bi)

#define GETBITS(in, n) (					\
  (le < (n) ? le = fillbits(in, le, bi), bi = in->bits : 0),	\
  (le -= (n)),							\
  bi >> le & ((1 << (n)) - 1)					\
)

#define UNGETBITS(in, n) (	\
  le += (n)			\
)

static int
dec_rec2 (in, hu, runp, c, i)
     struct in *in;
     struct dec_hufftbl *hu;
     int *runp;
     int c, i;
{
  LEBI_DCL;

  LEBI_GET (in);
  if (i) {
    UNGETBITS (in, i & 127);
    *runp = i >> 8 & 15;
    i >>= 16;
  } else {
    for (i = DECBITS;
	 (c = ((c << 1) | GETBITS (in, 1))) >= (hu->maxcode[i]); i++);
    if (i >= 16) {
      in->marker = M_BADHUFF;
      return 0;
    }
    i = hu->vals[hu->valptr[i] + c - hu->maxcode[i - 1] * 2];
    *runp = i >> 4;
    i &= 15;
  }
  if (i == 0) {			/* sigh, 0xf0 is 11 bit */
    LEBI_PUT (in);
    return 0;
  }
  /* receive part */
  c = GETBITS (in, i);
  if (c < (1 << (i - 1)))
    c += (-1 << i) + 1;
  LEBI_PUT (in);
  return c;
}

#define DEC_REC(in, hu, r, i)	 (	\
  r = GETBITS(in, DECBITS),		\
  i = hu->llvals[r],			\
  i & 128 ?				\
    (					\
      UNGETBITS(in, i & 127),		\
      r = i >> 8 & 15,			\
      i >> 16				\
    )					\
  :					\
    (					\
      LEBI_PUT(in),			\
      i = dec_rec2(in, hu, &r, r, i),	\
      LEBI_GET(in),			\
      i					\
    )					\
)

static void
decode_mcus (in, dct, n, sc, maxp)
     struct in *in;
     int *dct;
     int n;
     struct scan *sc;
     int *maxp;
{
  struct dec_hufftbl *hu;
  int i, r, t;

  LEBI_DCL;

  memset (dct, 0, n * 64 * sizeof (*dct));
  LEBI_GET (in);
  while (n-- > 0) {
    hu = sc->hudc.dhuff;
    *dct++ = (sc->dc += DEC_REC (in, hu, r, t));

    hu = sc->huac.dhuff;
    i = 63;
    while (i > 0) {
      t = DEC_REC (in, hu, r, t);
      if (t == 0 && r == 0) {
	dct += i;
	break;
      }
      dct += r;
      *dct++ = t;
      i -= r + 1;
    }
    *maxp++ = 64 - i;
    if (n == sc->next)
      sc++;
  }
  LEBI_PUT (in);
}

static void
dec_makehuff (hu, hufflen, huffvals)
     struct dec_hufftbl *hu;
     int *hufflen;
     unsigned char *huffvals;
{
  int code, k, i, j, d, x, c, v;

  for (i = 0; i < (1 << DECBITS); i++)
    hu->llvals[i] = 0;

/*
 * llvals layout:
 *
 * value v already known, run r, backup u bits:
 *  vvvvvvvvvvvvvvvv 0000 rrrr 1 uuuuuuu
 * value unknown, size b bits, run r, backup u bits:
 *  000000000000bbbb 0000 rrrr 0 uuuuuuu
 * value and size unknown:
 *  0000000000000000 0000 0000 0 0000000
 */
  code = 0;
  k = 0;
  for (i = 0; i < 16; i++, code <<= 1) {	/* sizes */
    hu->valptr[i] = k;
    for (j = 0; j < hufflen[i]; j++) {
      hu->vals[k] = *huffvals++;
      if (i < DECBITS) {
	c = code << (DECBITS - 1 - i);
	v = hu->vals[k] & 0x0f;	/* size */
	for (d = 1 << (DECBITS - 1 - i); --d >= 0;) {
	  if (v + i < DECBITS) {	/* both fit in table */
	    x = d >> (DECBITS - 1 - v - i);
	    if (v && x < (1 << (v - 1)))
	      x += (-1 << v) + 1;
	    x = x << 16 | (hu->vals[k] & 0xf0) << 4 |
	      (DECBITS - (i + 1 + v)) | 128;
	  } else
	    x = v << 16 | (hu->vals[k] & 0xf0) << 4 | (DECBITS - (i + 1));
	  hu->llvals[c | d] = x;
	}
      }
      code++;
      k++;
    }
    hu->maxcode[i] = code;
  }
  hu->maxcode[16] = 0x20000;	/* always terminate decode */
}

/****************************************************************/

/**************             idct                  ***************/

/****************************************************************/

#define ONE ((PREC)IFIX(1.))
#define S2  ((PREC)IFIX(0.382683432))
#define C2  ((PREC)IFIX(0.923879532))
#define C4  ((PREC)IFIX(0.707106781))

#define S22 ((PREC)IFIX(2 * 0.382683432))
#define C22 ((PREC)IFIX(2 * 0.923879532))
#define IC4 ((PREC)IFIX(1 / 0.707106781))

#define C3IC1 ((PREC)IFIX(0.847759065))	/* c3/c1 */
#define C5IC1 ((PREC)IFIX(0.566454497))	/* c5/c1 */
#define C7IC1 ((PREC)IFIX(0.198912367))	/* c7/c1 */

#define XPP(a,b) (t = a + b, b = a - b, a = t)
#define XMP(a,b) (t = a - b, b = a + b, a = t)
#define XPM(a,b) (t = a + b, b = b - a, a = t)

#define ROT(a,b,s,c) (	t = IMULT(a + b, s),	\
			a = IMULT(a, c - s) + t,	\
			b = IMULT(b, c + s) - t)

#define IDCT		\
(			\
  XPP(t0, t1),		\
  XMP(t2, t3),		\
  t2 = IMULT(t2, IC4) - t3,	\
  XPP(t0, t3),		\
  XPP(t1, t2),		\
  XMP(t4, t7),		\
  XPP(t5, t6),		\
  XMP(t5, t7),		\
  t5 = IMULT(t5, IC4),	\
  ROT(t4, t6, S22, C22),\
  t6 -= t7,		\
  t5 -= t6,		\
  t4 -= t5,		\
  XPP(t0, t7),		\
  XPP(t1, t6),		\
  XPP(t2, t5),		\
  XPP(t3, t4)		\
)

static unsigned char zig2[64] = {
  0, 2, 3, 9, 10, 20, 21, 35,
  14, 16, 25, 31, 39, 46, 50, 57,
  5, 7, 12, 18, 23, 33, 37, 48,
  27, 29, 41, 44, 52, 55, 59, 62,
  15, 26, 30, 40, 45, 51, 56, 58,
  1, 4, 8, 11, 19, 22, 34, 36,
  28, 42, 43, 53, 54, 60, 61, 63,
  6, 13, 17, 24, 32, 38, 47, 49
};

void
idct (in, out, quant, off, max)
     int *in;
     int *out;
     PREC *quant;
     PREC off;
     int max;
{
  PREC t0, t1, t2, t3, t4, t5, t6, t7, t;
  PREC tmp[64], *tmpp;
  int i, j;
  unsigned char *zig2p;

  t0 = off;
  if (max == 1) {
    t0 += in[0] * quant[0];
    for (i = 0; i < 64; i++)
      out[i] = ITOINT (t0);
    return;
  }
  zig2p = zig2;
  tmpp = tmp;
  for (i = 0; i < 8; i++) {
    j = *zig2p++;
    t0 += in[j] * quant[j];
    j = *zig2p++;
    t5 = in[j] * quant[j];
    j = *zig2p++;
    t2 = in[j] * quant[j];
    j = *zig2p++;
    t7 = in[j] * quant[j];
    j = *zig2p++;
    t1 = in[j] * quant[j];
    j = *zig2p++;
    t4 = in[j] * quant[j];
    j = *zig2p++;
    t3 = in[j] * quant[j];
    j = *zig2p++;
    t6 = in[j] * quant[j];
    IDCT;
    tmpp[0 * 8] = t0;
    tmpp[1 * 8] = t1;
    tmpp[2 * 8] = t2;
    tmpp[3 * 8] = t3;
    tmpp[4 * 8] = t4;
    tmpp[5 * 8] = t5;
    tmpp[6 * 8] = t6;
    tmpp[7 * 8] = t7;
    tmpp++;
    t0 = 0;
  }
  for (i = 0; i < 8; i++) {
    t0 = tmp[8 * i + 0];
    t1 = tmp[8 * i + 1];
    t2 = tmp[8 * i + 2];
    t3 = tmp[8 * i + 3];
    t4 = tmp[8 * i + 4];
    t5 = tmp[8 * i + 5];
    t6 = tmp[8 * i + 6];
    t7 = tmp[8 * i + 7];
    IDCT;
    out[8 * i + 0] = ITOINT (t0);
    out[8 * i + 1] = ITOINT (t1);
    out[8 * i + 2] = ITOINT (t2);
    out[8 * i + 3] = ITOINT (t3);
    out[8 * i + 4] = ITOINT (t4);
    out[8 * i + 5] = ITOINT (t5);
    out[8 * i + 6] = ITOINT (t6);
    out[8 * i + 7] = ITOINT (t7);
  }
}

static unsigned char zig[64] = {
  0, 1, 5, 6, 14, 15, 27, 28,
  2, 4, 7, 13, 16, 26, 29, 42,
  3, 8, 12, 17, 25, 30, 41, 43,
  9, 11, 18, 24, 31, 40, 44, 53,
  10, 19, 23, 32, 39, 45, 52, 54,
  20, 22, 33, 38, 46, 51, 55, 60,
  21, 34, 37, 47, 50, 56, 59, 61,
  35, 36, 48, 49, 57, 58, 62, 63
};

static PREC aaidct[8] = {
  IFIX (0.3535533906), IFIX (0.4903926402),
  IFIX (0.4619397663), IFIX (0.4157348062),
  IFIX (0.3535533906), IFIX (0.2777851165),
  IFIX (0.1913417162), IFIX (0.0975451610)
};

static void
idctqtab (qin, qout)
     unsigned char *qin;
     PREC *qout;
{
  int i, j;

  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++)
      qout[zig[i * 8 + j]] = qin[zig[i * 8 + j]] *
	IMULT (aaidct[i], aaidct[j]);
}

static void
scaleidctqtab (q, sc)
     PREC *q;
     PREC sc;
{
  int i;

  for (i = 0; i < 64; i++)
    q[i] = IMULT (q[i], sc);
}

/****************************************************************/

/**************          color decoder            ***************/

/****************************************************************/

#define ROUND

/*
 * YCbCr Color transformation:
 *
 * y:0..255   Cb:-128..127   Cr:-128..127
 *
 *      R = Y                + 1.40200 * Cr
 *      G = Y - 0.34414 * Cb - 0.71414 * Cr
 *      B = Y + 1.77200 * Cb
 *
 * =>
 *      Cr *= 1.40200;
 *      Cb *= 1.77200;
 *      Cg = 0.19421 * Cb + .50937 * Cr;
 *      R = Y + Cr;
 *      G = Y - Cg;
 *      B = Y + Cb;
 *
 * =>
 *      Cg = (50 * Cb + 130 * Cr + 128) >> 8;
 */

static void
initcol (q)
     PREC q[][64];
{
  scaleidctqtab (q[1], IFIX (1.77200));
  scaleidctqtab (q[2], IFIX (1.40200));
}

/* This is optimized for the stupid sun SUNWspro compiler. */
#define STORECLAMP(a,x)				\
(						\
  (a) = (x),					\
  (unsigned int)(x) >= 256 ? 			\
    ((a) = (x) < 0 ? 0 : 255)			\
  :						\
    0						\
)

#define CLAMP(x) ((unsigned int)(x) >= 256 ? ((x) < 0 ? 0 : 255) : (x))

#ifdef ROUND

#define CBCRCG(yin, xin)			\
(						\
  cb = outc[0 +yin*8+xin],			\
  cr = outc[64+yin*8+xin],			\
  cg = (50 * cb + 130 * cr + 128) >> 8		\
)

#else

#define CBCRCG(yin, xin)			\
(						\
  cb = outc[0 +yin*8+xin],			\
  cr = outc[64+yin*8+xin],			\
  cg = (3 * cb + 8 * cr) >> 4			\
)

#endif
// BGR instead RGB
#define PIC(yin, xin, p, xout)			\
(						\
  y = outy[(yin) * 8 + xin],			\
  STORECLAMP(p[(xout) * 3 + 2], y + cr),	\
  STORECLAMP(p[(xout) * 3 + 1], y - cg),	\
  STORECLAMP(p[(xout) * 3 + 0], y + cb)		\
)

#define PIC221111(xin)						\
(								\
  CBCRCG(0, xin),						\
  PIC(xin / 4 * 8 + 0, (xin & 3) * 2 + 0, pic0, xin * 2 + 0),	\
  PIC(xin / 4 * 8 + 0, (xin & 3) * 2 + 1, pic0, xin * 2 + 1),	\
  PIC(xin / 4 * 8 + 1, (xin & 3) * 2 + 0, pic1, xin * 2 + 0),	\
  PIC(xin / 4 * 8 + 1, (xin & 3) * 2 + 1, pic1, xin * 2 + 1)	\
)

#define UV(yin, xin)			\
(						\
  u = outc[0 +yin*8+xin],			\
  v = outc[64+yin*8+xin]			\
)

#define PIC1(yin, xin, p, xout)			\
(						\
  y = outy[(yin) * 8 + xin],			\
  STORECLAMP(p[(xout) * 2 + 0], y),	\
  STORECLAMP(p[(xout) * 2 + 1], u + 128)	\
)

#define PIC2(yin, xin, p, xout)			\
(						\
  y = outy[(yin) * 8 + xin],			\
  STORECLAMP(p[(xout) * 2 + 0], y),	\
  STORECLAMP(p[(xout) * 2 + 1], v + 128)	\
)

#define PICYUYV(xin)						\
(								\
  UV(0, xin),						\
  PIC1(xin / 4 * 8 + 0, (xin & 3) * 2 + 0, pic0, xin * 2 + 0),	\
  PIC2(xin / 4 * 8 + 0, (xin & 3) * 2 + 1, pic0, xin * 2 + 1),	\
  UV(1, xin),						\
  PIC1(xin / 4 * 8 + 1, (xin & 3) * 2 + 0, pic1, xin * 2 + 0),	\
  PIC2(xin / 4 * 8 + 1, (xin & 3) * 2 + 1, pic1, xin * 2 + 1)	\
)

static void
yuyvtopict (out, pic, width)
     int *out;
     unsigned char *pic;
     int width;
{
  int i, j, k;
  unsigned char *pic0, *pic1;
  int *outy, *outc;
  int u, v, y;

  pic0 = pic;
  pic1 = pic + width;
  outy = out;
  outc = out + 64 * 4;
  for (i = 2; i > 0; i--) {
    for (j = 4; j > 0; j--) {
      for (k = 0; k < 8; k++) {
	PICYUYV (k);
      }
      outc += 8;
      outy += 16;
      pic0 += 2 * width;
      pic1 += 2 * width;
    }
    outy += 64 * 2 - 16 * 4;
  }
}

static void
col221111 (out, pic, width)
     int *out;
     unsigned char *pic;
     int width;
{
  int i, j, k;
  unsigned char *pic0, *pic1;
  int *outy, *outc;
  int cr, cg, cb, y;

  pic0 = pic;
  pic1 = pic + width;
  outy = out;
  outc = out + 64 * 4;
  for (i = 2; i > 0; i--) {
    for (j = 4; j > 0; j--) {
      for (k = 0; k < 8; k++) {
	PIC221111 (k);
      }
      outc += 8;
      outy += 16;
      pic0 += 2 * width;
      pic1 += 2 * width;
    }
    outy += 64 * 2 - 16 * 4;
  }
}
