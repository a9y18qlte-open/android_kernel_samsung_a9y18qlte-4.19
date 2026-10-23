/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 and
* only version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <linux/delay.h>
#include <sound/soc.h>
#include <dsp/apr_audio-v2.h>
#include <dsp/q6audio-v2.h>
#include <dsp/msm_audio_ion.h>
#include <dsp/rtac.h>
#include <dsp/q6common.h>
#include <dsp/q6voice.h>
#include <ipc/apr.h>
#include "adsp_err.h"

#include <dsp/sec_adaptation.h>
#include "../asoc/msm-pcm-routing-v2.h"
#include "q6_init.h"

#define TIMEOUT_MS	1000
#define TRUE        0x01
#define FALSE       0x00

#define SEC_ADAPTATAION_DSM_SRC_PORT		5	/* AFE */
#define SEC_ADAPTATAION_DSM_DEST_PORT		0	/* AFE */
#define SEC_ADAPTATAION_AUDIO_PORT			3	/* ASM */
#define SEC_ADAPTATAION_LOOPBACK_SRC_PORT	2	/* CVS */
#define SEC_ADAPTATAION_VOICE_SRC_PORT		2	/* CVP */

enum {
	LOOPBACK_DISABLE = 0,
	LOOPBACK_ENABLE,
	LOOPBACK_NODELAY,
	LOOPBACK_MAX,
};

struct afe_ctl {
	void *apr;
	atomic_t state;
	atomic_t status;
	wait_queue_head_t wait;
#if defined(CONFIG_SND_SOC_TFA9872)
	struct rtac_cal_block_data tfa_cal;
	atomic_t tfa_state;
#endif
};

/* Our own APR client of the CVS or CVP service */
struct voice_ctl {
	void *apr;
	atomic_t state;
	wait_queue_head_t wait;
};

static struct afe_ctl this_afe;
static struct voice_ctl this_cvs;
static struct voice_ctl this_cvp;
static struct common_data *common;

static struct mutex asm_lock;

static int loopback_mode;
static int loopback_prev_mode;

struct afe_port {
	unsigned int voice_tracking_id;
	unsigned int amp_rx_id;
	unsigned int amp_tx_id;
	unsigned int rx_topology;
	unsigned int tx_topology;
};
static struct afe_port afe_port;

#ifdef CONFIG_SND_SOC_TFA9872
/* shared mem */
static int afe_nxp_mmap_create(void)
{
	int rc = 0;
	size_t len;
	struct rtac_cal_block_data *tfa_cal = &(this_afe.tfa_cal);

	tfa_cal->map_data.map_size = SZ_4K;

	rc = msm_audio_ion_alloc(&tfa_cal->map_data.dma_buf,
			tfa_cal->map_data.map_size,
			&tfa_cal->cal_data.paddr,
			&len,
			&tfa_cal->cal_data.kvaddr);
	if (rc) {
		pr_err("%s: ION_alloc failed, rc = %d\n", __func__, rc);
		return rc;
	}

	rc = afe_map_rtac_block(tfa_cal);
	if (rc < 0) {
		pr_err("%s : memory mapping failed = %d\n", __func__, rc);
		msm_audio_ion_free(tfa_cal->map_data.dma_buf);

		tfa_cal->map_data.dma_buf = NULL;
	}

	return rc;
}

static void afe_nxp_mmap_destroy(void)
{
	int rc = 0;
	struct rtac_cal_block_data *tfa_cal = &(this_afe.tfa_cal);

	if (tfa_cal->map_data.map_handle == 0) {
		pr_debug("%s: mmap handle is 0, nothing to unmap\n",
			__func__);
		return;
	}

	rc = afe_cmd_memory_unmap(tfa_cal->map_data.map_handle);
	if (rc) {
		pr_err("%s: AFE memory unmap failed %d, handle 0x%x\n",
			__func__, rc, tfa_cal->map_data.map_handle);
	}
	tfa_cal->map_data.map_handle = 0;

	rc = msm_audio_ion_free(tfa_cal->map_data.dma_buf);
	if (rc < 0)
		pr_err("%s: msm_audio_ion_free failed:\n", __func__);

	tfa_cal->map_data.dma_buf = NULL;
}

