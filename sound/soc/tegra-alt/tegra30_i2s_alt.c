/*
 * tegra30_i2s_alt.c - Tegra30 I2S driver
 *
 * Author: Stephen Warren <swarren@nvidia.com>
 * Copyright (c) 2010-2015 NVIDIA CORPORATION.  All rights reserved.
 *
 * Based on code copyright/by:
 *
 * Copyright (c) 2009-2010, NVIDIA Corporation.
 * Scott Peterson <speterson@nvidia.com>
 *
 * Copyright (C) 2010 Google, Inc.
 * Iliyan Malchev <malchev@google.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_device.h>

#include "tegra30_xbar_alt.h"
#include "tegra30_i2s_alt.h"
#include "tegra30_apbif_alt.h"

#define DRV_NAME "tegra30-i2s"

static const struct reg_default tegra30_i2s_reg_defaults[] = {
	{TEGRA30_I2S_CTRL, 0x00000000},
	{TEGRA30_I2S_TIMING, 0x0000001f},
	{TEGRA30_I2S_OFFSET, 0x00000000},
	{TEGRA30_I2S_CH_CTRL, 0x00000000},
	{TEGRA30_I2S_SLOT_CTRL, 0x00000000},
	{TEGRA30_I2S_AUDIOCIF_I2STX_CTRL, 0x00001100},
	{TEGRA30_I2S_AUDIOCIF_I2SRX_CTRL, 0x00001100},
	{TEGRA30_I2S_FLOWCTL, 0x80000000},
	{TEGRA30_I2S_TX_STEP, 0x00008000},
	{TEGRA30_I2S_FLOW_STATUS, 0x00000000},
	{TEGRA30_I2S_LCOEF_1_4_0, 0x0000002e},
	{TEGRA30_I2S_LCOEF_1_4_1, 0x0000f9e6},
	{TEGRA30_I2S_LCOEF_1_4_2, 0x000020ca},
	{TEGRA30_I2S_LCOEF_1_4_3, 0x00007147},
	{TEGRA30_I2S_LCOEF_1_4_4, 0x0000f17e},
	{TEGRA30_I2S_LCOEF_1_4_5, 0x000001e0},
	{TEGRA30_I2S_LCOEF_2_4_0, 0x00000117},
	{TEGRA30_I2S_LCOEF_2_4_1, 0x0000f26b},
	{TEGRA30_I2S_LCOEF_2_4_2, 0x00004c07},
	{TEGRA114_I2S_SLOT_CTRL2, 0x00000000},
};

static void tegra30_i2s_set_slot_ctrl(struct regmap *regmap,
				unsigned int total_slots,
				unsigned int tx_slot_mask,
				unsigned int rx_slot_mask)
{
	regmap_write(regmap, TEGRA30_I2S_SLOT_CTRL,
		((total_slots - 1) << TEGRA30_I2S_SLOT_CTRL_TOTAL_SLOTS_SHIFT) |
		(rx_slot_mask << TEGRA30_I2S_SLOT_CTRL_RX_SLOT_ENABLES_SHIFT) |
		(tx_slot_mask << TEGRA30_I2S_SLOT_CTRL_TX_SLOT_ENABLES_SHIFT));
}

static void tegra114_i2s_set_slot_ctrl(struct regmap *regmap,
				unsigned int total_slots,
				unsigned int tx_slot_mask,
				unsigned int rx_slot_mask)
{
	regmap_write(regmap, TEGRA30_I2S_SLOT_CTRL, total_slots - 1);
	regmap_write(regmap, TEGRA114_I2S_SLOT_CTRL2,
		(rx_slot_mask <<
		TEGRA114_I2S_SLOT_CTRL2_RX_SLOT_ENABLES_SHIFT) |
		tx_slot_mask);
}

static int tegra30_i2s_set_clock_rate(struct device *dev, int clock_rate)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	switch (i2s->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		ret = clk_set_rate(i2s->clk_i2s_sync, clock_rate);
		if (ret) {
			dev_err(dev, "Can't set I2S sync clock rate\n");
			return ret;
		}

		ret = clk_set_parent(clk_get_parent(i2s->clk_audio_2x),
						    i2s->clk_i2s_sync);
		if (ret) {
			dev_err(dev, "Can't set parent of audio2x clock\n");
			return ret;
		}

		ret = clk_set_rate(i2s->clk_audio_2x, clock_rate);
		if (ret) {
			dev_err(dev, "Can't set audio2x clock rate\n");
			return ret;
		}

		ret = clk_set_parent(i2s->clk_i2s, i2s->clk_audio_2x);
		if (ret) {
			dev_err(dev, "Can't set parent of i2s clock\n");
			return ret;
		}
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		ret = clk_set_parent(i2s->clk_i2s, i2s->clk_pll_a_out0);
		if (ret) {
			dev_err(dev, "Can't set parent of I2S clock\n");
			return ret;
		}
		ret = clk_set_rate(i2s->clk_i2s, clock_rate);
		if (ret) {
			dev_err(dev, "Can't set I2S clock rate: %d\n", ret);
			return ret;
		}
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int tegra30_i2s_set_format(struct device *dev)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);
	unsigned int mask, val, i2s_ctrl, edge_ctrl, data_offset;

	mask = TEGRA30_I2S_CH_CTRL_EGDE_CTRL_MASK;
	switch (i2s->dai_fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		edge_ctrl = TEGRA30_I2S_CH_CTRL_EGDE_CTRL_POS_EDGE;
		i2s_ctrl = TEGRA30_I2S_CTRL_LRCK_L_LOW;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		edge_ctrl = TEGRA30_I2S_CH_CTRL_EGDE_CTRL_POS_EDGE;
		i2s_ctrl = TEGRA30_I2S_CTRL_LRCK_R_LOW;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		edge_ctrl = TEGRA30_I2S_CH_CTRL_EGDE_CTRL_NEG_EDGE;
		i2s_ctrl = TEGRA30_I2S_CTRL_LRCK_L_LOW;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		edge_ctrl = TEGRA30_I2S_CH_CTRL_EGDE_CTRL_NEG_EDGE;
		i2s_ctrl = TEGRA30_I2S_CTRL_LRCK_R_LOW;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CH_CTRL,
		mask, edge_ctrl);

	mask = TEGRA30_I2S_CTRL_MASTER_MASK;
	switch (i2s->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		i2s_ctrl |= TEGRA30_I2S_CTRL_SLAVE_ENABLE;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		i2s_ctrl |= TEGRA30_I2S_CTRL_MASTER_ENABLE;
		break;
	default:
		return -EINVAL;
	}

	mask |= TEGRA30_I2S_CTRL_FRAME_FORMAT_MASK |
		TEGRA30_I2S_CTRL_LRCK_MASK;
	switch (i2s->dai_fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
		i2s_ctrl |= TEGRA30_I2S_CTRL_FRAME_FORMAT_FSYNC;
		data_offset = 1;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		i2s_ctrl |= TEGRA30_I2S_CTRL_FRAME_FORMAT_FSYNC;
		data_offset = 0;
		break;
	/* I2S mode has data offset of 1 */
	case SND_SOC_DAIFMT_I2S:
		i2s_ctrl |= TEGRA30_I2S_FRAME_FORMAT_LRCK;
		data_offset = 1;
		break;
	/* LJ/RJ mode assumed to operate at bclk = 64fs */
	case SND_SOC_DAIFMT_RIGHT_J:
		i2s_ctrl |= TEGRA30_I2S_FRAME_FORMAT_LRCK;
		data_offset = 16;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		i2s_ctrl |= TEGRA30_I2S_FRAME_FORMAT_LRCK;
		data_offset = 0;
		break;
	default:
		return -EINVAL;
	}

	val = (data_offset << TEGRA30_I2S_OFFSET_RX_DATA_OFFSET_SHIFT) |
	      (data_offset << TEGRA30_I2S_OFFSET_TX_DATA_OFFSET_SHIFT);

	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CTRL,
		mask, i2s_ctrl);
	regmap_write(i2s->regmap, TEGRA30_I2S_OFFSET, val);
	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CH_CTRL,
		TEGRA30_I2S_CH_CTRL_FSYNC_WIDTH_MASK,
		i2s->fsync_width << TEGRA30_I2S_CH_CTRL_FSYNC_WIDTH_SHIFT);

	return 0;
}

