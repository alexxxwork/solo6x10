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

#include "solo6010.h"
#include "solo6010-tw28.h"

static u8 tbl_tw2864_template[] = {
	0x00, 0xd4, 0x70, 0x30, 0x80, 0x80, 0x00, 0x02, // 0x00
	0x12, 0xf5, 0x09, 0xd0, 0x00, 0x00, 0x00, 0x7f,
	0x00, 0xd4, 0x70, 0x30, 0x80, 0x80, 0x00, 0x02, // 0x10
	0x12, 0xf5, 0x09, 0xd0, 0x00, 0x00, 0x00, 0x7f,
	0x00, 0xd4, 0x70, 0x30, 0x80, 0x80, 0x00, 0x02, // 0x20
	0x12, 0xf5, 0x09, 0xd0, 0x00, 0x00, 0x00, 0x7f,
	0x00, 0xd4, 0x70, 0x30, 0x80, 0x80, 0x00, 0x02, // 0x30
	0x12, 0xf5, 0x09, 0xd0, 0x00, 0x00, 0x00, 0x7f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x40
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x50
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x60
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x70
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA3, 0x00,
	0x00, 0x02, 0x00, 0xcc, 0x00, 0x80, 0x44, 0x50, // 0x80
	0x22, 0x01, 0xd8, 0xbc, 0xb8, 0x44, 0x38, 0x00,
	0x00, 0x78, 0x72, 0x3e, 0x14, 0xa5, 0xe4, 0x05, // 0x90
	0x00, 0x28, 0x44, 0x44, 0xa0, 0x88, 0x5a, 0x01,
	0x08, 0x08, 0x08, 0x08, 0x1a, 0x1a, 0x1a, 0x1a, // 0xa0
	0x00, 0x00, 0x00, 0xf0, 0xf0, 0xf0, 0xf0, 0x44,
	0x44, 0x0a, 0x00, 0xff, 0xef, 0xef, 0xef, 0xef, // 0xb0
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xc0
	0x00, 0x00, 0x55, 0x00, 0xb1, 0xe4, 0x40, 0x00,
	0x77, 0x77, 0x01, 0x13, 0x57, 0x9b, 0xdf, 0x20, // 0xd0
	0x64, 0xa8, 0xec, 0xd1, 0x0f, 0x11, 0x11, 0x81,
	0x10, 0xe0, 0xbb, 0xbb, 0x00, 0x11, 0x00, 0x00, // 0xe0
	0x11, 0x00, 0x00, 0x11, 0x00, 0x00, 0x11, 0x00,
	0x83, 0xb5, 0x09, 0x78, 0x85, 0x00, 0x01, 0x20, // 0xf0
	0x64, 0x11, 0x40, 0xaf, 0xff, 0x00, 0x00, 0x00,
};

static u8 tbl_saa7128_template[128] = {
	0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, // 0x00
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0x10
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x1C, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, // 0x20
	0x59, 0x1d, 0x75, 0x3f, 0x06, 0x3f, 0x00, 0x00,
	0x1c, 0x33, 0x00, 0x3f, 0x00, 0x00, 0x3f, 0x00, // 0x30
	0x1a, 0x1a, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x68, 0x10, 0x97, 0x4c, 0x18, // 0x40
	0x9b, 0x93, 0x9f, 0xff, 0x7c, 0x34, 0x3f, 0x3f,
	0x3f, 0x83, 0x83, 0x80, 0x0d, 0x0f, 0xc3, 0x06, // 0x50
	0x02, 0x80, 0x71, 0x77, 0xa7, 0x67, 0x66, 0x2e,
	0x7b, 0x11, 0x4f, 0x1f, 0x7c, 0xf0, 0x21, 0x77, // 0x60
	0x41, 0x88, 0x41, 0x12, 0xed, 0x10, 0x10, 0x00,
	0x41, 0xc3, 0x00, 0x3e, 0xb8, 0x02, 0x00, 0x00, // 0x70
	0x00, 0x00, 0x08, 0x02, 0xc0, 0x00, 0xff, 0xff,
};

static u8 tbl_ntsc_tw2815_common[] = {
	0x00, 0xc8, 0x20, 0xd0, 0x06, 0xf0, 0x08, 0x80,
	0x80, 0x80, 0x80, 0x02, 0x06, 0x24, 0x11,
};

