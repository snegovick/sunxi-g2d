/*
 * Allwinner SoCs g2d driver.
 *
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2024 Brandon Cheo Fusi <fusibrandon13@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.	 This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/dmaengine.h>
#include <linux/bitfield.h>

#include "sunxi_g2d_hw.h"
#include "sunxi_g2d_regs.h"
#include "sunxi_g2d_scaler.h"

static const uint32_t logic_op_to_bld[] = {
	[G2D_BLT_BLACKNESS] = 0x40000,
	[G2D_BLT_NOTMERGEPEN] = 0x41440,
	[G2D_BLT_MASKNOTPEN] = 0x41010,
	[G2D_BLT_NOTCOPYPEN] = 0x61090,
	[G2D_BLT_MASKPENNOT] = 0x41008,
	[G2D_BLT_NOT] = 0x51088,
	[G2D_BLT_XORPEN] = 0x41080,
	[G2D_BLT_NOTMASKPEN] = 0x41400,
	[G2D_BLT_MASKPEN] = 0x41000,
	[G2D_BLT_NOTXORPEN] = 0x41480,
	[G2D_BLT_NOP] = 0x51080,
	[G2D_BLT_MERGENOTPEN] = 0x41048,
	[G2D_BLT_COPYPEN] = 0x61080,
	[G2D_BLT_MERGEPENNOT] = 0x41048,
	[G2D_BLT_MERGEPEN] = 0x41040,
	[G2D_BLT_WHITENESS] = 0x48000,
};

static const uint32_t porter_duff_to_bld[] = {
	[G2D_PDM_CLEAR] = 0,
	[G2D_PDM_COPY] = 0x00010001,
	[G2D_PDM_DST] = 0x01000100,
	[G2D_PDM_SRCOVER] = 0x03010301,
	[G2D_PDM_DSTOVER] = 0x01030103,
	[G2D_PDM_SRCIN] = 0x00020002,
	[G2D_PDM_DSTIN] = 0x02000200,
	[G2D_PDM_SRCOUT] = 0x00030003,
	[G2D_PDM_DSTOUT] = 0x03000300,
	[G2D_PDM_SRCATOP] = 0x03020302,
	[G2D_PDM_DSTATOP] = 0x02030203,
	[G2D_PDM_XOR] = 0x03030303,
};


/* TODO: need this procs in other places too */
uint32_t g2d_read(struct sunxi_g2d *g2d, uint32_t reg)
{
	return readl(g2d->base + reg);
}

void g2d_write(struct sunxi_g2d *g2d,
						 uint32_t reg, uint32_t val)
{
	writel(val, g2d->base + reg);
}

static inline void g2d_set_bits(struct sunxi_g2d *g2d,
					uint32_t reg, uint32_t bits)
{
	writel(readl(g2d->base + reg) | bits, g2d->base + reg);
}

static inline void g2d_clr_bits(struct sunxi_g2d *g2d,
							uint32_t reg, uint32_t bits)
{
	writel(readl(g2d->base + reg) & ~bits, g2d->base + reg);
}

static uint32_t v4l2_fmt_to_hw_id(struct v4l2_pix_format *v4l2_pix_fmt)
{
	struct g2d_fmt *fmt;

	fmt = find_fmt(v4l2_pix_fmt); // !! (rectfill assumption) src_fmt
	return (fmt) ? fmt->hw_id : G2D_FORMAT_XBGR8888;
}

void g2d_hw_open(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_hw_open\n");
	g2d_set_bits(g2d, G2D_SCLK_GATE,
			(G2D_SCLK_GATE_MIXER | G2D_SCLK_GATE_ROT));
	g2d_set_bits(g2d, G2D_HCLK_GATE,
			(G2D_HCLK_GATE_MIXER | G2D_HCLK_GATE_ROT));
	g2d_set_bits(g2d, G2D_AHB_RESET,
			(G2D_AHB_MIXER_RESET | G2D_AHB_ROT_RESET));
}

void g2d_hw_close(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_hw_close\n");
	g2d_write(g2d, G2D_SCLK_GATE, 0);
	g2d_write(g2d, G2D_HCLK_GATE, 0);
	g2d_write(g2d, G2D_AHB_RESET, 0);
}

void g2d_hw_reset(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_hw_reset\n");
	g2d_write(g2d, G2D_AHB_RESET, 0);
	g2d_set_bits(g2d, G2D_AHB_RESET,
			(G2D_AHB_MIXER_RESET | G2D_AHB_ROT_RESET));
}

static void g2d_mixer_irq_enable(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_mixer_irq_enable\n");
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_FINISH_IRQ_EN);
}

int g2d_mixer_irq_query(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_mixer_irq_query\n");
	uint32_t tmp;

	tmp = g2d_read(g2d, G2D_MIXER_INT);
	if (tmp & G2D_MIXER_INT_IRQ_PENDING) {
		g2d_clr_bits(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING
					| G2D_MIXER_INT_FINISH_IRQ_EN);

		return 1;
	}

	return 0;
}