static int tegra30_i2s_runtime_suspend(struct device *dev)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);

	regcache_cache_only(i2s->regmap, true);

	clk_disable_unprepare(i2s->clk_i2s);

	return 0;
}

static int tegra30_i2s_runtime_resume(struct device *dev)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(i2s->clk_i2s);
	if (ret) {
		dev_err(dev, "clk_enable failed: %d\n", ret);
		return ret;
	}
	regcache_cache_only(i2s->regmap, false);
	regcache_sync(i2s->regmap);

	/* set the format */
	tegra30_i2s_set_format(dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int tegra30_i2s_suspend(struct device *dev)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);

	regcache_mark_dirty(i2s->regmap);

	return 0;
}
#endif

static int tegra30_i2s_set_tdm_slot(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width)
{
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	/* copy the required tx and rx mask */
	i2s->tx_mask = (tx_mask > 0xFFFF) ? 0xFFFF : tx_mask;
	i2s->rx_mask = (rx_mask > 0xFFFF) ? 0xFFFF : rx_mask;

	return 0;
}
static int tegra30_i2s_set_dai_bclk_ratio(struct snd_soc_dai *dai,
		unsigned int ratio)
{
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	i2s->bclk_ratio = ratio;

	return 0;
}

static int tegra30_i2s_set_dai_fmt(struct snd_soc_dai *dai,
				unsigned int fmt)
{
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	i2s->dai_fmt = fmt;

	return 0;
}