static int32_t q6audio_tfadsp_callback(struct apr_client_data *data, void *priv)
{
	uint32_t *payload = NULL;

	if ((data == NULL) || (priv == NULL)) {
		pr_err("%s: data or priv is NULL\n", __func__);
		return -EINVAL;
	}

	pr_debug("%s: data->opcode = 0x%x\n", __func__, data->opcode);

	if (data->opcode == RESET_EVENTS) {
		int rc = 0;
		struct rtac_cal_block_data *tfa_cal = &(this_afe.tfa_cal);

		pr_debug("%s: reset event = %d %d apr[%pK]\n",
			__func__,
			data->reset_event, data->reset_proc, this_afe.apr);

		if (this_afe.apr) {
			apr_reset(this_afe.apr);
			atomic_set(&this_afe.state, 0);
			this_afe.apr = NULL;
		}

		/* Free ion memory to fix a SSR(Sub System Reset) test issue*/
		tfa_cal->map_data.map_handle = 0;
		if (tfa_cal->map_data.dma_buf) {
			rc = msm_audio_ion_free(tfa_cal->map_data.dma_buf);
			if (rc < 0)
				pr_err("%s: msm_audio_ion_free failed:\n",
					__func__);
		}

		tfa_cal->map_data.dma_buf = NULL;

		return 0;
	}

	payload = data->payload;
	if (data->opcode == AFE_PORT_CMDRSP_GET_PARAM_V2 ||
	    data->opcode == AFE_PORT_CMDRSP_GET_PARAM_V3) {
		if (atomic_read(&this_afe.tfa_state) == 1 && data->token ==
			q6audio_get_port_index(afe_port.amp_rx_id)) {
			pr_debug("%s:opcode=0x%x, token = %d, payload_size = %d, payload = %d, %d\n",
				__func__, data->opcode,
				data->token, data->payload_size, payload[0], payload[1]);

			if (data->payload_size == sizeof(uint32_t))
				atomic_set(&this_afe.status, payload[0]);
			else if (data->payload_size == (2*sizeof(uint32_t)))
				atomic_set(&this_afe.status, payload[1]);
			atomic_set(&this_afe.tfa_state, 0);
			wake_up(&this_afe.wait);
		}
	} else if (data->payload_size) {
		if (data->opcode == APR_BASIC_RSP_RESULT) {
			/* payload[1] contains the error status for response */
			pr_debug("%s:opcode = 0x%x cmd = 0x%x status = 0x%x token=%d\n",
				__func__, data->opcode,
				payload[0], payload[1], data->token);

			switch (payload[0]) {
			case AFE_PORT_CMD_SET_PARAM_V2:
			case AFE_PORT_CMD_SET_PARAM_V3:
				if (atomic_read(&this_afe.tfa_state) == 1) {
					pr_info("%s:AFE_PORT_CMD_SET_PARAM_V2, token=%d, payload=%d, %d\n",
						__func__, data->token,
						payload[0], payload[1]);

					if (data->payload_size ==
						sizeof(uint32_t))
						atomic_set(&this_afe.status,
						payload[0]);
					else if (data->payload_size ==
						(2*sizeof(uint32_t)))
						atomic_set(&this_afe.status,
						payload[1]);
					atomic_set(&this_afe.tfa_state, 0);
				}
				break;
			case AFE_PORT_CMD_GET_PARAM_V2:
			case AFE_PORT_CMD_GET_PARAM_V3:
				pr_debug("%s: AFE Get Param opcode 0x%x token 0x%x src %d dest %d\n",
					__func__, data->opcode, data->token,
					data->src_port, data->dest_port);

				atomic_set(&this_afe.status,	0);
				atomic_set(&this_afe.tfa_state, 0);
				wake_up(&this_afe.wait);
				break;
			default:
				pr_err("%s: Unknown cmd 0x%x\n",
							__func__, payload[0]);
				break;
			}
		}
	}
	return 0;
}

static int q6audio_tfadsp_read(int dev, int buf_size, char *buf)
{
	int result = 0;
	int tfa_port_id = afe_port.amp_rx_id;
	int index = 0;
	struct afe_tfa_dsp_read_msg_t tfa_dsp_read_msg;
	struct param_hdr_v1 pdata;

	if (buf_size < 0
		|| buf_size > this_afe.tfa_cal.map_data.map_size - sizeof(pdata)
		|| buf == NULL) {
		pr_err("%s: invalid param or buf: buf_size=%d\n",
					__func__, buf_size);
		result = -EINVAL;
		goto fail_cmd;
	}

	if (this_afe.tfa_cal.map_data.dma_buf == NULL) {
		result = afe_nxp_mmap_create();
		if (result) {
			pr_err("%s: could not create mmap for TFADSP! %d\n",
				__func__, result);
			result = -EINVAL;
			goto fail_cmd;
		}
	}

	if (this_afe.apr == NULL) {
		this_afe.apr = apr_register("ADSP", "AFE",
				q6audio_tfadsp_callback,
				SEC_ADAPTATAION_DSM_SRC_PORT, &this_afe);
	}

	pr_info("%s: buf_size = %d\n", __func__, buf_size);

	index = q6audio_get_port_index(tfa_port_id);
	if (index < 0 || index >= AFE_MAX_PORTS) {
		pr_err("%s: AFE port index[%d] invalid!\n",
			__func__, index);
		result = -EINVAL;
		goto fail_cmd;
	}

	/*clean buffer*/
	memset(&tfa_dsp_read_msg, 0x00, sizeof(tfa_dsp_read_msg));

	/*APR header*/
	tfa_dsp_read_msg.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	tfa_dsp_read_msg.hdr.pkt_size = sizeof(tfa_dsp_read_msg);
	tfa_dsp_read_msg.hdr.src_port = SEC_ADAPTATAION_DSM_SRC_PORT;
	tfa_dsp_read_msg.hdr.dest_port = SEC_ADAPTATAION_DSM_DEST_PORT;
	tfa_dsp_read_msg.hdr.token = index;
	tfa_dsp_read_msg.hdr.opcode = AFE_PORT_CMD_GET_PARAM_V2;

	/*get param*/
	tfa_dsp_read_msg.get_param.module_id = AFE_MODULE_ID_TFADSP;
	tfa_dsp_read_msg.get_param.param_id = AFE_PARAM_ID_TFADSP_READ_MSG;
	tfa_dsp_read_msg.get_param.port_id = tfa_port_id;
	tfa_dsp_read_msg.get_param.payload_size = buf_size;

	tfa_dsp_read_msg.get_param.payload_address_lsw =
		lower_32_bits(this_afe.tfa_cal.cal_data.paddr);
	tfa_dsp_read_msg.get_param.payload_address_msw =
		msm_audio_populate_upper_32_bits(this_afe.tfa_cal.cal_data.paddr);
	tfa_dsp_read_msg.get_param.mem_map_handle = this_afe.tfa_cal.map_data.map_handle;

	atomic_set(&this_afe.tfa_state, 1);
	atomic_set(&this_afe.status, 0);

	result = apr_send_pkt(this_afe.apr, (uint32_t *) &tfa_dsp_read_msg);
	if (result < 0) {
		pr_err("%s: failed port = 0x%x, ret = %d\n",
			__func__, afe_port.amp_rx_id, result);
		goto fail_cmd;
	}

	result = wait_event_timeout(this_afe.wait,
				(atomic_read(&this_afe.tfa_state) == 0),
				msecs_to_jiffies(TIMEOUT_MS));
	if (!result) {
		pr_err("%s: wait_event timeout\n", __func__);
		result = -EINVAL;
		goto fail_cmd;
	} else {
		result = 0;
	}

	if (atomic_read(&this_afe.status) > 0) {
		pr_err("%s: config cmd failed [%s]\n", __func__,
			adsp_err_get_err_str(atomic_read(&this_afe.status)));
		result = adsp_err_get_lnx_err_code(
					atomic_read(&this_afe.status));
		goto fail_cmd;
	}

	memcpy(buf, (char *)this_afe.tfa_cal.cal_data.kvaddr + sizeof(pdata),
		buf_size);

	if (buf_size >= 12) {
		int *pReadData = (int *)buf;

		pr_debug("%s: read data pReadData[1]=%d, Impedance=%d mOhms\n"
			, __func__, pReadData[1], (pReadData[1]*1000)/65536);
		pr_debug("%s: read data buf[0]=%02x, buf[1]=%02x, buf[2]=%02x, buf[3]=%02x\n"
			, __func__, buf[0], buf[1], buf[2], buf[3]);
		pr_debug("%s: read data buf[4]=%02x, buf[5]=%02x, buf[6]=%02x, buf[7]=%02x\n"
			, __func__, buf[4], buf[5], buf[6], buf[7]);
		pr_debug("%s: read data buf[8]=%02x, buf[9]=%02x, buf[10]=%02x, buf[11]=%02x\n"
			, __func__, buf[8], buf[9], buf[10], buf[11]);
	}

fail_cmd:
	return result;
}

