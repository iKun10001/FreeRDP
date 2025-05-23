/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression
 *
 * Copyright 2015 Marc-André Moreau <marcandre.moreau@gmail.com>
 * Copyright 2014 Mike McDonald <Mike.McDonald@software.dell.com>
 * Copyright 2014 erbth <t.erbesdobler@team103.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <freerdp/config.h>

#include <winpr/wlog.h>
#include <freerdp/log.h>
#include <freerdp/codec/h264.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "h264.h"

#ifdef WITH_VAAPI
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(55, 9, 0)
#include <libavutil/hwcontext.h>
#else
#pragma warning You have asked for VA - API decoding, \
    but your version of libavutil is too old !Disabling.
#undef WITH_VAAPI
#endif
#endif

/* Fallback support for older libavcodec versions */
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54, 59, 100)
#define AV_CODEC_ID_H264 CODEC_ID_H264
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56, 34, 2)
#define AV_CODEC_FLAG_LOOP_FILTER CODEC_FLAG_LOOP_FILTER
#define AV_CODEC_CAP_TRUNCATED CODEC_CAP_TRUNCATED
#define AV_CODEC_FLAG_TRUNCATED CODEC_FLAG_TRUNCATED
#endif

#if LIBAVUTIL_VERSION_MAJOR < 52
#define AV_PIX_FMT_YUV420P PIX_FMT_YUV420P
#endif

/* Ubuntu 14.04 ships without the functions provided by avutil,
 * so define error to string methods here. */
#if !defined(av_err2str)
static inline char* error_string(char* errbuf, size_t errbuf_size, int errnum)
{
	av_strerror(errnum, errbuf, errbuf_size);
	return errbuf;
}

#define av_err2str(errnum) error_string((char[64]){ 0 }, 64, errnum)
#endif

#if defined(WITH_VAAPI) || defined(WITH_VAAPI_H264_ENCODING)
#define VAAPI_DEVICE "/dev/dri/renderD128"
#endif

typedef struct
{
	const AVCodec* codecDecoder;
	AVCodecContext* codecDecoderContext;
	const AVCodec* codecEncoder;
	AVCodecContext* codecEncoderContext;
	AVCodecParserContext* codecParser;
	AVFrame* videoFrame;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 133, 100)
	AVPacket bufferpacket;
#endif
	AVPacket* packet;
#if defined(WITH_VAAPI) || defined(WITH_VAAPI_H264_ENCODING)
	AVBufferRef* hwctx;
	AVFrame* hwVideoFrame;
	enum AVPixelFormat hw_pix_fmt;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 80, 100)
	AVBufferRef* hw_frames_ctx;
#endif
#endif
} H264_CONTEXT_LIBAVCODEC;

static void libavcodec_destroy_encoder_context(H264_CONTEXT* WINPR_RESTRICT h264)
{
	H264_CONTEXT_LIBAVCODEC* sys = NULL;

	if (!h264 || !h264->subsystem)
		return;

	sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;

	if (sys->codecEncoderContext)
	{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 69, 100)
		avcodec_free_context(&sys->codecEncoderContext);
#else
		avcodec_close(sys->codecEncoderContext);
		av_free(sys->codecEncoderContext);
#endif
	}

	sys->codecEncoderContext = NULL;
}

#ifdef WITH_VAAPI_H264_ENCODING
static int set_hw_frames_ctx(H264_CONTEXT* WINPR_RESTRICT h264)
{
	H264_CONTEXT_LIBAVCODEC* sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;
	AVBufferRef* hw_frames_ref = NULL;
	AVHWFramesContext* frames_ctx = NULL;
	int err = 0;

	if (!(hw_frames_ref = av_hwframe_ctx_alloc(sys->hwctx)))
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to create VAAPI frame context");
		return -1;
	}
	frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
	frames_ctx->format = AV_PIX_FMT_VAAPI;
	frames_ctx->sw_format = AV_PIX_FMT_NV12;
	frames_ctx->width = sys->codecEncoderContext->width;
	frames_ctx->height = sys->codecEncoderContext->height;
	frames_ctx->initial_pool_size = 20;
	if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "Failed to initialize VAAPI frame context."
		           "Error code: %s",
		           av_err2str(err));
		av_buffer_unref(&hw_frames_ref);
		return err;
	}
	sys->codecEncoderContext->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
	if (!sys->codecEncoderContext->hw_frames_ctx)
		err = AVERROR(ENOMEM);

	av_buffer_unref(&hw_frames_ref);
	return err;
}
#endif