void g2d_mixer_reset(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_mixer_reset\n");
	g2d_clr_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
	g2d_set_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
}

void g2d_rot_reset(struct sunxi_g2d *g2d)
{
	G2D_DEBUG_MSG(g2d, "g2d_rot_reset\n");
	g2d_clr_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
	g2d_set_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
}

/*
 * Basically, this should map the format to a tuple (ycnt, ucnt, vcnt),
 * with each entry corresponding to the # of bytes for each channel in the
 * YUV representation
 *
 * TODO: Map only formats defined in g2d_formats.
 */
void fmt2yuvcnt(uint32_t format, uint32_t *ycnt, uint32_t *ucnt, uint32_t *vcnt)
{
	*ycnt = 0;
	*ucnt = 0;
	*vcnt = 0;
	if (format <= G2D_FORMAT_BGRX8888)
		*ycnt = 4;

	else if (format <= G2D_FORMAT_BGR888)
		*ycnt = 3;

	else if (format <= G2D_FORMAT_BGRA5551)
		*ycnt = 2;

	else if (format <= G2D_FORMAT_BGRA1010102)
		*ycnt = 4;

	else if (format <= 0x23) {
		*ycnt = 2;
	}

	else if (format <= 0x25) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x26) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format <= 0x29) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x2a) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format <= 0x2d) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x2e) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format == 0x30)
		*ycnt = 1;

	else if (format <= 0x36) {
		*ycnt = 2;
		*ucnt = 4;
	}

	else if (format <= 0x39)
		*ycnt = 6;
}

/* TODO: convert layer_no to an enumeration */
void g2d_fc_set(struct sunxi_g2d *g2d, uint32_t layer_no, uint32_t color_value)
{
	G2D_DEBUG_MSG(g2d, "g2d_fc_set\n");
	G2D_INFO_MSG(g2d, "FILLCOLOR: sel: %d, color: 0x%x\n", layer_no, color_value);

	switch (layer_no)
	{
		case 0:
			/* Video Layer */
			g2d_set_bits(g2d, V0_ATTCTL, V0_ATTCTL_FILLCOLOR_EN);
			g2d_write(g2d, V0_FILLC, color_value);
			break;

		case 1:
			/* UI0 Layer */
			g2d_set_bits(g2d, UI0_ATTCTL, BIT(4));
			g2d_write(g2d, UI0_FILLC, color_value);
			break;

		case 2:
			/* UI1 Layer */
			g2d_set_bits(g2d, UI1_ATTCTL, BIT(4));
			g2d_write(g2d, UI1_FILLC, color_value);
			break;

		case 3:
			/* UI2 Layer */
			g2d_set_bits(g2d, UI2_ATTCTL, BIT(4));
			g2d_write(g2d, UI2_FILLC, color_value);
			break;

		default:
			return;
	}
}

/* TODO: convert pipe_no to an enumeration */
void g2d_bldin_set(struct sunxi_g2d *g2d, struct g2d_frame *frm,
		uint32_t pipe_no)
{
	G2D_DEBUG_MSG(g2d, "g2d_bldin_set\n");
	uint32_t rect_x, rect_y, rect_w, rect_h;
	uint32_t reg;
	uint32_t tmp;

	if (pipe_no == 0) {
		tmp = FIELD_PREP(BLD_PIPE0_FENCE_EN, 1);
		tmp |= FIELD_PREP(BLD_PIPE0_EN, 1);
		g2d_set_bits(g2d, BLD_EN_CTL, tmp);
		/* TODO: is FENCE_EN setting missing ? */
		if (frm->premult_alpha)
			g2d_set_bits(g2d, BLD_PREMUL_CTL,
				BLD_PREMUL_CTL_PIPE0_ALPHA_MODE);
	} else {
		g2d_set_bits(g2d, BLD_EN_CTL, BLD_PIPE1_EN);
		if (frm->premult_alpha)
			g2d_set_bits(g2d, BLD_PREMUL_CTL,
				BLD_PREMUL_CTL_PIPE1_ALPHA_MODE);
	}

	/* the horizontal (rect_x) and vertical (rect_y) blend offsets are
	 * always set to zero.
	 */
	rect_x = 0;
	rect_y = 0;
	rect_w = frm->sel.r.width;
	rect_h = frm->sel.r.height;

	tmp = ((rect_h - 1) << 16) | (rect_w - 1);
	G2D_INFO_MSG(g2d, "BLD_CH_ISIZE W:	0x%x\n", rect_w);
	G2D_INFO_MSG(g2d, "BLD_CH_ISIZE H:	0x%x\n", rect_h);

	reg = (pipe_no) ? BLD_CH_ISIZE1 : BLD_CH_ISIZE0;
	g2d_write(g2d, reg, tmp);

	tmp = ((rect_y <= 0 ? 0 : rect_y - 1) << 16)
		| (rect_x <= 0 ? 0 : rect_x - 1);
	G2D_INFO_MSG(g2d, "BLD_CH_ISIZE X:	0x%x\n", rect_x);
	G2D_INFO_MSG(g2d, "BLD_CH_ISIZE Y:	0x%x\n", rect_y);

	reg = (pipe_no) ? BLD_CH_OFFSET1 : BLD_CH_OFFSET0;
	g2d_write(g2d, reg, tmp);
}