static int tegra30_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct tegra30_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	unsigned int mask, val, reg;
	int ret, sample_size, channels, srate, i2sclock, bitcnt;
	struct tegra30_xbar_cif_conf cif_conf;

	channels = params_channels(params);
	if (channels < 1) {
		dev_err(dev, "Doesn't support %d channels\n", channels);
		return -EINVAL;
	}

	mask = TEGRA30_I2S_CTRL_BIT_SIZE_MASK;
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		val = TEGRA30_I2S_CTRL_BIT_SIZE_8;
		sample_size = 8;
		cif_conf.audio_bits = TEGRA30_AUDIOCIF_BITS_8;
		cif_conf.client_bits = TEGRA30_AUDIOCIF_BITS_8;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		val = TEGRA30_I2S_CTRL_BIT_SIZE_16;
		sample_size = 16;
		cif_conf.audio_bits = TEGRA30_AUDIOCIF_BITS_16;
		cif_conf.client_bits = TEGRA30_AUDIOCIF_BITS_16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		val = TEGRA30_I2S_CTRL_BIT_SIZE_24;
		sample_size = 24;
		cif_conf.audio_bits = TEGRA30_AUDIOCIF_BITS_24;
		cif_conf.client_bits = TEGRA30_AUDIOCIF_BITS_24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		val = TEGRA30_I2S_CTRL_BIT_SIZE_32;
		sample_size = 32;
		cif_conf.audio_bits = TEGRA30_AUDIOCIF_BITS_32;
		cif_conf.client_bits = TEGRA30_AUDIOCIF_BITS_32;
		break;
	default:
		dev_err(dev, "Wrong format!\n");
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CTRL, mask, val);

	srate = params_rate(params);

	regmap_read(i2s->regmap, TEGRA30_I2S_CTRL, &val);
	if ((val & TEGRA30_I2S_CTRL_FRAME_FORMAT_MASK) ==
		TEGRA30_I2S_CTRL_FRAME_FORMAT_FSYNC) {
		i2s->soc_data->set_slot_ctrl(i2s->regmap, channels,
				i2s->tx_mask, i2s->rx_mask);
		/* I2S fifo threshold set to 3 when AFC is connected */
		cif_conf.threshold = 3;
		cif_conf.audio_channels = channels;
		cif_conf.client_channels = channels;
		cif_conf.expand = 0;
		cif_conf.stereo_conv = 0;
		cif_conf.replicate = 0;
		cif_conf.truncate = 0;
		cif_conf.mono_conv = 0;
	} else {
		cif_conf.threshold = 3;
		cif_conf.audio_channels = channels;
		cif_conf.client_channels = (channels == 1) ? 2 : channels;
		cif_conf.expand = 0;
		cif_conf.stereo_conv = 0;
		cif_conf.replicate = 0;
		cif_conf.truncate = 0;
		cif_conf.mono_conv = 0;
	}

	i2sclock = srate * sample_size * cif_conf.client_channels;

	if (i2s->bclk_ratio != 0)
		i2sclock *= i2s->bclk_ratio;

	bitcnt = (i2sclock / srate) - 1;
	if ((bitcnt < 0) ||
		(bitcnt > TEGRA30_I2S_TIMING_CHANNEL_BIT_COUNT_MASK_US)) {
		dev_err(dev, "Can't set channel bit count\n");
		return -EINVAL;
	}

	ret = tegra30_i2s_set_clock_rate(dev, i2sclock);
	if (ret) {
		dev_err(dev, "Can't set I2S clock rate: %d\n", ret);
		return ret;
	}

	if ((val & TEGRA30_I2S_CTRL_FRAME_FORMAT_MASK) ==
		TEGRA30_I2S_CTRL_FRAME_FORMAT_FSYNC)
		val = bitcnt << TEGRA30_I2S_TIMING_CHANNEL_BIT_COUNT_SHIFT;
	else
		val = (bitcnt >> 1) <<
			TEGRA30_I2S_TIMING_CHANNEL_BIT_COUNT_SHIFT;

	if (i2sclock % (2 * srate))
		val |= TEGRA30_I2S_TIMING_NON_SYM_ENABLE;

	regmap_write(i2s->regmap, TEGRA30_I2S_TIMING, val);

	/* As a COCEC DAI, CAPTURE is transmit */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		cif_conf.direction = TEGRA30_AUDIOCIF_DIRECTION_RX;
		reg = TEGRA30_I2S_AUDIOCIF_I2STX_CTRL;
	} else {
		cif_conf.direction = TEGRA30_AUDIOCIF_DIRECTION_TX;
		reg = TEGRA30_I2S_AUDIOCIF_I2SRX_CTRL;
	}
	i2s->soc_data->set_audio_cif(i2s->regmap, reg, &cif_conf);

	return 0;
}