static u8 tbl_pal_tw2815_common[] = {
	0x00, 0x88, 0x20, 0xd0, 0x05, 0x20, 0x28, 0x80,
	0x80, 0x80, 0x80, 0x82, 0x06, 0x24, 0x11,
};

#define is_tw286x(__solo, __id) (!(__solo->tw2815 & (1 << __id)))

static u8 tw_readbyte(struct solo6010_dev *solo_dev, int chip_id, u8 tw68x_off,
		      u8 tw_off)
{
	if (is_tw286x(solo_dev, chip_id))
		return solo_i2c_readbyte(solo_dev, SOLO_I2C_TW,
					 TW_CHIP_OFFSET_ADDR(chip_id),
					 tw68x_off);
	else
		return solo_i2c_readbyte(solo_dev, SOLO_I2C_TW,
					 TW_CHIP_OFFSET_ADDR(chip_id),
					 tw_off);
}

static void tw_writebyte(struct solo6010_dev *solo_dev, int chip_id,
			 u8 tw68x_off, u8 tw_off, u8 val)
{
	if (is_tw286x(solo_dev, chip_id))
		solo_i2c_writebyte(solo_dev, SOLO_I2C_TW,
				   TW_CHIP_OFFSET_ADDR(chip_id),
				   tw68x_off, val);
	else
		solo_i2c_writebyte(solo_dev, SOLO_I2C_TW,
				   TW_CHIP_OFFSET_ADDR(chip_id),
				   tw_off, val);
}

static int tw2864_setup(struct solo6010_dev *solo_dev, u8 dev_addr)
{
	u8 *tbl_tw2864_common = tbl_tw2864_template;
	int i;

	/* IRQ Mode */
	if (solo_dev->nr_chans == 4) {
		tbl_tw2864_common[0xd2] = 0x01;
		tbl_tw2864_common[0xcf] = 0x00;
	} else if (solo_dev->nr_chans == 8) {
		tbl_tw2864_common[0xd2] = 0x02;
		if (dev_addr == TW_CHIP_OFFSET_ADDR(0))
			tbl_tw2864_common[0xcf] = 0x43;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(1))
			tbl_tw2864_common[0xcf] = 0x40;
	} else if (solo_dev->nr_chans == 16) {
		tbl_tw2864_common[0xd2] = 0x03;
		if (dev_addr == TW_CHIP_OFFSET_ADDR(0))
			tbl_tw2864_common[0xcf] = 0x43;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(1))
			tbl_tw2864_common[0xcf] = 0x43;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(2))
			tbl_tw2864_common[0xcf] = 0x43;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(3))
			tbl_tw2864_common[0xcf] = 0x40;
	}

	/* PAL */
	if (solo_dev->video_type == SOLO_VO_FMT_TYPE_PAL) {
		for (i = 0; i < 4; i++) {
			tbl_tw2864_common[0x07 | (i << 4)] |= 0x10;
			tbl_tw2864_common[0x08 | (i << 4)] |= 0x06;
			tbl_tw2864_common[0x0a | (i << 4)] |= 0x08;
			tbl_tw2864_common[0x0b | (i << 4)] |= 0x13;
			tbl_tw2864_common[0x0e | (i << 4)] |= 0x01;
		}
		tbl_tw2864_common[0x9d] = 0x90;
		tbl_tw2864_common[0xf3] = 0x00;
		tbl_tw2864_common[0xf4] = 0xa0;
	}

	for (i = 0; i < 0xff; i++) {
		/* Skip read only registers */
		if (i >= 0xb8 && i <= 0xc1 )
			continue;
		if ((i & ~0x30) == 0x00 ||
		    (i & ~0x30) == 0x0c ||
		    (i & ~0x30) == 0x0d)
			continue;
		if (i == 0x74 || i == 0x77 || i == 0x78 ||
		    i == 0x79 || i == 0x7a)
			continue;
                if (i == 0xfd)
                        continue;

		solo_i2c_writebyte(solo_dev, SOLO_I2C_TW, dev_addr, i,
				   tbl_tw2864_common[i]);
	}

	return 0;
}