/**
 * set the bld color space based on the format
 * if the format is UI, then set the bld in RGB color space
 * if the format is Video, then set the bld in YUV color space
 */
void g2d_bld_cs_set(struct sunxi_g2d *g2d, struct g2d_frame *frm)
{
	G2D_DEBUG_MSG(g2d, "g2d_bld_cs_set\n");
	uint32_t fmt_hw_id;

	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);

	if (fmt_hw_id <= G2D_FORMAT_BGRA1010102)
		g2d_clr_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_ALPHA_MODE);
	else if (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)
		g2d_set_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_ALPHA_MODE);
}

void g2d_wb_set(struct sunxi_g2d *g2d, struct g2d_frame *frm,
		dma_addr_t addr[3])
{
	G2D_DEBUG_MSG(g2d, "g2d_wb_set\n");
	uintptr_t addr0, addr1, addr2;
	uint32_t fmt_hw_id;
	uint32_t ycnt, ucnt, vcnt;
	uint32_t pitch0, pitch1, pitch2;
	uint32_t cw, cy, cx;
	uint32_t tmp;

	/* write-back pixel format */
	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);
	g2d_write(g2d, WB_ATT, fmt_hw_id);

	/* write-back size */
	tmp = FIELD_PREP(WB_SIZE_WIDTH, (frm->sel.r.width == 0 ?
				0 : frm->sel.r.width - 1));
	tmp |= FIELD_PREP(WB_SIZE_HEIGHT, (frm->sel.r.height == 0 ?
				0 : frm->sel.r.height - 1));
	g2d_write(g2d, WB_SIZE, tmp);

	/* blend output size */
	G2D_INFO_MSG(g2d, "BLD_CH_OSIZE W:	0x%x\n", frm->sel.r.width);
	G2D_INFO_MSG(g2d, "BLD_CH_OSIZE H:	0x%x\n", frm->sel.r.height);
	g2d_write(g2d, BLD_OUT_SIZE, tmp);

	if (frm->premult_alpha)
		g2d_set_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);
	else
		g2d_clr_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);

	if ((fmt_hw_id >= G2D_FORMAT_YUV422UVC_V1U1V0U0)
				&& (fmt_hw_id <= G2D_FORMAT_YUV422_PLANAR)) {
		cw = frm->v4l2_pix_fmt.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV420UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV420_PLANAR)) {
		cw = frm->v4l2_pix_fmt.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top >> 1;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV411UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)) {
		cw = frm->sel.r.left >> 2;
		cy = frm->sel.r.top;
	}

	else {
		cw = 0;
		cx = 0;
		cy = 0;
	}

	fmt2yuvcnt(fmt_hw_id, &ycnt, &ucnt, &vcnt);

	pitch0 = ALIGN(ycnt * frm->v4l2_pix_fmt.width, frm->alignment);
	g2d_write(g2d, WB_PITCH0, pitch0);

	pitch1 = ALIGN(ucnt * cw, frm->alignment);
	g2d_write(g2d, WB_PITCH1, pitch1);
	pitch2 = ALIGN(vcnt * cw, frm->alignment);
	g2d_write(g2d, WB_PITCH2, pitch2);

	G2D_INFO_MSG(g2d, "OutputPitch: %d, %d, %d\n", pitch0, pitch1, pitch2);

	addr0 =
		addr[0] + pitch0 * frm->sel.r.top + ycnt * frm->sel.r.left;
	g2d_write(g2d, WB_LADD0, addr0 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD0, addr0 >> 32);
#endif

	addr1 = addr[1] + pitch1 * cy + ucnt * cx;
	g2d_write(g2d, WB_LADD1, addr1 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD1, addr1 >> 32);
#endif

	addr2 = addr[2] + pitch2 * cy + vcnt * cx;
	g2d_write(g2d, WB_LADD2, addr2 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD2, addr2 >> 32);
#endif

	G2D_INFO_MSG(g2d, "WbAddr: 0x%lx, 0x%lx, 0x%lx\n", addr0, addr1, addr2);
}

