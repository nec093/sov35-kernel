// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * MSM8996 + WCD9335 (tasha) ASoC machine driver.
 *
 * Port of the kernel-4.9 techpack msm8996.c (snd_soc_codec/snd_soc_platform
 * API) to the 5.4 audio-kernel (snd_soc_component API, SND_SOC_DAILINK_DEFS).
 * Card name, DAI link names/stream names/ids and order (i.e. PCM device
 * numbers), mixer controls and DAPM widgets are kept identical to the 4.9
 * driver, because the Android audio HAL and mixer_paths depend on them.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/pm_qos.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/info.h>
#include <soc/snd_event.h>
#include <dsp/q6afe-v2.h>
#include <dsp/q6core.h>
#include "msm-pcm-routing-v2.h"
#include "asoc/wcd-mbhc-v2.h"
#include "codecs/wcd9335.h"
#include "codecs/wsa881x.h"
#if IS_ENABLED(CONFIG_SND_SOC_WSA881X)
/*
 * <soc/soundwire.h> clashes with this kernel's mod_devicetable.h
 * (struct swr_device_id), so only the soundwire bus type is declared here.
 */
extern struct bus_type soundwire_type;

/*
 * Values of <dt-bindings/sound/audio-codec-port-types.h>, which is not
 * shipped in this tree. The tasha soundwire master (swr-wcd-ctrl) uses its
 * own master port types and ignores these; they are only passed on to
 * wsa881x_set_channel_map().
 */
#ifndef SPKR_L
#define SPKR_L		1
#define SPKR_L_COMP	2
#define SPKR_L_BOOST	3
#define SPKR_L_VI	4
#define SPKR_R		5
#define SPKR_R_COMP	6
#define SPKR_R_BOOST	7
#define SPKR_R_VI	8
#endif
#endif

#define DRV_NAME "msm8996-asoc-snd"

#define SAMPLING_RATE_8KHZ      8000
#define SAMPLING_RATE_16KHZ     16000
#define SAMPLING_RATE_32KHZ     32000
#define SAMPLING_RATE_48KHZ     48000
#define SAMPLING_RATE_96KHZ     96000
#define SAMPLING_RATE_192KHZ    192000
#define SAMPLING_RATE_44P1KHZ   44100

#define MSM8996_SPK_ON     1
#define MSM8996_HIFI_ON    1

#define WCD9XXX_MBHC_DEF_BUTTONS    8
#define WCD9XXX_MBHC_DEF_RLOADS     5
#define CODEC_EXT_CLK_RATE         9600000
#define ADSP_STATE_READY_TIMEOUT_MS    3000
#define DEV_NAME_STR_LEN            32

#define WSA8810_NAME_1 "wsa881x.20170211"
#define WSA8810_NAME_2 "wsa881x.20170212"

#define TASHA_CODEC_NAME "tasha_codec"

static int slim0_rx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim0_tx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim1_tx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim0_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int slim0_tx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int slim1_tx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int hdmi_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int msm8996_auxpcm_rate = SAMPLING_RATE_8KHZ;
static int slim5_rx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim5_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int slim6_rx_sample_rate = SAMPLING_RATE_48KHZ;
static int slim6_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;

static int msm8996_spk_control = 1;
static int msm_slim_0_rx_ch = 1;
static int msm_slim_0_tx_ch = 1;
static int msm_slim_1_tx_ch = 1;
static int msm_slim_5_rx_ch = 1;
static int msm_slim_6_rx_ch = 1;
static int msm_hifi_control;
static int msm_vi_feed_tx_ch = 2;

static int msm_hdmi_rx_ch = 2;
static int msm_proxy_rx_ch = 2;
static int hdmi_rx_sample_rate = SAMPLING_RATE_48KHZ;
static int msm_tert_mi2s_tx_ch = 2;
static int pm8996_ear_enable_states;

static bool codec_reg_done;

static const char *const hifi_function[] = {"Off", "On"};
static const char *const spk_function[] = {"Off", "On"};
static const char *const slim0_rx_ch_text[] = {"One", "Two"};
static const char *const slim5_rx_ch_text[] = {"One", "Two"};
static const char *const slim6_rx_ch_text[] = {"One", "Two"};
static const char *const slim0_tx_ch_text[] = {"One", "Two", "Three", "Four",
						"Five", "Six", "Seven",
						"Eight"};
static const char *const vi_feed_ch_text[] = {"One", "Two"};
static char const *hdmi_rx_ch_text[] = {"Two", "Three", "Four", "Five",
					"Six", "Seven", "Eight"};
static char const *rx_bit_format_text[] = {"S16_LE", "S24_LE", "S24_3LE"};
static char const *slim5_rx_bit_format_text[] = {"S16_LE", "S24_LE", "S24_3LE"};
static char const *slim6_rx_bit_format_text[] = {"S16_LE", "S24_LE", "S24_3LE"};
static char const *slim0_rx_sample_rate_text[] = {"KHZ_48", "KHZ_96",
					"KHZ_192", "KHZ_44P1", "KHZ_8",
					"KHZ_16", "KHZ_32"};
static char const *slim5_rx_sample_rate_text[] = {"KHZ_48", "KHZ_96",
						  "KHZ_192", "KHZ_44P1"};
static char const *slim6_rx_sample_rate_text[] = {"KHZ_48", "KHZ_96",
						  "KHZ_192", "KHZ_44P1"};
static const char *const proxy_rx_ch_text[] = {"One", "Two", "Three", "Four",
	"Five", "Six", "Seven", "Eight"};

static char const *hdmi_rx_sample_rate_text[] = {"KHZ_48", "KHZ_96",
					"KHZ_192"};
static const char *const pm8996_ear_enable_states_text[] = {"Disable", "Enable"};

static const char *const auxpcm_rate_text[] = {"8000", "16000"};
static const struct soc_enum msm8996_auxpcm_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, auxpcm_rate_text),
};

static struct afe_clk_set mi2s_tx_clk = {
	AFE_API_VERSION_I2S_CONFIG,
	Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
	Q6AFE_LPASS_IBIT_CLK_1_P536_MHZ,
	Q6AFE_LPASS_CLK_ATTRIBUTE_COUPLE_NO,
	Q6AFE_LPASS_CLK_ROOT_DEFAULT,
	0,
};

struct msm8996_wsa881x_dev_info {
	struct device_node *of_node;
	u32 index;
};

struct msm8996_asoc_mach_data {
	u32 mclk_freq;
	int us_euro_gpio;
	int hph_en1_gpio;
	int hph_en0_gpio;
	int ear_en_gpio;
	int ext_us_amp_gpio;
	bool us_euro_gpio_requested;
	bool hph_en1_gpio_requested;
	bool hph_en0_gpio_requested;
	bool ext_us_amp_gpio_requested;
	bool is_afe_config_done;
	bool ssr_master_registered;
	struct snd_info_entry *codec_root;
};

struct msm8996_asoc_wcd93xx_codec {
	void* (*get_afe_config_fn)(struct snd_soc_component *component,
				   enum afe_config_type config_type);
	void (*mbhc_hs_detect_exit)(struct snd_soc_component *component);
};

static struct msm8996_asoc_wcd93xx_codec msm8996_codec_fn;

static void *def_tasha_mbhc_cal(void);
static int msm_snd_enable_codec_ext_clk(struct snd_soc_component *component,
					int enable, bool dapm);

/*
 * Need to report LINEIN
 * if R/L channel impedance is larger than 5K ohm
 */
static struct wcd_mbhc_config wcd_mbhc_cfg = {
	.read_fw_bin = false,
	.calibration = NULL,
	.detect_extn_cable = true,
	.mono_stero_detection = false,
	.swap_gnd_mic = NULL,
	.hs_ext_micbias = true,
	.key_code[0] = KEY_MEDIA,
	.key_code[1] = KEY_VOICECOMMAND,
	.key_code[2] = KEY_VOLUMEUP,
	.key_code[3] = KEY_VOLUMEDOWN,
	.key_code[4] = 0,
	.key_code[5] = 0,
	.key_code[6] = 0,
	.key_code[7] = 0,
	.linein_th = 5000,
	.moisture_en = true,
	.mbhc_micbias = MIC_BIAS_2,
	.anc_micbias = MIC_BIAS_3,
	.enable_anc_mic_detect = true,
};

static inline int param_is_mask(int p)
{
	return (p >= SNDRV_PCM_HW_PARAM_FIRST_MASK) &&
			(p <= SNDRV_PCM_HW_PARAM_LAST_MASK);
}

static inline struct snd_mask *param_to_mask(struct snd_pcm_hw_params *p,
					     int n)
{
	return &(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]);
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
	if (bit >= SNDRV_MASK_MAX)
		return;
	if (param_is_mask(n)) {
		struct snd_mask *m = param_to_mask(p, n);

		m->bits[0] = 0;
		m->bits[1] = 0;
		m->bits[bit >> 5] |= (1 << (bit & 31));
	}
}

static void msm8996_ext_control(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm =
			snd_soc_component_get_dapm(component);

	pr_debug("%s: msm8996_spk_control = %d", __func__,
		 msm8996_spk_control);
	if (msm8996_spk_control == MSM8996_SPK_ON) {
		snd_soc_dapm_enable_pin(dapm, "Lineout_1 amp");
		snd_soc_dapm_enable_pin(dapm, "Lineout_2 amp");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Lineout_1 amp");
		snd_soc_dapm_disable_pin(dapm, "Lineout_2 amp");
	}
	snd_soc_dapm_sync(dapm);
}

static int msm8996_get_spk(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8996_spk_control = %d\n",
			 __func__, msm8996_spk_control);
	ucontrol->value.integer.value[0] = msm8996_spk_control;
	return 0;
}

static int msm8996_set_spk(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
			snd_soc_kcontrol_component(kcontrol);

	pr_debug("%s() ucontrol->value.integer.value[0] = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	if (msm8996_spk_control == ucontrol->value.integer.value[0])
		return 0;

	msm8996_spk_control = ucontrol->value.integer.value[0];
	msm8996_ext_control(component);
	return 1;
}

static int msm8996_hifi_ctrl(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm =
			snd_soc_component_get_dapm(component);
	struct snd_soc_card *card = component->card;
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);

	pr_debug("%s: msm_hifi_control = %d", __func__,
		 msm_hifi_control);
	if (!pdata || !gpio_is_valid(pdata->hph_en1_gpio)) {
		pr_err("%s: hph_en1_gpio is invalid\n", __func__);
		return -EINVAL;
	}
	if (msm_hifi_control == MSM8996_HIFI_ON) {
		gpio_direction_output(pdata->hph_en1_gpio, 1);
		/* 5msec delay needed as per HW requirement */
		usleep_range(5000, 5010);
	} else {
		gpio_direction_output(pdata->hph_en1_gpio, 0);
	}
	snd_soc_dapm_sync(dapm);
	return 0;
}

static int msm8996_hifi_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_hifi_control = %d\n",
			 __func__, msm_hifi_control);
	ucontrol->value.integer.value[0] = msm_hifi_control;
	return 0;
}

static int msm8996_hifi_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
			snd_soc_kcontrol_component(kcontrol);

	pr_debug("%s() ucontrol->value.integer.value[0] = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);

	msm_hifi_control = ucontrol->value.integer.value[0];
	msm8996_hifi_ctrl(component);
	return 1;
}

static int msm8996_ext_us_amp_init(struct snd_soc_card *card)
{
	struct msm8996_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	int ret = 0;

	/* The card may be re-bound without a new probe: request only once */
	if (pdata->ext_us_amp_gpio_requested)
		return 0;

	pdata->ext_us_amp_gpio = of_get_named_gpio(card->dev->of_node,
				"qcom,ext-ult-spk-amp-gpio", 0);
	if (gpio_is_valid(pdata->ext_us_amp_gpio)) {
		ret = gpio_request(pdata->ext_us_amp_gpio, "ext_us_amp_gpio");
		if (ret) {
			pr_err("%s: ext_us_amp_gpio request failed, ret:%d\n",
				__func__, ret);
			return ret;
		}
		pdata->ext_us_amp_gpio_requested = true;
		gpio_direction_output(pdata->ext_us_amp_gpio, 0);
	}
	return ret;
}

static int msm_ext_ultrasound_event(struct snd_soc_dapm_widget *w,
				    struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct msm8996_asoc_mach_data *pdata =
			snd_soc_card_get_drvdata(component->card);

	pr_debug("%s()\n", __func__);
	/*
	 * The 4.9 driver had this test inverted (strcmp() != 0), which made
	 * the only widget using this event ("ultrasound amp") always fail.
	 */
	if (!strcmp(w->name, "ultrasound amp")) {
		if (!pdata || !gpio_is_valid(pdata->ext_us_amp_gpio)) {
			pr_err("%s: ext_us_amp_gpio isn't configured\n",
				__func__);
			return -EINVAL;
		}
		gpio_direction_output(pdata->ext_us_amp_gpio,
				      SND_SOC_DAPM_EVENT_ON(event) ? 1 : 0);
		pr_debug("%s: US Emitter GPIO enable:%s\n", __func__,
			 SND_SOC_DAPM_EVENT_ON(event) ? "Enable" : "Disable");
	} else {
		pr_err("%s() Invalid Widget = %s\n",
				__func__, w->name);
		return -EINVAL;
	}
	return 0;
}

static int msm_snd_enable_codec_ext_clk(struct snd_soc_component *component,
					int enable, bool dapm)
{
	int ret = 0;

	if (!strcmp(dev_name(component->dev), TASHA_CODEC_NAME)) {
		ret = tasha_cdc_mclk_enable(component, enable, dapm);
	} else {
		dev_err(component->dev, "%s: unknown codec to enable ext clk\n",
			__func__);
		ret = -EINVAL;
	}

	return ret;
}

static int msm8996_mclk_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);

	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return msm_snd_enable_codec_ext_clk(component, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return msm_snd_enable_codec_ext_clk(component, 0, true);
	}
	return 0;
}

static int msm_snd_enable_codec_ext_tx_clk(struct snd_soc_component *component,
					   int enable, bool dapm)
{
	int ret = 0;

	if (!strcmp(dev_name(component->dev), TASHA_CODEC_NAME)) {
		ret = tasha_cdc_mclk_tx_enable(component, enable, dapm);
	} else {
		dev_err(component->dev, "%s: unknown codec to enable ext clk\n",
			__func__);
		ret = -EINVAL;
	}
	return ret;
}