static int q6audio_tfadsp_write(int dev, int buf_size, const char *buf)
{
	int ret = -EINVAL;
	int tfa_port_id = afe_port.amp_rx_id;
	struct param_hdr_v3 param_hdr;
	int index = 0;

	if (dev < 0) { /*free memory*/
		pr_info("%s: kfree(tfa_dsp_send_msg)\n", __func__);
		return 0;
	}

	if (buf == NULL) {
		pr_err("%s: error! send buf = NULL\n", __func__);
		goto fail_cmd;
	}

	if (this_afe.apr == NULL) {
		this_afe.apr = apr_register("ADSP", "AFE",
				q6audio_tfadsp_callback,
				SEC_ADAPTATAION_DSM_SRC_PORT, &this_afe);
	}

	index = q6audio_get_port_index(tfa_port_id);
	if (index < 0 || index >= AFE_MAX_PORTS) {
		pr_err("%s: AFE port index[%d] invalid!\n",
					__func__, index);
		goto fail_cmd;
	}

	if (((buf[0]<<8)|buf[1]) == 65535)
		pr_info("%s: [0]:0x%x-[1]:0x%x-[2]:0x%x-[3]:0x%x, [4]:0x%x-[5]:0x%x, packet_id:%d, packet_size:%d\n",
			__func__, buf[0], buf[1], buf[2], buf[3],
			buf[4], buf[5], (buf[0]<<8)|buf[1], (buf[2]<<8)|buf[3]);
	pr_debug("%s:buf_size:%d\n", __func__, buf_size);

	memset(&param_hdr, 0, sizeof(param_hdr));
	param_hdr.module_id = AFE_MODULE_ID_TFADSP;
	param_hdr.instance_id = INSTANCE_ID_0;
	param_hdr.param_id = AFE_PARAM_ID_TFADSP_SEND_MSG;
	param_hdr.param_size = buf_size;

	atomic_set(&this_afe.tfa_state, 1);
	ret = q6afe_pack_and_set_param_in_band(tfa_port_id, index,
						param_hdr, (u8 *)buf);
	if (ret) {
		pr_err("%s: q6afe_pack_and_set_param_in_band failed %d\n",
			__func__, ret);
		goto fail_cmd;
	}

fail_cmd:
	pr_debug("%s: status %d\n", __func__, ret);
	return ret;
}
#endif /* CONFIG_SND_SOC_TFA9872 */

int q6audio_get_afe_cal_validation(u16 port_id, u32 topology_id)
{
	int rc = 0;

	if (((topology_id == afe_port.tx_topology) &&
		(port_id == afe_port.amp_tx_id)) ||
		((topology_id == afe_port.rx_topology) &&
		(port_id == afe_port.amp_rx_id))) {
		pr_info("%s: afe cal, port[0x%x] topology[0x%x]\n",
				__func__, port_id, topology_id);
		rc =  TRUE;
	}

	return rc;
}

/****************************************************************************/
/*//////////////////////////// AUDIO SOLUTION //////////////////////////////*/
/****************************************************************************/
static int q6asm_set_pp_param(struct audio_client *ac, u32 module_id,
			      u32 param_id, void *data, u32 size)
{
	struct param_hdr_v3 param_info;
	int rc;

	if (ac == NULL) {
		pr_err("%s: audio client is null\n", __func__);
		return -EINVAL;
	}

	memset(&param_info, 0, sizeof(param_info));
	param_info.module_id = module_id;
	param_info.instance_id = INSTANCE_ID_0;
	param_info.param_id = param_id;
	param_info.param_size = size;

	rc = q6asm_pack_and_set_pp_param_in_band(ac, param_info, data);
	if (rc)
		pr_err("%s: set-params send failed paramid[0x%x] rc %d\n",
			   __func__, param_id, rc);
	return rc;
}

