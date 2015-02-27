﻿//  -----------------------------------------------------------------------------------------
//    NVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  -----------------------------------------------------------------------------------------

#include <Windows.h>
#include <Process.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib") 
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>

#include "output.h"
#include "vphelp_client.h"

#pragma warning( push )
#pragma warning( disable: 4127 )
#include "afs_client.h"
#pragma warning( pop )

#include "convert.h"

#include "auo.h"
#include "auo_frm.h"
#include "auo_pipe.h"
#include "auo_error.h"
#include "auo_conf.h"
#include "auo_util.h"
#include "auo_system.h"
#include "auo_version.h"

#include "auo_encode.h"
#include "auo_video.h"
#include "auo_audio_parallel.h"

#include "auo_nvenc.h"
#include "ConvertCSP.h"

AUO_RESULT aud_parallel_task(const OUTPUT_INFO *oip, PRM_ENC *pe);

static int calc_input_frame_size(int width, int height, int color_format) {
	width = (color_format == CF_RGB) ? (width+3) & ~3 : (width+1) & ~1;
	return width * height * COLORFORMATS[color_format].size;
}

BOOL setup_afsvideo(const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat, CONF_GUIEX *conf, PRM_ENC *pe) {
	//すでに初期化してある または 必要ない
	if (pe->afs_init || pe->video_out_type == VIDEO_OUTPUT_DISABLED || !conf->vid.afs)
		return TRUE;

	const int color_format = CF_YUY2;
	const int frame_size = calc_input_frame_size(oip->w, oip->h, color_format);
	//Aviutl(自動フィールドシフト)からの映像入力
	if (afs_vbuf_setup((OUTPUT_INFO *)oip, conf->vid.afs, frame_size, COLORFORMATS[color_format].FOURCC)) {
		pe->afs_init = TRUE;
		return TRUE;
	} else if (conf->vid.afs && sys_dat->exstg->s_local.auto_afs_disable) {
		afs_vbuf_release(); //一度解放
		warning_auto_afs_disable();
		conf->vid.afs = FALSE;
		//再度使用するmuxerをチェックする
		pe->muxer_to_be_used = check_muxer_to_be_used(conf, sys_dat, pe->temp_filename, pe->video_out_type, (oip->flag & OUTPUT_INFO_FLAG_AUDIO) != 0);
		return TRUE;
	}
	//エラー
	error_afs_setup(conf->vid.afs, sys_dat->exstg->s_local.auto_afs_disable);
	return FALSE;
}

void close_afsvideo(PRM_ENC *pe) {
	if (!pe->afs_init || pe->video_out_type == VIDEO_OUTPUT_DISABLED)
		return;

	afs_vbuf_release();

	pe->afs_init = FALSE;
}

AuoEncodeStatus::AuoEncodeStatus() {

}

AuoEncodeStatus::~AuoEncodeStatus() {

}

void AuoEncodeStatus::UpdateDisplay(const TCHAR *mes) {
	set_log_title_and_progress(mes, (m_sData.frameOut + m_sData.frameDrop) / (double)m_sData.frameTotal);
}

void AuoEncodeStatus::WriteLine(const TCHAR *mes) {
	const char *HEADER = "nvenc [info]: ";
	int buf_len = strlen(mes) + 1 + strlen(HEADER);
	char *buf = (char *)calloc(buf_len, sizeof(buf[0]));
	if (buf) {
		memcpy(buf, HEADER, strlen(HEADER));
		memcpy(buf + strlen(HEADER), mes, strlen(mes) + 1);
		write_log_line(LOG_INFO, buf);
		free(buf);
	}
}

AuoInput::AuoInput() {
	oip = NULL;
	conf = NULL;
	pe = NULL;
	m_tmLastUpdate = timeGetTime();
	m_pause = FALSE;
}

AuoInput::~AuoInput() {
	Close();
}

