/*
 * wm8987.c -- WM8987 ALSA SoC audio driver
 *
 * Copyright 2005 Openedhand Ltd.
 *
 * Author: Richard Purdie <richard@openedhand.com>
 *
 * Based on WM8753.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * 
 * ChangeLog:
 *   o 0.1 2010-0204, Jackal Chan <jackal.cvkk@gmail.com>
 *         - porting to 2.6.29 kernel for SmartQ5 MID
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>

#include "wm8987.h"

#define WM8987_VERSION "0.12"

/* codec private data */
struct wm8987_priv {
	unsigned int sysclk;
};

/*
 * wm8987 register cache
 * We can't read the WM8987 register space when we
 * are using 2 wire for device control, so we cache them instead.
 */
static const u16 wm8987_reg[] = {
	0x00b7, 0x0097, 0x0000, 0x0000,  /*  0 */
	0x0000, 0x0008, 0x0000, 0x002a,  /*  4 */
	0x0000, 0x0000, 0x007f, 0x007f,  /*  8 */
	0x000f, 0x000f, 0x0000, 0x0000,  /* 12 */
	0x0080, 0x007b, 0x0000, 0x0032,  /* 16 */
	0x0000, 0x00e0, 0x00e0, 0x00c0,  /* 20 */
	0x0000, 0x0000, 0x0000, 0x0000,  /* 24 */
	0x0000, 0x0000, 0x0000, 0x0000,  /* 28 */
	0x0000, 0x0000, 0x0050, 0x0050,  /* 32 */
	0x0050, 0x0050, 0x0050, 0x0050,  /* 36 */
	0x0000, 0x0000, 0x0079,          /* 40 */
};

/*
 * read wm8987 register cache
 */
static inline unsigned int wm8987_read_reg_cache(struct snd_soc_codec *codec,
	unsigned int reg)
{
	u16 *cache = codec->reg_cache;
	if (reg > WM8987_CACHE_REGNUM)
		return -1;
	return cache[reg];
}

/*
 * write wm8987 register cache
 */
static inline void wm8987_write_reg_cache(struct snd_soc_codec *codec,
	unsigned int reg, unsigned int value)
{
	u16 *cache = codec->reg_cache;
	if (reg > WM8987_CACHE_REGNUM)
		return;
	cache[reg] = value;
}

static int wm8987_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	u8 data[2];

	/* data is
	 *   D15..D9 WM8753 register offset
	 *   D8...D0 register data
	 */
	data[0] = (reg << 1) | ((value >> 8) & 0x0001);
	data[1] = value & 0x00ff;

	wm8987_write_reg_cache(codec, reg, value);
	if (codec->hw_write(codec->control_data, data, 2) == 2)
		return 0;
	else
		return -EIO;
}

#define wm8987_reset(c)	wm8987_write(c, WM8987_RESET, 0)

/*
 * WM8987 Controls
 */
static const char *wm8987_bass[] = {"Linear Control", "Adaptive Boost"};
static const char *wm8987_bass_filter[] = { "130Hz @ 48kHz", "200Hz @ 48kHz" };
static const char *wm8987_treble[] = {"8kHz", "4kHz"};
static const char *wm8987_3d_lc[] = {"200Hz", "500Hz"};
static const char *wm8987_3d_uc[] = {"2.2kHz", "1.5kHz"};
static const char *wm8987_3d_func[] = {"Capture", "Playback"};
static const char *wm8987_alc_func[] = {"Off", "Right", "Left", "Stereo"};
static const char *wm8987_ng_type[] = {"Constant PGA Gain",
	"Mute ADC Output"};
static const char *wm8987_line_mux[] = {"Line 1", "Line 2", "Line 3", "PGA",
	"Differential"};
static const char *wm8987_pga_sel[] = {"Line 1", "Line 2", "Line 3",
	"Differential"};
static const char *wm8987_out3[] = {"VREF", "ROUT1 + Vol", "MonoOut",
	"ROUT1"};
static const char *wm8987_diff_sel[] = {"Line 1", "Line 2"};
static const char *wm8987_adcpol[] = {"Normal", "L Invert", "R Invert",
	"L + R Invert"};
static const char *wm8987_deemph[] = {"None", "32Khz", "44.1Khz", "48Khz"};
static const char *wm8987_mono_mux[] = {"Stereo", "Mono (Left)",
	"Mono (Right)", "Digital Mono"};

