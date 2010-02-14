/*
 * Copyright (C) 2010 Ben Collins <bcollins@bluecherry.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/videodev2.h>
#include <media/v4l2-ioctl.h>

#include "solo6010.h"

static void solo_vin_config(struct solo6010_dev *solo_dev)
{
	solo_dev->vin_hstart = 8;
	solo_dev->vin_vstart = 2;

	solo_reg_write(solo_dev, SOLO_SYS_VCLK,
		       SOLO_VCLK_SELECT(2) |
		       SOLO_VCLK_VIN1415_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN1213_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN1011_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0809_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0607_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0405_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0203_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0001_DELAY(SOLO_VCLK_DELAY));

	solo_reg_write(solo_dev, SOLO_VI_ACT_I_P,
		       SOLO_VI_H_START(solo_dev->vin_hstart) |
		       SOLO_VI_V_START(solo_dev->vin_vstart) |
		       SOLO_VI_V_STOP(solo_dev->vin_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_ACT_I_S,
		       SOLO_VI_H_START(solo_dev->vout_hstart) |
		       SOLO_VI_V_START(solo_dev->vout_vstart) |
		       SOLO_VI_V_STOP(solo_dev->vout_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_ACT_P,
		       SOLO_VI_H_START(0) |
		       SOLO_VI_V_START(1) |
		       SOLO_VI_V_STOP(SOLO_PROGRESSIVE_VSIZE));

	solo_reg_write(solo_dev, SOLO_VI_CH_FORMAT,
		       SOLO_VI_FD_SEL_MASK(0) | SOLO_VI_PROG_MASK(0));

	/* XXX: Use this for stable check? */
	solo_reg_write(solo_dev, SOLO_VI_FMT_CFG, 0);
	solo_reg_write(solo_dev, SOLO_VI_CH_ENA, 0xffff);
	solo_reg_write(solo_dev, SOLO_VI_PAGE_SW, 2);

	if (solo_dev->video_type == 0) {
		solo_reg_write(solo_dev, SOLO_VI_PB_CONFIG,
			       SOLO_VI_PB_USER_MODE);
		solo_reg_write(solo_dev, SOLO_VI_PB_RANGE_HV,
			       SOLO_VI_PB_HSIZE(858) | SOLO_VI_PB_VSIZE(246));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_H,
			       SOLO_VI_PB_HSTART(16) |
			       SOLO_VI_PB_HSTOP(16 + 720));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_V,
			       SOLO_VI_PB_VSTART(4) |
			       SOLO_VI_PB_VSTOP(4 + 240));
	} else {
		solo_reg_write(solo_dev, SOLO_VI_PB_CONFIG,
			       SOLO_VI_PB_USER_MODE | SOLO_VI_PB_PAL);
		solo_reg_write(solo_dev, SOLO_VI_PB_RANGE_HV,
			       SOLO_VI_PB_HSIZE(864) | SOLO_VI_PB_VSIZE(294));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_H,
			       SOLO_VI_PB_HSTART(16) |
			       SOLO_VI_PB_HSTOP(16 + 720));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_V,
			       SOLO_VI_PB_VSTART(4) |
			       SOLO_VI_PB_VSTOP(4 + 288));
	}

}