static int tegra30_i2s_soft_reset(struct tegra30_i2s *i2s)
{
	int dcnt = 10;
	unsigned int val, i2s_ctrl, edge_ctrl, offset;

	regmap_read(i2s->regmap, TEGRA30_I2S_CTRL, &i2s_ctrl);
	regmap_read(i2s->regmap, TEGRA30_I2S_CH_CTRL, &edge_ctrl);
	regmap_read(i2s->regmap, TEGRA30_I2S_OFFSET, &offset);

	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CTRL,
		TEGRA30_I2S_CTRL_SOFT_RESET, TEGRA30_I2S_CTRL_SOFT_RESET);

	do {
		udelay(100);
		regmap_read(i2s->regmap, TEGRA30_I2S_CTRL, &val);
	} while ((val & TEGRA30_I2S_CTRL_SOFT_RESET) && dcnt--);

	/* Restore reg_ctrl to ensure if a concurrent playback/capture
	   session was active it continues after SOFT_RESET */
	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CTRL,
		TEGRA30_I2S_CTRL_SOFT_RESET, 0);

	regmap_write(i2s->regmap, TEGRA30_I2S_CTRL, i2s_ctrl);
	regmap_write(i2s->regmap, TEGRA30_I2S_CH_CTRL, edge_ctrl);
	regmap_write(i2s->regmap, TEGRA30_I2S_OFFSET, offset);

	return (dcnt < 0) ? -ETIMEDOUT : 0;
}

static int tegra30_i2s_rx_stop(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct device *dev = codec->dev;
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);
	int dcnt = 10;

	/* wait until rx fifo is disabled */
	while (tegra30_apbif_i2s_rx_fifo_is_enabled(dev->id) && dcnt--)
		udelay(100);

	dcnt = 10;
	while (!tegra30_apbif_i2s_rx_fifo_is_empty(dev->id) && dcnt--)
		udelay(100);

	if (dcnt < 0) {
		dcnt = 10;
		tegra30_i2s_soft_reset(i2s);
		while (!tegra30_apbif_i2s_rx_fifo_is_empty(dev->id) &&
		       dcnt--)
			udelay(100);
	}
	return 0;
}