void g2d_vlayer_set(struct sunxi_g2d *g2d, struct g2d_frame *frm,
		dma_addr_t addr[3], uint32_t layer_alpha)
{
	G2D_DEBUG_MSG(g2d, "g2d_vlayer_set\n");
	uintptr_t addr0, addr1, addr2;
	uint32_t fmt_hw_id;
	uint32_t ycnt, ucnt, vcnt;
	uint32_t pitch0, pitch1, pitch2;
	uint32_t cw, cy, cx;
	uint32_t tmp;

	tmp = FIELD_PREP(V0_ATTCTL_GLBALPHA, layer_alpha);

	if (frm->premult_alpha)
		tmp |= FIELD_PREP(V0_ATTCTL_PREMUL_CTL, 0x2);

	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);
	tmp |= FIELD_PREP(V0_ATTCTL_FBFMT, fmt_hw_id);
	tmp |= FIELD_PREP(V0_ATTCTL_ALPHA_MODE, frm->alpha_bld_mode);
	tmp |= FIELD_PREP(V0_ATTCTL_EN, 1);
	g2d_write(g2d, V0_ATTCTL, tmp);

	tmp = FIELD_PREP(V0_MBSIZE_WIDTH, (frm->sel.r.width == 0 ?
				0 : frm->sel.r.width - 1));
	tmp |= FIELD_PREP(V0_MBSIZE_HEIGHT, (frm->sel.r.height == 0 ?
				0 : frm->sel.r.height - 1));
	g2d_write(g2d, V0_MBSIZE, tmp);

	/* offset is set to 0, overlay size is set to layer size */
	g2d_write(g2d, V0_SIZE, tmp);
	g2d_write(g2d, V0_COOR, 0);

	if ((fmt_hw_id >= G2D_FORMAT_YUV422UVC_V1U1V0U0)
				&& (fmt_hw_id <= G2D_FORMAT_YUV422_PLANAR)) {
		cw = frm->sel.r.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV420UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV420_PLANAR)) {
		cw = frm->sel.r.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top >> 1;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV411UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)) {
		cx = frm->sel.r.left >> 2;
		cy = frm->sel.r.top;
	}

	else {
		cw = 0;
		cx = 0;
		cy = 0;
	}

	fmt2yuvcnt(fmt_hw_id, &ycnt, &ucnt, &vcnt);

	pitch0 = ALIGN(ycnt * frm->v4l2_pix_fmt.width, frm->alignment);
	g2d_write(g2d, V0_PITCH0, pitch0);

	pitch1 = ALIGN(ucnt * cw, frm->alignment);
	g2d_write(g2d, V0_PITCH1, pitch1);

	pitch2 = ALIGN(vcnt * cw, frm->alignment);
	g2d_write(g2d, V0_PITCH2, pitch2);

	G2D_INFO_MSG(g2d, "VInPITCH: %d, %d, %d\n",
				pitch0, pitch1, pitch2);
	G2D_INFO_MSG(g2d, "VInAddrB: 0x%x, 0x%x, 0x%x\n",
			addr[0], addr[1], addr[2]);

	/* address of the rectangle */
	addr0 =
		addr[0] + pitch0 * frm->sel.r.top + ycnt * frm->sel.r.left;
	g2d_write(g2d, V0_LADDR0, addr0 & GENMASK(31, 0));

	addr1 = addr[1] + pitch1 * cy + ucnt * cx;
	g2d_write(g2d, V0_LADDR1, addr1 & GENMASK(31, 0));

	addr2 = addr[2] + pitch2 * cy + vcnt * cx;
	g2d_write(g2d, V0_LADDR2, addr2 & GENMASK(31, 0));

	/* The G2D can support 40-bit bus addresses. Only fill V0_HADDR if we're dealing
	 * with 64-bit DMA addresses
	 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	tmp = FIELD_PREP(V0_HADDR0, addr[0]);
	tmp |= FIELD_PREP(V0_HADDR1, addr[1]);
	tmp |= FIELD_PREP(V0_HADDR2, addr[2]);
	g2d_write(g2d, V0_HADDR, tmp);
#endif

	G2D_INFO_MSG(g2d, "VInAddrA: 0x%lx, 0x%lx, 0x%lx\n",
							addr0, addr1, addr2);
}

int g2d_uilayer_set(struct sunxi_g2d *g2d, struct g2d_frame *frm, dma_addr_t addr[3], int layer_no, uint32_t layer_alpha)
{
	uintptr_t addr0;
	uint32_t ycnt, ucnt, vcnt;
	uint32_t pitch0;
	int ret = -1;
	uint32_t tmp;

  uint32_t fmt_hw_id;

	uint32_t reg_attctl;
	uint32_t reg_size;
	uint32_t reg_mbsize;
	uint32_t reg_coor;
	uint32_t reg_pitch;
	uint32_t reg_laddr0;
	uint32_t reg_haddr;

	switch (layer_no) {
	case 0:
		reg_attctl = UI0_ATTCTL;
		reg_size = UI0_SIZE;
		reg_mbsize = UI0_MBSIZE;
		reg_coor = UI0_COOR;
		reg_pitch = UI0_PITCH;
		reg_laddr0 = UI0_LADDR0;
		reg_haddr = UI0_HADDR;
		break;
	case 1:
		reg_attctl = UI1_ATTCTL;
		reg_size = UI1_SIZE;
		reg_mbsize = UI1_MBSIZE;
		reg_coor = UI1_COOR;
		reg_pitch = UI1_PITCH;
		reg_laddr0 = UI1_LADDR0;
		reg_haddr = UI1_HADDR;
		break;
	case 2:
		reg_attctl = UI2_ATTCTL;
		reg_size = UI2_SIZE;
		reg_mbsize = UI2_MBSIZE;
		reg_coor = UI2_COOR;
		reg_pitch = UI2_PITCH;
		reg_laddr0 = UI2_LADDR0;
		reg_haddr = UI2_HADDR;
		break;
	default:
		v4l2_err(&g2d->v4l2_dev, "UI layer out of range\n");
		return -EINVAL;
	}

	tmp = FIELD_PREP(UIX_ATTCTL_GLBALPHA, layer_alpha);
	if (frm->premult_alpha)
		tmp |= FIELD_PREP(UIX_ATTCTL_PREMUL_CTL, 0x2);

	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);
	tmp |= FIELD_PREP(UIX_ATTCTL_FBFMT, fmt_hw_id);
	tmp |= FIELD_PREP(UIX_ATTCTL_ALPHA_MODE, frm->alpha_bld_mode);
	tmp |= FIELD_PREP(UIX_ATTCTL_EN, 1);
	g2d_write(g2d, reg_attctl, tmp);

	tmp = FIELD_PREP(UIX_MBSIZE_WIDTH, (frm->sel.r.width == 0 ?
				0 : frm->sel.r.width - 1));
	tmp |= FIELD_PREP(UIX_MBSIZE_HEIGHT, (frm->sel.r.height == 0 ?
				0 : frm->sel.r.height - 1));
	g2d_write(g2d, reg_mbsize, tmp); //mem.bits

	g2d_write(g2d, reg_size, tmp); //winsize
	g2d_write(g2d, reg_coor, 0); //dwval

	fmt2yuvcnt(fmt_hw_id, &ycnt, &ucnt, &vcnt);

	pitch0 = ALIGN(ycnt * frm->v4l2_pix_fmt.width, frm->alignment);
	g2d_write(g2d, reg_pitch, pitch0);

	/* addr0 = */
	/*     p_img->laddr[0] + ((__u64) p_img->haddr[0] << 32) + */
	/*     pitch0 * p_img->clip_rect.y + ycnt * p_img->clip_rect.x; */
	/* p_reg->ovl_mem_low_addr0 = addr0 & 0xffffffff; */
	/* p_reg->ovl_mem_high_addr = (addr0 >> 32) & 0xff; */

	addr0 = addr[0] + pitch0 * frm->sel.r.top + ycnt * frm->sel.r.left;
	g2d_write(g2d, reg_laddr0, addr0 & GENMASK(31, 0));

	/* The G2D can support 40-bit bus addresses. Only fill V0_HADDR if we're dealing
	 * with 64-bit DMA addresses
	 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	tmp = FIELD_PREP(UIX_HADDR0, addr[0]);
	g2d_write(g2d, reg_haddr, tmp);
#endif

	//TODO: port the following
	/* if (p_img->bbuff == 0) */
	/* 	g2d_ovl_u_fc_set(p_ovl_u, sel, p_img->color); */
  return 0;
}