static int tw2815_setup(struct solo6010_dev *solo_dev, u8 dev_addr)
{
	u8 tbl_tw2815_sfr[] = {
		0x00, 0x00, 0x00, 0xc0, 0x45, 0xa0, 0xd0, 0x2f, // 0x40
		0x64, 0x80, 0x80, 0x82, 0x82, 0x04, 0x00, 0x00,
		0x00, 0x0f, 0x05, 0x00, 0x00, 0x80, 0x06, 0x00, // 0x50
		0x00, 0x00, 0x00, 0xff, 0x9f, 0x00, 0x00, 0x00,
		0x88, 0x88, 0x40, 0x00, 0x20, 0x64, 0xa8, 0xec, // 0x60
		0x31, 0x75, 0xb9, 0xfd, 0x10, 0x0f, 0x88, 0x88,
		0xff, 0x11, 0x00, 0x88, 0x88, 0x00,		// 0x70
	};
	u8 *tbl_tw2815_common;
	int i;
	int ch;

	if (solo_dev->video_type == SOLO_VO_FMT_TYPE_NTSC)
		tbl_tw2815_common = tbl_ntsc_tw2815_common;
	else
		tbl_tw2815_common = tbl_pal_tw2815_common;

	/* Audio configuration */
	if (solo_dev->nr_chans == 4) {
		tbl_tw2815_sfr[0x63 - 0x40] |= 1;
		tbl_tw2815_sfr[0x62 - 0x40] |= 3 << 6;
	} else if (solo_dev->nr_chans == 8) {
		tbl_tw2815_sfr[0x63 - 0x40] |= 2;
		if (dev_addr == TW_CHIP_OFFSET_ADDR(0))
			tbl_tw2815_sfr[0x62 - 0x40] |= 1 << 6;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(1))
			tbl_tw2815_sfr[0x62 - 0x40] |= 2 << 6;
	} else if (solo_dev->nr_chans == 16) {
		tbl_tw2815_sfr[0x63 - 0x40] |= 3;
		if (dev_addr == TW_CHIP_OFFSET_ADDR(0))
			tbl_tw2815_sfr[0x62 - 0x40] |= 1 << 6;
		else if (dev_addr == TW_CHIP_OFFSET_ADDR(3))
			tbl_tw2815_sfr[0x62 - 0x40] |= 2 << 6;
	}

	for (ch = 0; ch < 4; ch++) {
		tbl_tw2815_common[0x0d] &= ~3;
		switch (ch) {
		case 0:
			tbl_tw2815_common[0x0d] |= 0x01;
			break;
		case 1:
			tbl_tw2815_common[0x0d] |= 0x00;
			break;
		case 2:
			tbl_tw2815_common[0x0d] |= 0x03;
			break;
		case 3:
			tbl_tw2815_common[0x0d] |= 0x02;
			break;
		}

		for (i = 0x01; i < 0x0f; i++) {
			solo_i2c_writebyte(solo_dev, SOLO_I2C_TW,
					   dev_addr, (ch * 0x10) + i,
					   tbl_tw2815_common[i]);
		}
	}

	for (i = 0x41; i < 0x76; i++) {
		/* Skip read-only and nop registers */
		if (i == 0x59 || i == 0x5a ||
		    i == 0x5d || i == 0x5e || i == 0x5f)
			continue;

		solo_i2c_writebyte(solo_dev, SOLO_I2C_TW, dev_addr, i,
				       tbl_tw2815_sfr[i - 0x40]);
	}

	return 0;
}

static void saa7128_setup(struct solo6010_dev *solo_dev)
{
	int i;
	u8 *regs = tbl_saa7128_template;

	/* PAL: We could do a second set of regs to avoid this */
	if (solo_dev->video_type == SOLO_VO_FMT_TYPE_PAL) {
		regs[0x28] = 0xE1;

		regs[0x5A] = 0x0F;
		regs[0x61] = 0x02;
		regs[0x62] = 0x35;
		regs[0x63] = 0xCB;
		regs[0x64] = 0x8A;
		regs[0x65] = 0x09;
		regs[0x66] = 0x2A;

		regs[0x6C] = 0xf1;
		regs[0x6E] = 0x20;

		regs[0x7A] = 0x06 + 12;
		regs[0x7b] = 0x24 + 12;
		regs[0x7c] |= 1 << 6;
	}

	/* 0x00-0x25 are RO? */
	for (i = 0x26; i < 128; i++) {
		if (i == 0x60 || i == 0x7D)
			continue;
		solo_i2c_writebyte(solo_dev, SOLO_I2C_SAA, 0x46, i, regs[i]);
	}

	return;
}