static const struct soc_enum wm8987_enum[] = {
SOC_ENUM_SINGLE(WM8987_BASS, 7, 2, wm8987_bass),
SOC_ENUM_SINGLE(WM8987_BASS, 6, 2, wm8987_bass_filter),
SOC_ENUM_SINGLE(WM8987_TREBLE, 6, 2, wm8987_treble),
SOC_ENUM_SINGLE(WM8987_3D, 5, 2, wm8987_3d_lc),
SOC_ENUM_SINGLE(WM8987_3D, 6, 2, wm8987_3d_uc),
SOC_ENUM_SINGLE(WM8987_3D, 7, 2, wm8987_3d_func),
SOC_ENUM_SINGLE(WM8987_ALC1, 7, 4, wm8987_alc_func),
SOC_ENUM_SINGLE(WM8987_NGATE, 1, 2, wm8987_ng_type),
SOC_ENUM_SINGLE(WM8987_LOUTM1, 0, 5, wm8987_line_mux),
SOC_ENUM_SINGLE(WM8987_ROUTM1, 0, 5, wm8987_line_mux),
SOC_ENUM_SINGLE(WM8987_LADCIN, 6, 4, wm8987_pga_sel), /* 10 */
SOC_ENUM_SINGLE(WM8987_RADCIN, 6, 4, wm8987_pga_sel),
SOC_ENUM_SINGLE(WM8987_ADCTL2, 7, 4, wm8987_out3),
SOC_ENUM_SINGLE(WM8987_ADCIN, 8, 2, wm8987_diff_sel),
SOC_ENUM_SINGLE(WM8987_ADCDAC, 5, 4, wm8987_adcpol),
SOC_ENUM_SINGLE(WM8987_ADCDAC, 1, 4, wm8987_deemph),
SOC_ENUM_SINGLE(WM8987_ADCIN, 6, 4, wm8987_mono_mux), /* 16 */

};

static const struct snd_kcontrol_new wm8987_snd_controls[] = {

SOC_DOUBLE_R("Capture Volume", WM8987_LINVOL, WM8987_RINVOL, 0, 63, 0),
SOC_DOUBLE_R("Capture ZC Switch", WM8987_LINVOL, WM8987_RINVOL, 6, 1, 0),
SOC_DOUBLE_R("Capture Switch", WM8987_LINVOL, WM8987_RINVOL, 7, 1, 1),

SOC_DOUBLE_R("Headphone Playback ZC Switch", WM8987_LOUT1V,
	WM8987_ROUT1V, 7, 1, 0),
SOC_DOUBLE_R("Speaker Playback ZC Switch", WM8987_LOUT2V,
	WM8987_ROUT2V, 7, 1, 0),

SOC_ENUM("Playback De-emphasis", wm8987_enum[15]),

SOC_ENUM("Capture Polarity", wm8987_enum[14]),
SOC_SINGLE("Playback 6dB Attenuate", WM8987_ADCDAC, 7, 1, 0),
SOC_SINGLE("Capture 6dB Attenuate", WM8987_ADCDAC, 8, 1, 0),

SOC_DOUBLE_R("PCM Volume", WM8987_LDAC, WM8987_RDAC, 0, 255, 0),

SOC_ENUM("Bass Boost", wm8987_enum[0]),
SOC_ENUM("Bass Filter", wm8987_enum[1]),
SOC_SINGLE("Bass Volume", WM8987_BASS, 0, 15, 1),

SOC_SINGLE("Treble Volume", WM8987_TREBLE, 0, 15, 1),
SOC_ENUM("Treble Cut-off", wm8987_enum[2]),

SOC_SINGLE("3D Switch", WM8987_3D, 0, 1, 0),
SOC_SINGLE("3D Volume", WM8987_3D, 1, 15, 0),
SOC_ENUM("3D Lower Cut-off", wm8987_enum[3]),
SOC_ENUM("3D Upper Cut-off", wm8987_enum[4]),
SOC_ENUM("3D Mode", wm8987_enum[5]),

SOC_SINGLE("ALC Capture Target Volume", WM8987_ALC1, 0, 7, 0),
SOC_SINGLE("ALC Capture Max Volume", WM8987_ALC1, 4, 7, 0),
SOC_ENUM("ALC Capture Function", wm8987_enum[6]),
SOC_SINGLE("ALC Capture ZC Switch", WM8987_ALC2, 7, 1, 0),
SOC_SINGLE("ALC Capture Hold Time", WM8987_ALC2, 0, 15, 0),
SOC_SINGLE("ALC Capture Decay Time", WM8987_ALC3, 4, 15, 0),
SOC_SINGLE("ALC Capture Attack Time", WM8987_ALC3, 0, 15, 0),
SOC_SINGLE("ALC Capture NG Threshold", WM8987_NGATE, 3, 31, 0),
SOC_ENUM("ALC Capture NG Type", wm8987_enum[4]),
SOC_SINGLE("ALC Capture NG Switch", WM8987_NGATE, 0, 1, 0),

SOC_SINGLE("Left ADC Capture Volume", WM8987_LADC, 0, 255, 0),
SOC_SINGLE("Right ADC Capture Volume", WM8987_RADC, 0, 255, 0),

SOC_SINGLE("ZC Timeout Switch", WM8987_ADCTL1, 0, 1, 0),
SOC_SINGLE("Playback Invert Switch", WM8987_ADCTL1, 1, 1, 0),

SOC_SINGLE("Right Speaker Playback Invert Switch", WM8987_ADCTL2, 4, 1, 0),

/* Unimplemented */
/* ADCDAC Bit 0 - ADCHPD */
/* ADCDAC Bit 4 - HPOR */
/* ADCTL1 Bit 2,3 - DATSEL */
/* ADCTL1 Bit 4,5 - DMONOMIX */
/* ADCTL1 Bit 6,7 - VSEL */
/* ADCTL2 Bit 2 - LRCM */
/* ADCTL2 Bit 3 - TRI */
/* ADCTL3 Bit 5 - HPFLREN */
/* ADCTL3 Bit 6 - VROI */
/* ADCTL3 Bit 7,8 - ADCLRM */
/* ADCIN Bit 4 - LDCM */
/* ADCIN Bit 5 - RDCM */

SOC_DOUBLE_R("Mic Boost", WM8987_LADCIN, WM8987_RADCIN, 4, 3, 0),

SOC_DOUBLE_R("Bypass Left Playback Volume", WM8987_LOUTM1,
	WM8987_LOUTM2, 4, 7, 1),
SOC_DOUBLE_R("Bypass Right Playback Volume", WM8987_ROUTM1,
	WM8987_ROUTM2, 4, 7, 1),
SOC_DOUBLE_R("Bypass Mono Playback Volume", WM8987_MOUTM1,
	WM8987_MOUTM2, 4, 7, 1),

SOC_SINGLE("Mono Playback ZC Switch", WM8987_MOUTV, 7, 1, 0),

SOC_DOUBLE_R("Headphone Playback Volume", WM8987_LOUT1V, WM8987_ROUT1V,
	0, 127, 0),
SOC_DOUBLE_R("Speaker Playback Volume", WM8987_LOUT2V, WM8987_ROUT2V,
	0, 127, 0),

SOC_SINGLE("Mono Playback Volume", WM8987_MOUTV, 0, 127, 0),

};