static int msm8996_mclk_tx_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);

	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return msm_snd_enable_codec_ext_tx_clk(component, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return msm_snd_enable_codec_ext_tx_clk(component, 0, true);
	}
	return 0;
}

static int msm_hifi_ctrl_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct snd_soc_card *card = component->card;
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);
	int ret = 0;

	pr_debug("%s: msm_hifi_control = %d", __func__,
		 msm_hifi_control);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (msm_hifi_control == MSM8996_HIFI_ON) {
			if (!pdata || !gpio_is_valid(pdata->hph_en0_gpio)) {
				pr_err("%s: hph_en0_gpio is invalid\n",
					__func__);
				ret = -EINVAL;
				goto err;
			}
			gpio_direction_output(pdata->hph_en0_gpio, 1);
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (msm_hifi_control == MSM8996_HIFI_ON) {
			if (!pdata || !gpio_is_valid(pdata->hph_en0_gpio)) {
				pr_err("%s: hph_en0_gpio is invalid\n",
					__func__);
				ret = -EINVAL;
				goto err;
			}
			gpio_direction_output(pdata->hph_en0_gpio, 0);
		}
		break;
	}
err:
	return ret;
}

static const struct snd_soc_dapm_widget msm8996_dapm_widgets[] = {

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	msm8996_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("MCLK TX",  SND_SOC_NOPM, 0, 0,
	msm8996_mclk_tx_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SPK("Lineout_1 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_3 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_2 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_4 amp", NULL),
	SND_SOC_DAPM_SPK("ultrasound amp", msm_ext_ultrasound_event),
	SND_SOC_DAPM_SPK("hifi amp", msm_hifi_ctrl_event),
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Analog Mic4", NULL),
	SND_SOC_DAPM_MIC("Analog Mic6", NULL),
	SND_SOC_DAPM_MIC("Analog Mic7", NULL),
	SND_SOC_DAPM_MIC("Analog Mic8", NULL),

	SND_SOC_DAPM_MIC("Digital Mic0", NULL),
	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
	SND_SOC_DAPM_MIC("Digital Mic6", NULL),
};

static const struct snd_soc_dapm_route wcd9335_audio_paths[] = {
	{"MIC BIAS1", NULL, "MCLK TX"},
	{"MIC BIAS2", NULL, "MCLK TX"},
	{"MIC BIAS3", NULL, "MCLK TX"},
	{"MIC BIAS4", NULL, "MCLK TX"},
};

static int slim5_rx_sample_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (slim5_rx_sample_rate) {
	case SAMPLING_RATE_44P1KHZ:
		sample_rate_val = 3;
		break;

	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;

	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;

	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: slim5_rx_sample_rate = %d\n", __func__,
		 slim5_rx_sample_rate);

	return 0;
}

static int slim5_rx_sample_rate_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ucontrol value = %ld\n", __func__,
		 ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 3:
		slim5_rx_sample_rate = SAMPLING_RATE_44P1KHZ;
		break;
	case 2:
		slim5_rx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		slim5_rx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
	default:
		slim5_rx_sample_rate = SAMPLING_RATE_48KHZ;
	}

	pr_debug("%s: slim5_rx_sample_rate = %d\n", __func__,
		 slim5_rx_sample_rate);

	return 0;
}

static int slim6_rx_sample_rate_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (slim6_rx_sample_rate) {
	case SAMPLING_RATE_44P1KHZ:
		sample_rate_val = 3;
		break;

	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;

	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;

	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: slim6_rx_sample_rate = %d\n", __func__,
		 slim6_rx_sample_rate);

	return 0;
}

static int slim6_rx_sample_rate_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 3:
		slim6_rx_sample_rate = SAMPLING_RATE_44P1KHZ;
		break;
	case 2:
		slim6_rx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		slim6_rx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
	default:
		slim6_rx_sample_rate = SAMPLING_RATE_48KHZ;
		break;
	}

	pr_debug("%s: ucontrol value = %ld, slim6_rx_sample_rate = %d\n",
		 __func__, ucontrol->value.integer.value[0],
		 slim6_rx_sample_rate);

	return 0;
}

static int slim0_tx_bit_format_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	switch (slim0_tx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: slim0_tx_bit_format = %d, ucontrol value = %ld\n",
			 __func__, slim0_tx_bit_format,
			ucontrol->value.integer.value[0]);
	return 0;
}

static int slim0_tx_bit_format_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	int rc = 0;

	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim0_tx_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		slim0_tx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
		slim0_tx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	default:
		pr_err("%s: invalid value %ld\n", __func__,
		       ucontrol->value.integer.value[0]);
		rc = -EINVAL;
		break;
	}

	pr_debug("%s: ucontrol value = %ld, slim0_tx_bit_format = %d\n",
		 __func__, ucontrol->value.integer.value[0],
		 slim0_tx_bit_format);

	return rc;
}

static int slim0_rx_sample_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (slim0_rx_sample_rate) {
	case SAMPLING_RATE_32KHZ:
		sample_rate_val = 6;
		break;

	case SAMPLING_RATE_16KHZ:
		sample_rate_val = 5;
		break;

	case SAMPLING_RATE_8KHZ:
		sample_rate_val = 4;
		break;

	case SAMPLING_RATE_44P1KHZ:
		sample_rate_val = 3;
		break;

	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;

	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;

	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: slim0_rx_sample_rate = %d\n", __func__,
		 slim0_rx_sample_rate);

	return 0;
}

static int slim0_rx_sample_rate_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ucontrol value = %ld\n", __func__,
		 ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 6:
		slim0_rx_sample_rate = SAMPLING_RATE_32KHZ;
		break;
	case 5:
		slim0_rx_sample_rate = SAMPLING_RATE_16KHZ;
		break;
	case 4:
		slim0_rx_sample_rate = SAMPLING_RATE_8KHZ;
		break;
	case 3:
		slim0_rx_sample_rate = SAMPLING_RATE_44P1KHZ;
		break;
	case 2:
		slim0_rx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		slim0_rx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
	default:
		slim0_rx_sample_rate = SAMPLING_RATE_48KHZ;
	}

	pr_debug("%s: slim0_rx_sample_rate = %d\n", __func__,
		 slim0_rx_sample_rate);

	return 0;
}

static int slim0_tx_sample_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (slim0_tx_sample_rate) {
	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;
	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;
	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: slim0_tx_sample_rate = %d\n", __func__,
				slim0_tx_sample_rate);
	return 0;
}

static int slim0_tx_sample_rate_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int rc = 0;

	pr_debug("%s: ucontrol value = %ld\n", __func__,
			ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim0_tx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		slim0_tx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
		slim0_tx_sample_rate = SAMPLING_RATE_48KHZ;
		break;
	default:
		rc = -EINVAL;
		pr_err("%s: invalid sample rate being passed\n", __func__);
		break;
	}

	pr_debug("%s: slim0_tx_sample_rate = %d\n", __func__,
			slim0_tx_sample_rate);
	return rc;
}

static int slim5_rx_bit_format_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{

	switch (slim5_rx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;

	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: slim5_rx_bit_format = %d, ucontrol value = %ld\n",
		 __func__, slim5_rx_bit_format,
			ucontrol->value.integer.value[0]);

	return 0;
}

static int slim5_rx_bit_format_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim5_rx_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		slim5_rx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		slim5_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	return 0;
}

static int slim6_rx_bit_format_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{

	switch (slim6_rx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;

	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: slim6_rx_bit_format = %d, ucontrol value = %ld\n",
		 __func__, slim6_rx_bit_format,
		 ucontrol->value.integer.value[0]);

	return 0;
}

static int slim6_rx_bit_format_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim6_rx_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		slim6_rx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		slim6_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	return 0;
}

static int slim0_rx_bit_format_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{

	switch (slim0_rx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;

	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: slim0_rx_bit_format = %d, ucontrol value = %ld\n",
		 __func__, slim0_rx_bit_format,
			ucontrol->value.integer.value[0]);

	return 0;
}

static int slim0_rx_bit_format_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 2:
		slim0_rx_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		slim0_rx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		slim0_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	return 0;
}

static int msm_slim_5_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_5_rx_ch  = %d\n", __func__,
		 msm_slim_5_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_5_rx_ch - 1;
	return 0;
}

static int msm_slim_5_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_5_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_slim_5_rx_ch = %d\n", __func__,
		 msm_slim_5_rx_ch);
	return 1;
}

static int msm_slim_6_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_6_rx_ch  = %d\n", __func__,
		 msm_slim_6_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_6_rx_ch - 1;
	return 0;
}

static int msm_slim_6_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_6_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_slim_6_rx_ch = %d\n", __func__,
		 msm_slim_6_rx_ch);
	return 1;
}

static int msm_slim_0_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_rx_ch  = %d\n", __func__,
		 msm_slim_0_rx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_rx_ch - 1;
	return 0;
}

static int msm_slim_0_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_slim_0_rx_ch = %d\n", __func__,
		 msm_slim_0_rx_ch);
	return 1;
}

static int msm_slim_0_tx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_0_tx_ch  = %d\n", __func__,
		 msm_slim_0_tx_ch);
	ucontrol->value.integer.value[0] = msm_slim_0_tx_ch - 1;
	return 0;
}

static int msm_slim_0_tx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_0_tx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_slim_0_tx_ch = %d\n", __func__, msm_slim_0_tx_ch);
	return 1;
}

static int msm_slim_1_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_slim_1_tx_ch  = %d\n", __func__,
		 msm_slim_1_tx_ch);
	ucontrol->value.integer.value[0] = msm_slim_1_tx_ch - 1;
	return 0;
}

static int msm_slim_1_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_slim_1_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_slim_1_tx_ch = %d\n", __func__, msm_slim_1_tx_ch);
	return 1;
}

static int msm_vi_feed_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_vi_feed_tx_ch - 1;
	pr_debug("%s: msm_vi_feed_tx_ch = %ld\n", __func__,
		 ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_vi_feed_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm_vi_feed_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm_vi_feed_tx_ch = %d\n", __func__, msm_vi_feed_tx_ch);
	return 1;
}

static int hdmi_rx_bit_format_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{

	switch (hdmi_rx_bit_format) {
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;

	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}

	pr_debug("%s: hdmi_rx_bit_format = %d, ucontrol value = %ld\n",
		 __func__, hdmi_rx_bit_format,
			ucontrol->value.integer.value[0]);

	return 0;
}

static int hdmi_rx_bit_format_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 2:
		hdmi_rx_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		hdmi_rx_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		hdmi_rx_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	pr_debug("%s: hdmi_rx_bit_format = %d, ucontrol value = %ld\n",
		 __func__, hdmi_rx_bit_format,
			ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_hdmi_rx_ch_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_hdmi_rx_ch  = %d\n", __func__,
		 msm_hdmi_rx_ch);
	ucontrol->value.integer.value[0] = msm_hdmi_rx_ch - 2;

	return 0;
}

static int msm_hdmi_rx_ch_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	msm_hdmi_rx_ch = ucontrol->value.integer.value[0] + 2;
	if (msm_hdmi_rx_ch > 8) {
		pr_err("%s: channels %d exceeded 8.Limiting to max chs-8\n",
			__func__, msm_hdmi_rx_ch);
		msm_hdmi_rx_ch = 8;
	}
	pr_debug("%s: msm_hdmi_rx_ch = %d\n", __func__, msm_hdmi_rx_ch);

	return 1;
}

static int hdmi_rx_sample_rate_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	int sample_rate_val = 0;

	switch (hdmi_rx_sample_rate) {
	case SAMPLING_RATE_192KHZ:
		sample_rate_val = 2;
		break;

	case SAMPLING_RATE_96KHZ:
		sample_rate_val = 1;
		break;

	case SAMPLING_RATE_48KHZ:
	default:
		sample_rate_val = 0;
		break;
	}

	ucontrol->value.integer.value[0] = sample_rate_val;
	pr_debug("%s: hdmi_rx_sample_rate = %d\n", __func__,
		 hdmi_rx_sample_rate);

	return 0;
}

static int hdmi_rx_sample_rate_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ucontrol value = %ld\n", __func__,
		 ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 2:
		hdmi_rx_sample_rate = SAMPLING_RATE_192KHZ;
		break;
	case 1:
		hdmi_rx_sample_rate = SAMPLING_RATE_96KHZ;
		break;
	case 0:
	default:
		hdmi_rx_sample_rate = SAMPLING_RATE_48KHZ;
	}

	pr_debug("%s: hdmi_rx_sample_rate = %d\n", __func__,
		 hdmi_rx_sample_rate);

	return 0;
}

static int pm8996_ear_enable_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int gpio_state = 0;

	switch (pm8996_ear_enable_states) {
	case 1:
		gpio_state = 1;
		break;
	case 0:
	default:
		gpio_state = 0;
		break;
	}

	ucontrol->value.integer.value[0] = gpio_state;
	pr_debug("%s: pm8996_ear_enable_states = %d\n", __func__,
			pm8996_ear_enable_states);

	return 0;
}

static int pm8996_ear_enable_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	struct snd_soc_component *component =
			snd_soc_kcontrol_component(kcontrol);
	struct msm8996_asoc_mach_data *pdata =
			snd_soc_card_get_drvdata(component->card);

	pr_debug("%s: ucontrol value = %ld\n", __func__,
			ucontrol->value.integer.value[0]);

	if (pdata && gpio_is_valid(pdata->ear_en_gpio)) {
		ret = gpio_request(pdata->ear_en_gpio, "ear_en_gpio");
		if (ret) {
			pr_err("%s: request ear_en_gpio failed, ret:%d\n",
				__func__, ret);
			return ret;
		}
		switch (ucontrol->value.integer.value[0]) {
		case 1:
			gpio_set_value_cansleep(pdata->ear_en_gpio, 1);
			break;
		case 0:
		default:
			gpio_set_value_cansleep(pdata->ear_en_gpio, 0);
			break;
		}
		gpio_free(pdata->ear_en_gpio);
		pm8996_ear_enable_states = ucontrol->value.integer.value[0];
	}

	return 0;
}

static int msm8996_auxpcm_rate_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm8996_auxpcm_rate;
	return 0;
}

static int msm8996_auxpcm_rate_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm8996_auxpcm_rate = SAMPLING_RATE_8KHZ;
		break;
	case 1:
		msm8996_auxpcm_rate = SAMPLING_RATE_16KHZ;
		break;
	default:
		msm8996_auxpcm_rate = SAMPLING_RATE_8KHZ;
		break;
	}
	return 0;
}