int solo_tw28_init(struct solo6010_dev *solo_dev)
{
	int i;
	u8 value;

	/* Detect techwell chip type */
	for (i = 0; i < TW_NUM_CHIP; i++) {
		value = solo_i2c_readbyte(solo_dev, SOLO_I2C_TW,
					  TW_CHIP_OFFSET_ADDR(i), 0xFF);

		switch (value >> 3) {
		case 0x0c:
			solo_dev->tw2864 |= 1 << i;
			solo_dev->tw28_cnt++;
			break;
		default:
			value = solo_i2c_readbyte(solo_dev, SOLO_I2C_TW,
						  TW_CHIP_OFFSET_ADDR(i), 0x59);
			/* Unknown chip */
			if ((value >> 3) != 0x04) {
				printk("solo6010: Unknown techwell chipset\n");
				return -EINVAL;
			}
			solo_dev->tw2815 |= 1 << i;
			solo_dev->tw28_cnt++;
		}
	}

	if (!solo_dev->tw28_cnt)
		return -EINVAL;

	saa7128_setup(solo_dev);

	for (i = 0; i < solo_dev->tw28_cnt; i++) {
		if ((solo_dev->tw2864 & (1 << i)))
			tw2864_setup(solo_dev, TW_CHIP_OFFSET_ADDR(i));
		else
			tw2815_setup(solo_dev, TW_CHIP_OFFSET_ADDR(i));
	}

	dev_info(&solo_dev->pdev->dev, "Initialized %d tw28xx chips:",
		 solo_dev->tw28_cnt);

	if (solo_dev->tw2864)
		printk(" tw2864[%d]", hweight32(solo_dev->tw2864));
	if (solo_dev->tw2815)
		printk(" tw2815[%d]", hweight32(solo_dev->tw2815));
	printk("\n");

	return 0;
}

#if 0
/* 
 * XXX: We accessed the video status signal in the Techwell chip through
 * iic/i2c because the video status reported by register REG_VI_STATUS1
 * (address 0x012C) of the SOLO6010 chip doesn't give the correct video
 * status signal values. But take note that there are 4 techwell chip in
 * solo6010. Each techwell chip handles 4 video channels. The base address
 * of the techwell chip is 0x28. We can access the 4 techwell chip through
 * the base address and an offset starting from 0 (i.e. 0-3 to access all 4,
 * that is 0x28+0, 0x28+1, ox28+2, 0x28+3). The 4-channel video status we
 * read in each techwell chip is set on the lower 4-bits. Also bit 0 means
 * signal loss for the corresponding channel, bit 1 means signal gain.
 */
u16 tw28_get_video_status(struct solo6010_dev *solo_dev)
{
	un16 status = 0;
	u8 val;
	int i;

	/* Read the video signal status for the channels in the
         * techwell chip(s) */
	for (i = 0; i < solo_dev->tw28_cnt; i++) {
		val = tw_readbyte(solo_dev, i, TW286X_AV_STAT_ADDR,
				   TW_AV_STAT_ADDR) & 0x0f;
		status |= val << (i * 4);
	}

	return status;
}

/* Status of audio from up to 4 techwell chips are combined into 1 variable.
 * See techwell datasheet for details. */