static int q6asm_set_sound_alive(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_sa cmd;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	cmd.OutDevice = param[0];
	cmd.Preset = param[1];
	for (i = 0; i < 9; i++)
		cmd.EqLev[i] = param[i+2];
	cmd.m3Dlevel = param[11];
	cmd.BElevel = param[12];
	cmd.CHlevel = param[13];
	cmd.CHRoomSize = param[14];
	cmd.Clalevel = param[15];
	cmd.volume = param[16];
	cmd.Sqrow = param[17];
	cmd.Sqcol = param[18];
	cmd.TabInfo = param[19];
	cmd.NewUI = param[20];
	cmd.m3DPositionOn = param[21];
	cmd.m3DPositionAngle[0] = param[22];
	cmd.m3DPositionAngle[1] = param[23];
	cmd.m3DPositionGain[0] = param[24];
	cmd.m3DPositionGain[1] = param[25];
	cmd.AHDRonoff = param[26];
	pr_info("%s: device %d preset %d volume %d\n", __func__,
		cmd.OutDevice, cmd.Preset, cmd.volume);

	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SA,
				  ASM_PARAM_ID_PP_SA_PARAMS, &cmd, sizeof(cmd));
}

static int q6asm_set_play_speed(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_vsp cmd = {
		.speed_int = param[0],
	};

	pr_info("%s: %d\n", __func__, cmd.speed_int);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SA_VSP,
				  ASM_PARAM_ID_PP_SA_VSP_PARAMS, &cmd, sizeof(cmd));
}

static int q6asm_set_adaptation_sound(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_adaptation_sound cmd;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable = param[0];
	for (i = 0; i < 12; i++)
		cmd.gain[i/6][i%6] = param[i+1];
	cmd.device = param[13];
	pr_info("%s: enable %d device %d\n", __func__, cmd.enable, cmd.device);

	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_ADAPTATION_SOUND,
				  ASM_PARAM_ID_PP_ADAPTATION_SOUND_PARAMS,
				  &cmd, sizeof(cmd));
}

static int q6asm_set_sound_balance(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_lrsm cmd = {
		.sm = param[0],
		.lr = param[1],
	};

	pr_info("%s: %d %d\n", __func__, cmd.sm, cmd.lr);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_LRSM,
				  ASM_PARAM_ID_PP_LRSM_PARAMS, &cmd, sizeof(cmd));
}

static int q6asm_set_myspace(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_msp cmd = {
		.msp_int = param[0],
	};

	pr_info("%s: %d\n", __func__, cmd.msp_int);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SA_MSP,
				  ASM_MODULE_ID_PP_SA_MSP_PARAM, &cmd, sizeof(cmd));
}

static int q6asm_set_sound_boost(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_sb cmd = {
		.sb_enable = param[0],
	};

	pr_info("%s: %d\n", __func__, cmd.sb_enable);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SB,
				  ASM_PARAM_ID_PP_SB_PARAM, &cmd, sizeof(cmd));
}

static int q6asm_set_upscaler(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_upscaler cmd = {
		.upscaler_enable = param[0],
	};

	pr_info("%s: %d\n", __func__, cmd.upscaler_enable);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SA_UPSCALER_COLOR,
				  ASM_PARAM_ID_PP_SA_UPSCALER_COLOR_PARAMS,
				  &cmd, sizeof(cmd));
}

static int q6asm_set_sb_rotation(struct audio_client *ac, long *param)
{
	struct asm_stream_cmd_set_pp_params_sb_rotation cmd = {
		.sb_rotation = (unsigned int)param[0],
	};

	pr_info("%s: %d\n", __func__, cmd.sb_rotation);
	return q6asm_set_pp_param(ac, ASM_MODULE_ID_PP_SB,
				  ASM_PARAM_ID_PP_SB_ROTATION_PARAM, &cmd, sizeof(cmd));
}

/* The Samsung effects run in the session of the offload front end */
static int sec_audio_asm_put(struct snd_ctl_elem_value *ucontrol,
			     int (*set)(struct audio_client *ac, long *param))
{
	struct msm_pcm_routing_fdai_data fe_dai_map;
	struct audio_client *ac;
	int ret;

	mutex_lock(&asm_lock);
	msm_pcm_routing_get_fedai_info(SEC_ADAPTATAION_AUDIO_PORT,
			SESSION_TYPE_RX, &fe_dai_map);
	ac = q6asm_get_audio_client(fe_dai_map.strm_id);
	ret = set(ac, (long *)ucontrol->value.integer.value);
	mutex_unlock(&asm_lock);

	return ret;
}

static int sec_audio_dummy_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int sec_audio_sound_alive_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_sound_alive);
}

static int sec_audio_play_speed_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_play_speed);
}

static int sec_audio_adaptation_sound_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_adaptation_sound);
}

static int sec_audio_sound_balance_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_sound_balance);
}

static int sec_audio_myspace_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_myspace);
}

static int sec_audio_sound_boost_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_sound_boost);
}

static int sec_audio_upscaler_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_upscaler);
}

static int sec_audio_sb_rotation_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_audio_asm_put(ucontrol, q6asm_set_sb_rotation);
}