static int msm_proxy_rx_ch_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm_proxy_rx_ch = %d\n", __func__, msm_proxy_rx_ch);
	ucontrol->value.integer.value[0] = msm_proxy_rx_ch - 1;
	return 0;
}

static int msm_proxy_rx_ch_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	msm_proxy_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: msm_proxy_rx_ch = %d\n", __func__, msm_proxy_rx_ch);
	return 1;
}

static int msm_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate =
	    hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels =
	    hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm8996_auxpcm_rate;
	channels->min = channels->max = 1;

	return 0;
}

static int msm_proxy_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					   struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s: msm_proxy_rx_ch =%d\n", __func__, msm_proxy_rx_ch);

	if (channels->max < 2)
		channels->min = channels->max = 2;
	channels->min = channels->max = msm_proxy_rx_ch;
	rate->min = rate->max = SAMPLING_RATE_48KHZ;
	return 0;
}

static int msm_proxy_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					   struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	rate->min = rate->max = SAMPLING_RATE_48KHZ;
	return 0;
}

static int msm8996_hdmi_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					   struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s channels->min %u channels->max %u ()\n", __func__,
		 channels->min, channels->max);

	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
				hdmi_rx_bit_format);
	if (channels->max < 2)
		channels->min = channels->max = 2;
	rate->min = rate->max = hdmi_rx_sample_rate;
	channels->min = channels->max = msm_hdmi_rx_ch;

	return 0;
}

static int msm_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s: channel:%d\n", __func__, msm_tert_mi2s_tx_ch);
	rate->min = rate->max = SAMPLING_RATE_48KHZ;
	channels->min = channels->max = msm_tert_mi2s_tx_ch;
	return 0;
}

static int msm8996_mi2s_snd_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s: substream = %s  stream = %d\n", __func__,
		 substream->name, substream->stream);

	mi2s_tx_clk.enable = 1;
	ret = afe_set_lpass_clock_v2(AFE_PORT_ID_TERTIARY_MI2S_TX,
				&mi2s_tx_clk);
	if (ret < 0) {
		pr_err("%s: afe lpass clock failed, err:%d\n", __func__, ret);
		goto err;
	}
	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0)
		pr_err("%s: set fmt cpu dai failed, err:%d\n", __func__, ret);
err:
	return ret;
}

static void msm8996_mi2s_snd_shutdown(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_debug("%s: substream = %s  stream = %d\n", __func__,
		substream->name, substream->stream);

	mi2s_tx_clk.enable = 0;
	ret = afe_set_lpass_clock_v2(AFE_PORT_ID_TERTIARY_MI2S_TX,
				&mi2s_tx_clk);
	if (ret < 0)
		pr_err("%s: afe lpass clock failed, err:%d\n", __func__, ret);
}

static const struct snd_soc_ops msm8996_mi2s_be_ops = {
	.startup = msm8996_mi2s_snd_startup,
	.shutdown = msm8996_mi2s_snd_shutdown,
};

static int msm_slim_5_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
				   slim5_rx_bit_format);
	rate->min = rate->max = slim5_rx_sample_rate;
	channels->min = channels->max = msm_slim_5_rx_ch;

	pr_debug("%s: format = %d, rate = %d, channels = %d\n",
		 __func__, params_format(params), params_rate(params),
		 msm_slim_5_rx_ch);

	return 0;
}

static int msm_slim_6_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
				   slim6_rx_bit_format);
	rate->min = rate->max = slim6_rx_sample_rate;
	channels->min = channels->max = msm_slim_6_rx_ch;

	pr_debug("%s: format = %d, rate = %d, channels = %d\n",
		 __func__, params_format(params), params_rate(params),
		 msm_slim_6_rx_ch);

	return 0;
}

static int msm_slim_0_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
				   slim0_rx_bit_format);
	rate->min = rate->max = slim0_rx_sample_rate;
	channels->min = channels->max = msm_slim_0_rx_ch;

	pr_debug("%s: format = %d, rate = %d, channels = %d\n",
		 __func__, params_format(params), params_rate(params),
		 msm_slim_0_rx_ch);

	return 0;
}

static int msm_slim_0_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, slim0_tx_bit_format);
	rate->min = rate->max = slim0_tx_sample_rate;
	channels->min = channels->max = msm_slim_0_tx_ch;

	return 0;
}

static int msm_slim_1_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, slim1_tx_bit_format);
	rate->min = rate->max = slim1_tx_sample_rate;
	channels->min = channels->max = msm_slim_1_tx_ch;

	return 0;
}

static int msm_slim_4_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S32_LE);

	rate->min = rate->max = SAMPLING_RATE_8KHZ;
	channels->min = channels->max = msm_vi_feed_tx_ch;
	pr_debug("%s: msm_vi_feed_tx_ch: %d\n", __func__, msm_vi_feed_tx_ch);

	return 0;
}

static int msm_slim_5_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					    struct snd_pcm_hw_params *params)
{
	int rc = 0;
	void *config = NULL;
	struct snd_soc_component *component;
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s: enter\n", __func__);
	rate->min = rate->max = SAMPLING_RATE_16KHZ;
	channels->min = channels->max = 1;

	component = snd_soc_rtdcom_lookup(rtd, TASHA_CODEC_NAME);
	if (!component || !msm8996_codec_fn.get_afe_config_fn) {
		pr_err("%s: codec not ready\n", __func__);
		return -EINVAL;
	}

	config = msm8996_codec_fn.get_afe_config_fn(component,
				AFE_SLIMBUS_SLAVE_PORT_CONFIG);
	if (config) {
		rc = afe_set_config(AFE_SLIMBUS_SLAVE_PORT_CONFIG, config,
				    SLIMBUS_5_TX);
		if (rc) {
			pr_err("%s: Failed to set slimbus slave port config %d\n",
				__func__, rc);
		}
	}

	return rc;
}

static int msm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				  struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	pr_debug("%s:\n", __func__);
	rate->min = rate->max = SAMPLING_RATE_48KHZ;
	return 0;
}

static const struct soc_enum msm_snd_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, spk_function),
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(8, slim0_tx_ch_text),
	SOC_ENUM_SINGLE_EXT(7, hdmi_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(rx_bit_format_text),
			    rx_bit_format_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(slim0_rx_sample_rate_text),
			    slim0_rx_sample_rate_text),
	SOC_ENUM_SINGLE_EXT(8, proxy_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(3, hdmi_rx_sample_rate_text),
	SOC_ENUM_SINGLE_EXT(4, slim5_rx_sample_rate_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(slim5_rx_bit_format_text),
			    slim5_rx_bit_format_text),
	SOC_ENUM_SINGLE_EXT(2, slim5_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(2, hifi_function),
	SOC_ENUM_SINGLE_EXT(2, vi_feed_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim6_rx_sample_rate_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(slim6_rx_bit_format_text),
			    slim6_rx_bit_format_text),
	SOC_ENUM_SINGLE_EXT(2, slim6_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(2, pm8996_ear_enable_states_text),
};

static const struct snd_kcontrol_new msm_snd_controls[] = {
	SOC_ENUM_EXT("Speaker Function", msm_snd_enum[0], msm8996_get_spk,
			msm8996_set_spk),
	SOC_ENUM_EXT("SLIM_0_RX Channels", msm_snd_enum[1],
			msm_slim_0_rx_ch_get, msm_slim_0_rx_ch_put),
	SOC_ENUM_EXT("SLIM_5_RX Channels", msm_snd_enum[10],
			msm_slim_5_rx_ch_get, msm_slim_5_rx_ch_put),
	SOC_ENUM_EXT("SLIM_6_RX Channels", msm_snd_enum[15],
			msm_slim_6_rx_ch_get, msm_slim_6_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_TX Channels", msm_snd_enum[2],
			msm_slim_0_tx_ch_get, msm_slim_0_tx_ch_put),
	SOC_ENUM_EXT("SLIM_1_TX Channels", msm_snd_enum[2],
			msm_slim_1_tx_ch_get, msm_slim_1_tx_ch_put),
	SOC_ENUM_EXT("AUX PCM SampleRate", msm8996_auxpcm_enum[0],
			msm8996_auxpcm_rate_get,
			msm8996_auxpcm_rate_put),
	SOC_ENUM_EXT("HDMI_RX Channels", msm_snd_enum[3],
			msm_hdmi_rx_ch_get, msm_hdmi_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_RX Format", msm_snd_enum[4],
			slim0_rx_bit_format_get, slim0_rx_bit_format_put),
	SOC_ENUM_EXT("SLIM_5_RX Format", msm_snd_enum[9],
			slim5_rx_bit_format_get, slim5_rx_bit_format_put),
	SOC_ENUM_EXT("SLIM_6_RX Format", msm_snd_enum[14],
			slim6_rx_bit_format_get, slim6_rx_bit_format_put),
	SOC_ENUM_EXT("SLIM_0_RX SampleRate", msm_snd_enum[5],
			slim0_rx_sample_rate_get, slim0_rx_sample_rate_put),
	SOC_ENUM_EXT("SLIM_5_RX SampleRate", msm_snd_enum[8],
			slim5_rx_sample_rate_get, slim5_rx_sample_rate_put),
	SOC_ENUM_EXT("SLIM_6_RX SampleRate", msm_snd_enum[13],
			slim6_rx_sample_rate_get, slim6_rx_sample_rate_put),
	SOC_ENUM_EXT("HDMI_RX Bit Format", msm_snd_enum[4],
			hdmi_rx_bit_format_get, hdmi_rx_bit_format_put),
	SOC_ENUM_EXT("PROXY_RX Channels", msm_snd_enum[6],
			msm_proxy_rx_ch_get, msm_proxy_rx_ch_put),
	SOC_ENUM_EXT("HDMI_RX SampleRate", msm_snd_enum[7],
			hdmi_rx_sample_rate_get, hdmi_rx_sample_rate_put),
	SOC_ENUM_EXT("SLIM_0_TX SampleRate", msm_snd_enum[5],
			slim0_tx_sample_rate_get, slim0_tx_sample_rate_put),
	SOC_ENUM_EXT("SLIM_0_TX Format", msm_snd_enum[4],
			slim0_tx_bit_format_get, slim0_tx_bit_format_put),
	SOC_ENUM_EXT("HiFi Function", msm_snd_enum[11], msm8996_hifi_get,
			msm8996_hifi_put),
	SOC_ENUM_EXT("VI_FEED_TX Channels", msm_snd_enum[12],
			msm_vi_feed_tx_ch_get, msm_vi_feed_tx_ch_put),
	SOC_ENUM_EXT("PM8996_Ear_Enable_States", msm_snd_enum[16],
		pm8996_ear_enable_get,
		pm8996_ear_enable_put),
};

static bool msm8996_swap_gnd_mic(struct snd_soc_component *component,
				 bool active)
{
	struct snd_soc_card *card = component->card;
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);
	int value;

	if (!pdata || !gpio_is_valid(pdata->us_euro_gpio))
		return false;

	value = gpio_get_value_cansleep(pdata->us_euro_gpio);
	pr_debug("%s: swap select switch %d to %d\n", __func__, value, !value);
	gpio_set_value_cansleep(pdata->us_euro_gpio, !value);
	return true;
}

static int msm_afe_set_config(struct snd_soc_component *component)
{
	int rc;
	void *config_data = NULL;

	pr_debug("%s: enter\n", __func__);

	if (!msm8996_codec_fn.get_afe_config_fn) {
		dev_err(component->dev, "%s: codec get afe config not init'ed\n",
			__func__);
		return -EINVAL;
	}

	config_data = msm8996_codec_fn.get_afe_config_fn(component,
			AFE_CDC_REGISTERS_CONFIG);
	if (config_data) {
		rc = afe_set_config(AFE_CDC_REGISTERS_CONFIG, config_data, 0);
		if (rc) {
			pr_err("%s: Failed to set codec registers config %d\n",
					__func__, rc);
			return rc;
		}
	}

	config_data = msm8996_codec_fn.get_afe_config_fn(component,
			AFE_CDC_REGISTER_PAGE_CONFIG);
	if (config_data) {
		rc = afe_set_config(AFE_CDC_REGISTER_PAGE_CONFIG, config_data,
				    0);
		if (rc)
			pr_err("%s: Failed to set cdc register page config\n",
				__func__);
	}

	config_data = msm8996_codec_fn.get_afe_config_fn(component,
			AFE_SLIMBUS_SLAVE_CONFIG);
	if (config_data) {
		rc = afe_set_config(AFE_SLIMBUS_SLAVE_CONFIG, config_data, 0);
		if (rc) {
			pr_err("%s: Failed to set slimbus slave config %d\n",
					__func__, rc);
			return rc;
		}
	}

	return 0;
}

static void msm_afe_clear_config(void)
{
	afe_clear_config(AFE_CDC_REGISTERS_CONFIG);
	afe_clear_config(AFE_SLIMBUS_SLAVE_CONFIG);
}

static int msm8996_wcd93xx_codec_up(struct snd_soc_component *component)
{
	int err;
	unsigned long timeout;
	int adsp_ready = 0;
	struct msm8996_asoc_mach_data *pdata = NULL;

	timeout = jiffies +
		msecs_to_jiffies(ADSP_STATE_READY_TIMEOUT_MS);

	do {
		if (!q6core_is_adsp_ready()) {
			pr_err_ratelimited("%s: ADSP Audio isn't ready\n",
					   __func__);
			/*
			 * ADSP will be coming up after subsystem restart and
			 * it might not be fully up when the control reaches
			 * here. So, wait for 50msec before checking ADSP state
			 */
			msleep(50);
		} else {
			pr_debug("%s: ADSP Audio is ready\n", __func__);
			adsp_ready = 1;
			break;
		}
	} while (time_after(timeout, jiffies));

	if (!adsp_ready) {
		pr_err("%s: timed out waiting for ADSP Audio\n", __func__);
		return -ETIMEDOUT;
	}

	err = msm_afe_set_config(component);
	if (err) {
		pr_err("%s: Failed to set AFE config. err %d\n",
			__func__, err);
	} else {
		if (component->card)
			pdata = snd_soc_card_get_drvdata(component->card);
		if (pdata)
			pdata->is_afe_config_done = true;
	}
	return err;
}

static int msm8996_tasha_codec_event_cb(struct snd_soc_component *component,
					enum wcd9335_codec_event codec_event)
{
	switch (codec_event) {
	case WCD9335_CODEC_EVENT_CODEC_UP:
		return msm8996_wcd93xx_codec_up(component);
	default:
		pr_err("%s: UnSupported codec event %d\n",
			__func__, codec_event);
		return -EINVAL;
	}
}

static int msm8996_config_hph_en0_gpio(struct snd_soc_component *component,
				       bool high)
{
	struct snd_soc_card *card = component->card;
	struct msm8996_asoc_mach_data *pdata;
	int val;

	if (!card)
		return 0;

	pdata = snd_soc_card_get_drvdata(card);
	if (!pdata || !gpio_is_valid(pdata->hph_en0_gpio))
		return 0;

	val = gpio_get_value_cansleep(pdata->hph_en0_gpio);
	if ((!!val) == high)
		return 0;

	gpio_direction_output(pdata->hph_en0_gpio, (int)high);

	return 1;
}

static struct snd_info_entry *msm8996_snd_info_create_subdir(
				struct module *mod, const char *name,
				struct snd_info_entry *parent)
{
	struct snd_info_entry *entry;

	entry = snd_info_create_module_entry(mod, name, parent);
	if (!entry)
		return NULL;
	entry->mode = S_IFDIR | 0555;
	if (snd_info_register(entry) < 0) {
		snd_info_free_entry(entry);
		return NULL;
	}
	return entry;
}

/* Create (once per card instantiation) the "codecs" proc directory */
static struct snd_info_entry *msm8996_get_codec_root(struct snd_soc_card *card)
{
	struct msm8996_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);
	struct snd_card *snd_card = card->snd_card;

	if (!pdata || !snd_card)
		return NULL;

	if (!pdata->codec_root)
		pdata->codec_root = msm8996_snd_info_create_subdir(
					snd_card->module, "codecs",
					snd_card->proc_root);
	if (!pdata->codec_root)
		pr_debug("%s: Cannot create codecs module entry\n", __func__);

	return pdata->codec_root;
}

