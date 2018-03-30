/*
 * Copyright (C) 2014-2015 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __CYGNUS_SSP_H__
#define __CYGNUS_SSP_H__

#include <linux/regmap.h>
#include "iproc-pcm.h"

#define CYGNUS_TDM_DAI_MAX_SLOTS 16

#define CYGNUS_MAX_PLAYBACK_PORTS 4
#define CYGNUS_MAX_CAPTURE_PORTS 3
#define CYGNUS_MAX_I2S_PORTS 3
#define CYGNUS_MAX_PORTS  CYGNUS_MAX_PLAYBACK_PORTS

#define CYGNUS_SSP_FRAMEBITS_DIV 1

#define CYGNUS_SSPMODE_I2S 0
#define CYGNUS_SSPMODE_TDM 1
#define CYGNUS_SSPMODE_UNKNOWN -1

#define CYGNUS_SSP_CLKSRC_PLL      0

/* Max string length of our dt property names */
#define PROP_LEN_MAX 40

enum cygnus_audio_port_type {
	PORT_TDM,
	PORT_SPDIF,
};

struct cygnus_ssp_regs {
	u32 i2s_stream_cfg;
	u32 i2s_cfg;
	u32 i2s_cap_stream_cfg;
	u32 i2s_cap_cfg;
	u32 i2s_mclk_cfg;

	u32 bf_destch_ctrl;
	u32 bf_destch_cfg;
	u32 bf_sourcech_ctrl;
	u32 bf_sourcech_cfg;
};

struct audio_io {
	struct regmap *audio;
	struct regmap *cmn_io;
	struct regmap *i2s_in;
};

struct cygnus_audio_clkinfo {
	struct clk *audio_clk;
};

struct cygnus_aio_port {
	struct device *dev;

	int portnum;
	int mode;
	bool is_slave;
	int streams_on;   /* will be 0 if both capture and play are off */
	int port_type;

	unsigned int fsync_width;
	unsigned int fs_delay;
	bool invert_bclk;
	bool invert_fs;

	u32 mclk;
	u32 lrclk;
	u32 pll_clk_num;

	unsigned int slot_width;
	unsigned int slots_per_frame;
	unsigned int active_slots;

	struct audio_io *io;

	struct cygnus_ssp_regs regs;

	struct cygnus_audio_clkinfo clk_info;
};


struct cygnus_audio {
	struct cygnus_aio_port  portinfo[CYGNUS_MAX_PORTS];
	struct iproc_rb_info    rb_info;
	struct iproc_pcm_dma_info   dma_info_play[CYGNUS_MAX_PLAYBACK_PORTS];
	struct iproc_pcm_dma_info   dma_info_cap[CYGNUS_MAX_CAPTURE_PORTS];

	struct audio_io io;
	struct device *dev;
};

extern int cygnus_ssp_set_custom_fsync_width(struct snd_soc_dai *cpu_dai,
						int len);
int cygnus_ssp_get_clk(struct snd_soc_dai *dai, unsigned int freq);
int cygnus_ssp_put_clk(struct snd_soc_dai *dai);
#endif