static BOOL libavcodec_create_encoder_context(H264_CONTEXT* WINPR_RESTRICT h264)
{
	BOOL recreate = FALSE;
	H264_CONTEXT_LIBAVCODEC* sys = NULL;

	if (!h264 || !h264->subsystem)
		return FALSE;

	if ((h264->width > INT_MAX) || (h264->height > INT_MAX))
		return FALSE;

	sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;
	if (!sys || !sys->codecEncoder)
		return FALSE;

	recreate = !sys->codecEncoderContext;

	if (sys->codecEncoderContext)
	{
		if ((sys->codecEncoderContext->width != (int)h264->width) ||
		    (sys->codecEncoderContext->height != (int)h264->height))
			recreate = TRUE;
	}

	if (!recreate)
		return TRUE;

	libavcodec_destroy_encoder_context(h264);

	sys->codecEncoderContext = avcodec_alloc_context3(sys->codecEncoder);

	if (!sys->codecEncoderContext)
		goto EXCEPTION;

	switch (h264->RateControlMode)
	{
		case H264_RATECONTROL_VBR:
			sys->codecEncoderContext->bit_rate = h264->BitRate;
			break;

		case H264_RATECONTROL_CQP:
			if (av_opt_set_int(sys->codecEncoderContext, "qp", h264->QP, AV_OPT_SEARCH_CHILDREN) <
			    0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "av_opt_set_int failed");
			}
			break;

		default:
			break;
	}

	sys->codecEncoderContext->width = (int)MIN(INT32_MAX, h264->width);
	sys->codecEncoderContext->height = (int)MIN(INT32_MAX, h264->height);
	sys->codecEncoderContext->delay = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 13, 100)
	sys->codecEncoderContext->framerate =
	    (AVRational){ WINPR_ASSERTING_INT_CAST(int, h264->FrameRate), 1 };
#endif
	sys->codecEncoderContext->time_base =
	    (AVRational){ 1, WINPR_ASSERTING_INT_CAST(int, h264->FrameRate) };
	av_opt_set(sys->codecEncoderContext, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);

	sys->codecEncoderContext->flags |= AV_CODEC_FLAG_LOOP_FILTER;

#ifdef WITH_VAAPI_H264_ENCODING
	if (sys->hwctx)
	{
		av_opt_set(sys->codecEncoderContext, "preset", "veryslow", AV_OPT_SEARCH_CHILDREN);

		sys->codecEncoderContext->pix_fmt = AV_PIX_FMT_VAAPI;
		/* set hw_frames_ctx for encoder's AVCodecContext */
		if (set_hw_frames_ctx(h264) < 0)
			goto EXCEPTION;
	}
	else
#endif
	{
		av_opt_set(sys->codecEncoderContext, "preset", "medium", AV_OPT_SEARCH_CHILDREN);
		sys->codecEncoderContext->pix_fmt = AV_PIX_FMT_YUV420P;
	}

	if (avcodec_open2(sys->codecEncoderContext, sys->codecEncoder, NULL) < 0)
		goto EXCEPTION;

	return TRUE;
EXCEPTION:
	libavcodec_destroy_encoder_context(h264);
	return FALSE;
}

static int libavcodec_decompress(H264_CONTEXT* WINPR_RESTRICT h264,
                                 const BYTE* WINPR_RESTRICT pSrcData, UINT32 SrcSize)
{
	union
	{
		const BYTE* cpv;
		BYTE* pv;
	} cnv;
	int rc = -1;
	int status = 0;
	int gotFrame = 0;
	AVPacket* packet = NULL;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData || (SrcSize == 0));

	H264_CONTEXT_LIBAVCODEC* sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;
	BYTE** pYUVData = h264->pYUVData;
	UINT32* iStride = h264->iStride;

	WINPR_ASSERT(sys);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 133, 100)
	packet = &sys->bufferpacket;
	WINPR_ASSERT(packet);
	av_init_packet(packet);
