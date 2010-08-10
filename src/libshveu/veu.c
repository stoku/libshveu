/*
 * libshveu: A library for controlling SH-Mobile VEU
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2010 Renesas Electronics Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA  02110-1301 USA
 */

/*
 * SuperH VEU color space conversion and stretching
 * Based on MPlayer Vidix driver by Magnus Damm
 * Modified by Takanari Hayama to support NV12->RGB565 conversion
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>

#include <uiomux/uiomux.h>
#include "shveu/shveu.h"
#include "shveu_regs.h"

struct uio_map {
	unsigned long address;
	unsigned long size;
	void *iomem;
};

struct point {
	int x;
	int y;
};

struct rect {
	struct point tl;	/* Top left */
	struct point br;	/* Bottom right */
};

struct SHVEU {
	UIOMux *uiomux;
	struct uio_map uio_mmio;

	int crop_src;
	struct rect scrop;
	int crop_dst;
	struct rect dcrop;
};


/* Helper functions for reading registers. */

static unsigned long read_reg(struct uio_map *ump, int reg_nr)
{
	volatile unsigned long *reg = ump->iomem + reg_nr;

	return *reg;
}

static void write_reg(struct uio_map *ump, unsigned long value, int reg_nr)
{
	volatile unsigned long *reg = ump->iomem + reg_nr;

	*reg = value;
}

static int sh_veu_is_veu2h(struct uio_map *ump)
{
	return ump->size == 0x27c;
}

static int sh_veu_is_veu3f(struct uio_map *ump)
{
	return ump->size == 0xcc;
}

static void set_scale(struct uio_map *ump, int vertical,
		      int size_in, int size_out, int zoom, int clip_out)
{
	unsigned long fixpoint, mant, frac, value, vb;

	/* calculate FRAC and MANT */

	fixpoint = (4096 * (size_in - 1)) / (size_out - 1);
	mant = fixpoint / 4096;
	frac = fixpoint - (mant * 4096);

	if (sh_veu_is_veu2h(ump)) {
		if (frac & 0x07) {
			frac &= ~0x07;

			if (size_out > size_in)
				frac -= 8;	/* round down if scaling up */
			else
				frac += 8;	/* round up if scaling down */
		}
	}

	/* Fix calculation for 1 to 1 scaling */
	if (size_in == size_out){
		mant = 0;
		frac = 0;
	}

	/* set scale */

	value = read_reg(ump, VRFCR);

	if (vertical) {
		value &= ~0xffff0000;
		value |= ((mant << 12) | frac) << 16;
	} else {
		value &= ~0xffff;
		value |= (mant << 12) | frac;
	}

	write_reg(ump, value, VRFCR);

	/* set clip */

	value = read_reg(ump, VRFSR);

	if (vertical) {
		value &= ~0xffff0000;
		value |= clip_out << 16;
	} else {
		value &= ~0xffff;
		value |= clip_out;
	}

	write_reg(ump, value, VRFSR);

	/* VEU3F needs additional VRPBR register handling */
#ifdef KERNEL2_6_33
	if (sh_veu_is_veu3f(ump)) {
#endif
	    if (size_out >= size_in)
	        vb = 64;
	    else {
	        if ((mant >= 8) && (mant < 16))
	            value = 4;
	        else if ((mant >= 4) && (mant < 8))
	            value = 2;
	        else
	            value = 1;

	        vb = 64 * 4096 * value;
	        vb /= 4096 * mant + frac;
	    }

	    /* set resize passband register */
	    value = read_reg(ump, VRPBR);
	    if (vertical) {
	        value &= ~0xffff0000;
		value |= vb << 16;
	    } else {
	        value &= ~0xffff;
	        value |= vb;
	    }
	    write_reg(ump, value, VRPBR);
#ifdef KERNEL2_6_33
	}
#endif
}

static void align(int fmt, struct point *p)
{
	/* YCbCr x,y must be modulo 4 due to chroma sub-sampling and
	   requirement for chroma address to be word aligned */
	if (fmt != V4L2_PIX_FMT_RGB565) {
		p->x = (p->x & ~3);
		p->y = (p->y & ~3);
	}
}

static void
crop_offset(
	unsigned long *py,
	unsigned long *pc,
	int fmt,
	unsigned long width,
	struct point *p)
{
	int offset = (p->y * width) + p->x;

	if (fmt == V4L2_PIX_FMT_RGB565) {
		*py += 2 * offset;
	} else if (fmt == V4L2_PIX_FMT_RGB32) {
		*py += 4 * offset;
	} else if (fmt == V4L2_PIX_FMT_NV12) {
		*py += offset;
		*pc += offset/2;
	} else {
		/* V4L2_PIX_FMT_NV16 */
		*py += offset;
		*pc += offset;
	}
}