static int tegra30_i2s_tx_stop(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct device *dev = codec->dev;
	struct tegra30_i2s *i2s = dev_get_drvdata(dev);
	int dcnt = 10;

	/* wait until tx fifo is disabled */
	while (tegra30_apbif_i2s_tx_fifo_is_enabled(dev->id) && dcnt--)
		udelay(100);

	dcnt = 10;
	while (!tegra30_apbif_i2s_tx_fifo_is_empty(dev->id) && dcnt--)
		udelay(100);

	if (dcnt < 0) {
		dcnt = 10;
		tegra30_i2s_soft_reset(i2s);
		while (!tegra30_apbif_i2s_tx_fifo_is_empty(dev->id) &&
		       dcnt--)
			udelay(100);
	}
	return 0;
}

static int tegra30_i2s_codec_probe(struct snd_soc_codec *codec)
{
	struct tegra30_i2s *i2s = snd_soc_codec_get_drvdata(codec);
	int ret;

	codec->control_data = i2s->regmap;
	ret = snd_soc_codec_set_cache_io(codec, 32, 32, SND_SOC_REGMAP);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
		return ret;
	}

	return 0;
}

static struct snd_soc_dai_ops tegra30_i2s_dai_ops = {
	.set_fmt	= tegra30_i2s_set_dai_fmt,
	.hw_params	= tegra30_i2s_hw_params,
	.set_bclk_ratio	= tegra30_i2s_set_dai_bclk_ratio,
	.set_tdm_slot	= tegra30_i2s_set_tdm_slot,
};

static struct snd_soc_dai_driver tegra30_i2s_dais[] = {
	{
		.name = "CIF",
		.playback = {
			.stream_name = "CIF Receive",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "CIF Transmit",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
	},
	{
		.name = "DAP",
		.playback = {
			.stream_name = "DAP Receive",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.capture = {
			.stream_name = "DAP Transmit",
			.channels_min = 1,
			.channels_max = 16,
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &tegra30_i2s_dai_ops,
		.symmetric_rates = 1,
	}
};

static int tegra30_i2s_loopback_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tegra30_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = i2s->loopback;

	return 0;
}

static int tegra30_i2s_loopback_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tegra30_i2s *i2s = snd_soc_codec_get_drvdata(codec);

	i2s->loopback =	ucontrol->value.integer.value[0];

	pm_runtime_get_sync(codec->dev);
	regmap_update_bits(i2s->regmap, TEGRA30_I2S_CTRL,
		TEGRA30_I2S_CTRL_LPBK_MASK,
		i2s->loopback << TEGRA30_I2S_CTRL_LPBK_SHIFT);
	pm_runtime_put(codec->dev);