#else
	packet = av_packet_alloc();
#endif
	if (!packet)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate AVPacket");
		goto fail;
	}

	cnv.cpv = pSrcData;
	packet->data = cnv.pv;
	packet->size = (int)MIN(SrcSize, INT32_MAX);

	WINPR_ASSERT(sys->codecDecoderContext);
	/* avcodec_decode_video2 is deprecated with libavcodec 57.48.101 */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	status = avcodec_send_packet(sys->codecDecoderContext, packet);

	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to decode video frame (status=%d)", status);
		goto fail;
	}

	sys->videoFrame->format = AV_PIX_FMT_YUV420P;

#ifdef WITH_VAAPI
		status = avcodec_receive_frame(sys->codecDecoderContext,
		                               sys->hwctx ? sys->hwVideoFrame : sys->videoFrame);
#else
		status = avcodec_receive_frame(sys->codecDecoderContext, sys->videoFrame);
#endif
	    if (status == AVERROR(EAGAIN))
	    {
		    rc = 0;
		    goto fail;
	    }

	gotFrame = (status == 0);
#else
#ifdef WITH_VAAPI
	status =
	    avcodec_decode_video2(sys->codecDecoderContext,
	                          sys->hwctx ? sys->hwVideoFrame : sys->videoFrame, &gotFrame, packet);
#else
	status = avcodec_decode_video2(sys->codecDecoderContext, sys->videoFrame, &gotFrame, packet);
#endif
#endif
	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to decode video frame (status=%d)", status);
		goto fail;
	}

#ifdef WITH_VAAPI

	if (sys->hwctx)
	{
		if (sys->hwVideoFrame->format == sys->hw_pix_fmt)
		{
			sys->videoFrame->width = sys->hwVideoFrame->width;
			sys->videoFrame->height = sys->hwVideoFrame->height;
			status = av_hwframe_transfer_data(sys->videoFrame, sys->hwVideoFrame, 0);
		}
		else
		{
			status = av_frame_copy(sys->videoFrame, sys->hwVideoFrame);
		}
	}

	gotFrame = (status == 0);

	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to transfer video frame (status=%d) (%s)", status,
		           av_err2str(status));
		goto fail;
	}

#endif

	if (gotFrame)
	{
		WINPR_ASSERT(sys->videoFrame);

		pYUVData[0] = sys->videoFrame->data[0];
		pYUVData[1] = sys->videoFrame->data[1];
		pYUVData[2] = sys->videoFrame->data[2];
		iStride[0] = (UINT32)MAX(0, sys->videoFrame->linesize[0]);
		iStride[1] = (UINT32)MAX(0, sys->videoFrame->linesize[1]);
		iStride[2] = (UINT32)MAX(0, sys->videoFrame->linesize[2]);

		rc = 1;
	}
	else
		rc = -2;

fail:
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 133, 100)
	av_packet_unref(packet);
#else
	av_packet_free(&packet);
#endif

	return rc;
}

static int libavcodec_compress(H264_CONTEXT* WINPR_RESTRICT h264,
                               const BYTE** WINPR_RESTRICT pSrcYuv,
                               const UINT32* WINPR_RESTRICT pStride,
                               BYTE** WINPR_RESTRICT ppDstData, UINT32* WINPR_RESTRICT pDstSize)
{
	union
	{
		const BYTE* cpv;
		uint8_t* pv;
	} cnv;
	int rc = -1;
	int status = 0;
	int gotFrame = 0;

	WINPR_ASSERT(h264);

	H264_CONTEXT_LIBAVCODEC* sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	if (!libavcodec_create_encoder_context(h264))
		return -1;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 133, 100)
	sys->packet = &sys->bufferpacket;
	av_packet_unref(sys->packet);
	av_init_packet(sys->packet);
#else
	av_packet_free(&sys->packet);
	sys->packet = av_packet_alloc();