void g2d_rectfill(struct sunxi_g2d_ctx *ctx, dma_addr_t addr[3])
{
	struct sunxi_g2d *g2d = ctx->g2d;
	G2D_DEBUG_MSG(g2d, "g2d_rectfill\n");
	/* Maybe only reset the mixer ?? */
	// g2d_mixer_reset(ctx->g2d);
	g2d_hw_reset(ctx->g2d);

	/* prepare the mixer video layer */
	g2d_vlayer_set(ctx->g2d, &ctx->dst, addr, ctx->rectfill_color_alpha);

	/* set the fill color */
	g2d_fc_set(ctx->g2d, 0, ctx->rectfill_color);

	g2d_bldin_set(ctx->g2d, &ctx->dst, 0);
	g2d_bld_cs_set(ctx->g2d, &ctx->dst);

	/* ROP sel ch0 pass */
	g2d_write(ctx->g2d, ROP_CTL, ROP_CTL_BLUE_BYPASS_EN
				| ROP_CTL_GREEN_BYPASS_EN
				| ROP_CTL_RED_BYPASS_EN
				| ROP_CTL_ALPHA_BYPASS_EN);

	g2d_wb_set(ctx->g2d, &ctx->dst, addr);

	/* start the module */
	G2D_INFO_MSG(g2d, "Starting the module");
	g2d_mixer_irq_enable(ctx->g2d);
	g2d_set_bits(ctx->g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_START);
}

void g2d_porter_duff(struct sunxi_g2d *g2d, enum g2d_pd_mode mode)
{
	uint32_t val = porter_duff_to_bld[mode];
	g2d_write(g2d, BLD_CTL, val);
}