/* add non dapm controls */
static int wm8987_add_controls(struct snd_soc_codec *codec)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(wm8987_snd_controls); i++) {
		err = snd_ctl_add(codec->card,
				snd_soc_cnew(&wm8987_snd_controls[i],
						codec, NULL));
		if (err < 0)
			return err;
	}
	return 0;
}

/*
 * DAPM Controls
 */

/* Left Mixer */
static const struct snd_kcontrol_new wm8987_left_mixer_controls[] = {
SOC_DAPM_SINGLE("Playback Switch", WM8987_LOUTM1, 8, 1, 0),
SOC_DAPM_SINGLE("Left Bypass Switch", WM8987_LOUTM1, 7, 1, 0),
SOC_DAPM_SINGLE("Right Playback Switch", WM8987_LOUTM2, 8, 1, 0),
SOC_DAPM_SINGLE("Right Bypass Switch", WM8987_LOUTM2, 7, 1, 0),
};

/* Right Mixer */
static const struct snd_kcontrol_new wm8987_right_mixer_controls[] = {
SOC_DAPM_SINGLE("Left Playback Switch", WM8987_ROUTM1, 8, 1, 0),
SOC_DAPM_SINGLE("Left Bypass Switch", WM8987_ROUTM1, 7, 1, 0),
SOC_DAPM_SINGLE("Playback Switch", WM8987_ROUTM2, 8, 1, 0),
SOC_DAPM_SINGLE("Right Bypass Switch", WM8987_ROUTM2, 7, 1, 0),
};

/* Mono Mixer */
static const struct snd_kcontrol_new wm8987_mono_mixer_controls[] = {
SOC_DAPM_SINGLE("Left Playback Switch", WM8987_MOUTM1, 8, 1, 0),
SOC_DAPM_SINGLE("Left Bypass Switch", WM8987_MOUTM1, 7, 1, 0),
SOC_DAPM_SINGLE("Right Playback Switch", WM8987_MOUTM2, 8, 1, 0),
SOC_DAPM_SINGLE("Right Bypass Switch", WM8987_MOUTM2, 7, 1, 0),
};

/* Left Line Mux */
static const struct snd_kcontrol_new wm8987_left_line_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[8]);

/* Right Line Mux */
static const struct snd_kcontrol_new wm8987_right_line_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[9]);

/* Left PGA Mux */
static const struct snd_kcontrol_new wm8987_left_pga_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[10]);

/* Right PGA Mux */
static const struct snd_kcontrol_new wm8987_right_pga_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[11]);

/* Out 3 Mux */
static const struct snd_kcontrol_new wm8987_out3_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[12]);

/* Differential Mux */
static const struct snd_kcontrol_new wm8987_diffmux_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[13]);

/* Mono ADC Mux */
static const struct snd_kcontrol_new wm8987_monomux_controls =
SOC_DAPM_ENUM("Route", wm8987_enum[16]);