#endif
	if (!sys->packet)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate AVPacket");
		goto fail;
	}

	WINPR_ASSERT(sys->packet);
	sys->packet->data = NULL;
	sys->packet->size = 0;

	WINPR_ASSERT(sys->videoFrame);
	WINPR_ASSERT(sys->codecEncoderContext);
	sys->videoFrame->format = AV_PIX_FMT_YUV420P;
	sys->videoFrame->width = sys->codecEncoderContext->width;
	sys->videoFrame->height = sys->codecEncoderContext->height;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 48, 100)
	sys->videoFrame->colorspace = AVCOL_SPC_BT709;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 92, 100)
	sys->videoFrame->chroma_location = AVCHROMA_LOC_LEFT;
#endif
	cnv.cpv = pSrcYuv[0];
	sys->videoFrame->data[0] = cnv.pv;

	cnv.cpv = pSrcYuv[1];
	sys->videoFrame->data[1] = cnv.pv;

	cnv.cpv = pSrcYuv[2];
	sys->videoFrame->data[2] = cnv.pv;

	sys->videoFrame->linesize[0] = (int)pStride[0];
	sys->videoFrame->linesize[1] = (int)pStride[1];
	sys->videoFrame->linesize[2] = (int)pStride[2];
	sys->videoFrame->pts++;

#ifdef WITH_VAAPI_H264_ENCODING
	if (sys->hwctx)
	{
		av_frame_unref(sys->hwVideoFrame);
		if ((status = av_hwframe_get_buffer(sys->codecEncoderContext->hw_frames_ctx,
		                                    sys->hwVideoFrame, 0)) < 0 ||
		    !sys->hwVideoFrame->hw_frames_ctx)
		{
			WLog_Print(h264->log, WLOG_ERROR, "av_hwframe_get_buffer failed (%s [%d])",
			           av_err2str(status), status);
			goto fail;
		}
		sys->videoFrame->format = AV_PIX_FMT_NV12;
		if ((status = av_hwframe_transfer_data(sys->hwVideoFrame, sys->videoFrame, 0)) < 0)
		{
			WLog_Print(h264->log, WLOG_ERROR, "av_hwframe_transfer_data failed (%s [%d])",
			           av_err2str(status), status);
			goto fail;
		}
	}
#endif

	/* avcodec_encode_video2 is deprecated with libavcodec 57.48.101 */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
#ifdef WITH_VAAPI_H264_ENCODING
	status = avcodec_send_frame(sys->codecEncoderContext,
	                            sys->hwctx ? sys->hwVideoFrame : sys->videoFrame);
#else
	status = avcodec_send_frame(sys->codecEncoderContext, sys->videoFrame);
#endif

	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to encode video frame (%s [%d])",
		           av_err2str(status), status);
		goto fail;
	}

	status = avcodec_receive_packet(sys->codecEncoderContext, sys->packet);

	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to encode video frame (%s [%d])",
		           av_err2str(status), status);
		goto fail;
	}

	gotFrame = (status == 0);
#elif LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 59, 100)

	do
	{
		status = avcodec_encode_video2(sys->codecEncoderContext, sys->packet, sys->videoFrame,
		                               &gotFrame);
	} while ((status >= 0) && (gotFrame == 0));

#else
	sys->packet->size =
	    avpicture_get_size(sys->codecDecoderContext->pix_fmt, sys->codecDecoderContext->width,
	                       sys->codecDecoderContext->height);
	sys->packet->data = av_malloc(sys->packet->size);

	if (!sys->packet->data)
		status = -1;
	else
	{
		status = avcodec_encode_video(sys->codecDecoderContext, sys->packet->data,
		                              sys->packet->size, sys->videoFrame);
	}

#endif

	if (status < 0)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to encode video frame (%s [%d])",
		           av_err2str(status), status);
		goto fail;
	}

	WINPR_ASSERT(sys->packet);
	*ppDstData = sys->packet->data;
	*pDstSize = (UINT32)MAX(0, sys->packet->size);

	if (!gotFrame)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Did not get frame! (%s [%d])", av_err2str(status),
		           status);
		rc = -2;
	}
	else
		rc = 1;