static int sec_audio_voice_tracking_info_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int rc = 0;
	int be_idx = 0;
	char *param_value;
	int *update_param_value;
	uint32_t param_size = (RMS_PAYLOAD_LEN + 1) * sizeof(uint32_t);
	struct msm_pcm_routing_bdai_data msm_bedai;
	struct param_hdr_v3 param_hdr;

	param_value = kzalloc(param_size, GFP_KERNEL);
	if (!param_value)
		return -ENOMEM;

	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		msm_pcm_routing_get_bedai_info(be_idx, &msm_bedai);
		if (msm_bedai.port_id == afe_port.voice_tracking_id)
			break;
	}
	if ((be_idx < MSM_BACKEND_DAI_MAX) && msm_bedai.active) {
		memset(&param_hdr, 0, sizeof(param_hdr));
		param_hdr.module_id = ADM_MODULE_ID_PP_SS_REC;
		param_hdr.instance_id = 0x8000;
		param_hdr.param_id = ADM_PARAM_ID_PP_SS_REC_GETPARAMS;
		param_hdr.param_size = param_size;
		rc = adm_get_pp_params(afe_port.voice_tracking_id, 0,
				ADM_CLIENT_ID_DEFAULT, NULL, &param_hdr, param_value);
		if (rc) {
			pr_err("%s: get parameters failed:%d\n", __func__, rc);
			kfree(param_value);
			return -EINVAL;
		}
		update_param_value = (int *)param_value;
		ucontrol->value.integer.value[0] = update_param_value[0];

		pr_debug("%s: FROM DSP value[0] 0x%x\n",
			  __func__, update_param_value[0]);
	}
	kfree(param_value);

	return 0;
}

static int sec_audio_voice_tracking_info_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

/****************************************************************************/
/*//////////////////////////// VOICE SOLUTION //////////////////////////////*/
/****************************************************************************/
/* Session walking as in q6voice.c, whose helpers are static */
static struct voice_data *voice_get_session_by_idx(int idx)
{
	return ((idx < 0 || idx >= MAX_VOC_SESSIONS) ?
				NULL : &common->voice[idx]);
}

static bool voice_itr_get_next_session(struct voice_session_itr *itr,
					struct voice_data **voice)
{
	if (itr->cur_idx <= itr->session_idx) {
		*voice = voice_get_session_by_idx(itr->cur_idx);
		itr->cur_idx++;
		return true;
	}

	*voice = NULL;
	return false;
}

static void voice_itr_init(struct voice_session_itr *itr, u32 session_id)
{
	itr->session_idx = voice_get_idx_for_session(session_id);
	if (session_id == ALL_SESSION_VSID)
		itr->cur_idx = 0;
	else
		itr->cur_idx = itr->session_idx;
}

static bool is_voc_state_active(int voc_state)
{
	return voc_state == VOC_RUN || voc_state == VOC_CHANGE ||
	       voc_state == VOC_STANDBY;
}

/*
 * The voice sessions belong to q6voice, which does the SSR cleanup of them
 * in its own callbacks; on a reset only our APR handles go.
 */
static int32_t q6audio_adaptation_cvs_callback(struct apr_client_data *data,
	void *priv)
{
	uint32_t *ptr = NULL;

	if ((data == NULL) || (priv == NULL)) {
		pr_err("%s: data or priv is NULL\n", __func__);
		return -EINVAL;
	}

	if (data->opcode == RESET_EVENTS) {
		pr_debug("%s: reset event = %d %d apr[%pK]\n", __func__,
			data->reset_event, data->reset_proc, this_cvs.apr);
		if (this_cvs.apr) {
			apr_reset(this_cvs.apr);
			atomic_set(&this_cvs.state, 0);
			this_cvs.apr = NULL;
		}
		return 0;
	}

	if (data->opcode == APR_BASIC_RSP_RESULT && data->payload_size) {
		ptr = data->payload;
		if (ptr[1] != 0)
			pr_err("%s: cmd = 0x%x returned error = 0x%x\n",
				__func__, ptr[0], ptr[1]);

		switch (ptr[0]) {
		case VSS_ICOMMON_CMD_SET_PARAM_V2:
			atomic_set(&this_cvs.state, 0);
			wake_up(&this_cvs.wait);
			break;
		default:
			pr_err("%s: cmd = 0x%x\n", __func__, ptr[0]);
			break;
		}
	}
	return 0;
}

static int32_t q6audio_adaptation_cvp_callback(struct apr_client_data *data,
	void *priv)
{
	uint32_t *ptr = NULL;

	if ((data == NULL) || (priv == NULL)) {
		pr_err("%s: data or priv is NULL\n", __func__);
		return -EINVAL;
	}

	if (data->opcode == RESET_EVENTS) {
		pr_debug("%s: reset event = %d %d apr[%pK]\n", __func__,
			data->reset_event, data->reset_proc, this_cvp.apr);
		if (this_cvp.apr) {
			apr_reset(this_cvp.apr);
			atomic_set(&this_cvp.state, 0);
			this_cvp.apr = NULL;
		}
		return 0;
	}

	if (data->opcode == APR_BASIC_RSP_RESULT && data->payload_size) {
		ptr = data->payload;
		if (ptr[1] != 0)
			pr_err("%s: cmd = 0x%x returned error = 0x%x\n",
				__func__, ptr[0], ptr[1]);

		switch (ptr[0]) {
		case VSS_ICOMMON_CMD_SET_PARAM_V2:
		case VSS_ICOMMON_CMD_SET_UI_PROPERTY:
		case VSS_ICOMMON_CMD_SET_UI_PROPERTY_V2:
			atomic_set(&this_cvp.state, 0);
			wake_up(&this_cvp.wait);
			break;
		default:
			pr_err("%s: cmd = 0x%x\n", __func__, ptr[0]);
			break;
		}
	}
	return 0;
}

