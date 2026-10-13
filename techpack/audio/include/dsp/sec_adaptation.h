/*
 * ALSA SoC - Samsung Adaptation driver
 *
 * Copyright (c) 2016 Samsung Electronics Co. Ltd.
  *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SEC_ADAPTATION_H
#define __SEC_ADAPTATION_H

#include <dsp/q6afe-v2.h>
#include <dsp/q6asm-v2.h>
#include <dsp/q6adm-v2.h>

/****************************************************************************/
/*//////////////////// NXP SPEAKER AMP /////////////////////////////////////*/
/****************************************************************************/
#if defined(CONFIG_SND_SOC_TFA9872)
#include <sound/tfa_ext.h>

/*Module ID*/
#define AFE_MODULE_ID_TFADSP          0x1000B910

/*Param ID*/
#define AFE_PARAM_ID_TFADSP_SEND_MSG  0x1000B921
#define AFE_PARAM_ID_TFADSP_READ_MSG  0x1000B922
#define AFE_PARAM_ID_TFADSP_RESP_MSG  0x1000B922

/*Topology ID*/
#define AFE_TOPOLOGY_ID_TFADSP_TX     0x1000B901
#define AFE_TOPOLOGY_ID_TFADSP_RX     0x1000B900

#define AFE_OPCODE_TFADSP_STATUS      0x00010B01
#define AFE_EVENT_TFADSP_STATE_INIT   0x1
#define AFE_EVENT_TFADSP_STATE_CLOSE  0x2

#define AFE_RX_MODULE_ID_TFADSP       0x1000B900
#define AFE_RX_NONE_TOPOLOGY          0x000112fc

#define AFE_TFADSP_STATIC_MEMORY

/*
 * in case of CONFIG_MSM_QDSP6_APRV2_GLINK/APRV3_GLINK,
 * with smaller APR_MAX_BUF (512)
 */
#define AFE_APR_MAX_PKT_SIZE  APR_MAX_BUF

/*afe tfadsp msg type*/
#define AFE_TFADSP_MSG_TYPE_NORMAL 0
#define AFE_TFADSP_MSG_TYPE_RAW    1

/*afe tfa dsp get param (out-of-band, 4.4 afe_rtac_get_param_v2 layout)*/
struct afe_tfa_dsp_get_param_t {
	u16 port_id;
	u16 payload_size;
	u32 payload_address_lsw;
	u32 payload_address_msw;
	u32 mem_map_handle;
	u32 module_id;
	u32 param_id;
} __packed;

/*afe tfa dsp read message*/
struct afe_tfa_dsp_read_msg_t {
	struct apr_hdr hdr;
	struct afe_tfa_dsp_get_param_t get_param;
} __packed;

int q6afe_pack_and_set_param_in_band(u16 port_id, int index,
				     struct param_hdr_v3 param_hdr,
				     u8 *param_data);
#endif /* CONFIG_SND_SOC_TFA9872 */

int q6audio_get_afe_cal_validation(u16 port_id, u32 topology_id);

/****************************************************************************/
/*//////////////////////////// VOICE SOLUTION //////////////////////////////*/
/****************************************************************************/
/* NXP LVVEFQ */
#define VPM_TX_SM_LVVEFQ_COPP_TOPOLOGY      0x1000BFF0
#define VPM_TX_DM_LVVEFQ_COPP_TOPOLOGY      0x1000BFF1
#define VPM_TX_SM_LVSAFQ_COPP_TOPOLOGY      0x1000BFF4
/* Fotemeia */
#define VOICE_TX_DIAMONDVOICE_FVSAM_SM      0x1000110B
#define VOICE_TX_DIAMONDVOICE_FVSAM_DM      0x1000110A
#define VOICE_TX_DIAMONDVOICE_FVSAM_QM      0x10001109

#endif /* __SEC_ADAPTATION_H */