fail:
	return rc;
}

static void libavcodec_uninit(H264_CONTEXT* h264)
{
	WINPR_ASSERT(h264);

	H264_CONTEXT_LIBAVCODEC* sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;

	if (!sys)
		return;

	if (sys->packet)
	{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 133, 100)
		av_packet_unref(sys->packet);
#else
		av_packet_free(&sys->packet);
#endif
	}

	if (sys->videoFrame)
	{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 18, 102)
		av_frame_free(&sys->videoFrame);
#else
		av_free(sys->videoFrame);
#endif
	}

#if defined(WITH_VAAPI) || defined(WITH_VAAPI_H264_ENCODING)
	if (sys->hwVideoFrame)
	{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 18, 102)
		av_frame_free(&sys->hwVideoFrame);
#else
		av_free(sys->hwVideoFrame);
#endif
	}

	if (sys->hwctx)
		av_buffer_unref(&sys->hwctx);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 80, 100)

	if (sys->hw_frames_ctx)
		av_buffer_unref(&sys->hw_frames_ctx);

#endif

#endif

	if (sys->codecParser)
		av_parser_close(sys->codecParser);

	if (sys->codecDecoderContext)
	{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 69, 100)
		avcodec_free_context(&sys->codecDecoderContext);
#else
		avcodec_close(sys->codecDecoderContext);
		av_free(sys->codecDecoderContext);
#endif
	}

	libavcodec_destroy_encoder_context(h264);
	free(sys);
	h264->pSystemData = NULL;
}

#ifdef WITH_VAAPI
static enum AVPixelFormat libavcodec_get_format(struct AVCodecContext* ctx,
                                                const enum AVPixelFormat* fmts)
{
	WINPR_ASSERT(ctx);

	H264_CONTEXT* h264 = (H264_CONTEXT*)ctx->opaque;
	WINPR_ASSERT(h264);

	H264_CONTEXT_LIBAVCODEC* sys = (H264_CONTEXT_LIBAVCODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
	{
		if (*p == sys->hw_pix_fmt)
		{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 80, 100)
			sys->hw_frames_ctx = av_hwframe_ctx_alloc(sys->hwctx);

			if (!sys->hw_frames_ctx)
			{
				return AV_PIX_FMT_NONE;
			}

			sys->codecDecoderContext->pix_fmt = *p;
			AVHWFramesContext* frames = (AVHWFramesContext*)sys->hw_frames_ctx->data;
			frames->format = *p;
			frames->height = sys->codecDecoderContext->coded_height;
			frames->width = sys->codecDecoderContext->coded_width;
			frames->sw_format =
			    (sys->codecDecoderContext->sw_pix_fmt == AV_PIX_FMT_YUV420P10 ? AV_PIX_FMT_P010
			                                                                  : AV_PIX_FMT_NV12);
			frames->initial_pool_size = 20;

			if (sys->codecDecoderContext->active_thread_type & FF_THREAD_FRAME)
				frames->initial_pool_size += sys->codecDecoderContext->thread_count;

			int err = av_hwframe_ctx_init(sys->hw_frames_ctx);

			if (err < 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "Could not init hwframes context: %s",
				           av_err2str(err));
				return AV_PIX_FMT_NONE;
			}

			sys->codecDecoderContext->hw_frames_ctx = av_buffer_ref(sys->hw_frames_ctx);
#endif
			return *p;
		}
	}

	return AV_PIX_FMT_NONE;
}
#endif

static BOOL libavcodec_init(H264_CONTEXT* h264)
{
	H264_CONTEXT_LIBAVCODEC* sys = NULL;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_LIBAVCODEC*)calloc(1, sizeof(H264_CONTEXT_LIBAVCODEC));

	if (!sys)
	{
		goto EXCEPTION;
	}

	h264->pSystemData = (void*)sys;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
	avcodec_register_all();