	return 0;
}

static const struct snd_kcontrol_new tegra30_i2s_controls[] = {
	SOC_SINGLE_EXT("Loopback", SND_SOC_NOPM, 0, 1, 0,
		tegra30_i2s_loopback_get, tegra30_i2s_loopback_put),
};

static const struct snd_soc_dapm_widget tegra30_i2s_widgets[] = {
	SND_SOC_DAPM_AIF_IN("CIF RX", NULL, 0, SND_SOC_NOPM,
				0, 0),
	SND_SOC_DAPM_AIF_OUT("CIF TX", NULL, 0, SND_SOC_NOPM,
				0, 0),
	SND_SOC_DAPM_AIF_IN_E("DAP RX", NULL, 0, TEGRA30_I2S_CTRL,
				TEGRA30_I2S_CTRL_XFER_EN_RX_SHIFT, 0,
				tegra30_i2s_rx_stop, SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_OUT_E("DAP TX", NULL, 0, TEGRA30_I2S_CTRL,
				TEGRA30_I2S_CTRL_XFER_EN_TX_SHIFT, 0,
				tegra30_i2s_tx_stop, SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route tegra30_i2s_routes[] = {
	{ "CIF RX",       NULL, "CIF Receive" },
	{ "DAP TX",       NULL, "CIF RX" },
	{ "DAP Transmit", NULL, "DAP TX" },

	{ "DAP RX",       NULL, "DAP Receive" },
	{ "CIF TX",       NULL, "DAP RX" },
	{ "CIF Transmit", NULL, "CIF TX" },
};

static struct snd_soc_codec_driver tegra30_i2s_codec = {
	.probe = tegra30_i2s_codec_probe,
	.dapm_widgets = tegra30_i2s_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra30_i2s_widgets),
	.dapm_routes = tegra30_i2s_routes,
	.num_dapm_routes = ARRAY_SIZE(tegra30_i2s_routes),
	.controls = tegra30_i2s_controls,
	.num_controls = ARRAY_SIZE(tegra30_i2s_controls),
	.idle_bias_off = 1,
};

static bool tegra30_i2s_wr_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA30_I2S_CTRL:
	case TEGRA30_I2S_TIMING:
	case TEGRA30_I2S_OFFSET:
	case TEGRA30_I2S_CH_CTRL:
	case TEGRA30_I2S_SLOT_CTRL:
	case TEGRA30_I2S_AUDIOCIF_I2STX_CTRL:
	case TEGRA30_I2S_AUDIOCIF_I2SRX_CTRL:
	case TEGRA30_I2S_FLOWCTL:
	case TEGRA30_I2S_TX_STEP:
	case TEGRA30_I2S_FLOW_STATUS:
	case TEGRA30_I2S_LCOEF_1_4_0:
	case TEGRA30_I2S_LCOEF_1_4_1:
	case TEGRA30_I2S_LCOEF_1_4_2:
	case TEGRA30_I2S_LCOEF_1_4_3:
	case TEGRA30_I2S_LCOEF_1_4_4:
	case TEGRA30_I2S_LCOEF_1_4_5:
	case TEGRA30_I2S_LCOEF_2_4_0:
	case TEGRA30_I2S_LCOEF_2_4_1:
	case TEGRA30_I2S_LCOEF_2_4_2:
	case TEGRA114_I2S_SLOT_CTRL2:
		return true;
	default:
		return false;
	};
}

static bool tegra30_i2s_rd_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA30_I2S_CTRL:
	case TEGRA30_I2S_TIMING:
	case TEGRA30_I2S_OFFSET:
	case TEGRA30_I2S_CH_CTRL:
	case TEGRA30_I2S_SLOT_CTRL:
	case TEGRA30_I2S_AUDIOCIF_I2STX_CTRL:
	case TEGRA30_I2S_AUDIOCIF_I2SRX_CTRL:
	case TEGRA30_I2S_FLOWCTL:
	case TEGRA30_I2S_TX_STEP:
	case TEGRA30_I2S_FLOW_STATUS:
	case TEGRA30_I2S_FLOW_TOTAL:
	case TEGRA30_I2S_FLOW_OVER:
	case TEGRA30_I2S_FLOW_UNDER:
	case TEGRA30_I2S_LCOEF_1_4_0:
	case TEGRA30_I2S_LCOEF_1_4_1:
	case TEGRA30_I2S_LCOEF_1_4_2:
	case TEGRA30_I2S_LCOEF_1_4_3:
	case TEGRA30_I2S_LCOEF_1_4_4:
	case TEGRA30_I2S_LCOEF_1_4_5:
	case TEGRA30_I2S_LCOEF_2_4_0:
	case TEGRA30_I2S_LCOEF_2_4_1:
	case TEGRA30_I2S_LCOEF_2_4_2:
	case TEGRA114_I2S_SLOT_CTRL2:
		return true;
	default:
		return false;
	};
}

static bool tegra30_i2s_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA30_I2S_CTRL:
	case TEGRA30_I2S_FLOW_STATUS:
	case TEGRA30_I2S_FLOW_TOTAL:
	case TEGRA30_I2S_FLOW_OVER:
	case TEGRA30_I2S_FLOW_UNDER:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config tegra30_i2s_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = TEGRA114_I2S_SLOT_CTRL2,
	.writeable_reg = tegra30_i2s_wr_reg,
	.readable_reg = tegra30_i2s_rd_reg,
	.volatile_reg = tegra30_i2s_volatile_reg,
	.cache_type = REGCACHE_FLAT,
	.reg_defaults = tegra30_i2s_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tegra30_i2s_reg_defaults),
};

static const struct tegra30_i2s_soc_data soc_data_tegra30 = {
	.set_audio_cif = tegra30_xbar_set_cif,
	.set_slot_ctrl = tegra30_i2s_set_slot_ctrl,
};

static const struct tegra30_i2s_soc_data soc_data_tegra114 = {
	.set_audio_cif = tegra30_xbar_set_cif,
	.set_slot_ctrl = tegra114_i2s_set_slot_ctrl,
};

static const struct tegra30_i2s_soc_data soc_data_tegra124 = {
	.set_audio_cif = tegra124_xbar_set_cif,
	.set_slot_ctrl = tegra114_i2s_set_slot_ctrl,
};