int g2d_ovl_v_calc_coarse(struct sunxi_g2d *g2d, enum g2d_fmt_hw_id format, uint32_t inw,
                          uint32_t inh, uint32_t outw, uint32_t outh,
                          uint32_t *midw, uint32_t *midh)
{
	uint32_t tmp;

	switch (format) {
	case G2D_FORMAT_IYUV422_V0Y1U0Y0:
	case G2D_FORMAT_IYUV422_Y1V0Y0U0:
	case G2D_FORMAT_IYUV422_U0Y1V0Y0:
	case G2D_FORMAT_IYUV422_Y1U0Y0V0: {
		/* interleaved YUV422 format */
		*midw = inw;
		*midh = inh;
		break;
	}
	case G2D_FORMAT_YUV422UVC_V1U1V0U0:
	case G2D_FORMAT_YUV422UVC_U1V1U0V0:
	case G2D_FORMAT_YUV422_PLANAR: {
		if (inw >= (outw << 3)) {
			*midw = outw << 3;
			tmp = (*midw << 16) | inw;
			g2d_write(g2d, V0_HDS_CTL0, tmp);
			/* p_reg->hor_down_sample0.dwval = tmp; */
			tmp = (*midw << 15) | ((inw + 1) >> 1);
			g2d_write(g2d, V0_HDS_CTL1, tmp);
			/* p_reg->hor_down_sample1.dwval = tmp; */
		} else {
			*midw = inw;
		}
		if (inh >= (outh << 2)) {
			*midh = (outh << 2);
			tmp = (*midh << 16) | inh;
			g2d_write(g2d, V0_VDS_CTL0, tmp);
			/* p_reg->ver_down_sample0.dwval = tmp; */
			g2d_write(g2d, V0_VDS_CTL1, tmp);
			/* p_reg->ver_down_sample1.dwval = tmp; */
		} else {
			*midh = inh;
		}
		break;
	}
	case G2D_FORMAT_Y8:
	case G2D_FORMAT_YUV420_PLANAR:
	case G2D_FORMAT_YUV420UVC_V1U1V0U0:
	case G2D_FORMAT_YUV420UVC_U1V1U0V0: {
		if (inw >= (outw << 3)) {
			*midw = outw << 3;
			tmp = (*midw << 16) | inw;
			g2d_write(g2d, V0_HDS_CTL0, tmp);
			/* p_reg->hor_down_sample0.dwval = tmp; */
			tmp = (*midw << 15) | ((inw + 1) >> 1);
			g2d_write(g2d, V0_HDS_CTL1, tmp);
			/* p_reg->hor_down_sample1.dwval = tmp; */
		} else {
			*midw = inw;
		}
		if (inh >= (outh << 2)) {
			*midh = (outh << 2);
			tmp = (*midh << 16) | inh;
			g2d_write(g2d, V0_VDS_CTL0, tmp);
			/* p_reg->ver_down_sample0.dwval = tmp; */
			tmp = (*midh << 15) | ((inh + 1) >> 1);
			g2d_write(g2d, V0_VDS_CTL1, tmp);
			/* p_reg->ver_down_sample1.dwval = tmp; */
		} else {
			*midh = inh;
		}
		break;
	}
	case G2D_FORMAT_YUV411_PLANAR:
	case G2D_FORMAT_YUV411UVC_V1U1V0U0:
	case G2D_FORMAT_YUV411UVC_U1V1U0V0: {
		if (inw >= (outw << 3)) {
			*midw = outw << 3;
			tmp = ((*midw) << 16) | inw;
			g2d_write(g2d, V0_HDS_CTL0, tmp);
			/* p_reg->hor_down_sample0.dwval = tmp; */
			tmp = ((*midw) << 14) | ((inw + 3) >> 2);
			g2d_write(g2d, V0_HDS_CTL1, tmp);
			/* p_reg->hor_down_sample1.dwval = tmp; */
		} else {
			*midw = inw;
		}
		if (inh >= (outh << 2)) {
			*midh = (outh << 2);
			tmp = ((*midh) << 16) | inh;
			g2d_write(g2d, V0_VDS_CTL0, tmp);
			/* p_reg->ver_down_sample0.dwval = tmp; */
			g2d_write(g2d, V0_VDS_CTL1, tmp);
			/* p_reg->ver_down_sample1.dwval = tmp; */
		} else {
			*midh = inh;
		}
		break;
	}
	default:
		if (inw >= (outw << 3)) {
			*midw = outw << 3;
			tmp = ((*midw) << 16) | inw;
			g2d_write(g2d, V0_HDS_CTL0, tmp);
			/* p_reg->hor_down_sample0.dwval = tmp; */
			g2d_write(g2d, V0_HDS_CTL1, tmp);
			/* p_reg->hor_down_sample1.dwval = tmp; */
		} else {
			*midw = inw;
		}
		if (inh >= (outh << 2)) {
			*midh = (outh << 2);
			tmp = ((*midh) << 16) | inh;
			g2d_write(g2d, V0_VDS_CTL0, tmp);
			/* p_reg->ver_down_sample0.dwval = tmp; */
			g2d_write(g2d, V0_VDS_CTL1, tmp);
			/* p_reg->ver_down_sample1.dwval = tmp; */
		} else {
			*midh = inh;
		}
		break;
	}

  return 0;
}

