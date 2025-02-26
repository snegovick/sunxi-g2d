/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner G2D - 2D Graphics Accelerator driver
 *
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2024 Brandon Cheo Fusi <fusibrandon13@gmail.com>
 */

#ifndef _SUNXI_G2D_H_
#define _SUNXI_G2D_H_

#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-ctrls.h>

#include <linux/platform_device.h>
#include <linux/types.h>

#define G2D_NAME "sunxi-g2d"

#define G2D_MIN_WIDTH	8U
#define G2D_MIN_HEIGHT	8U
#define G2D_MAX_WIDTH	2048U
#define G2D_MAX_HEIGHT	2048U

enum g2d_op {
	G2D_RECTFILL,
	G2D_BITBLT
};

/*
 * Blend Layer alpha modes
 * G2D_PIXEL_ALPHA: Each pixel carries its own alpha value
 * G2D_GLOBAL_ALPHA: Each layer has an alpha value shared by all pixels
 * in that layer
 * G2D_MIXER_ALPHA: All pixels in all layers share a single alpha value
 */
enum g2d_alpha_bld_mode {
	G2D_PIXEL_ALPHA,
	G2D_GLOBAL_ALPHA,
	G2D_MIXER_ALPHA,
};

enum g2d_rectfill_bld_mode {
	G2D_FILL_NONE,
	G2D_FILL_PIXEL_ALPHA,
	G2D_FILL_PLANE_ALPHA,
	G2D_FILL_MULTI_ALPHA,
};

/*
 * Porter-duff color mixing mode
 */
enum g2d_pd_mode {
	G2D_PDM_CLEAR,
	G2D_PDM_COPY,
	G2D_PDM_DST,
	G2D_PDM_SRCOVER,
	G2D_PDM_DSTOVER,
	G2D_PDM_SRCIN,
	G2D_PDM_DSTIN,
	G2D_PDM_SRCOUT,
	G2D_PDM_DSTOUT,
	G2D_PDM_SRCATOP,
	G2D_PDM_DSTATOP,
	G2D_PDM_XOR,
};

/*
 * Colorkey mode
 */
enum g2d_ck_mode {
	G2D_CK_SRC,
	G2D_CK_DST,
};

/*
 * Color gamut
 */
enum g2d_color_gmt {
	G2D_BT601,
	G2D_BT709,
	G2D_BT2020,
};

enum g2d_scaler_pixel_format {
	VSU_FORMAT_YUV422,
	VSU_FORMAT_YUV420,
	VSU_FORMAT_YUV411,
	VSU_FORMAT_RGB,
	VSU_FORMAT_BUTT,
};

/*
 * Logicops reordered to better follow opengl's logicops
 */
enum g2d_blt_logicop {
	G2D_BLT_BLACKNESS,            // all 0's
	G2D_BLT_NOTMERGEPEN,          // NOT (S OR D)
	G2D_BLT_MASKNOTPEN,           // (NOT S) AND D
	G2D_BLT_NOTCOPYPEN,           // NOT S
	G2D_BLT_MASKPENNOT,           // S AND (NOT D)
	G2D_BLT_NOT,                  // NOT D
	G2D_BLT_XORPEN,               // S XOR D
	G2D_BLT_NOTMASKPEN,           // NOT (S AND D)
	G2D_BLT_MASKPEN,              // S AND D
	G2D_BLT_NOTXORPEN,            // NOT (S XOR D)
	G2D_BLT_NOP,                  // D
	G2D_BLT_MERGENOTPEN,          // (NOT S) OR D
	G2D_BLT_COPYPEN,              // S
	G2D_BLT_MERGEPENNOT,          // S OR (NOT D)
	G2D_BLT_MERGEPEN,             // S OR D
	G2D_BLT_WHITENESS,            // all 1's
	G2D_BLT_NONE,
};

/* enum g2d_blt_rot_flags { */
/* 	G2D_ROT_90  = 0x00000100, */
/* 	G2D_ROT_180 = 0x00000200, */
/* 	G2D_ROT_270 = 0x00000300, */
/* 	G2D_ROT_0   = 0x00000400, */
/* 	G2D_ROT_H = 0x00001000, */
/* 	G2D_ROT_V = 0x00002000, */
/* }; */


/*	G2D_SM_TDLR_1  =    0x10000000, */
//G2D_SM_DTLR_1 = 0x10000000,
/*	G2D_SM_TDRL_1  =    0x20000000, */
/*	G2D_SM_DTRL_1  =    0x30000000, */


enum g2d_debug_level {
	gdl_disabled = 0,
	gdl_info,
	gdl_debug,
};

struct g2d_fmt {
	u32 fourcc;
	int depth;
	u32 hw_id;
};

struct g2d_frame {
	struct v4l2_pix_format v4l2_pix_fmt;
	bool premult_alpha;
	enum g2d_alpha_bld_mode alpha_bld_mode;
	uint32_t alignment;
	enum g2d_color_gmt gamut;
	struct v4l2_selection sel;
};

struct sunxi_g2d {
	void __iomem	*base;
	int irq;
	struct clk *mod_clk;
	struct clk *bus_clk;
	struct clk *ram_clk;
	struct reset_control *rstc;

    /* Device file mutex */
	struct mutex		dev_mutex;

	struct device		*dev;
  struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct v4l2_m2m_dev	*m2m_dev;

	struct g2d_fmt *supported_fmts;

	enum g2d_debug_level debug_level;
};

struct sunxi_g2d_ctx {
	struct v4l2_fh		fh;
	struct sunxi_g2d	*g2d;

	struct g2d_frame src;
	struct g2d_frame dst;

	/* only useful for rectfill operations */
	uint32_t rectfill_color;
	uint32_t rectfill_color_alpha;

	/* active g2d operation */
	enum g2d_op chosen_g2d_op;
	enum g2d_rectfill_bld_mode rectfill_bld_mode;

	enum g2d_blt_logicop blt_logicop;

	struct v4l2_ctrl_handler ctrl_handler;
};

struct g2d_fmt *find_fmt(struct v4l2_pix_format *);

#define G2D_INFO_MSG(g2d, ...)                                  \
	do {                                                          \
		if (g2d->debug_level >= gdl_info) {                         \
			printk("[G2D info] (%s) line:%d: ", __func__, __LINE__);  \
			printk(__VA_ARGS__);                                      \
		}                                                           \
	} while (0)

#define G2D_DEBUG_MSG(g2d, ...)                                 \
	do {                                                          \
		if (g2d->debug_level >= gdl_debug) {                        \
			printk("[G2D debug] (%s) line:%d: ", __func__, __LINE__); \
			printk(__VA_ARGS__);                                      \
		}                                                           \
	} while (0)

#define G2D_ERR_MSG(g2d, ...)                                 \
	do {                                                        \
		printk("[G2D error] (%s) line:%d: ", __func__, __LINE__); \
		printk(__VA_ARGS__);                                      \
	} while (0)

#endif