static void solo_disp_config(struct solo6010_dev *solo_dev)
{
	int i;

	solo_dev->vout_hstart = 6;
	solo_dev->vout_vstart = 8;

	solo_reg_write(solo_dev, SOLO_VO_BORDER_LINE_COLOR,
		       (0xa0 << 24) | (0x88 << 16) | (0xa0 << 8) | 0x88);
	solo_reg_write(solo_dev, SOLO_VO_BORDER_FILL_COLOR,
		       (0x10 << 24) | (0x8f << 16) | (0x10 << 8) | 0x8f);
	solo_reg_write(solo_dev, SOLO_VO_BKG_COLOR,
		       (16 << 24) | (128 << 16) | (16 << 8) | 128);
		       //SOLO_VO_BG_YUV(16, 128, 128));

	solo_reg_write(solo_dev, SOLO_VO_FMT_ENC,
		       solo_dev->video_type |
		       SOLO_VO_USER_COLOR_SET_NAV |
		       SOLO_VO_NA_COLOR_Y(0) |
		       SOLO_VO_NA_COLOR_CB(0) |
		       SOLO_VO_NA_COLOR_CR(0));

	solo_reg_write(solo_dev, SOLO_VO_ACT_H,
		       SOLO_VO_H_START(solo_dev->vout_hstart) |
		       SOLO_VO_H_STOP(solo_dev->vout_hstart +
				      solo_dev->video_hsize));

	solo_reg_write(solo_dev, SOLO_VO_ACT_V,
		       SOLO_VO_V_START(solo_dev->vout_vstart) |
		       SOLO_VO_V_STOP(solo_dev->vout_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VO_RANGE_HV,
		       SOLO_VO_H_LEN(solo_dev->video_hsize) |
		       SOLO_VO_V_LEN(solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_WIN_SW, 5);

	solo_reg_write(solo_dev, SOLO_VO_DISP_CTRL, SOLO_VO_DISP_ON |
		       SOLO_VO_DISP_ERASE_COUNT(8) |
		       SOLO_VO_DISP_BASE(SOLO_DISP_EXT_ADDR(solo_dev)));

	solo_reg_write(solo_dev, SOLO_VO_DISP_ERASE, SOLO_VO_DISP_ERASE_ON);

	/* Mute channels we aren't supporting */
	for (i = solo_dev->nr_chans; i < 16; i++) {
		int val = ((~(1 << i) & 0xffff) &
			solo_reg_read(solo_dev, SOLO_VI_CH_ENA));
		solo_reg_write(solo_dev, SOLO_VI_CH_ENA, val);
	}

	/* Disable the watchdog */
	solo_reg_write(solo_dev, SOLO_WATCHDOG, 0);

	/* Test signal - BENC */
	//solo_reg_write(solo_dev, SOLO_VI_FMT_CFG, SOLO_VI_FMT_TEST_SIGNAL);
}

int solo_disp_init(struct solo6010_dev *solo_dev)
{
	/* Start out with NTSC */
	solo_dev->video_type = SOLO_VO_FMT_TYPE_NTSC;
	solo_dev->video_hsize = 704;
	solo_dev->video_vsize = 240;

	solo_vin_config(solo_dev);
	solo_disp_config(solo_dev);

	return 0;
}

void solo_disp_exit(struct solo6010_dev *solo_dev)
{
#if 0
	int i;

	solo_reg_write(solo_dev, SOLO_VO_DISP_CTRL, 0);
	solo_reg_write(solo_dev, SOLO_VO_ZOOM_CTRL, 0);
	solo_reg_write(solo_dev, SOLO_VO_FREEZE_CTRL, 0);

	for (i = 0; i < solo6010->max_channel; i++) {
		solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL0(i), 0);
		solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL1(i), 0);
		solo_reg_write(solo_dev, SOLO_VI_WIN_ON(i), 0);
	}

	/* Set default border */
	for (i = 0; i < 5; i++)
		solo_reg_write(solo_dev, SOLO_VO_BORDER_X(i), 0);

	for (i = 0; i < 5; i++)
		solo_reg_write(solo_dev, SOLO_VO_BORDER_Y(i), 0);

	solo_reg_write(solo_dev, SOLO_VO_BORDER_LINE_MASK, 0);
	solo_reg_write(solo6010, SOLO_VO_BORDER_FILL_MASK, 0);

	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_CTRL(0), 0);
	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_START(0), 0);
	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_STOP(0), 0);
	
	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_CTRL(1), 0);
	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_START(1), 0);
	solo_reg_write(solo_dev, SOLO_VO_RECTANGLE_STOP(1), 0);
#endif
}