#endif

	if (!h264->Compressor)
	{
		sys->codecDecoder = avcodec_find_decoder(AV_CODEC_ID_H264);

		if (!sys->codecDecoder)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to find libav H.264 codec");
			goto EXCEPTION;
		}

		sys->codecDecoderContext = avcodec_alloc_context3(sys->codecDecoder);

		if (!sys->codecDecoderContext)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate libav codec context");
			goto EXCEPTION;
		}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(59, 18, 100)
		if (sys->codecDecoder->capabilities & AV_CODEC_CAP_TRUNCATED)
		{
			sys->codecDecoderContext->flags |= AV_CODEC_FLAG_TRUNCATED;
		}
#endif

#ifdef WITH_VAAPI

		if (!sys->hwctx)
		{
			int ret =
			    av_hwdevice_ctx_create(&sys->hwctx, AV_HWDEVICE_TYPE_VAAPI, VAAPI_DEVICE, NULL, 0);

			if (ret < 0)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "Could not initialize hardware decoder, falling back to software: %s",
				           av_err2str(ret));
				sys->hwctx = NULL;
				goto fail_hwdevice_create;
			}
		}
		WLog_Print(h264->log, WLOG_INFO, "Using VAAPI for accelerated H264 decoding");

		sys->codecDecoderContext->get_format = libavcodec_get_format;
		sys->hw_pix_fmt = AV_PIX_FMT_VAAPI;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 80, 100)
		sys->codecDecoderContext->hw_device_ctx = av_buffer_ref(sys->hwctx);
#endif
		sys->codecDecoderContext->opaque = (void*)h264;
	fail_hwdevice_create:
#endif

		if (avcodec_open2(sys->codecDecoderContext, sys->codecDecoder, NULL) < 0)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to open libav codec");
			goto EXCEPTION;
		}

		sys->codecParser = av_parser_init(AV_CODEC_ID_H264);

		if (!sys->codecParser)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to initialize libav parser");
			goto EXCEPTION;
		}
	}
	else
	{
#ifdef WITH_VAAPI_H264_ENCODING
		if (h264->hwAccel) /* user requested hw accel */
		{
			sys->codecEncoder = avcodec_find_encoder_by_name("h264_vaapi");
			if (!sys->codecEncoder)
			{
				WLog_Print(h264->log, WLOG_ERROR, "H264 VAAPI encoder not found");
			}
			else if (av_hwdevice_ctx_create(&sys->hwctx, AV_HWDEVICE_TYPE_VAAPI, VAAPI_DEVICE, NULL,
			                                0) < 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "av_hwdevice_ctx_create failed");
				sys->codecEncoder = NULL;
				sys->hwctx = NULL;
			}
			else
			{
				WLog_Print(h264->log, WLOG_INFO, "Using VAAPI for accelerated H264 encoding");
			}
		}
#endif
		if (!sys->codecEncoder)
		{
			sys->codecEncoder = avcodec_find_encoder(AV_CODEC_ID_H264);
			h264->hwAccel = FALSE; /* not supported */
		}

		if (!sys->codecEncoder)
		{
			WLog_Print(h264->log, WLOG_ERROR, "Failed to initialize H264 encoder");
			goto EXCEPTION;
		}
	}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 18, 102)
	sys->videoFrame = av_frame_alloc();
#if defined(WITH_VAAPI) || defined(WITH_VAAPI_H264_ENCODING)
	sys->hwVideoFrame = av_frame_alloc();
#endif
#else
	sys->videoFrame = avcodec_alloc_frame();
#endif

	if (!sys->videoFrame)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate libav frame");
		goto EXCEPTION;
	}

#if defined(WITH_VAAPI) || defined(WITH_VAAPI_H264_ENCODING)
	if (!sys->hwVideoFrame)
	{
		WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate libav hw frame");
		goto EXCEPTION;
	}

#endif
	sys->videoFrame->pts = 0;
	return TRUE;
EXCEPTION:
	libavcodec_uninit(h264);
	return FALSE;
}

const H264_CONTEXT_SUBSYSTEM g_Subsystem_libavcodec = { "libavcodec", libavcodec_init,
	                                                    libavcodec_uninit, libavcodec_decompress,
	                                                    libavcodec_compress };