static const struct snd_soc_dapm_widget wm8987_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER("Left Mixer", SND_SOC_NOPM, 0, 0,
		&wm8987_left_mixer_controls[0],
		ARRAY_SIZE(wm8987_left_mixer_controls)),
	SND_SOC_DAPM_MIXER("Right Mixer", SND_SOC_NOPM, 0, 0,
		&wm8987_right_mixer_controls[0],
		ARRAY_SIZE(wm8987_right_mixer_controls)),
	SND_SOC_DAPM_MIXER("Mono Mixer", WM8987_PWR2, 2, 0,
		&wm8987_mono_mixer_controls[0],
		ARRAY_SIZE(wm8987_mono_mixer_controls)),

	SND_SOC_DAPM_PGA("Right Out 2", WM8987_PWR2, 3, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Left Out 2", WM8987_PWR2, 4, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Right Out 1", WM8987_PWR2, 5, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Left Out 1", WM8987_PWR2, 6, 0, NULL, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Right Playback", WM8987_PWR2, 7, 0),
	SND_SOC_DAPM_DAC("Left DAC", "Left Playback", WM8987_PWR2, 8, 0),

	SND_SOC_DAPM_MICBIAS("Mic Bias", WM8987_PWR1, 1, 0),
	SND_SOC_DAPM_ADC("Right ADC", "Right Capture", WM8987_PWR1, 2, 0),
	SND_SOC_DAPM_ADC("Left ADC", "Left Capture", WM8987_PWR1, 3, 0),

	SND_SOC_DAPM_MUX("Left PGA Mux", WM8987_PWR1, 5, 0,
		&wm8987_left_pga_controls),
	SND_SOC_DAPM_MUX("Right PGA Mux", WM8987_PWR1, 4, 0,
		&wm8987_right_pga_controls),
	SND_SOC_DAPM_MUX("Left Line Mux", SND_SOC_NOPM, 0, 0,
		&wm8987_left_line_controls),
	SND_SOC_DAPM_MUX("Right Line Mux", SND_SOC_NOPM, 0, 0,
		&wm8987_right_line_controls),

	SND_SOC_DAPM_MUX("Out3 Mux", SND_SOC_NOPM, 0, 0, &wm8987_out3_controls),
	SND_SOC_DAPM_PGA("Out 3", WM8987_PWR2, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Mono Out 1", WM8987_PWR2, 2, 0, NULL, 0),

	SND_SOC_DAPM_MUX("Differential Mux", SND_SOC_NOPM, 0, 0,
		&wm8987_diffmux_controls),
	SND_SOC_DAPM_MUX("Left ADC Mux", SND_SOC_NOPM, 0, 0,
		&wm8987_monomux_controls),
	SND_SOC_DAPM_MUX("Right ADC Mux", SND_SOC_NOPM, 0, 0,
		&wm8987_monomux_controls),

	SND_SOC_DAPM_OUTPUT("LOUT1"),
	SND_SOC_DAPM_OUTPUT("ROUT1"),
	SND_SOC_DAPM_OUTPUT("LOUT2"),
	SND_SOC_DAPM_OUTPUT("ROUT2"),
	SND_SOC_DAPM_OUTPUT("MONO1"),
	SND_SOC_DAPM_OUTPUT("OUT3"),
	SND_SOC_DAPM_OUTPUT("VREF"),

	SND_SOC_DAPM_INPUT("LINPUT1"),
	SND_SOC_DAPM_INPUT("LINPUT2"),
	SND_SOC_DAPM_INPUT("LINPUT3"),
	SND_SOC_DAPM_INPUT("RINPUT1"),
	SND_SOC_DAPM_INPUT("RINPUT2"),
	SND_SOC_DAPM_INPUT("RINPUT3"),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* left mixer */
	{"Left Mixer", "Playback Switch", "Left DAC"},
	{"Left Mixer", "Left Bypass Switch", "Left Line Mux"},
	{"Left Mixer", "Right Playback Switch", "Right DAC"},
	{"Left Mixer", "Right Bypass Switch", "Right Line Mux"},

	/* right mixer */
	{"Right Mixer", "Left Playback Switch", "Left DAC"},
	{"Right Mixer", "Left Bypass Switch", "Left Line Mux"},
	{"Right Mixer", "Playback Switch", "Right DAC"},
	{"Right Mixer", "Right Bypass Switch", "Right Line Mux"},

	/* left out 1 */
	{"Left Out 1", NULL, "Left Mixer"},
	{"LOUT1", NULL, "Left Out 1"},

	/* left out 2 */
	{"Left Out 2", NULL, "Left Mixer"},
	{"LOUT2", NULL, "Left Out 2"},

	/* right out 1 */
	{"Right Out 1", NULL, "Right Mixer"},
	{"ROUT1", NULL, "Right Out 1"},

	/* right out 2 */
	{"Right Out 2", NULL, "Right Mixer"},
	{"ROUT2", NULL, "Right Out 2"},

	/* mono mixer */
	{"Mono Mixer", "Left Playback Switch", "Left DAC"},
	{"Mono Mixer", "Left Bypass Switch", "Left Line Mux"},
	{"Mono Mixer", "Right Playback Switch", "Right DAC"},
	{"Mono Mixer", "Right Bypass Switch", "Right Line Mux"},

	/* mono out */
	{"Mono Out 1", NULL, "Mono Mixer"},
	{"MONO1", NULL, "Mono Out 1"},

	/* out 3 */
	{"Out3 Mux", "VREF", "VREF"},
	{"Out3 Mux", "ROUT1 + Vol", "ROUT1"},
	{"Out3 Mux", "ROUT1", "Right Mixer"},
	{"Out3 Mux", "MonoOut", "MONO1"},
	{"Out 3", NULL, "Out3 Mux"},
	{"OUT3", NULL, "Out 3"},

	/* Left Line Mux */
	{"Left Line Mux", "Line 1", "LINPUT1"},
	{"Left Line Mux", "Line 2", "LINPUT2"},
	{"Left Line Mux", "Line 3", "LINPUT3"},
	{"Left Line Mux", "PGA", "Left PGA Mux"},
	{"Left Line Mux", "Differential", "Differential Mux"},

	/* Right Line Mux */
	{"Right Line Mux", "Line 1", "RINPUT1"},
	{"Right Line Mux", "Line 2", "RINPUT2"},
	{"Right Line Mux", "Line 3", "RINPUT3"},
	{"Right Line Mux", "PGA", "Right PGA Mux"},
	{"Right Line Mux", "Differential", "Differential Mux"},

	/* Left PGA Mux */
	{"Left PGA Mux", "Line 1", "LINPUT1"},
	{"Left PGA Mux", "Line 2", "LINPUT2"},
	{"Left PGA Mux", "Line 3", "LINPUT3"},
	{"Left PGA Mux", "Differential", "Differential Mux"},

	/* Right PGA Mux */
	{"Right PGA Mux", "Line 1", "RINPUT1"},
	{"Right PGA Mux", "Line 2", "RINPUT2"},
	{"Right PGA Mux", "Line 3", "RINPUT3"},
	{"Right PGA Mux", "Differential", "Differential Mux"},

	/* Differential Mux */
	{"Differential Mux", "Line 1", "LINPUT1"},
	{"Differential Mux", "Line 1", "RINPUT1"},
	{"Differential Mux", "Line 2", "LINPUT2"},
	{"Differential Mux", "Line 2", "RINPUT2"},

	/* Left ADC Mux */
	{"Left ADC Mux", "Stereo", "Left PGA Mux"},
	{"Left ADC Mux", "Mono (Left)", "Left PGA Mux"},
	{"Left ADC Mux", "Digital Mono", "Left PGA Mux"},

	/* Right ADC Mux */
	{"Right ADC Mux", "Stereo", "Right PGA Mux"},
	{"Right ADC Mux", "Mono (Right)", "Right PGA Mux"},
	{"Right ADC Mux", "Digital Mono", "Right PGA Mux"},

	/* ADC */
	{"Left ADC", NULL, "Left ADC Mux"},
	{"Right ADC", NULL, "Right ADC Mux"},
};