u16 tw28_get_audio_status(struct solo6010_dev *solo_dev)
{
	u8 val;
	u16 status = 0;
	int i;

	for (i = 0; i < solo_dev->tw28_cnt; i++) {
		val = (tw_readbyte(solo_dev, i, TW286X_AV_STAT_ADDR,
				   TW_AV_STAT_ADDR) & 0xf0) >> 4;
		status |= val << (i * 4);
	}

	return status;
}
#endif
int tw28_set_ctrl_val(struct solo6010_dev *solo_dev, u32 ctrl, u8 ch,
		      __s32 val)
{
	int ret = 0;
	/* Get the right chip and on-chip channel */
	u8 chip_num = ch / 4;

	ch %= 4;

	if (val > 255 || val < 0)
		return -ERANGE;

	switch (ctrl) {
	case V4L2_CID_HUE:
		if (is_tw286x(solo_dev, chip_num)) {
			if (val >= 125)
				val -= 125;
			else
				val += 125;
		}
		tw_writebyte(solo_dev, chip_num, TW286x_HUE_ADDR(ch),
			     TW_HUE_ADDR(ch), val);

		break;

	case V4L2_CID_SATURATION:
		if (is_tw286x(solo_dev, chip_num)) {
			solo_i2c_writebyte(solo_dev, SOLO_I2C_TW,
					   TW_CHIP_OFFSET_ADDR(chip_num),
					   TW286x_SATURATIONU_ADDR(ch), val);
		}
		tw_writebyte(solo_dev, chip_num, TW286x_SATURATIONV_ADDR(ch),
			     TW_SATURATION_ADDR(ch), val);

		break;

	case V4L2_CID_CONTRAST:
		tw_writebyte(solo_dev, chip_num, TW286x_CONTRAST_ADDR(ch),
			     TW_CONTRAST_ADDR(ch), val);
		break;

	case V4L2_CID_BRIGHTNESS:
		if (is_tw286x(solo_dev, chip_num)) {
			if (val == 125 || !val)
				val = 212;
			else if (val > 125)
				val -= 125;
			else
				val += 125;
		}
		tw_writebyte(solo_dev, chip_num, TW286x_BRIGHTNESS_ADDR(ch),
			     TW_BRIGHTNESS_ADDR(ch), val);

		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

int tw28_get_ctrl_val(struct solo6010_dev *solo_dev, u32 ctrl, u8 ch,
		      __s32 *val)
{
	int ret = 0;
	/* Get the right chip and on-chip channel */
	u8 chip_num = ch / 4;

	ch %= 4;

	switch (ctrl) {
	case V4L2_CID_HUE:
		*val = tw_readbyte(solo_dev, chip_num, TW286x_HUE_ADDR(ch),
				   TW_HUE_ADDR(ch));
		break;
	case V4L2_CID_SATURATION:
		*val = tw_readbyte(solo_dev, chip_num,
				   TW286x_SATURATIONU_ADDR(ch),
				   TW_SATURATION_ADDR(ch));
		break;
	case V4L2_CID_CONTRAST:
		*val = tw_readbyte(solo_dev, chip_num,
				   TW286x_CONTRAST_ADDR(ch),
				   TW_CONTRAST_ADDR(ch));
		break;
	case V4L2_CID_BRIGHTNESS:
		*val = tw_readbyte(solo_dev, chip_num,
				   TW286x_BRIGHTNESS_ADDR(ch),
				   TW_BRIGHTNESS_ADDR(ch));
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
#if 0
/*
 * For audio output volume, the output channel is only 1. In this case we
 * don't need to offset TW_CHIP_OFFSET_ADDR. The TW_CHIP_OFFSET_ADDR used
 * is the base address of the techwell chip.
 */
void tw2815_Set_AudioOutVol(struct solo6010_dev *solo_dev, unsigned int u_val)
{
	unsigned int val;
	unsigned int chip_num;

	chip_num = (solo_dev->nr_chans - 1) / 4;

	val = tw_readbyte(solo_dev, chip_num, TW286x_AUDIO_OUTPUT_VOL_ADDR,
			  TW_AUDIO_OUTPUT_VOL_ADDR);

	u_val = (val & 0x0f) | (u_val << 4);

	tw_writebyte(solo_dev, chip_num, TW286x_AUDIO_OUTPUT_VOL_ADDR,
		     TW_AUDIO_OUTPUT_VOL_ADDR, u_val);
}

void tw2815_Set_AudioInGain(struct solo6010_dev *solo_dev, unsigned int channel,
			    unsigned int u_val)
{
	unsigned int val;
	unsigned int chip_num;

	/* Determine which techwell chip number this channel belongs to */
	chip_num = channel / 4;

	/* Determine the corresponding channel for this techwell chip. We
	 * use 1-based notation so we won't be confuse on the techwell data
	 * sheet */
	channel = (channel % 4) + 1;

	val = tw_readbyte(solo_dev, chip_num,
			  TW286x_AUDIO_INPUT_GAIN_ADDR(channel),
			  TW_AUDIO_INPUT_GAIN_ADDR(channel));

	u_val = (val & ((channel % 2 == 1) ? 0xf0 : 0x0f)) |
		((channel % 2 == 1) ? u_val : u_val << 4);

	tw_writebyte(solo_dev, chip_num, TW286x_AUDIO_INPUT_GAIN_ADDR(channel),
		     TW_AUDIO_INPUT_GAIN_ADDR(channel), u_val);
}
#endif