static int msm_audrx_init(struct snd_soc_pcm_runtime *rtd)
{
	int err;
	void *config_data;
	struct snd_soc_component *component;
	struct snd_soc_dapm_context *dapm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_info_entry *codec_root;
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(rtd->card);
#if IS_ENABLED(CONFIG_SND_SOC_WSA881X)
	struct snd_soc_component *aux_comp;
#endif

	/* Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[TASHA_RX_MAX] = {144, 145, 146, 147, 148, 149, 150,
					    151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[TASHA_TX_MAX] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};

	pr_info("%s: dev_name%s\n", __func__, dev_name(cpu_dai->dev));

	rtd->pmdown_time = 0;

	component = snd_soc_rtdcom_lookup(rtd, TASHA_CODEC_NAME);
	if (!component) {
		pr_err("%s: component is NULL\n", __func__);
		return -EINVAL;
	}
	dapm = snd_soc_component_get_dapm(component);

	err = snd_soc_add_component_controls(component, msm_snd_controls,
					     ARRAY_SIZE(msm_snd_controls));
	if (err < 0) {
		pr_err("%s: add_codec_controls failed, err %d\n",
			__func__, err);
		return err;
	}

	err = msm8996_ext_us_amp_init(rtd->card);
	if (err) {
		pr_err("%s: 8996 US Emitter GPIO init failed (%d)\n",
			__func__, err);
		return err;
	}

	snd_soc_dapm_new_controls(dapm, msm8996_dapm_widgets,
				ARRAY_SIZE(msm8996_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, wcd9335_audio_paths,
				ARRAY_SIZE(wcd9335_audio_paths));
	snd_soc_dapm_enable_pin(dapm, "Lineout_1 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_3 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_2 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_4 amp");

	snd_soc_dapm_ignore_suspend(dapm, "Lineout_1 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_3 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_2 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_4 amp");
	snd_soc_dapm_ignore_suspend(dapm, "ultrasound amp");
	snd_soc_dapm_ignore_suspend(dapm, "Handset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "ANCRight Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "ANCLeft Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic1");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic2");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic3");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic4");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic5");
	snd_soc_dapm_ignore_suspend(dapm, "Analog Mic4");
	snd_soc_dapm_ignore_suspend(dapm, "Analog Mic6");
	snd_soc_dapm_ignore_suspend(dapm, "Analog Mic7");
	snd_soc_dapm_ignore_suspend(dapm, "Analog Mic8");
	snd_soc_dapm_ignore_suspend(dapm, "MADINPUT");
	snd_soc_dapm_ignore_suspend(dapm, "MAD_CPE_INPUT");
	snd_soc_dapm_ignore_suspend(dapm, "EAR");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT1");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT2");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT3");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT4");
	snd_soc_dapm_ignore_suspend(dapm, "ANC EAR");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC1");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC2");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC3");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC4");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC5");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC6");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC1");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC2");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC3");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC4");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC5");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic0");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC0");
	snd_soc_dapm_ignore_suspend(dapm, "SPK1 OUT");
	snd_soc_dapm_ignore_suspend(dapm, "SPK2 OUT");
	snd_soc_dapm_ignore_suspend(dapm, "HPHL");
	snd_soc_dapm_ignore_suspend(dapm, "HPHR");
	snd_soc_dapm_ignore_suspend(dapm, "ANC HPHL");
	snd_soc_dapm_ignore_suspend(dapm, "ANC HPHR");
	snd_soc_dapm_ignore_suspend(dapm, "ANC LINEOUT1");
	snd_soc_dapm_ignore_suspend(dapm, "ANC LINEOUT2");
	snd_soc_dapm_ignore_suspend(dapm, "AIF4 VI");
	snd_soc_dapm_ignore_suspend(dapm, "VIINPUT");

	snd_soc_dapm_sync(dapm);

	snd_soc_dai_set_channel_map(codec_dai, ARRAY_SIZE(tx_ch),
				    tx_ch, ARRAY_SIZE(rx_ch), rx_ch);

	msm8996_codec_fn.get_afe_config_fn = tasha_get_afe_config;
	msm8996_codec_fn.mbhc_hs_detect_exit = tasha_mbhc_hs_detect_exit;

	err = msm_afe_set_config(component);
	if (err) {
		pr_err("%s: Failed to set AFE config %d\n", __func__, err);
		goto out;
	}
	pdata->is_afe_config_done = true;

	config_data = msm8996_codec_fn.get_afe_config_fn(component,
						AFE_AANC_VERSION);
	if (config_data) {
		err = afe_set_config(AFE_AANC_VERSION, config_data, 0);
		if (err) {
			pr_err("%s: Failed to set aanc version %d\n",
				__func__, err);
			goto out;
		}
	}
	config_data = msm8996_codec_fn.get_afe_config_fn(component,
					    AFE_CDC_CLIP_REGISTERS_CONFIG);
	if (config_data) {
		err = afe_set_config(AFE_CDC_CLIP_REGISTERS_CONFIG,
				     config_data, 0);
		if (err) {
			pr_err("%s: Failed to set clip registers %d\n",
				__func__, err);
			goto out;
		}
	}
	config_data = msm8996_codec_fn.get_afe_config_fn(component,
			AFE_CLIP_BANK_SEL);
	if (config_data) {
		err = afe_set_config(AFE_CLIP_BANK_SEL, config_data, 0);
		if (err) {
			pr_err("%s: Failed to set AFE bank selection %d\n",
				__func__, err);
			goto out;
		}
	}

	/*
	 * MBHC is started from the card late_probe (as the 5.4 techpack
	 * machine drivers do), once the whole card is instantiated.
	 */
	tasha_mbhc_zdet_gpio_ctrl(msm8996_config_hph_en0_gpio, component);

	/*
	 * ADSP SSR is handled through the SND event framework (see
	 * msm8996_ssr_enable/disable) instead of the 4.9 subsys notifier.
	 * The codec-up callback re-sends the codec AFE config after a
	 * codec (slimbus) restart, as in 4.9.
	 */
	tasha_event_register(msm8996_tasha_codec_event_cb, component);

#if IS_ENABLED(CONFIG_SND_SOC_WSA881X)
	/*
	 * Send speaker configuration only for WSA8810.
	 * Defalut configuration is for WSA8815.
	 */
	if (!list_empty(&rtd->card->aux_comp_list)) {
		aux_comp = list_first_entry(&rtd->card->aux_comp_list,
				struct snd_soc_component, card_aux_list);
		if (!strcmp(aux_comp->name, WSA8810_NAME_1) ||
		    !strcmp(aux_comp->name, WSA8810_NAME_2)) {
			tasha_set_spkr_mode(component, SPKR_MODE_1);
			tasha_set_spkr_gain_offset(component,
						   RX_GAIN_OFFSET_M1P5_DB);
		}
	}
#endif
	codec_reg_done = true;

	codec_root = msm8996_get_codec_root(rtd->card);
	if (!codec_root) {
		err = 0;
		goto out;
	}
	tasha_codec_info_create_codec_entry(codec_root, component);

	return 0;
out:
	return err;
}

static void *def_tasha_mbhc_cal(void)
{
	void *tasha_wcd_cal;
	struct wcd_mbhc_btn_detect_cfg *btn_cfg;
	u16 *btn_high;

	tasha_wcd_cal = kzalloc(WCD_MBHC_CAL_SIZE(WCD_MBHC_DEF_BUTTONS,
				WCD9XXX_MBHC_DEF_RLOADS), GFP_KERNEL);
	if (!tasha_wcd_cal)
		return NULL;

#define S(X, Y) ((WCD_MBHC_CAL_PLUG_TYPE_PTR(tasha_wcd_cal)->X) = (Y))
	S(v_hs_max, 1700);
#undef S
#define S(X, Y) ((WCD_MBHC_CAL_BTN_DET_PTR(tasha_wcd_cal)->X) = (Y))
	S(num_btn, WCD_MBHC_DEF_BUTTONS);
#undef S

	btn_cfg = WCD_MBHC_CAL_BTN_DET_PTR(tasha_wcd_cal);
	btn_high = ((void *)&btn_cfg->_v_btn_low) +
		(sizeof(btn_cfg->_v_btn_low[0]) * btn_cfg->num_btn);

	btn_high[0] = 75;
	btn_high[1] = 137;
	btn_high[2] = 237;
	btn_high[3] = 500;
	btn_high[4] = 500;
	btn_high[5] = 500;
	btn_high[6] = 500;
	btn_high[7] = 500;

	return tasha_wcd_cal;
}

static int msm_snd_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai_link *dai_link = rtd->dai_link;

	int ret = 0;
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	u32 user_set_tx_ch = 0;
	u32 rx_ch_count;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_get_channel_map(codec_dai,
					&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
		if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_5_RX) {
			pr_debug("%s: rx_5_ch=%d\n", __func__,
				  msm_slim_5_rx_ch);
			rx_ch_count = msm_slim_5_rx_ch;
		} else if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_6_RX) {
			pr_debug("%s: rx_6_ch=%d\n", __func__,
				  msm_slim_6_rx_ch);
			rx_ch_count = msm_slim_6_rx_ch;
		} else {
			pr_debug("%s: rx_0_ch=%d\n", __func__,
				  msm_slim_0_rx_ch);
			rx_ch_count = msm_slim_0_rx_ch;
		}
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
						  rx_ch_count, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
	} else {

		pr_debug("%s: %s_tx_dai_id_%d_ch=%d\n", __func__,
			 codec_dai->name, codec_dai->id, user_set_tx_ch);
		ret = snd_soc_dai_get_channel_map(codec_dai,
					 &tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n, err:%d\n",
				__func__, ret);
			goto end;
		}
		/* For <codec>_tx1 case */
		if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_0_TX)
			user_set_tx_ch = msm_slim_0_tx_ch;
		/* For <codec>_tx3 case */
		else if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_1_TX)
			user_set_tx_ch = msm_slim_1_tx_ch;
		else if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_3_TX)
			/* DAI 5 is used for external EC reference from codec.
			 * Since Rx is fed as reference for EC, the config of
			 * this DAI is based on that of the Rx path.
			 */
			user_set_tx_ch = msm_slim_0_rx_ch;
		else if (dai_link->id == MSM_BACKEND_DAI_SLIMBUS_4_TX)
			user_set_tx_ch = msm_vi_feed_tx_ch;
		else
			user_set_tx_ch = tx_ch_cnt;

		pr_debug("%s: msm_slim_0_tx_ch(%d) user_set_tx_ch(%d) tx_ch_cnt(%d), BE id (%d)\n",
			 __func__, msm_slim_0_tx_ch, user_set_tx_ch,
			 tx_ch_cnt, dai_link->id);

		ret = snd_soc_dai_set_channel_map(cpu_dai,
						  user_set_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
	}
end:
	return ret;
}

#if IS_ENABLED(CONFIG_SND_SOC_CPE)
static int msm_snd_cpe_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai_link *dai_link = rtd->dai_link;

	int ret = 0;
	u32 tx_ch[SLIM_MAX_TX_PORTS];
	u32 tx_ch_cnt = 0;
	u32 user_set_tx_ch = 0;

	if (substream->stream != SNDRV_PCM_STREAM_CAPTURE) {
		pr_err("%s: Invalid stream type %d\n",
			__func__, substream->stream);
		ret = -EINVAL;
		goto end;
	}

	pr_debug("%s: %s_tx_dai_id_%d\n", __func__,
		 codec_dai->name, codec_dai->id);
	ret = snd_soc_dai_get_channel_map(codec_dai,
				 &tx_ch_cnt, tx_ch, NULL, NULL);
	if (ret < 0) {
		pr_err("%s: failed to get codec chan map\n, err:%d\n",
			__func__, ret);
		goto end;
	}

	user_set_tx_ch = tx_ch_cnt;

	pr_debug("%s: tx_ch_cnt(%d) BE id %d\n",
		 __func__, tx_ch_cnt, dai_link->id);

	ret = snd_soc_dai_set_channel_map(cpu_dai,
					  user_set_tx_ch, tx_ch, 0, 0);
	if (ret < 0)
		pr_err("%s: failed to set cpu chan map, err:%d\n",
			__func__, ret);
end:
	return ret;
}

static const struct snd_soc_ops msm8996_cpe_ops = {
	.hw_params = msm_snd_cpe_hw_params,
};
#endif

