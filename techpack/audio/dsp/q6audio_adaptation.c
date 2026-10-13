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
#include <ipc/apr.h>
#include "adsp_err.h"

#include <dsp/sec_adaptation.h>
#include "q6_init.h"

#define TIMEOUT_MS	1000
#define TRUE        0x01
#define FALSE       0x00

#define SEC_ADAPTATAION_DSM_SRC_PORT		5	/* AFE */
#define SEC_ADAPTATAION_DSM_DEST_PORT		0	/* AFE */

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

static struct afe_ctl this_afe;

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

static const struct snd_soc_component_driver q6audio_adaptation = {
	.name		= "q6audio-adaptation",
};

static int samsung_q6audio_adaptation_probe(struct platform_device *pdev)
{
	int ret;

	pr_info("%s: dev name %s\n", __func__, dev_name(&pdev->dev));

	atomic_set(&this_afe.state, 0);
	atomic_set(&this_afe.status, 0);
	this_afe.apr = NULL;
	init_waitqueue_head(&this_afe.wait);

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
