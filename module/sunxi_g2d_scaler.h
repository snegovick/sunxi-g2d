#ifndef _SUNXI_G2D_SCALER_H_
#define _SUNXI_G2D_SCALER_H_

#include <linux/types.h>

#include "sunxi_g2d.h"

void g2d_scaler_params_set(struct sunxi_g2d *g2d, enum g2d_fmt_hw_id format, uint32_t inw, uint32_t inh,
                           uint32_t outw, uint32_t outh, uint8_t alpha);

#endif