/* Sends a packet to a CVS or CVP session and waits for the DSP to take it */
static int sec_voice_send_pkt(struct voice_ctl *ctl, char *svc, apr_fn callback,
			      u16 src_port, struct apr_hdr *hdr, u16 dest_port,
			      u32 opcode, size_t size)
{
	int ret;

	if (ctl->apr == NULL)
		ctl->apr = apr_register("ADSP", svc, callback, src_port, ctl);
	if (ctl->apr == NULL) {
		pr_err("%s: cannot register with %s\n", __func__, svc);
		return -ENODEV;
	}

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->pkt_size = APR_PKT_SIZE(APR_HDR_SIZE, size - APR_HDR_SIZE);
	hdr->src_port = src_port;
	hdr->dest_port = dest_port;
	hdr->token = 0;
	hdr->opcode = opcode;

	atomic_set(&ctl->state, 1);
	ret = apr_send_pkt(ctl->apr, (uint32_t *)hdr);
	if (ret < 0) {
		pr_err("%s: %s opcode 0x%x send failed %d\n",
			__func__, svc, opcode, ret);
		return -EINVAL;
	}

	if (!wait_event_timeout(ctl->wait, (atomic_read(&ctl->state) == 0),
				msecs_to_jiffies(TIMEOUT_MS))) {
		pr_err("%s: %s opcode 0x%x timed out\n", __func__, svc, opcode);
		return -ETIMEDOUT;
	}
	return 0;
}

static int sec_voice_send_cvp(struct voice_data *v, struct apr_hdr *hdr,
			      u32 opcode, size_t size)
{
	return sec_voice_send_pkt(&this_cvp, "CVP",
				  q6audio_adaptation_cvp_callback,
				  SEC_ADAPTATAION_VOICE_SRC_PORT, hdr,
				  v->cvp_handle, opcode, size);
}

/* Runs fn on every active voice session that has its own topology */
static int sec_voice_for_each_session(int (*fn)(struct voice_data *v, long arg),
				      long arg)
{
	struct voice_data *v = NULL;
	struct voice_session_itr itr;
	int ret = 0;

	if (common == NULL)
		return -ENODEV;

	mutex_lock(&common->common_lock);
	voice_itr_init(&itr, ALL_SESSION_VSID);
	while (voice_itr_get_next_session(&itr, &v)) {
		if (v == NULL) {
			pr_err("%s: invalid session\n", __func__);
			ret = -EINVAL;
			break;
		}
		mutex_lock(&v->lock);
		if (is_voc_state_active(v->voc_state) &&
		    (v->lch_mode != VOICE_LCH_START) &&
		    !v->disable_topology)
			ret = fn(v, arg);
		mutex_unlock(&v->lock);
	}
	mutex_unlock(&common->common_lock);

	return ret;
}

static int send_packet_loopback_cmd(struct voice_data *v, int mode)
{
	struct cvs_set_loopback_enable_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.mem_size = 0x10;
	cmd.vss_set_loopback.module_id = VOICEPROC_MODULE_VENC;
	cmd.vss_set_loopback.param_id = VOICE_PARAM_LOOPBACK_ENABLE;
	cmd.vss_set_loopback.param_size = 4;
	cmd.vss_set_loopback.loopback_enable = mode;

	return sec_voice_send_pkt(&this_cvs, "CVS",
				  q6audio_adaptation_cvs_callback,
				  SEC_ADAPTATAION_LOOPBACK_SRC_PORT, &cmd.hdr,
				  v->cvs_handle, VSS_ICOMMON_CMD_SET_PARAM_V2,
				  sizeof(cmd));
}

static int voice_sec_set_loopback_cmd(u32 session_id, uint16_t mode)
{
	struct voice_data *v = NULL;
	struct voice_session_itr itr;
	int ret = 0;

	if (common == NULL)
		return -ENODEV;

	voice_itr_init(&itr, session_id);
	while (voice_itr_get_next_session(&itr, &v)) {
		if (v == NULL) {
			pr_err("%s: invalid session\n", __func__);
			return -EINVAL;
		}
		ret = send_packet_loopback_cmd(v, mode);
	}

	return ret;
}

/* Called by q6voice with the session lock held */
void voice_sec_loopback_start_cmd(u32 session_id)
{
	int ret;

	if (loopback_mode != LOOPBACK_ENABLE && loopback_mode != LOOPBACK_NODELAY)
		return;

	ret = voice_sec_set_loopback_cmd(session_id, loopback_mode);
	if (ret < 0)
		pr_err("%s: send packet loopback cmd failed(%d)\n", __func__, ret);
	else
		pr_info("%s: enable packet loopback\n", __func__);
}

void voice_sec_loopback_end_cmd(u32 session_id)
{
	int ret;

	if (loopback_mode != LOOPBACK_DISABLE ||
	    (loopback_prev_mode != LOOPBACK_ENABLE &&
	     loopback_prev_mode != LOOPBACK_NODELAY))
		return;

	ret = voice_sec_set_loopback_cmd(session_id, loopback_mode);
	if (ret < 0)
		pr_err("%s: packet loopback disable cmd failed(%d)\n", __func__, ret);
	else
		pr_info("%s: disable packet loopback\n", __func__);
	loopback_prev_mode = 0;
}

struct sec_adaptation_sound {
	uint16_t mode;
	uint16_t select;
	int16_t param[12];
};

static int sec_voice_send_adaptation_sound_cmd(struct voice_data *v, long arg)
{
	struct sec_adaptation_sound *as = (struct sec_adaptation_sound *)arg;
	struct cvp_adaptation_sound_parm_send_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.mem_size = 40;
	cmd.adaptation_sound_data.module_id = VOICE_VOICEMODE_MODULE;
	cmd.adaptation_sound_data.param_id = VOICE_ADAPTATION_SOUND_PARAM;
	cmd.adaptation_sound_data.param_size = 28;
	cmd.adaptation_sound_data.eq_mode = as->mode;
	cmd.adaptation_sound_data.select = as->select;
	memcpy(cmd.adaptation_sound_data.param, as->param, sizeof(as->param));

	pr_info("%s: mode = %d, select = %d\n", __func__, as->mode, as->select);
	return sec_voice_send_cvp(v, &cmd.hdr, VSS_ICOMMON_CMD_SET_PARAM_V2,
				  sizeof(cmd));
}