static int format_supported(int fmt)
{
	if ((fmt == V4L2_PIX_FMT_NV12) ||
	    (fmt == V4L2_PIX_FMT_NV16) ||
	    (fmt == V4L2_PIX_FMT_RGB565) ||
	    (fmt == V4L2_PIX_FMT_RGB32))
		return 1;
	return 0;
}

static int is_ycbcr(int fmt)
{
	if ((fmt == V4L2_PIX_FMT_NV12) || (fmt == V4L2_PIX_FMT_NV16))
		return 1;
	return 0;
}

static int is_rgb(int fmt)
{
	if ((fmt == V4L2_PIX_FMT_RGB565) || (fmt == V4L2_PIX_FMT_RGB32))
		return 1;
	return 0;
}

static int different_colorspace(int fmt1, int fmt2)
{
	if ((is_rgb(fmt1) && is_ycbcr(fmt2))
	   || (is_ycbcr(fmt1) && is_rgb(fmt2)))
		return 1;
	return 0;
}

static int width(struct rect *r)
{
	return (r->br.x - r->tl.x);
}

static int height(struct rect *r)
{
	return (r->br.y - r->tl.y);
}

static void limit(struct rect *r, int x1, int y1, int x2, int y2)
{
	if (r->tl.x < x1) r->tl.x = x1;
	if (r->tl.y < y1) r->tl.y = y1;
	if (r->br.x > x2) r->br.x = x2;
	if (r->br.y > y2) r->br.y = y2;
}


SHVEU *shveu_open(void)
{
	SHVEU *veu;
	int ret;

	veu = calloc(1, sizeof(*veu));
	if (!veu)
		goto err;

	veu->uiomux = uiomux_open();
	if (!veu->uiomux)
		goto err;

	ret = uiomux_get_mmio (veu->uiomux, UIOMUX_SH_VEU,
		&veu->uio_mmio.address,
		&veu->uio_mmio.size,
		&veu->uio_mmio.iomem);
	if (!ret)
		goto err;

	veu->crop_src = 0;
	veu->crop_dst = 0;

	return veu;

err:
	shveu_close(veu);
	return 0;
}

void shveu_close(SHVEU *pvt)
{
	if (pvt) {
		if (pvt->uiomux)
			uiomux_close(pvt->uiomux);
		free(pvt);
	}
}