static const struct snd_soc_ops msm8996_be_ops = {
	.hw_params = msm_snd_hw_params,
};

static int msm8996_slimbus_2_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	unsigned int rx_ch_cnt = 0, tx_ch_cnt = 0;
	unsigned int num_tx_ch = 0;
	unsigned int num_rx_ch = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		num_rx_ch =  params_channels(params);
		pr_debug("%s: %s rx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, num_rx_ch);
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				num_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
	} else {
		num_tx_ch =  params_channels(params);
		pr_debug("%s: %s  tx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, num_tx_ch);
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(cpu_dai,
				num_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map, err:%d\n",
				__func__, ret);
			goto end;
		}
	}
end:
	return ret;
}

static const struct snd_soc_ops msm8996_slimbus_2_be_ops = {
	.hw_params = msm8996_slimbus_2_hw_params,
};

static int msm8996_get_ll_qos_val(struct snd_pcm_runtime *runtime)
{
	int usecs;

	/* take 10% of period time as the deadline */
	usecs = (100000 / runtime->rate) * runtime->period_size;
	usecs += ((100000 % runtime->rate) * runtime->period_size) /
		runtime->rate;

	return usecs;
}

static int msm8996_mm5_prepare(struct snd_pcm_substream *substream)
{
	if (pm_qos_request_active(&substream->latency_pm_qos_req))
		pm_qos_remove_request(&substream->latency_pm_qos_req);
	pm_qos_add_request(&substream->latency_pm_qos_req,
			   PM_QOS_CPU_DMA_LATENCY,
			   msm8996_get_ll_qos_val(substream->runtime));
	return 0;
}

static const struct snd_soc_ops msm8996_mm5_ops = {
	.prepare = msm8996_mm5_prepare,
};

/*
 * DAI link component definitions.
 *
 * Platform, CPU and codec names are resolved to DT nodes through the
 * asoc-platform(-names), asoc-cpu(-names) and asoc-codec(-names) properties
 * of the sound node, exactly like the 4.9 driver did.
 */