void g2d_bld_out_setting(struct sunxi_g2d *g2d, struct g2d_frame *frm)
{
	uint32_t tmp;
	if (frm->premult_alpha)
		g2d_set_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);
	else
		g2d_clr_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);

  tmp = FIELD_PREP(BLD_OUT_SIZE_WIDTH, (frm->sel.r.width == 0 ? 0 : frm->sel.r.width - 1));
  tmp |= FIELD_PREP(BLD_OUT_SIZE_HEIGHT, (frm->sel.r.height == 0 ? 0 : frm->sel.r.height - 1));
	g2d_write(g2d, BLD_OUT_SIZE, tmp);
	/* p_reg->out_size.bits.width = */
	/*     p_image->clip_rect.w == 0 ? 0 : p_image->clip_rect.w - 1; */
	/* p_reg->out_size.bits.height = */
	/*     p_image->clip_rect.h == 0 ? 0 : p_image->clip_rect.h - 1; */
}

void g2d_bitblt(struct sunxi_g2d_ctx *ctx, dma_addr_t src_addr[3], dma_addr_t dst_addr[3], enum g2d_blt_logicop logicop)
{
	struct sunxi_g2d *g2d = ctx->g2d;
	G2D_DEBUG_MSG(g2d, "g2d_bitblt\n");
	uint32_t src_fmt_hw_id;
	uint32_t dst_fmt_hw_id;
	uint32_t midw;
	uint32_t midh;
  uint32_t tmp;

	src_fmt_hw_id = v4l2_fmt_to_hw_id(&ctx->src.v4l2_pix_fmt);
	dst_fmt_hw_id = v4l2_fmt_to_hw_id(&ctx->dst.v4l2_pix_fmt);

	/* Maybe only reset the mixer ?? */
	// g2d_mixer_reset(ctx->g2d);
	g2d_hw_reset(ctx->g2d);

	if (logicop == G2D_BLT_NONE) {
		g2d_vlayer_set(ctx->g2d, &ctx->src, src_addr, ctx->rectfill_color_alpha);

		if (ctx->src.alpha_bld_mode != G2D_PIXEL_ALPHA) {
			g2d_uilayer_set(ctx->g2d, &ctx->dst, dst_addr, 2, ctx->rectfill_color_alpha);
		}

		if (src_fmt_hw_id >= G2D_FORMAT_YUV422UVC_V1U1V0U0 || (ctx->src.sel.r.width != ctx->dst.sel.r.width) || (ctx->src.sel.r.height != ctx->dst.sel.r.height)) {
			g2d_ovl_v_calc_coarse(ctx->g2d, src_fmt_hw_id,
                            ctx->src.sel.r.width, ctx->src.sel.r.height,
                            ctx->dst.sel.r.width, ctx->dst.sel.r.height,
                            &midw, &midh);
			g2d_scaler_params_set(ctx->g2d, src_fmt_hw_id, midw, midh,
                            ctx->dst.sel.r.width, ctx->dst.sel.r.height, 0xff);
			/* if ((src->format >= G2D_FORMAT_IYUV422_V0Y1U0Y0) || */
			/*		 (src->clip_rect.w != dst->clip_rect.w) || */
			/*		 (src->clip_rect.h != dst->clip_rect.h)) { */
			/*	 g2d_ovl_v_calc_coarse( */
			/*		 p_frame->ovl_v, src->format, src->clip_rect.w, */
			/*		 src->clip_rect.h, dst->clip_rect.w, */
			/*		 dst->clip_rect.h, &midw, &midh); */
			/*	 g2d_vsu_para_set(p_frame->scal, src->format, midw, midh, */
			/*										dst->clip_rect.w, dst->clip_rect.h, */
			/*										0xff); */
			/* } */
		}

		g2d_porter_duff(ctx->g2d, G2D_PDM_SRCOVER);
		/* bld_porter_duff(p_frame->bld, G2D_BLD_SRCOVER); */

		/*Default value*/
		g2d_write(ctx->g2d, ROP_CTL, ROP_CTL_BLUE_BYPASS_EN
							| ROP_CTL_GREEN_BYPASS_EN
							| ROP_CTL_RED_BYPASS_EN
							| ROP_CTL_ALPHA_BYPASS_EN);
		/* bld_set_rop_ctrl(p_frame->bld, 0xf0); */



		g2d_bldin_set(ctx->g2d, &ctx->dst, 0);
		/* rect0.x = 0; */
		/* rect0.y = 0; */
		/* rect0.w = dst->clip_rect.w; */
		/* rect0.h = dst->clip_rect.h; */
		/* bld_in_set(p_frame->bld, 0, rect0, dst->bpremul); */

		g2d_bld_cs_set(ctx->g2d, &ctx->src);
		/* bld_cs_set(p_frame->bld, src->format); */

		if (ctx->src.alpha_bld_mode != G2D_PIXEL_ALPHA) {
			g2d_bldin_set(ctx->g2d, &ctx->dst, 1);
		}
		/* if (src->mode) { */
		/* 	/\* need abp process *\/ */
		/* 	rect1.x = 0; */
		/* 	rect1.y = 0; */
		/* 	rect1.w = dst->clip_rect.w; */
		/* 	rect1.h = dst->clip_rect.h; */
		/* 	bld_in_set(p_frame->bld, 1, rect1, dst->bpremul); */
		/* } */

		//TODO: Make color space conversion code
		if ((src_fmt_hw_id <= G2D_FORMAT_BGRA1010102) &&
				(dst_fmt_hw_id > G2D_FORMAT_BGRA1010102)) {
			G2D_ERR_MSG(g2d, "Colorspace conversion is not supported yet\n");
			return;
		//	if (ctx->dst->gamut == G2D_BT601) {
		//		...
		//	} else {
		//		...
		//	}
		}
		if ((src_fmt_hw_id > G2D_FORMAT_BGRA1010102) &&
				(dst_fmt_hw_id <= G2D_FORMAT_BGRA1010102)) {
			G2D_ERR_MSG(g2d, "Colorspace conversion is not supported yet\n");
			return;
		//	if (ctx->dst->gamut == G2D_BT601) {
		//
		//	}
		}

    g2d_bld_out_setting(ctx->g2d, &ctx->dst);
		/* bld_out_setting(p_frame->bld, dst); */

		g2d_wb_set(ctx->g2d, &ctx->dst, dst_addr);
		/* g2d_wb_set(p_frame->wb, dst); */
	} else {
		if ((src_fmt_hw_id > G2D_FORMAT_BGRA1010102) |
				(dst_fmt_hw_id > G2D_FORMAT_BGRA1010102)) {
			G2D_ERR_MSG(g2d, "Only support rgb format!\n");
			return;
		}
		g2d_uilayer_set(ctx->g2d, &ctx->dst, dst_addr, 0, ctx->rectfill_color_alpha);
		/* g2d_uilayer_set(p_frame->ovl_u, 0, dst); */

		g2d_vlayer_set(ctx->g2d, &ctx->src, src_addr, ctx->rectfill_color_alpha);
		/* g2d_vlayer_set(p_frame->ovl_v, 0, src); */

		/* bpre = false; */
		/* if (src->bpremul || dst->bpremul) */
		/* 	bpre = true; */

    if ((ctx->src.sel.r.width != ctx->dst.sel.r.width) || (ctx->src.sel.r.height != ctx->dst.sel.r.height)) {
			g2d_ovl_v_calc_coarse(ctx->g2d, src_fmt_hw_id,
                            ctx->src.sel.r.width, ctx->src.sel.r.height,
                            ctx->dst.sel.r.width, ctx->dst.sel.r.height,
                            &midw, &midh);
			g2d_scaler_params_set(ctx->g2d, src_fmt_hw_id, midw, midh,
                            ctx->dst.sel.r.width, ctx->dst.sel.r.height, 0xff);
		}
		/* if ((src->clip_rect.w != dst->clip_rect.w) */
		/*     || (src->clip_rect.h != dst->clip_rect.h)) { */
		/* 	g2d_ovl_v_calc_coarse( */
		/* 	    p_frame->ovl_v, src->format, src->clip_rect.w, */
		/* 	    src->clip_rect.h, dst->clip_rect.w, */
		/* 	    dst->clip_rect.h, &midw, &midh); */
		/* 	g2d_vsu_para_set(p_frame->scal, src->format, midw, midh, */
		/* 			 dst->clip_rect.w, dst->clip_rect.h, */
		/* 			 0xff); */
		/* } */

		/*Default value*/
		g2d_porter_duff(ctx->g2d, G2D_PDM_SRCOVER);
		/* bld_porter_duff(p_frame->bld, G2D_BLD_SRCOVER); */

		g2d_write(ctx->g2d, ROP_CTL, 0);
		tmp = logic_op_to_bld[G2D_BLT_MASKPEN];
		g2d_write(ctx->g2d, ROP_INDEX0, tmp);
		/* bld_set_rop_ctrl(p_frame->bld, 0x00); */

		tmp = logic_op_to_bld[logicop];
		tmp |= FIELD_PREP(ROP_INDEX0_NODE0, 2);
		g2d_write(ctx->g2d, ROP_INDEX0, tmp);
		/* bld_rop2_set(p_frame->bld, flag & 0xff); */

		/*set bld para */
		g2d_bldin_set(ctx->g2d, &ctx->dst, 0);
		/* rect0.x = 0; */
		/* rect0.y = 0; */
		/* rect0.w = dst->clip_rect.w; */
		/* rect0.h = dst->clip_rect.h; */
		/* bld_in_set(p_frame->bld, 0, rect0, bpre); */

		g2d_bld_out_setting(ctx->g2d, &ctx->dst);
		/* bld_out_setting(p_frame->bld, dst); */

		g2d_wb_set(ctx->g2d, &ctx->dst, dst_addr);
		/* g2d_wb_set(p_frame->wb, dst); */
	}
	G2D_INFO_MSG(g2d, "Starting bitblt");
	g2d_mixer_irq_enable(ctx->g2d);
	g2d_set_bits(ctx->g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_START);
}