static int wm8987_add_widgets(struct snd_soc_codec *codec)
{
	snd_soc_dapm_new_controls(codec, wm8987_dapm_widgets,
				  ARRAY_SIZE(wm8987_dapm_widgets));

	snd_soc_dapm_add_routes(codec, audio_map, ARRAY_SIZE(audio_map));

	snd_soc_dapm_new_widgets(codec);
	return 0;
}

struct _coeff_div {
	u32 mclk;
	u32 rate;
	u16 fs;
	u8 sr:5;
	u8 usb:1;
};

/* codec hifi mclk clock divider coefficients */
static const struct _coeff_div coeff_div[] = {
	/* 8k */
	{12288000, 8000, 1536, 0x6, 0x0},
	{11289600, 8000, 1408, 0x16, 0x0},
	{18432000, 8000, 2304, 0x7, 0x0},
	{16934400, 8000, 2112, 0x17, 0x0},
	{12000000, 8000, 1500, 0x6, 0x1},

	/* 11.025k */
	{11289600, 11025, 1024, 0x18, 0x0},
	{16934400, 11025, 1536, 0x19, 0x0},
	{12000000, 11025, 1088, 0x19, 0x1},

	/* 16k */
	{12288000, 16000, 768, 0xa, 0x0},
	{18432000, 16000, 1152, 0xb, 0x0},
	{12000000, 16000, 750, 0xa, 0x1},

	/* 22.05k */
	{11289600, 22050, 512, 0x1a, 0x0},
	{16934400, 22050, 768, 0x1b, 0x0},
	{12000000, 22050, 544, 0x1b, 0x1},

	/* 32k */
	{12288000, 32000, 384, 0xc, 0x0},
	{18432000, 32000, 576, 0xd, 0x0},
	{12000000, 32000, 375, 0xa, 0x1},

	/* 44.1k */
	{11289600, 44100, 256, 0x10, 0x0},
	{16934400, 44100, 384, 0x11, 0x0},
	{12000000, 44100, 272, 0x11, 0x1},

	/* 48k */
	{12288000, 48000, 256, 0x0, 0x0},
	{18432000, 48000, 384, 0x1, 0x0},
	{12000000, 48000, 250, 0x0, 0x1},

	/* 88.2k */
	{11289600, 88200, 128, 0x1e, 0x0},
	{16934400, 88200, 192, 0x1f, 0x0},
	{12000000, 88200, 136, 0x1f, 0x1},

	/* 96k */
	{12288000, 96000, 128, 0xe, 0x0},
	{18432000, 96000, 192, 0xf, 0x0},
	{12000000, 96000, 125, 0xe, 0x1},
};

static inline int get_coeff(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
			return i;
	}

	printk(KERN_ERR "wm8987: could not get coeff for mclk %d @ rate %d\n",
		mclk, rate);
	return -EINVAL;
}

static int wm8987_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8987_priv *wm8987 = codec->private_data;

	switch (freq) {
	case 11289600:
	case 12000000:
	case 12288000:
	case 16934400:
	case 18432000:
		wm8987->sysclk = freq;
		return 0;
	}
	return -EINVAL;
}

static int wm8987_set_dai_fmt(struct snd_soc_dai *codec_dai,
		unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u16 iface = 0;

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		iface = 0x0040;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface |= 0x0002;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface |= 0x0001;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface |= 0x0003;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface |= 0x0013;
		break;
	default:
		return -EINVAL;
	}

	/* clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		iface |= 0x0090;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		iface |= 0x0080;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		iface |= 0x0010;
		break;
	default:
		return -EINVAL;
	}
   
        iface |= 0x20; /* 2010-0203, added by CVKK(JC), For SmartQ5 */
   
	wm8987_write(codec, WM8987_IFACE, iface);
	return 0;
}