#define MSM8996_FE_DAILINK(_name, _cpu, _platform)			\
	SND_SOC_DAILINK_DEFS(_name,					\
		DAILINK_COMP_ARRAY(COMP_CPU(_cpu)),			\
		DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy",		\
					      "snd-soc-dummy-dai")),	\
		DAILINK_COMP_ARRAY(COMP_PLATFORM(_platform)))

#define MSM8996_DAILINK(_name, _cpu, _codec, _codec_dai, _platform)	\
	SND_SOC_DAILINK_DEFS(_name,					\
		DAILINK_COMP_ARRAY(COMP_CPU(_cpu)),			\
		DAILINK_COMP_ARRAY(COMP_CODEC(_codec, _codec_dai)),	\
		DAILINK_COMP_ARRAY(COMP_PLATFORM(_platform)))

/* Front ends */
MSM8996_FE_DAILINK(multimedia1, "MultiMedia1", "msm-pcm-dsp.0");
MSM8996_FE_DAILINK(multimedia2, "MultiMedia2", "msm-pcm-dsp.0");
MSM8996_FE_DAILINK(voicemmode1, "VoiceMMode1", "msm-pcm-voice");
MSM8996_FE_DAILINK(msmvoip, "VoIP", "msm-voip-dsp");
MSM8996_FE_DAILINK(multimedia3, "MultiMedia3", "msm-pcm-dsp.2");
MSM8996_FE_DAILINK(slimbus0_hostless, "SLIMBUS0_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_FE_DAILINK(tert_mi2s_tx_hostless, "TERT_MI2S_TX_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_DAILINK(afepcm_rx, "msm-dai-q6-dev.241", "msm-stub-codec.1",
		"msm-stub-rx", "msm-pcm-afe");
MSM8996_DAILINK(afepcm_tx, "msm-dai-q6-dev.240", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-afe");
MSM8996_FE_DAILINK(multimedia4, "MultiMedia4", "msm-compress-dsp");
MSM8996_FE_DAILINK(auxpcm_hostless, "AUXPCM_HOSTLESS", "msm-pcm-hostless");
MSM8996_FE_DAILINK(slimbus1_hostless, "SLIMBUS1_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_FE_DAILINK(slimbus3_hostless, "SLIMBUS3_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_FE_DAILINK(slimbus4_hostless, "SLIMBUS4_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_FE_DAILINK(multimedia5, "MultiMedia5", "msm-pcm-dsp.1");
MSM8996_FE_DAILINK(listen1, "LSM1", "msm-lsm-client");
MSM8996_FE_DAILINK(multimedia7, "MultiMedia7", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia10, "MultiMedia10", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia8, "MultiMedia8", "msm-pcm-dsp-noirq");
MSM8996_FE_DAILINK(qchat, "QCHAT", "msm-pcm-voice");
MSM8996_FE_DAILINK(hdmi_rx_hostless, "HDMI_HOSTLESS", "msm-pcm-hostless");
MSM8996_FE_DAILINK(voicemmode2, "VoiceMMode2", "msm-pcm-voice");
MSM8996_FE_DAILINK(int_hfp_bt_hostless, "INT_HFP_BT_HOSTLESS",
		   "msm-pcm-hostless");
MSM8996_FE_DAILINK(multimedia6, "MultiMedia6", "msm-pcm-loopback");
MSM8996_FE_DAILINK(listen2, "LSM2", "msm-lsm-client");
MSM8996_FE_DAILINK(listen3, "LSM3", "msm-lsm-client");
MSM8996_FE_DAILINK(listen4, "LSM4", "msm-lsm-client");
MSM8996_FE_DAILINK(listen5, "LSM5", "msm-lsm-client");
MSM8996_FE_DAILINK(listen6, "LSM6", "msm-lsm-client");
MSM8996_FE_DAILINK(listen7, "LSM7", "msm-lsm-client");
MSM8996_FE_DAILINK(listen8, "LSM8", "msm-lsm-client");
MSM8996_FE_DAILINK(multimedia9, "MultiMedia9", "msm-pcm-dsp.0");
MSM8996_FE_DAILINK(multimedia11, "MultiMedia11", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia12, "MultiMedia12", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia13, "MultiMedia13", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia14, "MultiMedia14", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia15, "MultiMedia15", "msm-compress-dsp");
MSM8996_FE_DAILINK(multimedia16, "MultiMedia16", "msm-pcm-dsp-noirq");
MSM8996_FE_DAILINK(slimbus6_hostless, "SLIMBUS6_HOSTLESS",
		   "msm-pcm-hostless");

/*
 * Placeholder link: an inert (no_pcm) link on the ASoC dummy component.
 *
 * Used in place of 4.9 links whose FE DAIs / platforms no longer exist in
 * this audio-kernel (legacy CS-VOICE/VoLTE/Voice2/VoWLAN voice FEs) or are
 * not built (CPE LSM).  Keeping a link at the same position keeps the ALSA
 * PCM device numbers of all following links identical to the 4.9 kernel,
 * which the audio HAL relies on.  The dummy components are not looked up in
 * the DT (see msm8996_populate_dai_link_component_of_node()), so all
 * placeholders may share the same component arrays.
 */
#define MSM8996_DUMMY_NAME "snd-soc-dummy"
SND_SOC_DAILINK_DEFS(placeholder,
	DAILINK_COMP_ARRAY(COMP_CPU("snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC(MSM8996_DUMMY_NAME,
				      "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(MSM8996_DUMMY_NAME)));

#define MSM8996_PLACEHOLDER_LINK(_name, _stream_name)			\
	{								\
		.name = _name,						\
		.stream_name = _stream_name,				\
		.no_pcm = 1,						\
		.dpcm_playback = 1,					\
		.dpcm_capture = 1,					\
		.ignore_suspend = 1,					\
		.ignore_pmdown_time = 1,				\
		SND_SOC_DAILINK_REG(placeholder),			\
	}

/* Tasha front ends / direct links */
MSM8996_DAILINK(slimbus_4_tx, "msm-dai-q6-dev.16393", TASHA_CODEC_NAME,
		"tasha_vifeedback", "msm-pcm-hostless");
MSM8996_DAILINK(slimbus_2_hostless_playback, "msm-dai-q6-dev.16388",
		TASHA_CODEC_NAME, "tasha_rx2", "msm-pcm-hostless");
MSM8996_DAILINK(slimbus_2_hostless_capture, "msm-dai-q6-dev.16389",
		TASHA_CODEC_NAME, "tasha_tx2", "msm-pcm-hostless");
#if IS_ENABLED(CONFIG_SND_SOC_CPE)
MSM8996_DAILINK(cpe_listen_service, "msm-dai-slim", TASHA_CODEC_NAME,
		"tasha_mad1", "msm-cpe-lsm");
MSM8996_DAILINK(cpe_listen_service_ecpp, "CPE_LSM_NOHOST", TASHA_CODEC_NAME,
		"tasha_cpe", "msm-cpe-lsm.3");
#endif

/* Common back ends */
MSM8996_DAILINK(afe_pcm_rx, "msm-dai-q6-dev.224", "msm-stub-codec.1",
		"msm-stub-rx", "msm-pcm-routing");
MSM8996_DAILINK(afe_pcm_tx, "msm-dai-q6-dev.225", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-routing");
MSM8996_DAILINK(auxpcm_rx, "msm-dai-q6-auxpcm.1", "msm-stub-codec.1",
		"msm-stub-rx", "msm-pcm-routing");
MSM8996_DAILINK(auxpcm_tx, "msm-dai-q6-auxpcm.1", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-routing");
MSM8996_DAILINK(incall_record_tx, "msm-dai-q6-dev.32772", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-routing");
MSM8996_DAILINK(incall_record_rx, "msm-dai-q6-dev.32771", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-routing");
MSM8996_DAILINK(voice_playback_tx, "msm-dai-q6-dev.32773",
		"msm-stub-codec.1", "msm-stub-rx", "msm-pcm-routing");
MSM8996_DAILINK(voice2_playback_tx, "msm-dai-q6-dev.32770",
		"msm-stub-codec.1", "msm-stub-rx", "msm-pcm-routing");
MSM8996_DAILINK(tert_mi2s_tx, "msm-dai-q6-mi2s.2", "msm-stub-codec.1",
		"msm-stub-tx", "msm-pcm-routing");

/* Tasha back ends */
MSM8996_DAILINK(slimbus_0_rx, "msm-dai-q6-dev.16384", TASHA_CODEC_NAME,
		"tasha_mix_rx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_0_tx, "msm-dai-q6-dev.16385", TASHA_CODEC_NAME,
		"tasha_tx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_1_rx, "msm-dai-q6-dev.16386", TASHA_CODEC_NAME,
		"tasha_mix_rx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_1_tx, "msm-dai-q6-dev.16387", TASHA_CODEC_NAME,
		"tasha_tx3", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_3_rx, "msm-dai-q6-dev.16390", TASHA_CODEC_NAME,
		"tasha_mix_rx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_3_tx, "msm-dai-q6-dev.16391", TASHA_CODEC_NAME,
		"tasha_tx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_4_rx, "msm-dai-q6-dev.16392", TASHA_CODEC_NAME,
		"tasha_mix_rx1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_5_rx, "msm-dai-q6-dev.16394", TASHA_CODEC_NAME,
		"tasha_rx3", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_5_tx, "msm-dai-q6-dev.16395", TASHA_CODEC_NAME,
		"tasha_mad1", "msm-pcm-routing");
MSM8996_DAILINK(slimbus_6_rx, "msm-dai-q6-dev.16396", TASHA_CODEC_NAME,
		"tasha_rx4", "msm-pcm-routing");

/* HDMI back end */
MSM8996_DAILINK(hdmi, "msm-dai-q6-hdmi.8", "msm-hdmi-audio-codec-rx",
		"msm_hdmi_audio_codec_rx_dai", "msm-pcm-routing");

/* Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link msm8996_common_dai_links[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MSM8996 Media1",
		.stream_name = "MultiMedia1",
		.dynamic = 1,
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
		.async_ops = ASYNC_DPCM_SND_SOC_PREPARE,
#endif /* CONFIG_AUDIO_QGKI */
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA1,
		SND_SOC_DAILINK_REG(multimedia1),
	},
	{
		.name = "MSM8996 Media2",
		.stream_name = "MultiMedia2",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA2,
		SND_SOC_DAILINK_REG(multimedia2),
	},
	{
		.name = "VoiceMMode1",
		.stream_name = "VoiceMMode1",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_VOICEMMODE1,
		SND_SOC_DAILINK_REG(voicemmode1),
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_VOIP,
		SND_SOC_DAILINK_REG(msmvoip),
	},
	{
		.name = "MSM8996 ULL",
		.stream_name = "MultiMedia3",
		.dynamic = 1,
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
		.async_ops = ASYNC_DPCM_SND_SOC_PREPARE,
#endif /* CONFIG_AUDIO_QGKI */
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA3,
		SND_SOC_DAILINK_REG(multimedia3),
	},
	/* Hostless PCM purpose */
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		 /* this dailink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(slimbus0_hostless),
	},
	{
		.name = "Tertiary MI2S TX_Hostless",
		.stream_name = "Tertiary MI2S_TX Hostless Capture",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(tert_mi2s_tx_hostless),
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(afepcm_rx),
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(afepcm_tx),
	},
	{
		.name = "MSM8996 Compress1",
		.stream_name = "Compress1",
		.dynamic = 1,
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
		.async_ops = ASYNC_DPCM_SND_SOC_HW_PARAMS,
#endif /* CONFIG_AUDIO_QGKI */
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA4,
		SND_SOC_DAILINK_REG(multimedia4),
	},
	{
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(auxpcm_hostless),
	},
	{
		.name = "SLIMBUS_1 Hostless",
		.stream_name = "SLIMBUS_1 Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		 /* this dailink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(slimbus1_hostless),
	},
	{
		.name = "SLIMBUS_3 Hostless",
		.stream_name = "SLIMBUS_3 Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		 /* this dailink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(slimbus3_hostless),
	},
	{
		.name = "SLIMBUS_4 Hostless",
		.stream_name = "SLIMBUS_4 Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		 /* this dailink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(slimbus4_hostless),
	},
	/*
	 * 4.9: "VoLTE" FE (cpu "VoLTE", msm-pcm-voice). The legacy VoLTE
	 * FE DAI and MSM_FRONTEND_DAI_VOLTE no longer exist in this
	 * audio-kernel (VoLTE goes through VoiceMMode1/2); keep the slot.
	 */
	MSM8996_PLACEHOLDER_LINK("VoLTE", "VoLTE"),
	{
		.name = "MSM8996 LowLatency",
		.stream_name = "MultiMedia5",
		.dynamic = 1,
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
		.async_ops = ASYNC_DPCM_SND_SOC_PREPARE,
#endif /* CONFIG_AUDIO_QGKI */
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA5,
		.ops = &msm8996_mm5_ops,
		SND_SOC_DAILINK_REG(multimedia5),
	},
	{
		.name = "Listen 1 Audio Service",
		.stream_name = "Listen 1 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
			     SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM1,
		SND_SOC_DAILINK_REG(listen1),
	},
	/* Multiple Tunnel instances */
	{
		.name = "MSM8996 Compress2",
		.stream_name = "Compress2",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA7,
		SND_SOC_DAILINK_REG(multimedia7),
	},
	{
		.name = "MSM8996 Compress3",
		.stream_name = "Compress3",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA10,
		SND_SOC_DAILINK_REG(multimedia10),
	},
	{
		.name = "MSM8996 ULL NOIRQ",
		.stream_name = "MM_NOIRQ",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA8,
		SND_SOC_DAILINK_REG(multimedia8),
	},
	{
		.name = "QCHAT",
		.stream_name = "QCHAT",
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_QCHAT,
		SND_SOC_DAILINK_REG(qchat),
	},
	/* HDMI Hostless */
	{
		.name = "HDMI_RX_HOSTLESS",
		.stream_name = "HDMI_RX_HOSTLESS",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(hdmi_rx_hostless),
	},
	{
		.name = "VoiceMMode2",
		.stream_name = "VoiceMMode2",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_VOICEMMODE2,
		SND_SOC_DAILINK_REG(voicemmode2),
	},
	{
		.name = "INT_HFP_BT Hostless",
		.stream_name = "INT_HFP_BT Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(int_hfp_bt_hostless),
	},
	{
		.name = "MSM8996 HFP TX",
		.stream_name = "MultiMedia6",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA6,
		SND_SOC_DAILINK_REG(multimedia6),
	},
	/* LSM FE */
	{
		.name = "Listen 2 Audio Service",
		.stream_name = "Listen 2 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM2,
		SND_SOC_DAILINK_REG(listen2),
	},
	{
		.name = "Listen 3 Audio Service",
		.stream_name = "Listen 3 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM3,
		SND_SOC_DAILINK_REG(listen3),
	},
	{
		.name = "Listen 4 Audio Service",
		.stream_name = "Listen 4 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM4,
		SND_SOC_DAILINK_REG(listen4),
	},
	{
		.name = "Listen 5 Audio Service",
		.stream_name = "Listen 5 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM5,
		SND_SOC_DAILINK_REG(listen5),
	},
	{
		.name = "Listen 6 Audio Service",
		.stream_name = "Listen 6 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM6,
		SND_SOC_DAILINK_REG(listen6),
	},
	{
		.name = "Listen 7 Audio Service",
		.stream_name = "Listen 7 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM7,
		SND_SOC_DAILINK_REG(listen7),
	},
	{
		.name = "Listen 8 Audio Service",
		.stream_name = "Listen 8 Audio Service",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
				 SND_SOC_DPCM_TRIGGER_POST },
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_LSM8,
		SND_SOC_DAILINK_REG(listen8),
	},
	{
		.name = "MSM8996 Media9",
		.stream_name = "MultiMedia9",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.id = MSM_FRONTEND_DAI_MULTIMEDIA9,
		SND_SOC_DAILINK_REG(multimedia9),
	},
	/*
	 * 4.9: "VoWLAN" FE (cpu "VoWLAN", msm-pcm-voice). Not present in
	 * this audio-kernel; keep the slot.
	 */
	MSM8996_PLACEHOLDER_LINK("VoWLAN", "VoWLAN"),
	{
		.name = "MSM8996 Compress4",
		.stream_name = "Compress4",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA11,
		SND_SOC_DAILINK_REG(multimedia11),
	},
	{
		.name = "MSM8996 Compress5",
		.stream_name = "Compress5",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA12,
		SND_SOC_DAILINK_REG(multimedia12),
	},
	{
		.name = "MSM8996 Compress6",
		.stream_name = "Compress6",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA13,
		SND_SOC_DAILINK_REG(multimedia13),
	},
	{
		.name = "MSM8996 Compress7",
		.stream_name = "Compress7",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA14,
		SND_SOC_DAILINK_REG(multimedia14),
	},
	{
		.name = "MSM8996 Compress8",
		.stream_name = "Compress8",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA15,
		SND_SOC_DAILINK_REG(multimedia15),
	},
	{
		.name = "MSM8996 ULL NOIRQ_2",
		.stream_name = "MM_NOIRQ_2",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		 /* this dainlink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA16,
		SND_SOC_DAILINK_REG(multimedia16),
	},
	/*
	 * 4.9: "Circuit-Switch Voice" (cpu "CS-VOICE") and "Voice2" (cpu
	 * "Voice2") FEs on msm-pcm-voice. Not present in this audio-kernel
	 * (CS voice goes through VoiceMMode1/2); keep the slots.
	 */
	MSM8996_PLACEHOLDER_LINK("Circuit-Switch Voice", "CS-Voice"),
	MSM8996_PLACEHOLDER_LINK("Voice2", "Voice2"),
};

static struct snd_soc_dai_link msm8996_tasha_fe_dai_links[] = {
	{
		.name = LPASS_BE_SLIMBUS_4_TX,
		.stream_name = "Slimbus4 Capture",
		.id = MSM_BACKEND_DAI_SLIMBUS_4_TX,
		.be_hw_params_fixup = msm_slim_4_tx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_4_tx),
	},
	/* Ultrasound RX DAI Link */
	{
		.name = "SLIMBUS_2 Hostless Playback",
		.stream_name = "SLIMBUS_2 Hostless Playback",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8996_slimbus_2_be_ops,
		SND_SOC_DAILINK_REG(slimbus_2_hostless_playback),
	},
	/* Ultrasound TX DAI Link */
	{
		.name = "SLIMBUS_2 Hostless Capture",
		.stream_name = "SLIMBUS_2 Hostless Capture",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8996_slimbus_2_be_ops,
		SND_SOC_DAILINK_REG(slimbus_2_hostless_capture),
	},
	/* CPE LSM direct dai-link */
#if IS_ENABLED(CONFIG_SND_SOC_CPE)
	{
		.name = "CPE Listen service",
		.stream_name = "CPE Listen Audio Service",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.ops = &msm8996_cpe_ops,
		SND_SOC_DAILINK_REG(cpe_listen_service),
	},
#else
	/* msm-cpe-lsm (CONFIG_SND_SOC_CPE) is not built: keep the slot */
	MSM8996_PLACEHOLDER_LINK("CPE Listen service",
				 "CPE Listen Audio Service"),
#endif
	/* slimbus rx 6 hostless */
	{
		.name = "SLIMBUS_6 Hostless Playback",
		.stream_name = "SLIMBUS_6 Hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		 /* this dailink has playback support */
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(slimbus6_hostless),
	},
	/* CPE LSM EC PP direct dai-link */
#if IS_ENABLED(CONFIG_SND_SOC_CPE)
	{
		.name = "CPE Listen service ECPP",
		.stream_name = "CPE Listen Audio Service ECPP",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		SND_SOC_DAILINK_REG(cpe_listen_service_ecpp),
	},
#else
	MSM8996_PLACEHOLDER_LINK("CPE Listen service ECPP",
				 "CPE Listen Audio Service ECPP"),
#endif
};

static struct snd_soc_dai_link msm8996_common_be_dai_links[] = {
	/* Backend AFE DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.be_hw_params_fixup = msm_proxy_rx_be_hw_params_fixup,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(afe_pcm_rx),
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.be_hw_params_fixup = msm_proxy_tx_be_hw_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(afe_pcm_tx),
	},
	/* Primary AUX PCM Backend DAI Links */
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		SND_SOC_DAILINK_REG(auxpcm_rx),
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(auxpcm_tx),
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(incall_record_tx),
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(incall_record_rx),
	},
	/* Incall Music BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(voice_playback_tx),
	},
	/* Incall Music 2 BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE2_PLAYBACK_TX,
		.stream_name = "Voice2 Farend Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_VOICE2_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(voice2_playback_tx),
	},
	{
		.name = LPASS_BE_TERT_MI2S_TX,
		.stream_name = "Tertiary MI2S Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_TERTIARY_MI2S_TX,
		.be_hw_params_fixup = msm_tx_be_hw_params_fixup,
		.ops = &msm8996_mi2s_be_ops,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(tert_mi2s_tx),
	}
};

static struct snd_soc_dai_link msm8996_tasha_be_dai_links[] = {
	/* Backend DAI Links */
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm_audrx_init,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		.ops = &msm8996_be_ops,
		SND_SOC_DAILINK_REG(slimbus_0_rx),
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm_slim_0_tx_be_hw_params_fixup,
		.ignore_suspend = 1,
		.ops = &msm8996_be_ops,
		SND_SOC_DAILINK_REG(slimbus_0_tx),
	},
	{
		.name = LPASS_BE_SLIMBUS_1_RX,
		.stream_name = "Slimbus1 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_1_RX,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		/* dai link has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_1_rx),
	},
	{
		.name = LPASS_BE_SLIMBUS_1_TX,
		.stream_name = "Slimbus1 Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_1_TX,
		.be_hw_params_fixup = msm_slim_1_tx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_1_tx),
	},
	{
		.name = LPASS_BE_SLIMBUS_3_RX,
		.stream_name = "Slimbus3 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_3_RX,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		/* dai link has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_3_rx),
	},
	{
		.name = LPASS_BE_SLIMBUS_3_TX,
		.stream_name = "Slimbus3 Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_3_TX,
		.be_hw_params_fixup = msm_slim_0_tx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_3_tx),
	},
	{
		.name = LPASS_BE_SLIMBUS_4_RX,
		.stream_name = "Slimbus4 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_4_RX,
		.be_hw_params_fixup = msm_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		/* dai link has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_4_rx),
	},
	{
		.name = LPASS_BE_SLIMBUS_5_RX,
		.stream_name = "Slimbus5 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_5_RX,
		.be_hw_params_fixup = msm_slim_5_rx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		/* dai link has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_5_rx),
	},
	/* MAD BE */
	{
		.name = LPASS_BE_SLIMBUS_5_TX,
		.stream_name = "Slimbus5 Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_5_TX,
		.be_hw_params_fixup = msm_slim_5_tx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_5_tx),
	},
	{
		.name = LPASS_BE_SLIMBUS_6_RX,
		.stream_name = "Slimbus6 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SLIMBUS_6_RX,
		.be_hw_params_fixup = msm_slim_6_rx_be_hw_params_fixup,
		.ops = &msm8996_be_ops,
		/* dai link has playback support */
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(slimbus_6_rx),
	},
};

static struct snd_soc_dai_link msm8996_hdmi_dai_link[] = {
	/* HDMI BACK END DAI Link */
	{
		.name = LPASS_BE_HDMI,
		.stream_name = "HDMI Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_HDMI_RX,
		.be_hw_params_fixup = msm8996_hdmi_be_hw_params_fixup,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hdmi),
	},
};

static struct snd_soc_dai_link msm8996_tasha_dai_links[
			 ARRAY_SIZE(msm8996_common_dai_links) +
			 ARRAY_SIZE(msm8996_tasha_fe_dai_links) +
			 ARRAY_SIZE(msm8996_common_be_dai_links) +
			 ARRAY_SIZE(msm8996_tasha_be_dai_links) +
			 ARRAY_SIZE(msm8996_hdmi_dai_link)];

#if IS_ENABLED(CONFIG_SND_SOC_WSA881X)
static int msm8996_wsa881x_init(struct snd_soc_component *component)
{
	/* Port numbers/types/rates/masks as used by the 4.9 driver */
	u8 spkleft_ports[WSA881X_MAX_SWR_PORTS] = {100, 101, 102, 106};
	u8 spkright_ports[WSA881X_MAX_SWR_PORTS] = {103, 104, 105, 107};
	u8 spkleft_port_types[WSA881X_MAX_SWR_PORTS] = {SPKR_L, SPKR_L_COMP,
						SPKR_L_BOOST, SPKR_L_VI};
	u8 spkright_port_types[WSA881X_MAX_SWR_PORTS] = {SPKR_R, SPKR_R_COMP,
						SPKR_R_BOOST, SPKR_R_VI};
	unsigned int ch_rate[WSA881X_MAX_SWR_PORTS] = {2400, 600, 300, 1200};
	unsigned int ch_mask[WSA881X_MAX_SWR_PORTS] = {0x1, 0xF, 0x3, 0x3};
	struct snd_soc_dapm_context *dapm;
	struct snd_info_entry *codec_root;

	if (!component) {
		pr_err("%s component is NULL\n", __func__);
		return -EINVAL;
	}

	dapm = snd_soc_component_get_dapm(component);

	if (!component->name_prefix) {
		dev_err(component->dev, "%s: no name prefix for %s\n",
			__func__, component->name);
		return -EINVAL;
	}

	if (!strcmp(component->name_prefix, "SpkrLeft")) {
		dev_dbg(component->dev, "%s: setting left ch map to codec %s\n",
			__func__, component->name);
		wsa881x_set_channel_map(component, &spkleft_ports[0],
				WSA881X_MAX_SWR_PORTS, &ch_mask[0],
				&ch_rate[0], &spkleft_port_types[0]);
		if (dapm->component) {
			snd_soc_dapm_ignore_suspend(dapm, "SpkrLeft IN");
			snd_soc_dapm_ignore_suspend(dapm, "SpkrLeft SPKR");
		}
	} else if (!strcmp(component->name_prefix, "SpkrRight")) {
		dev_dbg(component->dev, "%s: setting right ch map to codec %s\n",
			__func__, component->name);
		wsa881x_set_channel_map(component, &spkright_ports[0],
				WSA881X_MAX_SWR_PORTS, &ch_mask[0],
				&ch_rate[0], &spkright_port_types[0]);
		if (dapm->component) {
			snd_soc_dapm_ignore_suspend(dapm, "SpkrRight IN");
			snd_soc_dapm_ignore_suspend(dapm, "SpkrRight SPKR");
		}
	} else {
		dev_err(component->dev, "%s: wrong codec name %s\n", __func__,
			component->name);
		return -EINVAL;
	}

	codec_root = msm8996_get_codec_root(component->card);
	if (codec_root)
		wsa881x_codec_info_create_codec_entry(codec_root, component);

	return 0;
}

/*
 * soc_find_component() is not exported by the 5.4 ASoC core. A WSA881x
 * registers its component from its soundwire driver probe, so treat it as
 * "registered with ALSA core" once its soundwire slave device is bound.
 */
static bool msm8996_wsa881x_registered(struct device_node *np)
{
	struct device *dev = bus_find_device_by_of_node(&soundwire_type, np);
	bool bound;

	if (!dev)
		return false;
	bound = !!dev->driver;
	put_device(dev);
	return bound;
}

static int msm8996_init_wsa_dev(struct platform_device *pdev,
				struct snd_soc_card *card)
{
	struct device_node *wsa_of_node;
	u32 wsa_max_devs;
	int wsa_dev_cnt;
	struct msm8996_wsa881x_dev_info *wsa881x_dev_info;
	struct snd_soc_aux_dev *msm8996_aux_dev;
	struct snd_soc_codec_conf *msm8996_codec_conf;
	const char *wsa_auxdev_name_prefix[1];
	u32 found = 0;
	int i;
	int ret;

	/* Get maximum WSA device count for this platform */
	ret = of_property_read_u32(pdev->dev.of_node,
				   "qcom,wsa-max-devs", &wsa_max_devs);
	if (ret) {
		dev_dbg(&pdev->dev,
			 "%s: wsa-max-devs property missing in DT %pOF, ret = %d\n",
			 __func__, pdev->dev.of_node, ret);
		return 0;
	}
	if (wsa_max_devs == 0) {
		dev_warn(&pdev->dev,
			 "%s: Max WSA devices is 0 for this target?\n",
			 __func__);
		return 0;
	}

	/* Get count of WSA device phandles for this platform */
	wsa_dev_cnt = of_count_phandle_with_args(pdev->dev.of_node,
						 "qcom,wsa-devs", NULL);
	if (wsa_dev_cnt == -ENOENT) {
		dev_warn(&pdev->dev, "%s: No wsa device defined in DT.\n",
			 __func__);
		return 0;
	} else if (wsa_dev_cnt <= 0) {
		dev_err(&pdev->dev,
			"%s: Error reading wsa device from DT. wsa_dev_cnt = %d\n",
			__func__, wsa_dev_cnt);
		return -EINVAL;
	}

	/*
	 * Expect total phandles count to be NOT less than maximum possible
	 * WSA count. However, if it is less, then assign same value to
	 * max count as well.
	 */
	if (wsa_dev_cnt < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: wsa_max_devs = %u cannot exceed wsa_dev_cnt = %d\n",
			__func__, wsa_max_devs, wsa_dev_cnt);
		wsa_max_devs = wsa_dev_cnt;
	}

	/* Make sure prefix string passed for each WSA device */
	ret = of_property_count_strings(pdev->dev.of_node,
					"qcom,wsa-aux-dev-prefix");
	if (ret != wsa_dev_cnt) {
		dev_err(&pdev->dev,
			"%s: expecting %d wsa prefix. Defined only %d in DT\n",
			__func__, wsa_dev_cnt, ret);
		return -EINVAL;
	}

	/*
	 * Alloc mem to store phandle and index info of WSA device, if already
	 * registered with ALSA core
	 */
	wsa881x_dev_info = devm_kcalloc(&pdev->dev, wsa_max_devs,
					sizeof(struct msm8996_wsa881x_dev_info),
					GFP_KERNEL);
	if (!wsa881x_dev_info)
		return -ENOMEM;

	/*
	 * search and check whether all WSA devices are already
	 * registered with ALSA core or not. If found a node, store
	 * the node and the index in a local array of struct for later
	 * use.
	 */
	for (i = 0; i < wsa_dev_cnt; i++) {
		wsa_of_node = of_parse_phandle(pdev->dev.of_node,
					    "qcom,wsa-devs", i);
		if (unlikely(!wsa_of_node)) {
			/* we should not be here */
			dev_err(&pdev->dev,
				"%s: wsa dev node is not present\n",
				__func__);
			return -EINVAL;
		}
		if (msm8996_wsa881x_registered(wsa_of_node)) {
			/* WSA device registered with ALSA core */
			wsa881x_dev_info[found].of_node = wsa_of_node;
			wsa881x_dev_info[found].index = i;
			found++;
			if (found == wsa_max_devs)
				break;
		} else {
			of_node_put(wsa_of_node);
		}
	}

	if (found < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: failed to find %u components. Found only %u\n",
			__func__, wsa_max_devs, found);
		return -EPROBE_DEFER;
	}
	dev_info(&pdev->dev,
		"%s: found %u wsa881x devices registered with ALSA core\n",
		__func__, found);

	card->num_aux_devs = wsa_max_devs;
	card->num_configs = wsa_max_devs;

	/* Alloc array of AUX devs struct */
	msm8996_aux_dev = devm_kcalloc(&pdev->dev, card->num_aux_devs,
				       sizeof(struct snd_soc_aux_dev),
				       GFP_KERNEL);
	if (!msm8996_aux_dev)
		return -ENOMEM;

	/* Alloc array of codec conf struct */
	msm8996_codec_conf = devm_kcalloc(&pdev->dev, card->num_aux_devs,
					  sizeof(struct snd_soc_codec_conf),
					  GFP_KERNEL);
	if (!msm8996_codec_conf)
		return -ENOMEM;

	for (i = 0; i < card->num_aux_devs; i++) {
		ret = of_property_read_string_index(pdev->dev.of_node,
						    "qcom,wsa-aux-dev-prefix",
						    wsa881x_dev_info[i].index,
						    wsa_auxdev_name_prefix);
		if (ret) {
			dev_err(&pdev->dev,
				"%s: failed to read wsa aux dev prefix, ret = %d\n",
				__func__, ret);
			return -EINVAL;
		}

		msm8996_aux_dev[i].dlc.name = NULL;
		msm8996_aux_dev[i].dlc.of_node =
					wsa881x_dev_info[i].of_node;
		msm8996_aux_dev[i].init = msm8996_wsa881x_init;
		msm8996_codec_conf[i].dev_name = NULL;
		msm8996_codec_conf[i].name_prefix = wsa_auxdev_name_prefix[0];
		msm8996_codec_conf[i].of_node =
					wsa881x_dev_info[i].of_node;
	}
	card->codec_conf = msm8996_codec_conf;
	card->aux_dev = msm8996_aux_dev;

	return 0;
}
#else
static int msm8996_init_wsa_dev(struct platform_device *pdev,
				struct snd_soc_card *card)
{
	u32 wsa_max_devs = 0;

	of_property_read_u32(pdev->dev.of_node, "qcom,wsa-max-devs",
			     &wsa_max_devs);
	if (wsa_max_devs)
		dev_warn(&pdev->dev,
			 "%s: DT requests %u WSA881x speaker amps but WSA881x/soundwire support is not built; registering the card without them\n",
			 __func__, wsa_max_devs);
	return 0;
}
#endif

static int msm8996_snd_card_late_probe(struct snd_soc_card *card)
{
	const char *be_dl_name = LPASS_BE_SLIMBUS_0_RX;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_component *component;
	void *mbhc_calibration;
	int ret;

	rtd = snd_soc_get_pcm_runtime(card, be_dl_name);
	if (!rtd) {
		dev_err(card->dev, "%s: snd_soc_get_pcm_runtime for %s failed!\n",
			__func__, be_dl_name);
		return -EINVAL;
	}

	component = snd_soc_rtdcom_lookup(rtd, TASHA_CODEC_NAME);
	if (!component) {
		pr_err("%s: component is NULL\n", __func__);
		return -EINVAL;
	}

	/* Start mbhc */
	mbhc_calibration = def_tasha_mbhc_cal();
	if (!mbhc_calibration) {
		pr_err("%s: mbhc_cfg calibration is NULL\n", __func__);
		return -ENOMEM;
	}
	wcd_mbhc_cfg.calibration = mbhc_calibration;
	ret = tasha_mbhc_hs_detect(component, &wcd_mbhc_cfg);
	if (ret) {
		pr_err("%s: mbhc hs detect failed, err:%d\n", __func__, ret);
		wcd_mbhc_cfg.calibration = NULL;
		kfree(mbhc_calibration);
		return ret;
	}

	return 0;
}

static int msm8996_snd_card_remove(struct snd_soc_card *card)
{
	struct msm8996_asoc_mach_data *pdata = snd_soc_card_get_drvdata(card);

	/* the proc entries are freed together with the snd_card */
	if (pdata)
		pdata->codec_root = NULL;
	return 0;
}

static struct snd_soc_card snd_soc_card_tasha_msm8996 = {
	.name		= "msm8996-tasha-snd-card",
	.late_probe	= msm8996_snd_card_late_probe,
	.remove		= msm8996_snd_card_remove,
};

static int msm8996_populate_dai_link_component_of_node(
					struct snd_soc_card *card)
{
	int i, j, index, ret = 0;
	struct device *cdev = card->dev;
	struct snd_soc_dai_link *dai_link = card->dai_link;
	struct snd_soc_dai_link_component *platform, *cpu, *codec;
	struct device_node *np;

	if (!cdev) {
		pr_err("%s: Sound card device memory NULL\n", __func__);
		return -ENODEV;
	}

	for (i = 0; i < card->num_links; i++) {
		platform = dai_link[i].num_platforms ?
				dai_link[i].platforms : NULL;
		cpu = dai_link[i].num_cpus ? dai_link[i].cpus : NULL;

		/* populate platform_of_node for snd card dai links */
		if (platform && platform->name && !platform->of_node &&
		    strcmp(platform->name, MSM8996_DUMMY_NAME)) {
			index = of_property_match_string(cdev->of_node,
						"asoc-platform-names",
						platform->name);
			if (index < 0) {
				pr_err("%s: No match found for platform name: %s\n",
					__func__, platform->name);
				ret = index;
				goto err;
			}
			np = of_parse_phandle(cdev->of_node, "asoc-platform",
					      index);
			if (!np) {
				pr_err("%s: retrieving phandle for platform %s, index %d failed\n",
					__func__, platform->name, index);
				ret = -ENODEV;
				goto err;
			}
			platform->of_node = np;
			platform->name = NULL;
		}

		/* populate cpu_of_node for snd card dai links */
		if (cpu && cpu->dai_name && !cpu->of_node) {
			index = of_property_match_string(cdev->of_node,
						 "asoc-cpu-names",
						 cpu->dai_name);
			if (index >= 0) {
				np = of_parse_phandle(cdev->of_node, "asoc-cpu",
						index);
				if (!np) {
					pr_err("%s: retrieving phandle for cpu dai %s failed\n",
						__func__, cpu->dai_name);
					ret = -ENODEV;
					goto err;
				}
				cpu->of_node = np;
				cpu->dai_name = NULL;
			}
		}

		/* populate codec_of_node for snd card dai links */
		for (j = 0; j < dai_link[i].num_codecs; j++) {
			codec = &dai_link[i].codecs[j];
			if (!codec->name || codec->of_node)
				continue;
			index = of_property_match_string(cdev->of_node,
						 "asoc-codec-names",
						 codec->name);
			if (index < 0)
				continue;
			np = of_parse_phandle(cdev->of_node, "asoc-codec",
					      index);
			if (!np) {
				pr_err("%s: retrieving phandle for codec %s failed\n",
					__func__, codec->name);
				ret = -ENODEV;
				goto err;
			}
			codec->of_node = np;
			codec->name = NULL;
		}
	}

err:
	return ret;
}

static int msm8996_prepare_us_euro(struct snd_soc_card *card)
{
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);
	int ret;

	if (gpio_is_valid(pdata->us_euro_gpio)) {
		dev_dbg(card->dev, "%s: us_euro gpio request %d", __func__,
			pdata->us_euro_gpio);
		ret = gpio_request(pdata->us_euro_gpio, "TASHA_CODEC_US_EURO");
		if (ret) {
			dev_err(card->dev,
				"%s: Failed to request codec US/EURO gpio %d error %d\n",
				__func__, pdata->us_euro_gpio, ret);
			return ret;
		}
		pdata->us_euro_gpio_requested = true;
	}

	return 0;
}