/*
 * Samsung's voice module UI properties: NB mode, speaker mode, device info.
 * Samsung's 4.4 driver sent a v3 parameter header with the v1 opcode, which
 * the DSP rejects (ADSP_EBADPARAM); pack it the way q6voice does instead.
 */
static int sec_voice_send_ui_property(struct voice_data *v, u32 module_id,
				      u32 param_id, int value)
{
	struct {
		struct apr_hdr hdr;
		u8 param_data[sizeof(union param_hdrs) + sizeof(u32)];
	} __packed cmd;
	struct param_hdr_v3 param_hdr;
	u32 data = (u16)value;
	u32 param_size = sizeof(cmd.param_data);
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	memset(&param_hdr, 0, sizeof(param_hdr));
	param_hdr.module_id = module_id;
	param_hdr.instance_id = INSTANCE_ID_0;
	param_hdr.param_id = param_id;
	param_hdr.param_size = sizeof(data);

	ret = q6common_pack_pp_params(cmd.param_data, &param_hdr, (u8 *)&data,
				      &param_size);
	if (ret) {
		pr_err("%s: Failed to pack params, error %d\n", __func__, ret);
		return ret;
	}

	pr_info("%s: module 0x%x param 0x%x = %d\n", __func__,
		module_id, param_id, value);
	return sec_voice_send_cvp(v, &cmd.hdr,
				  q6common_is_instance_id_supported() ?
				  VSS_ICOMMON_CMD_SET_UI_PROPERTY_V2 :
				  VSS_ICOMMON_CMD_SET_UI_PROPERTY,
				  sizeof(cmd.hdr) + param_size);
}

static int sec_voice_send_nb_mode_cmd(struct voice_data *v, long enable)
{
	return sec_voice_send_ui_property(v, VOICE_VOICEMODE_MODULE,
					  VOICE_NBMODE_PARAM, enable);
}

static int sec_voice_send_spk_mode_cmd(struct voice_data *v, long enable)
{
	static const u32 modules[] = {
		VOICE_VOICEMODE_MODULE, VOICE_FVSAM_MODULE, VOICE_WISEVOICE_MODULE,
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(modules); i++) {
		ret = sec_voice_send_ui_property(v, modules[i],
						 VOICE_SPKMODE_PARAM, enable);
		if (ret)
			return ret;
	}
	return 0;
}

static int sec_voice_send_device_info_cmd(struct voice_data *v, long device)
{
	return sec_voice_send_ui_property(v, VOICE_MODULE_SET_DEVICE,
					  VOICE_MODULE_SET_DEVICE_PARAM, device);
}

static int sec_voice_adaptation_sound_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct sec_adaptation_sound as;
	int i;

	as.mode = ucontrol->value.integer.value[0];
	as.select = ucontrol->value.integer.value[1];
	for (i = 0; i < 12; i++)
		as.param[i] = (short)ucontrol->value.integer.value[2+i];

	return sec_voice_for_each_session(sec_voice_send_adaptation_sound_cmd,
					  (long)&as);
}

static int sec_voice_nb_mode_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_voice_for_each_session(sec_voice_send_nb_mode_cmd,
					  ucontrol->value.integer.value[0]);
}

static int sec_voice_spk_mode_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_voice_for_each_session(sec_voice_send_spk_mode_cmd,
					  ucontrol->value.integer.value[0]);
}

static int sec_voice_loopback_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	int mode = ucontrol->value.integer.value[0];

	loopback_prev_mode = loopback_mode;
	if (mode >= LOOPBACK_MAX || mode < LOOPBACK_DISABLE) {
		pr_err("%s: out of range, mode = %d\n", __func__, mode);
		loopback_mode = LOOPBACK_DISABLE;
	} else {
		loopback_mode = mode;
	}
	pr_info("%s: prev_mode = %d, mode = %d\n", __func__,
		loopback_prev_mode, loopback_mode);
	return 0;
}

static int sec_voice_loopback_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = loopback_mode;
	return 0;
}

static const char * const voice_device[] = {
	"ETC", "SPK", "EAR", "BT", "RCV"
};

static const struct soc_enum sec_voice_device_enum[] = {
	SOC_ENUM_SINGLE_EXT(5, voice_device),
};

static int sec_voice_dev_info_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return sec_voice_for_each_session(sec_voice_send_device_info_cmd,
					  ucontrol->value.enumerated.item[0]);
}

/*******************************************************/
/*/////////////////////// COMMON //////////////////////*/
/*******************************************************/
static const struct snd_kcontrol_new samsung_solution_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("SA data", SND_SOC_NOPM, 0, 65535, 0, 27,
				sec_audio_dummy_get, sec_audio_sound_alive_put),
	SOC_SINGLE_MULTI_EXT("VSP data", SND_SOC_NOPM, 0, 65535, 0, 1,
				sec_audio_dummy_get, sec_audio_play_speed_put),
	SOC_SINGLE_MULTI_EXT("Audio DHA data", SND_SOC_NOPM, 0, 65535, 0, 14,
				sec_audio_dummy_get, sec_audio_adaptation_sound_put),
	SOC_SINGLE_MULTI_EXT("LRSM data", SND_SOC_NOPM, 0, 65535, 0, 2,
				sec_audio_dummy_get, sec_audio_sound_balance_put),
	SOC_SINGLE_EXT("VoiceTrackInfo", SND_SOC_NOPM, 0, 2147483647, 0,
				sec_audio_voice_tracking_info_get,
				sec_audio_voice_tracking_info_put),
	SOC_SINGLE_MULTI_EXT("MSP data", SND_SOC_NOPM, 0, 65535, 0, 1,
				sec_audio_dummy_get, sec_audio_myspace_put),
	SOC_SINGLE_MULTI_EXT("SB enable", SND_SOC_NOPM, 0, 65535, 0, 1,
				sec_audio_dummy_get, sec_audio_sound_boost_put),
	SOC_SINGLE_MULTI_EXT("UPSCALER", SND_SOC_NOPM, 0, 65535, 0, 1,
				sec_audio_dummy_get, sec_audio_upscaler_put),
	SOC_SINGLE_MULTI_EXT("SB rotation", SND_SOC_NOPM, 0, 65535, 0, 1,
				sec_audio_dummy_get, sec_audio_sb_rotation_put),
	SOC_SINGLE_MULTI_EXT("Sec Set DHA data", SND_SOC_NOPM, 0, 65535, 0, 14,
				sec_audio_dummy_get, sec_voice_adaptation_sound_put),
	SOC_SINGLE_EXT("NB Mode", SND_SOC_NOPM, 0, 1, 0,
				sec_audio_dummy_get, sec_voice_nb_mode_put),
	SOC_SINGLE_EXT("Speaker Sensor Mode", SND_SOC_NOPM, 0, 1, 0,
				sec_audio_dummy_get, sec_voice_spk_mode_put),
	SOC_SINGLE_EXT("Loopback Enable", SND_SOC_NOPM, 0, LOOPBACK_MAX, 0,
				sec_voice_loopback_get, sec_voice_loopback_put),
	SOC_ENUM_EXT("Voice Device Info", sec_voice_device_enum[0],
				sec_audio_dummy_get, sec_voice_dev_info_put),
};