static const struct of_device_id tegra30_i2s_of_match[] = {
	{ .compatible = "nvidia,tegra30-i2s", .data = &soc_data_tegra30 },
	{ .compatible = "nvidia,tegra114-i2s", .data = &soc_data_tegra114 },
	{ .compatible = "nvidia,tegra124-i2s", .data = &soc_data_tegra124 },
	{},
};

static irqreturn_t tegra30_i2s_interrupt_handler(int irq, void *data)
{

	struct device *dev = data;
	struct tegra30_i2s *i2s = dev_get_drvdata(data);
	unsigned long flags;

	spin_lock_irqsave(&i2s->int_lock, flags);

	if (tegra30_apbif_i2s_overrun_interrupt_status(dev->id)) {
		tegra30_apbif_i2s_overrun_interrupt_status_clear(dev->id);
		tegra30_i2s_soft_reset(i2s);
	} else if (tegra30_apbif_i2s_underrun_interrupt_status(dev->id)) {
		tegra30_apbif_i2s_underrun_interrupt_status_clear(dev->id);
		tegra30_i2s_soft_reset(i2s);
	}

	spin_unlock_irqrestore(&i2s->int_lock, flags);

	return IRQ_HANDLED;
}

static int tegra30_i2s_platform_probe(struct platform_device *pdev)
{
	struct tegra30_i2s *i2s;
	struct resource *mem, *memregion;
	void __iomem *regs;
	int ret = 0;
	const struct of_device_id *match;
	struct tegra30_i2s_soc_data *soc_data;

	match = of_match_device(tegra30_i2s_of_match, &pdev->dev);
	if (!match) {
		dev_err(&pdev->dev, "Error: No device match found\n");
		ret = -ENODEV;
		goto err;
	}
	soc_data = (struct tegra30_i2s_soc_data *)match->data;

	i2s = devm_kzalloc(&pdev->dev, sizeof(struct tegra30_i2s), GFP_KERNEL);
	if (!i2s) {
		dev_err(&pdev->dev, "Can't allocate i2s\n");
		ret = -ENOMEM;
		goto err;
	}
	dev_set_drvdata(&pdev->dev, i2s);

	i2s->soc_data = soc_data;
	i2s->tx_mask = i2s->rx_mask = 0xFFFF;
	i2s->bclk_ratio = 2;
	i2s->loopback = 0;

	i2s->clk_i2s = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2s->clk_i2s)) {
		dev_err(&pdev->dev, "Can't retrieve i2s clock\n");
		ret = PTR_ERR(i2s->clk_i2s);
		goto err;
	}

	i2s->clk_i2s_sync = devm_clk_get(&pdev->dev, "ext_audio_sync");
	if (IS_ERR(i2s->clk_i2s_sync)) {
		dev_err(&pdev->dev, "Can't retrieve i2s_sync clock\n");
		ret = PTR_ERR(i2s->clk_i2s_sync);
		goto err_clk_put;
	}

	i2s->clk_audio_2x = devm_clk_get(&pdev->dev, "audio_sync_2x");
	if (IS_ERR(i2s->clk_audio_2x)) {
		dev_err(&pdev->dev, "Can't retrieve audio 2x clock\n");
		ret = PTR_ERR(i2s->clk_audio_2x);
		goto err_i2s_sync_clk_put;
	}

	i2s->clk_pll_a_out0 = clk_get_sys(NULL, "pll_a_out0");
	if (IS_ERR(i2s->clk_pll_a_out0)) {
		dev_err(&pdev->dev, "Can't retrieve pll_a_out0 clock\n");
		ret = PTR_ERR(i2s->clk_pll_a_out0);
		goto err_audio_2x_clk_put;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "No memory resource\n");
		ret = -ENODEV;
		goto err_pll_a_out0_clk_put;
	}

	memregion = devm_request_mem_region(&pdev->dev, mem->start,
					    resource_size(mem), DRV_NAME);
	if (!memregion) {
		dev_err(&pdev->dev, "Memory region already claimed\n");
		ret = -EBUSY;
		goto err_pll_a_out0_clk_put;
	}

	regs = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (!regs) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENOMEM;
		goto err_pll_a_out0_clk_put;
	}

	i2s->regmap = devm_regmap_init_mmio(&pdev->dev, regs,
					    &tegra30_i2s_regmap_config);
	if (IS_ERR(i2s->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		ret = PTR_ERR(i2s->regmap);
		goto err_pll_a_out0_clk_put;
	}
	regcache_cache_only(i2s->regmap, true);

	if (of_property_read_u32(pdev->dev.of_node,
		"nvidia,ahub-i2s-id", &pdev->dev.id) < 0) {
		dev_err(&pdev->dev, "Missing property nvidia,ahub-i2s-id\n");
		ret = -ENODEV;
		goto err;
	}

	if (of_property_read_u32(pdev->dev.of_node,
		"fsync-width", &i2s->fsync_width) < 0) {
		dev_warn(&pdev->dev, "Missing prop fsync-width for I2S%d\n",
			pdev->dev.id);
		i2s->fsync_width = 31;
	}

	if (i2s->fsync_width > 255) {
		dev_warn(&pdev->dev, "Default fsync-width to 31 for I2S%d\n",
			pdev->dev.id);
		i2s->fsync_width = 31;
	}

	pm_runtime_enable(&pdev->dev);
	if (!pm_runtime_enabled(&pdev->dev)) {
		ret = tegra30_i2s_runtime_resume(&pdev->dev);
		if (ret)
			goto err_pm_disable;
	}

	ret = snd_soc_register_codec(&pdev->dev, &tegra30_i2s_codec,
				     tegra30_i2s_dais,
				     ARRAY_SIZE(tegra30_i2s_dais));
	if (ret != 0) {
		dev_err(&pdev->dev, "Could not register CODEC: %d\n", ret);
		goto err_suspend;
	}

	i2s->irq = platform_get_irq(pdev, 0);
	if (request_irq(i2s->irq, tegra30_i2s_interrupt_handler,
			IRQF_SHARED, pdev->name, &pdev->dev) < 0)
		dev_err(&pdev->dev, "Could not register INTERRUPT\n");
	 spin_lock_init(&i2s->int_lock);

	/* enable the underrun and overrun innterupts */
	tegra30_apbif_i2s_underrun_interrupt_mask_clear(pdev->dev.id);
	tegra30_apbif_i2s_overrun_interrupt_mask_clear(pdev->dev.id);
	return 0;

err_suspend:
	if (!pm_runtime_status_suspended(&pdev->dev))
		tegra30_i2s_runtime_suspend(&pdev->dev);
err_pm_disable:
	pm_runtime_disable(&pdev->dev);
err_pll_a_out0_clk_put:
	clk_put(i2s->clk_pll_a_out0);
err_audio_2x_clk_put:
	devm_clk_put(&pdev->dev, i2s->clk_audio_2x);
err_i2s_sync_clk_put:
	devm_clk_put(&pdev->dev, i2s->clk_i2s_sync);
err_clk_put:
	devm_clk_put(&pdev->dev, i2s->clk_i2s);
err:
	return ret;
}