static int msm8996_prepare_hifi(struct snd_soc_card *card)
{
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);
	int ret;

	if (gpio_is_valid(pdata->hph_en1_gpio)) {
		dev_dbg(card->dev, "%s: hph_en1_gpio request %d\n", __func__,
			pdata->hph_en1_gpio);
		ret = gpio_request(pdata->hph_en1_gpio, "hph_en1_gpio");
		if (ret) {
			dev_err(card->dev,
				"%s: hph_en1_gpio request failed, ret:%d\n",
				__func__, ret);
			return ret;
		}
		pdata->hph_en1_gpio_requested = true;
	}
	if (gpio_is_valid(pdata->hph_en0_gpio)) {
		dev_dbg(card->dev, "%s: hph_en0_gpio request %d\n", __func__,
			pdata->hph_en0_gpio);
		ret = gpio_request(pdata->hph_en0_gpio, "hph_en0_gpio");
		if (ret) {
			dev_err(card->dev,
				"%s: hph_en0_gpio request failed, ret:%d\n",
				__func__, ret);
			return ret;
		}
		pdata->hph_en0_gpio_requested = true;
	}
	return 0;
}

static void msm8996_free_gpios(struct msm8996_asoc_mach_data *pdata)
{
	if (pdata->ext_us_amp_gpio_requested) {
		gpio_free(pdata->ext_us_amp_gpio);
		pdata->ext_us_amp_gpio_requested = false;
	}
	if (pdata->us_euro_gpio_requested) {
		gpio_free(pdata->us_euro_gpio);
		pdata->us_euro_gpio_requested = false;
	}
	if (pdata->hph_en1_gpio_requested) {
		gpio_free(pdata->hph_en1_gpio);
		pdata->hph_en1_gpio_requested = false;
	}
	if (pdata->hph_en0_gpio_requested) {
		gpio_free(pdata->hph_en0_gpio);
		pdata->hph_en0_gpio_requested = false;
	}
}