static int wm8987_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_device *socdev = rtd->socdev;
	struct snd_soc_codec *codec = socdev->codec;
	struct wm8987_priv *wm8987 = codec->private_data;
	u16 iface = wm8987_read_reg_cache(codec, WM8987_IFACE) & 0x1f3;
	u16 srate = wm8987_read_reg_cache(codec, WM8987_SRATE) & 0x1c0;
	int coeff = get_coeff(wm8987->sysclk, params_rate(params));

	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		iface |= 0x0004;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		iface |= 0x0008;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		iface |= 0x000c;
		break;
	}

	/* set iface & srate */
	wm8987_write(codec, WM8987_IFACE, iface);
	if (coeff >= 0)
		wm8987_write(codec, WM8987_SRATE, srate |
			(coeff_div[coeff].sr << 1) | coeff_div[coeff].usb);

	return 0;
}

static int wm8987_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	u16 mute_reg = wm8987_read_reg_cache(codec, WM8987_ADCDAC) & 0xfff7;

	if (mute)
		wm8987_write(codec, WM8987_ADCDAC, mute_reg | 0x8);
	else
		wm8987_write(codec, WM8987_ADCDAC, mute_reg);
	return 0;
}

static int wm8987_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level level)
{
	u16 pwr_reg = wm8987_read_reg_cache(codec, WM8987_PWR1) & 0xfe3e;

	switch (level) {
	case SND_SOC_BIAS_ON:
		/* set vmid to 50k and unmute dac */
		wm8987_write(codec, WM8987_PWR1, pwr_reg | 0x00c0);
		break;
	case SND_SOC_BIAS_PREPARE:
		/* set vmid to 5k for quick power up */
		wm8987_write(codec, WM8987_PWR1, pwr_reg | 0x01c1);
		break;
	case SND_SOC_BIAS_STANDBY:
		/* mute dac and set vmid to 500k, enable VREF */
		wm8987_write(codec, WM8987_PWR1, pwr_reg | 0x0141);
		break;
	case SND_SOC_BIAS_OFF:
		wm8987_write(codec, WM8987_PWR1, 0x0001);
		break;
	}
	codec->bias_level = level;
	return 0;
}

#define WM8987_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |\
	SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_44100 | \
	SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000)

#define WM8987_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
	SNDRV_PCM_FMTBIT_S24_LE)

struct snd_soc_dai wm8987_dai = {
	.name = "WM8987",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = WM8987_RATES,
		.formats = WM8987_FORMATS,},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = WM8987_RATES,
		.formats = WM8987_FORMATS,},
	.ops = {
		.hw_params = wm8987_pcm_hw_params,
		.digital_mute = wm8987_mute,
		.set_fmt = wm8987_set_dai_fmt,
		.set_sysclk = wm8987_set_dai_sysclk,
	},
};
EXPORT_SYMBOL_GPL(wm8987_dai);

static void wm8987_work(struct work_struct *work)
{
	struct snd_soc_codec *codec =
		container_of(work, struct snd_soc_codec, delayed_work.work);
	wm8987_set_bias_level(codec, codec->bias_level);
}

static int wm8987_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;

	wm8987_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int wm8987_resume(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;
	int i;
	u8 data[2];
	u16 *cache = codec->reg_cache;

	/* Sync reg_cache with the hardware */
	for (i = 0; i < ARRAY_SIZE(wm8987_reg); i++) {
		if (i == WM8987_RESET)
			continue;
		data[0] = (i << 1) | ((cache[i] >> 8) & 0x0001);
		data[1] = cache[i] & 0x00ff;
		codec->hw_write(codec->control_data, data, 2);
	}

	wm8987_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/* charge wm8987 caps */
	if (codec->suspend_bias_level == SND_SOC_BIAS_ON) {
		wm8987_set_bias_level(codec, SND_SOC_BIAS_PREPARE);
		codec->bias_level = SND_SOC_BIAS_ON;
		schedule_delayed_work(&codec->delayed_work,
					msecs_to_jiffies(1000));
	}

	return 0;
}

/*
 * initialise the WM8987 driver
 * register the mixer and dsp interfaces with the kernel
 */