static int tegra30_i2s_platform_remove(struct platform_device *pdev)
{
	struct tegra30_i2s *i2s = dev_get_drvdata(&pdev->dev);

	tegra30_apbif_i2s_underrun_interrupt_mask_set(pdev->dev.id);
	tegra30_apbif_i2s_overrun_interrupt_mask_set(pdev->dev.id);

	snd_soc_unregister_codec(&pdev->dev);

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		tegra30_i2s_runtime_suspend(&pdev->dev);

	free_irq(i2s->irq, &pdev->dev);
	devm_clk_put(&pdev->dev, i2s->clk_i2s);
	devm_clk_put(&pdev->dev, i2s->clk_i2s_sync);
	devm_clk_put(&pdev->dev, i2s->clk_audio_2x);
	clk_put(i2s->clk_pll_a_out0);

	return 0;
}

static const struct dev_pm_ops tegra30_i2s_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra30_i2s_runtime_suspend,
			tegra30_i2s_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(tegra30_i2s_suspend,
			NULL)
};

static struct platform_driver tegra30_i2s_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tegra30_i2s_of_match,
		.pm = &tegra30_i2s_pm_ops,
	},
	.probe = tegra30_i2s_platform_probe,
	.remove = tegra30_i2s_platform_remove,
};
module_platform_driver(tegra30_i2s_driver);

MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_DESCRIPTION("Tegra30 I2S ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, tegra30_i2s_of_match);