int
shveu_start_locked(
	SHVEU *pvt,
	unsigned long src_py,
	unsigned long src_pc,
	unsigned long src_width,
	unsigned long src_height,
	unsigned long src_pitch,
	int src_fmt,
	unsigned long dst_py,
	unsigned long dst_pc,
	unsigned long dst_width,
	unsigned long dst_height,
	unsigned long dst_pitch,
	int dst_fmt,
	shveu_rotation_t rotate)
{
	struct uio_map *ump = &pvt->uio_mmio;
	float scale_x, scale_y;
	unsigned long vdst_width;
	unsigned long vdst_height;

#ifdef DEBUG
	fprintf(stderr, "%s IN\n", __FUNCTION__);
	fprintf(stderr, "src_fmt=%d: src_width=%lu, src_height=%lu src_pitch=%lu\n",
		src_fmt, src_width, src_height, src_pitch);
	fprintf(stderr, "dst_fmt=%d: dst_width=%lu, dst_height=%lu dst_pitch=%lu\n",
		dst_fmt, dst_width, dst_height, dst_pitch);
	fprintf(stderr, "rotate=%d\n", rotate);
#endif
	if (!pvt->crop_src) {
		pvt->scrop.tl.x = 0;
		pvt->scrop.tl.y = 0;
		pvt->scrop.br.x = src_width;
		pvt->scrop.br.y = src_height;
	}

	if (!pvt->crop_dst) {
		pvt->dcrop.tl.x = 0;
		pvt->dcrop.tl.y = 0;
		pvt->dcrop.br.x = dst_width;
		pvt->dcrop.br.y = dst_height;
	}

	/* Fix crop specification for required alignment */
	align(src_fmt, &pvt->scrop.tl);
	align(src_fmt, &pvt->scrop.br);
	align(dst_fmt, &pvt->dcrop.tl);
	align(dst_fmt, &pvt->dcrop.br);

	vdst_width  = width(&pvt->dcrop);
	vdst_height = height(&pvt->dcrop);

	/* scale factors */
	scale_x = (float)width(&pvt->dcrop)  / width(&pvt->scrop);
	scale_y = (float)height(&pvt->dcrop) / height(&pvt->scrop);

	/* limit cropping to the surface size */
	limit(&pvt->scrop, 0, 0, src_width, src_height);
	limit(&pvt->dcrop, 0, 0, dst_width, dst_height);

	src_width  = width(&pvt->scrop);
	src_height = height(&pvt->scrop);
	dst_width  = width(&pvt->dcrop);
	dst_height = height(&pvt->dcrop);

#ifdef DEBUG
	fprintf(stderr, "Scale x=%f, y=%f\n", scale_x, scale_y);
	fprintf(stderr, "New src_width=%lu, src_height=%lu\n", src_width, src_height);
	fprintf(stderr, "New dst_width=%lu, dst_height=%lu\n", dst_width, dst_height);
#endif

	/* Rotate can't be performed at the same time as a scale! */
	if (rotate && (src_width != dst_height))
		return -1;
	if (rotate && (dst_width != src_height))
		return -1;

	if (!format_supported(src_fmt) || !format_supported(dst_fmt))
		return -1;

	/* VESWR/VEDWR restrictions */
	if ((src_pitch % 4) || (dst_pitch % 4))
		return -1;

	/* VESSR restrictions */
	if ((src_height < 16) || (src_height > 4092) ||
	    (src_width  < 16) || (src_width  > 4092))
		return -1;

	/* Scaling limits */
	if (sh_veu_is_veu2h(ump)) {
		if ((scale_x > 8.0) || (scale_y > 8.0))
			return -1;
	} else {
		if ((scale_x > 16.0) || (scale_y > 16.0))
			return -1;
	}
	if ((scale_x < 1.0/16.0) || (scale_y < 1.0/16.0))
		return -1;


	uiomux_lock (pvt->uiomux, UIOMUX_SH_VEU);

	/* reset */
	write_reg(ump, 0x100, VBSRR);


	/* source */
	if (pvt->crop_src) {
		crop_offset(&src_py, &src_pc, src_fmt, src_pitch, &pvt->scrop.tl);
	}
	write_reg(ump, src_py, VSAYR);
	write_reg(ump, src_pc, VSACR);

	write_reg(ump, (src_height << 16) | src_width, VESSR);

	if (src_fmt == V4L2_PIX_FMT_RGB565)
		src_pitch *= 2;
	if (src_fmt == V4L2_PIX_FMT_RGB32)
		src_pitch *= 4;
	write_reg(ump, src_pitch, VESWR);
	write_reg(ump, 0, VBSSR);	/* not using bundle mode */


	/* dest */
	if (pvt->crop_dst) {
		crop_offset(&dst_py, &dst_pc, dst_fmt, dst_pitch, &pvt->dcrop.tl);
	}
	if (rotate) {
		int src_vblk  = (src_height+15)/16;
		int src_sidev = (src_height+15)%16 + 1;
		int dst_density = 2;	/* for RGB565 and YCbCr422 */
		int offset;

		if (dst_fmt == V4L2_PIX_FMT_NV12)
			dst_density = 1;
		offset = ((src_vblk-2)*16 + src_sidev) * dst_density;

		write_reg(ump, dst_py + offset, VDAYR);
		write_reg(ump, dst_pc + offset, VDACR);
	} else {
		write_reg(ump, dst_py, VDAYR);
		write_reg(ump, dst_pc, VDACR);
	}

	if (dst_fmt == V4L2_PIX_FMT_RGB565)
		dst_pitch *= 2;
	if (dst_fmt == V4L2_PIX_FMT_RGB32)
		dst_pitch *= 4;
	write_reg(ump, dst_pitch, VEDWR);

	/* byte/word swapping */
	{
		unsigned long vswpr = 0;
		if (src_fmt == V4L2_PIX_FMT_RGB32)
			vswpr |= 0;
		else if (src_fmt == V4L2_PIX_FMT_RGB565)
			vswpr |= 0x6;
		else
			vswpr |= 0x7;

		if (dst_fmt == V4L2_PIX_FMT_RGB32)
			vswpr |= 0;
		if (dst_fmt == V4L2_PIX_FMT_RGB565)
			vswpr |= 0x60;
		else
			vswpr |= 0x70;
		write_reg(ump, vswpr, VSWPR);
#if DEBUG
		fprintf(stderr, "vswpr=0x%X\n", vswpr);
#endif
	}

	/* transform control */
	{
		unsigned long vtrcr = 0;
		switch (src_fmt)
		{
		case V4L2_PIX_FMT_RGB565:
			vtrcr |= VTRCR_RY_SRC_RGB;
			vtrcr |= VTRCR_SRC_FMT_RGB565;
			break;
		case V4L2_PIX_FMT_RGB32:
			vtrcr |= VTRCR_RY_SRC_RGB;
			vtrcr |= VTRCR_SRC_FMT_RGBX888;
			break;
		case V4L2_PIX_FMT_NV12:
			vtrcr |= VTRCR_RY_SRC_YCBCR;
			vtrcr |= VTRCR_SRC_FMT_YCBCR420;
			break;
		case V4L2_PIX_FMT_NV16:
			vtrcr |= VTRCR_RY_SRC_YCBCR;
			vtrcr |= VTRCR_SRC_FMT_YCBCR422;
		}

		switch (dst_fmt)
		{
		case V4L2_PIX_FMT_RGB565:
			vtrcr |= VTRCR_DST_FMT_RGB565;
			break;
		case V4L2_PIX_FMT_RGB32:
			vtrcr |= VTRCR_DST_FMT_RGBX888;
			break;
		case V4L2_PIX_FMT_NV12:
			vtrcr |= VTRCR_DST_FMT_YCBCR420;
			break;
		case V4L2_PIX_FMT_NV16:
			vtrcr |= VTRCR_DST_FMT_YCBCR422;
		}

		if (different_colorspace(src_fmt, dst_fmt))
			vtrcr |= VTRCR_TE_BIT_SET;
		write_reg(ump, vtrcr, VTRCR);
#if DEBUG
		fprintf(stderr, "vtrcr=0x%X\n", vtrcr);
#endif
	}

	/* Is this a VEU2H on SH7723? */
	if (sh_veu_is_veu2h(ump)) {
		/* color conversion matrix */
		write_reg(ump, 0x0cc5, VMCR00);
		write_reg(ump, 0x0950, VMCR01);
		write_reg(ump, 0x0000, VMCR02);
		write_reg(ump, 0x397f, VMCR10);
		write_reg(ump, 0x0950, VMCR11);
		write_reg(ump, 0x3cdd, VMCR12);
		write_reg(ump, 0x0000, VMCR20);
		write_reg(ump, 0x0950, VMCR21);
		write_reg(ump, 0x1023, VMCR22);
		write_reg(ump, 0x00800010, VCOFFR);
	}

	write_reg(ump, 0, VRFCR);
	write_reg(ump, 0, VRFSR);
        if ((vdst_width*vdst_height) > (src_width*src_height)) {
                set_scale(ump, 0, src_width,  vdst_width, 1, dst_width);
                set_scale(ump, 1, src_height, vdst_height, 1, dst_height);
        } else {
                set_scale(ump, 0, src_width,  vdst_width, 0, dst_width);
                set_scale(ump, 1, src_height, vdst_height, 0, dst_height);
        }

	if (rotate) {
		write_reg(ump, 1, VFMCR);
		write_reg(ump, 0, VRFCR);
	} else {
		write_reg(ump, 0, VFMCR);
	}

	/* enable interrupt in VEU */
	write_reg(ump, 1, VEIER);

	/* start operation */
	write_reg(ump, 1, VESTR);

#ifdef DEBUG
	fprintf(stderr, "%s OUT\n", __FUNCTION__);
#endif

	return 0;
}