static int wm8987_init(struct snd_soc_device *socdev)
{
	struct snd_soc_codec *codec = socdev->codec;
	int reg, ret = 0;

	codec->name = "WM8987";
	codec->owner = THIS_MODULE;
	codec->read = wm8987_read_reg_cache;
	codec->write = wm8987_write;
	codec->set_bias_level = wm8987_set_bias_level;
	codec->dai = &wm8987_dai;
	codec->num_dai = 1;
	codec->reg_cache_size = ARRAY_SIZE(wm8987_reg);
	codec->reg_cache = kmemdup(wm8987_reg, sizeof(wm8987_reg), GFP_KERNEL);
	if (codec->reg_cache == NULL)
		return -ENOMEM;

	wm8987_reset(codec);

	/* register pcms */
	ret = snd_soc_new_pcms(socdev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1);
	if (ret < 0) {
		printk(KERN_ERR "wm8987: failed to create pcms\n");
		goto pcm_err;
	}

	/* charge output caps */
	wm8987_set_bias_level(codec, SND_SOC_BIAS_PREPARE);
	codec->bias_level = SND_SOC_BIAS_STANDBY;
	schedule_delayed_work(&codec->delayed_work, msecs_to_jiffies(1000));

        reg = wm8987_read_reg_cache(codec, WM8987_ADCDAC)  & 0xfff7;
        wm8987_write(codec, WM8987_ADCDAC, reg |  0x008); // mute 

        reg = wm8987_read_reg_cache(codec, WM8987_LOUTM1);
        wm8987_write(codec, WM8987_LOUTM1, reg |  0x0100);
        reg = wm8987_read_reg_cache(codec, WM8987_ROUTM2);
        wm8987_write(codec, WM8987_ROUTM2, reg |  0x0100);
   
        reg = wm8987_read_reg_cache(codec, WM8987_ADCTL1);
        wm8987_write(codec, WM8987_ADCTL1, reg |  0x0005);
        reg = wm8987_read_reg_cache(codec, WM8987_ADCTL2);
   
        reg = wm8987_read_reg_cache(codec, WM8987_LADCIN);
        wm8987_write(codec, WM8987_LADCIN, reg |  0x0060);
        /* digitial ADC volume control: R21, R22 */
        reg = wm8987_read_reg_cache(codec, WM8987_LADC);
//        wm8987_write(codec, WM8987_LADC, reg |  0x0100);
        wm8987_write(codec, WM8987_LADC, reg |  0x01c3);
        reg = wm8987_read_reg_cache(codec, WM8987_RADC);
//        wm8987_write(codec, WM8987_RADC, reg |  0x0100);
        wm8987_write(codec, WM8987_RADC, reg |  0x01c3);

        /* set the update bits */
        /* digital DAC volume control : R10 , R11 */
	reg = wm8987_read_reg_cache(codec, WM8987_LDAC);
//	wm8987_write(codec, WM8987_LDAC, reg | 0x01c7);
	wm8987_write(codec, WM8987_LDAC, reg | 0x01ff);
	reg = wm8987_read_reg_cache(codec, WM8987_RDAC);
//      wm8987_write(codec, WM8987_RDAC, reg | 0x01c7);
        wm8987_write(codec, WM8987_RDAC, reg | 0x01ff);
        /* PGA control : R0 , R1 */
        reg = wm8987_read_reg_cache(codec, WM8987_LINVOL);
//        wm8987_write(codec, WM8987_LINVOL, (reg & ~0x080) | 0x0140);
        wm8987_write(codec, WM8987_LINVOL, (reg & ~0x080) | 0x0157);
        reg = wm8987_read_reg_cache(codec, WM8987_RINVOL);
//        wm8987_write(codec, WM8987_RINVOL, (reg & ~0x080) | 0x0140);
        wm8987_write(codec, WM8987_RINVOL, (reg & ~0x080) | 0x0157);
   
        reg = wm8987_read_reg_cache(codec, WM8987_3D);
        wm8987_write(codec, WM8987_3D, (reg | 0x80));
   
        wm8987_write(codec, WM8987_LOUT2V, 0x079);
        wm8987_write(codec, WM8987_ROUT2V, 0x17a);
   
	wm8987_add_controls(codec);
	wm8987_add_widgets(codec);
	ret = snd_soc_init_card(socdev);
	if (ret < 0) {
		printk(KERN_ERR "wm8987: failed to register card\n");
		goto card_err;
	}
	return ret;

card_err:
	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);
pcm_err:
	kfree(codec->reg_cache);
	return ret;
}

/* If the i2c layer weren't so broken, we could pass this kind of data
   around */
static struct snd_soc_device *wm8987_socdev;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)

/*
 * WM8987 2 wire address is determined by GPIO5
 * state during powerup.
 *    low  = 0x1a
 *    high = 0x1b
 */

static int wm8987_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct snd_soc_device *socdev = wm8987_socdev;
	struct snd_soc_codec *codec = socdev->codec;
	int ret;

	i2c_set_clientdata(i2c, codec);
	codec->control_data = i2c;

	ret = wm8987_init(socdev);
	if (ret < 0)
		pr_err("failed to initialise WM8987\n");

	return ret;
}

static int wm8987_i2c_remove(struct i2c_client *client)
{
	struct snd_soc_codec *codec = i2c_get_clientdata(client);
	kfree(codec->reg_cache);
	return 0;
}