static int q6audio_adaptation_component_probe(struct snd_soc_component *component)
{
	common = voice_get_common_data();
	return 0;
}

/* Bound to the card by the machine driver's ADAPTATION Hostless link */
static const struct snd_soc_component_driver q6audio_adaptation = {
	.name		= "q6audio-adaptation",
	.probe		= q6audio_adaptation_component_probe,
	.controls	= samsung_solution_mixer_controls,
	.num_controls	= ARRAY_SIZE(samsung_solution_mixer_controls),
};

static int samsung_q6audio_adaptation_probe(struct platform_device *pdev)
{
	int ret;

	pr_info("%s: dev name %s\n", __func__, dev_name(&pdev->dev));

	atomic_set(&this_afe.state, 0);
	atomic_set(&this_afe.status, 0);
	atomic_set(&this_cvs.state, 0);
	atomic_set(&this_cvp.state, 0);
	this_afe.apr = NULL;
	this_cvs.apr = NULL;
	this_cvp.apr = NULL;
	init_waitqueue_head(&this_afe.wait);
	init_waitqueue_head(&this_cvs.wait);
	init_waitqueue_head(&this_cvp.wait);
	mutex_init(&asm_lock);

	ret = of_property_read_u32(pdev->dev.of_node,
			"adaptation,voice-tracking-tx-port-id",
			&afe_port.voice_tracking_id);
	if (ret)
		pr_err("%s : Unable to read Tx BE\n", __func__);

	ret = of_property_read_u32(pdev->dev.of_node,
			"adaptation,amp-rx-port-id",
			&afe_port.amp_rx_id);
	if (ret)
		pr_debug("%s : Unable to find amp-rx-port\n", __func__);

	ret = of_property_read_u32(pdev->dev.of_node,
			"adaptation,amp-tx-port-id",
			&afe_port.amp_tx_id);
	if (ret)
		pr_debug("%s : Unable to find amp-tx-port\n", __func__);

#if defined(CONFIG_SND_SOC_TFA9872)
	afe_port.tx_topology = AFE_TOPOLOGY_ID_TFADSP_TX;
	afe_port.rx_topology = AFE_TOPOLOGY_ID_TFADSP_RX;
#endif

#if defined(CONFIG_SND_SOC_TFA9872)
	if (this_afe.apr == NULL) {
		this_afe.apr = apr_register("ADSP", "AFE",
				q6audio_tfadsp_callback,
				SEC_ADAPTATAION_DSM_SRC_PORT, &this_afe);
	}
	atomic_set(&this_afe.tfa_state, 0);
	memset(&this_afe.tfa_cal, 0x00, sizeof(struct rtac_cal_block_data));
	tfa_ext_register(q6audio_tfadsp_write, q6audio_tfadsp_read, NULL);
#endif /* CONFIG_SND_SOC_TFA9872 */

	return snd_soc_register_component(&pdev->dev, &q6audio_adaptation,
					  NULL, 0);
}

static int samsung_q6audio_adaptation_remove(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
#if defined(CONFIG_SND_SOC_TFA9872)
	q6audio_tfadsp_write(-1, 0, NULL);
	afe_nxp_mmap_destroy(); /* shared mem */
#endif
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id samsung_q6audio_adaptation_dt_match[] = {
	{.compatible = "samsung,q6audio-adaptation"},
	{}
};
MODULE_DEVICE_TABLE(of, samsung_q6audio_adaptation_dt_match);

static struct platform_driver samsung_q6audio_adaptation_driver = {
	.driver = {
		.name = "samsung-q6audio-adaptation",
		.owner = THIS_MODULE,
		.of_match_table = samsung_q6audio_adaptation_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = samsung_q6audio_adaptation_probe,
	.remove = samsung_q6audio_adaptation_remove,
};

int __init sec_soc_platform_init(void)
{
	pr_debug("%s\n", __func__);
	return platform_driver_register(&samsung_q6audio_adaptation_driver);
}

void sec_soc_platform_exit(void)
{
	pr_debug("%s\n", __func__);
	platform_driver_unregister(&samsung_q6audio_adaptation_driver);
}

MODULE_AUTHOR("SeokYoung Jang, <quartz.jang@samsung.com>");
MODULE_DESCRIPTION("Samsung ASoC ADSP Adaptation driver");
MODULE_LICENSE("GPL v2");