void AuoInput::Close() {
	if (pe)
		close_afsvideo(pe);
	oip = NULL;
	conf = NULL;
	pe = NULL;
	m_iFrame = 0;
	disable_enc_control();
}
int AuoInput::Init(InputVideoInfo *inputPrm, EncodeStatus *pStatus) {
	Close();
	
	m_pStatus = pStatus;
	auto *info = reinterpret_cast<InputInfoAuo *>(inputPrm->otherPrm);

	oip = info->oip;
	conf = info->conf;
	pe = info->pe;
	jitter = info->jitter;
	m_interlaced = info->interlaced;

	int fps_gcd = nv_get_gcd(oip->rate, oip->scale);

	pStatus->m_sData.frameTotal = oip->n;
	inputPrm->width = oip->w;
	inputPrm->height = oip->h;
	inputPrm->rate = oip->rate / fps_gcd;
	inputPrm->scale = oip->scale / fps_gcd;

	m_pConvCSPInfo = get_convert_csp_func(NV_ENC_CSP_YUY2, NV_ENC_CSP_NV12, false);

	enable_enc_control(&m_pause, pe->afs_init, FALSE, timeGetTime(), oip->n);

	if (conf->vid.afs) {
		if (!setup_afsvideo(oip, info->sys_dat, conf, pe)) {
			m_inputMes = _T("raw: 自動フィールドシフトの初期化に失敗しました。\n");
			return 1;
		}
	}
	
	setSurfaceInfo(inputPrm);
	m_stSurface.src_pitch = inputPrm->width;
	CreateInputInfo(_T("auo"), _T("yuy2"), (m_interlaced) ? _T("nv12i") : _T("nv12p"), get_simd_str(m_pConvCSPInfo->simd), inputPrm);

	return 0;
}
int AuoInput::LoadNextFrame(void *dst, int dst_pitch) {
	if (FALSE != (pe->aud_parallel.abort = oip->func_is_abort()))
		return NVENC_THREAD_ABORT;

	while (m_pause) {
		Sleep(LOG_UPDATE_INTERVAL);
		if (oip->func_is_abort())
			return NVENC_THREAD_ABORT;
		log_process_events();
	}

	if (m_iFrame >= oip->n) {
		oip->func_rest_time_disp(m_iFrame-1, oip->n);
		release_audio_parallel_events(pe);
		return NVENC_THREAD_FINISHED;
	}

	void *frame = NULL;
	if (conf->vid.afs) {
		BOOL drop = FALSE;
		for (;;) {
			if ((frame = afs_get_video((OUTPUT_INFO *)oip, m_iFrame, &drop, &jitter[m_iFrame + 1])) == NULL) {
				error_afs_get_frame();
				return false;
			}
			if (!drop)
				break;
			jitter[m_iFrame] = DROP_FRAME_FLAG;
			pe->drop_count++;
			m_pStatus->m_sData.frameDrop++;
			m_iFrame++;
			if (m_iFrame >= oip->n) {
				oip->func_rest_time_disp(m_iFrame, oip->n);
				release_audio_parallel_events(pe);
				return false;
			}
		}
	} else {
		if ((frame = oip->func_get_video_ex(m_iFrame, COLORFORMATS[CF_YUY2].FOURCC)) == NULL) {
			error_afs_get_frame();
			return false;
		}
	}
	m_pConvCSPInfo->func[!!m_interlaced](&dst, (const void **)&frame, m_stSurface.width, m_stSurface.src_pitch * 2, 0, dst_pitch, m_stSurface.height, m_stSurface.height, m_stSurface.crop);

	m_iFrame++;
	if (!(m_iFrame & 7))
		aud_parallel_task(oip, pe);

	m_pStatus->m_sData.frameIn++;

	uint32_t tm = timeGetTime();
	if (tm - m_tmLastUpdate > 800) {
		m_tmLastUpdate = tm;
		oip->func_rest_time_disp(m_iFrame, oip->n);
		oip->func_update_preview();
		m_pStatus->UpdateDisplay();
	}

	return NVENC_THREAD_RUNNING;
}

CAuoNvEnc::CAuoNvEnc() {

}

CAuoNvEnc::~CAuoNvEnc() {

}

NVENCSTATUS CAuoNvEnc::InitInput(InEncodeVideoParam *inputParam) {
	m_pStatus = new AuoEncodeStatus();
	m_pInput = new AuoInput();
	int ret = m_pInput->Init(&inputParam->input, m_pStatus);
	m_pStatus->m_nOutputFPSRate = inputParam->input.rate;
	m_pStatus->m_nOutputFPSScale = inputParam->input.scale;
	return (ret) ? NV_ENC_ERR_GENERIC : NV_ENC_SUCCESS;
}

#pragma warning (push)
#pragma warning (disable:4100)
int CAuoNvEnc::NVPrintf(FILE *fp, int logLevel, const TCHAR *format, ...) {
	if (logLevel < m_nLogLevel)
		return 0;

	logLevel = clamp(logLevel, LOG_INFO, LOG_ERROR);

	va_list args;
	va_start(args, format);

	int len = _vscprintf(format, args);
	char *const buffer = (char*)malloc((len+1) * sizeof(buffer[0])); // _vscprintf doesn't count terminating '\0'

	vsprintf_s(buffer, len+1, format, args);

	static const char *const LOG_LEVEL_STR[] = { "info", "warning", "error" };
	const int mes_line_len = len+1 + strlen("nvenc [warning]: ");
	char *const mes_line = (char *)malloc(mes_line_len * sizeof(mes_line[0]));

	char *a, *b, *mes = buffer;
	char *const fin = mes + len+1; //null文字の位置
	while ((a = strchr(mes, '\n')) != NULL) {
		if ((b = strrchr(mes, '\r', a - mes - 2)) != NULL)
			mes = b + 1;
		*a = '\0';
		sprintf_s(mes_line, mes_line_len, "nvenc [%s]: %s", LOG_LEVEL_STR[logLevel], mes);
		write_log_line(logLevel, mes_line);
		mes = a + 1;
	}
	if ((a = strrchr(mes, '\r', fin - mes - 1)) != NULL) {
		b = a - 1;
		while (*b == ' ' || *b == '\r')
			b--;
		*(b+1) = '\0';
		if ((b = strrchr(mes, '\r', b - mes - 2)) != NULL)
			mes = b + 1;
		set_window_title(mes);
		mes = a + 1;
	}

	free(buffer);
	free(mes_line);
	return len;
}
#pragma warning(pop)