static const struct i2c_device_id wm8987_i2c_id[] = {
	{ "wm8987", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wm8987_i2c_id);

static struct i2c_driver wm8987_i2c_driver = {
	.driver = {
		.name = "WM8987 I2C Codec",
		.owner = THIS_MODULE,
	},
	.probe =    wm8987_i2c_probe,
	.remove =   wm8987_i2c_remove,
	.id_table = wm8987_i2c_id,
};

static int wm8987_add_i2c_device(struct platform_device *pdev,
				 const struct wm8987_setup_data *setup)
{
	struct i2c_board_info info;
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	int ret;

	ret = i2c_add_driver(&wm8987_i2c_driver);
	if (ret != 0) {
		dev_err(&pdev->dev, "can't add i2c driver\n");
		return ret;
	}

	memset(&info, 0, sizeof(struct i2c_board_info));
	info.addr = setup->i2c_address;
	strlcpy(info.type, "wm8987", I2C_NAME_SIZE);

	adapter = i2c_get_adapter(setup->i2c_bus);
	if (!adapter) {
		dev_err(&pdev->dev, "can't get i2c adapter %d\n",
			setup->i2c_bus);
		goto err_driver;
	}

	client = i2c_new_device(adapter, &info);
	i2c_put_adapter(adapter);
	if (!client) {
		dev_err(&pdev->dev, "can't add i2c device at 0x%x\n",
			(unsigned int)info.addr);
		goto err_driver;
	}

	return 0;

err_driver:
	i2c_del_driver(&wm8987_i2c_driver);
	return -ENODEV;
}
#endif

#if defined(CONFIG_SPI_MASTER)
static int __devinit wm8987_spi_probe(struct spi_device *spi)
{
	struct snd_soc_device *socdev = wm8987_socdev;
	struct snd_soc_codec *codec = socdev->codec;
	int ret;

	codec->control_data = spi;

	ret = wm8987_init(socdev);
	if (ret < 0)
		dev_err(&spi->dev, "failed to initialise WM8987\n");

	return ret;
}

static int __devexit wm8987_spi_remove(struct spi_device *spi)
{
	return 0;
}

static struct spi_driver wm8987_spi_driver = {
	.driver = {
		.name	= "wm8987",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.probe		= wm8987_spi_probe,
	.remove		= __devexit_p(wm8987_spi_remove),
};

static int wm8987_spi_write(struct spi_device *spi, const char *data, int len)
{
	struct spi_transfer t;
	struct spi_message m;
	u8 msg[2];

	if (len <= 0)
		return 0;

	msg[0] = data[0];
	msg[1] = data[1];

	spi_message_init(&m);
	memset(&t, 0, (sizeof t));

	t.tx_buf = &msg[0];
	t.len = len;

	spi_message_add_tail(&t, &m);
	spi_sync(spi, &m);

	return len;
}
#endif

static int wm8987_probe(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct wm8987_setup_data *setup = socdev->codec_data;
	struct snd_soc_codec *codec;
	struct wm8987_priv *wm8987;
	int ret;

	pr_info("WM8987 Audio Codec %s", WM8987_VERSION);
	codec = kzalloc(sizeof(struct snd_soc_codec), GFP_KERNEL);
	if (codec == NULL)
		return -ENOMEM;

	wm8987 = kzalloc(sizeof(struct wm8987_priv), GFP_KERNEL);
	if (wm8987 == NULL) {
		kfree(codec);
		return -ENOMEM;
	}

	codec->private_data = wm8987;
	socdev->codec = codec;
	mutex_init(&codec->mutex);
	INIT_LIST_HEAD(&codec->dapm_widgets);
	INIT_LIST_HEAD(&codec->dapm_paths);
	wm8987_socdev = socdev;
	INIT_DELAYED_WORK(&codec->delayed_work, wm8987_work);

	ret = -ENODEV;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	if (setup->i2c_address) {
		codec->hw_write = (hw_write_t)i2c_master_send;
		ret = wm8987_add_i2c_device(pdev, setup);
	}
#endif
#if defined(CONFIG_SPI_MASTER)
	if (setup->spi) {
		codec->hw_write = (hw_write_t)wm8987_spi_write;
		ret = spi_register_driver(&wm8987_spi_driver);
		if (ret != 0)
			printk(KERN_ERR "can't add spi driver");
	}
#endif

	if (ret != 0) {
		kfree(codec->private_data);
		kfree(codec);
	}
	return ret;
}

/*
 * This function forces any delayed work to be queued and run.
 */
static int run_delayed_work(struct delayed_work *dwork)
{
	int ret;

	/* cancel any work waiting to be queued. */
	ret = cancel_delayed_work(dwork);

	/* if there was any work waiting then we run it now and
	 * wait for it's completion */
	if (ret) {
		schedule_delayed_work(dwork, 0);
		flush_scheduled_work();
	}
	return ret;
}

/* power down chip */
static int wm8987_remove(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;

	if (codec->control_data)
		wm8987_set_bias_level(codec, SND_SOC_BIAS_OFF);
	run_delayed_work(&codec->delayed_work);
	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	i2c_unregister_device(codec->control_data);
	i2c_del_driver(&wm8987_i2c_driver);
#endif
#if defined(CONFIG_SPI_MASTER)
	spi_unregister_driver(&wm8987_spi_driver);
#endif
	kfree(codec->private_data);
	kfree(codec);

	return 0;
}

struct snd_soc_codec_device soc_codec_dev_wm8987 = {
	.probe = 	wm8987_probe,
	.remove = 	wm8987_remove,
	.suspend = 	wm8987_suspend,
	.resume =	wm8987_resume,
};
EXPORT_SYMBOL_GPL(soc_codec_dev_wm8987);

static int __init wm8987_modinit(void)
{
	return snd_soc_register_dai(&wm8987_dai);
}
module_init(wm8987_modinit);

static void __exit wm8987_exit(void)
{
	snd_soc_unregister_dai(&wm8987_dai);
}
module_exit(wm8987_exit);

MODULE_DESCRIPTION("ASoC WM8987 driver");
MODULE_AUTHOR("Liam Girdwood");
MODULE_LICENSE("GPL");