void
shveu_wait(SHVEU *pvt)
{
	uiomux_sleep(pvt->uiomux, UIOMUX_SH_VEU);
	write_reg(&pvt->uio_mmio, 0x100, VEVTR);   /* ack int, write 0 to bit 0 */

	/* Wait for VEU to stop */
	while (read_reg(&pvt->uio_mmio, VSTAR) & 1)
		;

	uiomux_unlock(pvt->uiomux, UIOMUX_SH_VEU);
}

int
shveu_rescale(
	SHVEU *veu,
	unsigned long src_py,
	unsigned long src_pc,
	unsigned long src_width,
	unsigned long src_height,
	int src_fmt,
	unsigned long dst_py,
	unsigned long dst_pc,
	unsigned long dst_width,
	unsigned long dst_height,
	int dst_fmt)
{
	int ret = 0;

	ret = shveu_start_locked(
		veu,
		src_py, src_pc, src_width, src_height, src_width, src_fmt,
		dst_py, dst_pc, dst_width, dst_height, dst_width, dst_fmt,
		SHVEU_NO_ROT);

	if (ret == 0)
		shveu_wait(veu);

	return ret;
}

int
shveu_rotate(
	SHVEU *veu,
	unsigned long src_py,
	unsigned long src_pc,
	unsigned long src_width,
	unsigned long src_height,
	int src_fmt,
	unsigned long dst_py,
	unsigned long dst_pc,
	int dst_fmt,
	shveu_rotation_t rotate)
{
	int ret = 0;

	ret = shveu_start_locked(
		veu,
		src_py, src_pc, src_width, src_height, src_width, src_fmt,
		dst_py, dst_pc, src_height, src_width, src_height, dst_fmt,
		rotate);

	if (ret == 0)
		shveu_wait(veu);

	return ret;
}

void
shveu_crop (
	SHVEU *veu,
	int crop_dst,
	int x1,
	int y1,
	int x2,
	int y2)
{
	struct rect * r;

	if (crop_dst) {
		veu->crop_dst = 1;
		r = &veu->dcrop;
	} else {
		veu->crop_src = 1;
		r = &veu->scrop;
	}

	r->tl.x = x1;
	r->tl.y = y1;
	r->br.x = x2;
	r->br.y = y2;
}