/*
 * SSR handling through the SND event framework (replaces the 4.9 "adsp"
 * subsys notifier + card online/offline done by the codec driver).
 * The sound card is the SND event master; its clients are the q6core
 * (ADSP) device and the WCD9335 slimbus device, which both register with
 * the framework themselves. When any of them goes down the codec AFE config
 * is cleared, and it is re-sent once all of them are up again.
 * snd_soc_card_change_online_state() only exists with CONFIG_AUDIO_QGKI.
 */
static int msm8996_ssr_enable(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct msm8996_asoc_mach_data *pdata;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_component *component;
	int ret = 0;

	if (!card) {
		dev_err(dev, "%s: card is NULL\n", __func__);
		return -EINVAL;
	}

	pdata = snd_soc_card_get_drvdata(card);
	if (pdata && !pdata->is_afe_config_done) {
		rtd = snd_soc_get_pcm_runtime(card, LPASS_BE_SLIMBUS_0_RX);
		if (!rtd) {
			dev_err(dev, "%s: snd_soc_get_pcm_runtime for %s failed!\n",
				__func__, LPASS_BE_SLIMBUS_0_RX);
			return -EINVAL;
		}
		component = snd_soc_rtdcom_lookup(rtd, TASHA_CODEC_NAME);
		if (!component) {
			dev_err(dev, "%s: component is NULL\n", __func__);
			return -EINVAL;
		}
		ret = msm_afe_set_config(component);
		if (ret)
			dev_err(dev, "%s: Failed to set AFE config. err %d\n",
				__func__, ret);
		else
			pdata->is_afe_config_done = true;
	}

#if IS_ENABLED(CONFIG_AUDIO_QGKI)
	snd_soc_card_change_online_state(card, 1);
#endif /* CONFIG_AUDIO_QGKI */
	dev_dbg(dev, "%s: setting snd_card to ONLINE\n", __func__);

	return ret;
}

static void msm8996_ssr_disable(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct msm8996_asoc_mach_data *pdata;

	if (!card) {
		dev_err(dev, "%s: card is NULL\n", __func__);
		return;
	}

	dev_dbg(dev, "%s: setting snd_card to OFFLINE\n", __func__);
#if IS_ENABLED(CONFIG_AUDIO_QGKI)
	snd_soc_card_change_online_state(card, 0);
#endif /* CONFIG_AUDIO_QGKI */

	pr_debug("%s: ADSP/codec is going down. Clearing AFE config\n",
		 __func__);
	msm_afe_clear_config();
	pdata = snd_soc_card_get_drvdata(card);
	if (pdata)
		pdata->is_afe_config_done = false;
}

static const struct snd_event_ops msm8996_ssr_ops = {
	.enable = msm8996_ssr_enable,
	.disable = msm8996_ssr_disable,
};

static int msm8996_audio_ssr_compare(struct device *dev, void *data)
{
	struct device_node *node = data;

	dev_dbg(dev, "%s: dev->of_node = 0x%p, node = 0x%p\n",
		__func__, dev->of_node, node);
	return (dev->of_node && dev->of_node == node);
}

static int msm8996_audio_ssr_register(struct device *dev)
{
	static const char * const default_ssr_compat[] = {
		"qcom,q6core-audio",	/* ADSP (q6core) */
		"qcom,tasha-slim-pgd",	/* WCD9335 slimbus device */
	};
	struct device_node *np = dev->of_node;
	struct snd_event_clients *ssr_clients = NULL;
	struct device_node *node;
	int ret;
	int i;

	for (i = 0; ; i++) {
		node = of_parse_phandle(np, "qcom,msm_audio_ssr_devs", i);
		if (!node)
			break;
		snd_event_mstr_add_client(&ssr_clients,
					  msm8996_audio_ssr_compare, node);
	}

	/*
	 * The msm8996 DT has no qcom,msm_audio_ssr_devs property: fall back
	 * to the q6core and tasha nodes.
	 */
	if (!i) {
		for (i = 0; i < ARRAY_SIZE(default_ssr_compat); i++) {
			node = of_find_compatible_node(NULL, NULL,
						       default_ssr_compat[i]);
			if (!node)
				continue;
			if (!of_device_is_available(node)) {
				of_node_put(node);
				continue;
			}
			snd_event_mstr_add_client(&ssr_clients,
						  msm8996_audio_ssr_compare,
						  node);
		}
	}

	if (IS_ERR_OR_NULL(ssr_clients)) {
		dev_warn(dev, "%s: no SSR clients found\n", __func__);
		return -ENODEV;
	}

	ret = snd_event_master_register(dev, &msm8996_ssr_ops,
					ssr_clients, NULL);
	if (!ret)
		snd_event_notify(dev, SND_EVENT_UP);

	return ret;
}

static const struct of_device_id msm8996_asoc_machine_of_match[]  = {
	{ .compatible = "qcom,msm8996-asoc-snd-tasha",
	  .data = "tasha_codec"},
	{},
};

static struct snd_soc_card *populate_snd_card_dailinks(struct device *dev)
{
	struct snd_soc_card *card = NULL;
	struct snd_soc_dai_link *dailink = NULL;
	int len_1, len_2, len_3, len_4 = 0;
	const struct of_device_id *match;

	match = of_match_node(msm8996_asoc_machine_of_match, dev->of_node);
	if (!match) {
		dev_err(dev, "%s: No DT match found for sound card\n",
			__func__);
		return NULL;
	}

	if (!strcmp(match->data, "tasha_codec")) {
		card = &snd_soc_card_tasha_msm8996;
		len_1 = ARRAY_SIZE(msm8996_common_dai_links);
		len_2 = len_1 + ARRAY_SIZE(msm8996_tasha_fe_dai_links);
		len_3 = len_2 + ARRAY_SIZE(msm8996_common_be_dai_links);

		memcpy(msm8996_tasha_dai_links,
		       msm8996_common_dai_links,
		       sizeof(msm8996_common_dai_links));
		memcpy(msm8996_tasha_dai_links + len_1,
		       msm8996_tasha_fe_dai_links,
		       sizeof(msm8996_tasha_fe_dai_links));
		memcpy(msm8996_tasha_dai_links + len_2,
		       msm8996_common_be_dai_links,
		       sizeof(msm8996_common_be_dai_links));
		memcpy(msm8996_tasha_dai_links + len_3,
		       msm8996_tasha_be_dai_links,
		       sizeof(msm8996_tasha_be_dai_links));

		dailink = msm8996_tasha_dai_links;
		len_4 = len_3 + ARRAY_SIZE(msm8996_tasha_be_dai_links);
	}

	if (!card)
		return NULL;

	if (of_property_read_bool(dev->of_node, "qcom,hdmi-audio-rx")) {
		dev_dbg(dev, "%s(): hdmi audio support present\n",
				__func__);
		memcpy(dailink + len_4, msm8996_hdmi_dai_link,
			sizeof(msm8996_hdmi_dai_link));
		len_4 += ARRAY_SIZE(msm8996_hdmi_dai_link);
	} else {
		dev_dbg(dev, "%s(): No hdmi audio support\n", __func__);
	}

	card->dai_link = dailink;
	card->num_links = len_4;

	return card;
}

static int msm8996_asoc_machine_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct msm8996_asoc_mach_data *pdata;
	const char *mbhc_audio_jack_type = NULL;
	char *mclk_freq_prop_name;
	int ret;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "No platform supplied from device tree\n");
		return -EINVAL;
	}

	pdata = devm_kzalloc(&pdev->dev,
			sizeof(struct msm8996_asoc_mach_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->us_euro_gpio = -EINVAL;
	pdata->hph_en1_gpio = -EINVAL;
	pdata->hph_en0_gpio = -EINVAL;
	pdata->ear_en_gpio = -EINVAL;
	pdata->ext_us_amp_gpio = -EINVAL;

	card = populate_snd_card_dailinks(&pdev->dev);
	if (!card) {
		dev_err(&pdev->dev, "%s: Card uninitialized\n", __func__);
		ret = -EINVAL;
		goto err;
	}
	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, pdata);

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret) {
		dev_err(&pdev->dev, "parse card name failed, err:%d\n",
			ret);
		goto err;
	}

	ret = snd_soc_of_parse_audio_routing(card, "qcom,audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "parse audio routing failed, err:%d\n",
			ret);
		goto err;
	}
	/*
	 * The 4.9 ASoC core only logged routes whose widgets do not exist
	 * (e.g. the WSA "SpkrLeft IN"/"SpkrRight IN" sinks when the WSA881x
	 * speaker amps are absent), while the 5.4 core fails the whole card
	 * on such of_dapm_routes. Register the DT routes as card routes with
	 * route checks disabled to keep the 4.9 behaviour.
	 */
	card->dapm_routes = card->of_dapm_routes;
	card->num_dapm_routes = card->num_of_dapm_routes;
	card->of_dapm_routes = NULL;
	card->num_of_dapm_routes = 0;
	card->disable_route_checks = true;

	mclk_freq_prop_name = "qcom,tasha-mclk-clk-freq";

	ret = of_property_read_u32(pdev->dev.of_node,
			mclk_freq_prop_name, &pdata->mclk_freq);
	if (ret) {
		dev_err(&pdev->dev,
			"Looking up %s property in node %pOF failed, err%d\n",
			mclk_freq_prop_name, pdev->dev.of_node, ret);
		goto err;
	}

	if (pdata->mclk_freq != CODEC_EXT_CLK_RATE) {
		dev_err(&pdev->dev, "unsupported mclk freq %u\n",
			pdata->mclk_freq);
		ret = -EINVAL;
		goto err;
	}

	ret = msm8996_populate_dai_link_component_of_node(card);
	if (ret) {
		ret = -EPROBE_DEFER;
		goto err;
	}

	ret = msm8996_init_wsa_dev(pdev, card);
	if (ret)
		goto err;

	pdata->hph_en1_gpio = of_get_named_gpio(pdev->dev.of_node,
						"qcom,hph-en1-gpio", 0);
	if (!gpio_is_valid(pdata->hph_en1_gpio)) {
		dev_dbg(&pdev->dev, "%s: %s property not found %d\n",
			__func__, "qcom,hph-en1-gpio", pdata->hph_en1_gpio);
	}

	pdata->hph_en0_gpio = of_get_named_gpio(pdev->dev.of_node,
						"qcom,hph-en0-gpio", 0);
	if (!gpio_is_valid(pdata->hph_en0_gpio)) {
		dev_dbg(&pdev->dev, "%s: %s property not found %d\n",
			__func__, "qcom,hph-en0-gpio", pdata->hph_en0_gpio);
	}
	ret = msm8996_prepare_hifi(card);
	if (ret)
		dev_dbg(&pdev->dev, "msm8996_prepare_hifi failed (%d)\n",
			ret);

	/*
	 * Parse US-Euro gpio info from DT. Report no error if us-euro
	 * entry is not found in DT file as some targets do not support
	 * US-Euro detection. (Parsed before the card is registered, as
	 * MBHC is started while the card is being instantiated.)
	 */
	pdata->us_euro_gpio = of_get_named_gpio(pdev->dev.of_node,
				"qcom,us-euro-gpios", 0);
	if (!gpio_is_valid(pdata->us_euro_gpio)) {
		dev_info(&pdev->dev, "property %s not detected in node %pOF",
			"qcom,us-euro-gpios", pdev->dev.of_node);
	} else {
		dev_dbg(&pdev->dev, "%s detected %d",
			"qcom,us-euro-gpios", pdata->us_euro_gpio);
		wcd_mbhc_cfg.swap_gnd_mic = msm8996_swap_gnd_mic;
	}

	ret = msm8996_prepare_us_euro(card);
	if (ret)
		dev_info(&pdev->dev, "msm8996_prepare_us_euro failed (%d)\n",
			ret);

	/* Parse EAR_EN info for NX5L2750C */
	pdata->ear_en_gpio = of_get_named_gpio(pdev->dev.of_node,
				"qcom,ear-en-gpios", 0);
	if (!gpio_is_valid(pdata->ear_en_gpio)) {
		dev_err(&pdev->dev, "property %s not detected in node %pOF",
			"qcom,ear-en-gpios", pdev->dev.of_node);
		ret = -ENODEV;
		goto err;
	}

	ret = snd_soc_register_card(card);
	if (ret == -EPROBE_DEFER) {
		if (codec_reg_done)
			ret = -EINVAL;
		goto err;
	} else if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err;
	}
	dev_info(&pdev->dev, "Sound card %s registered\n", card->name);

	ret = of_property_read_string(pdev->dev.of_node,
		"qcom,mbhc-audio-jack-type", &mbhc_audio_jack_type);
	if (ret) {
		dev_dbg(&pdev->dev, "Looking up %s property in node %pOF failed",
			"qcom,mbhc-audio-jack-type",
			pdev->dev.of_node);
		dev_dbg(&pdev->dev, "Jack type properties set to default");
	} else {
		if (!strcmp(mbhc_audio_jack_type, "4-pole-jack"))
			dev_dbg(&pdev->dev, "This hardware has 4 pole jack");
		else if (!strcmp(mbhc_audio_jack_type, "5-pole-jack"))
			dev_dbg(&pdev->dev, "This hardware has 5 pole jack");
		else if (!strcmp(mbhc_audio_jack_type, "6-pole-jack"))
			dev_dbg(&pdev->dev, "This hardware has 6 pole jack");
		else
			dev_dbg(&pdev->dev, "Unknown value, set to default");
	}

	ret = msm8996_audio_ssr_register(&pdev->dev);
	if (ret) {
		dev_warn(&pdev->dev,
			 "%s: Registration with SND event FWK failed ret = %d, no SSR handling\n",
			 __func__, ret);
	} else {
		pdata->ssr_master_registered = true;
	}

	return 0;
err:
	msm8996_free_gpios(pdata);
	return ret;
}

static int msm8996_asoc_machine_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct msm8996_asoc_mach_data *pdata =
				snd_soc_card_get_drvdata(card);

	if (pdata->ssr_master_registered)
		snd_event_master_deregister(&pdev->dev);

	snd_soc_unregister_card(card);
	msm8996_free_gpios(pdata);

	return 0;
}

static struct platform_driver msm8996_asoc_machine_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = msm8996_asoc_machine_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm8996_asoc_machine_probe,
	.remove = msm8996_asoc_machine_remove,
};
module_platform_driver(msm8996_asoc_machine_driver);

MODULE_DESCRIPTION("ALSA SoC msm");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, msm8996_asoc_machine_of_match);
