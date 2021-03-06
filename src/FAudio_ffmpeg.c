/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#ifdef HAVE_FFMPEG

#include "FAudio_internal.h"
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif /* __cplusplus */

typedef struct FAudioFFmpeg
{
	AVCodecContext *av_ctx;
	AVFrame *av_frame;

	uint32_t encOffset;	/* current position in encoded stream (in bytes) */
	uint32_t decOffset;	/* current position in decoded stream (in samples) */

	/* buffer used to decode the last frame */
	size_t paddingBytes;
	uint8_t *paddingBuffer;

	/* buffer to receive an entire decoded frame */
	uint32_t convertCapacity;
	uint32_t convertSamples;
	uint32_t convertOffset;
	float *convertCache;
} FAudioFFmpeg;

void FAudio_FFMPEG_reset(FAudioSourceVoice *voice)
{
	LOG_FUNC_ENTER(voice->audio)
	voice->src.ffmpeg->encOffset = 0;
	voice->src.ffmpeg->decOffset = 0;
	LOG_FUNC_EXIT(voice->audio)
}

uint32_t FAudio_FFMPEG_init(FAudioSourceVoice *pSourceVoice, uint32_t type)
{
	AVCodecContext *av_ctx;
	AVFrame *av_frame;
	AVCodec *codec = NULL;
	const char *typestring = "Unknown";

	LOG_FUNC_ENTER(pSourceVoice->audio)
	pSourceVoice->src.decode = FAudio_INTERNAL_DecodeFFMPEG;

	/* initialize ffmpeg state */
	if (type == FAUDIO_FORMAT_WMAUDIO2)
	{
		typestring = "WMAv2";
		codec = avcodec_find_decoder(AV_CODEC_ID_WMAV2);
	}
	else if (type == FAUDIO_FORMAT_WMAUDIO3)
	{
		typestring = "WMAv3";
		codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
	}
	else if (type == FAUDIO_FORMAT_XMAUDIO2)
	{
		typestring = "XMA2";
		codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
	}
	if (!codec)
	{
		LOG_ERROR(
			pSourceVoice->audio,
			"%s codec not supported!",
			typestring
		);
		FAudio_assert(0 && "FFmpeg codec not supported!");
		LOG_FUNC_EXIT(pSourceVoice->audio)
		return FAUDIO_E_UNSUPPORTED_FORMAT;
	}

	av_ctx = avcodec_alloc_context3(codec);
	if (!av_ctx)
	{
		LOG_ERROR(
			pSourceVoice->audio,
			"%s",
			"WMAv2 codec not supported!"
		);
		FAudio_assert(0 && "WMAv2 codec not supported!");
		LOG_FUNC_EXIT(pSourceVoice->audio)
		return FAUDIO_E_UNSUPPORTED_FORMAT;
	}

	av_ctx->bit_rate = pSourceVoice->src.format->nAvgBytesPerSec * 8;
	av_ctx->channels = pSourceVoice->src.format->nChannels;
	av_ctx->sample_rate = pSourceVoice->src.format->nSamplesPerSec;
	av_ctx->block_align = pSourceVoice->src.format->nBlockAlign;
	av_ctx->bits_per_coded_sample = pSourceVoice->src.format->wBitsPerSample;
	av_ctx->request_sample_fmt = AV_SAMPLE_FMT_FLT;

	/* pSourceVoice->src.format is actually pointing to a
	 * WAVEFORMATEXTENSIBLE struct, not just a WAVEFORMATEX struct.
	 * That means there's always at least 22 bytes following the struct, I
	 * assume the WMA data is behind that.
	 * Need to verify but haven't come across any samples data with cbSize > 22
	 * -@JohanSmet!
	 */
	FAudio_assert(pSourceVoice->src.format->cbSize <= 22);
	if (type == FAUDIO_FORMAT_WMAUDIO3)
	{
		av_ctx->extradata_size = pSourceVoice->src.format->cbSize;
		av_ctx->extradata = (uint8_t *) av_malloc(
			pSourceVoice->src.format->cbSize +
			AV_INPUT_BUFFER_PADDING_SIZE
		);
		FAudio_memcpy(
			av_ctx->extradata,
			&((FAudioWaveFormatExtensible*) pSourceVoice->src.format)->Samples,
			pSourceVoice->src.format->cbSize
		);
	}
	else if (type == FAUDIO_FORMAT_WMAUDIO2)
	{
		/* xWMA doesn't provide the extradata info that FFmpeg needs to
		 * decode WMA data, so we create some fake extradata. This is
		 * taken from <ffmpeg/libavformat/xwma.c>.
		 */
		av_ctx->extradata_size = 6;
		av_ctx->extradata = (uint8_t *) av_malloc(AV_INPUT_BUFFER_PADDING_SIZE);
		FAudio_zero(av_ctx->extradata, AV_INPUT_BUFFER_PADDING_SIZE);
		av_ctx->extradata[4] = 31;
	}
	else if (type == FAUDIO_FORMAT_XMAUDIO2)
	{
		/* FFmpeg expects XMA2WAVEFORMATEX or XMA2WAVEFORMAT.
		 * For more info, check <ffmpeg/libavcodec/wmaprodec.c>. */
		av_ctx->extradata_size = 34;
		av_ctx->extradata = (uint8_t *) av_malloc(AV_INPUT_BUFFER_PADDING_SIZE);
		FAudio_zero(av_ctx->extradata, AV_INPUT_BUFFER_PADDING_SIZE);
		av_ctx->extradata[1] = 1;
		av_ctx->extradata[5] = pSourceVoice->src.format->nChannels == 2 ? 3 : 0;
		av_ctx->extradata[31] = 4;
		av_ctx->extradata[33] = 1;
	}

	if (avcodec_open2(av_ctx, codec, NULL) < 0)
	{
		av_free(av_ctx->extradata);
		av_free(av_ctx);
		LOG_ERROR(pSourceVoice->audio, "%s", "avcodec_open2 failed!")
		LOG_FUNC_EXIT(pSourceVoice->audio)
		return FAUDIO_E_UNSUPPORTED_FORMAT;
	}

	av_frame = av_frame_alloc();
	if (!av_frame)
	{
		avcodec_close(av_ctx);
		av_free(av_ctx->extradata);
		av_free(av_ctx);
		LOG_ERROR(pSourceVoice->audio, "%s", "avcodec_open2 failed!")
		LOG_FUNC_EXIT(pSourceVoice->audio)
		return FAUDIO_E_UNSUPPORTED_FORMAT;
	}

	if (av_ctx->sample_fmt != AV_SAMPLE_FMT_FLT && av_ctx->sample_fmt != AV_SAMPLE_FMT_FLTP)
	{
		FAudio_assert(0 && "Got non-float format!!!");
	}

	pSourceVoice->src.ffmpeg = (FAudioFFmpeg *) pSourceVoice->audio->pMalloc(sizeof(FAudioFFmpeg));
	FAudio_zero(pSourceVoice->src.ffmpeg, sizeof(FAudioFFmpeg));

	pSourceVoice->src.ffmpeg->av_ctx = av_ctx;
	pSourceVoice->src.ffmpeg->av_frame = av_frame;
	LOG_FUNC_EXIT(pSourceVoice->audio)
	return 0;
}

void FAudio_FFMPEG_free(FAudioSourceVoice *voice)
{
	FAudioFFmpeg *ffmpeg = voice->src.ffmpeg;

	LOG_FUNC_ENTER(voice->audio)

	avcodec_close(ffmpeg->av_ctx);
	av_free(ffmpeg->av_ctx->extradata);
	av_free(ffmpeg->av_ctx);

	voice->audio->pFree(ffmpeg->convertCache);
	voice->audio->pFree(ffmpeg->paddingBuffer);
	voice->audio->pFree(ffmpeg);
	voice->src.ffmpeg = NULL;

	LOG_FUNC_EXIT(voice->audio)
}

void FAudio_INTERNAL_ResizeConvertCache(FAudioVoice *voice, uint32_t samples)
{
	LOG_FUNC_ENTER(voice->audio)
	if (samples > voice->src.ffmpeg->convertCapacity)
	{
		voice->src.ffmpeg->convertCapacity = samples;
		voice->src.ffmpeg->convertCache = (float*) voice->audio->pRealloc(
			voice->src.ffmpeg->convertCache,
			sizeof(float) * voice->src.ffmpeg->convertCapacity
		);
	}
	LOG_FUNC_EXIT(voice->audio)
}

void FAudio_INTERNAL_FillConvertCache(FAudioVoice *voice, FAudioBuffer *buffer)
{
	FAudioFFmpeg *ffmpeg = voice->src.ffmpeg;
	AVPacket avpkt = {0};
	int averr;
	uint32_t total_samples;

	LOG_FUNC_ENTER(voice->audio)

	avpkt.size = voice->src.format->nBlockAlign;
	avpkt.data = (unsigned char *) buffer->pAudioData + ffmpeg->encOffset;

	for(;;)
	{
		averr = avcodec_receive_frame(ffmpeg->av_ctx, ffmpeg->av_frame);
		if (averr == AVERROR(EAGAIN))
		{
			/* ffmpeg needs more data to decode */
			avpkt.pts = avpkt.dts = AV_NOPTS_VALUE;

			if (ffmpeg->encOffset >= buffer->AudioBytes)
			{
				/* no more data in this buffer */
				break;
			}

			if (ffmpeg->encOffset + avpkt.size + AV_INPUT_BUFFER_PADDING_SIZE > buffer->AudioBytes)
			{
				/* Unfortunately, the FFmpeg API requires that a number of
				 * extra bytes must be available past the end of the buffer.
				 * The xaudio2 client probably hasn't done this, so we have to
				 * perform a copy near the end of the buffer. */
				size_t remain = buffer->AudioBytes - ffmpeg->encOffset;

				if (ffmpeg->paddingBytes < remain + AV_INPUT_BUFFER_PADDING_SIZE)
				{
					ffmpeg->paddingBytes = remain + AV_INPUT_BUFFER_PADDING_SIZE;
					ffmpeg->paddingBuffer = (uint8_t *) voice->audio->pRealloc(
						ffmpeg->paddingBuffer,
						ffmpeg->paddingBytes
					);
				}
				FAudio_memcpy(ffmpeg->paddingBuffer, buffer->pAudioData + ffmpeg->encOffset, remain);
				FAudio_zero(ffmpeg->paddingBuffer + remain, AV_INPUT_BUFFER_PADDING_SIZE);
				avpkt.data = ffmpeg->paddingBuffer;
			}

			averr = avcodec_send_packet(ffmpeg->av_ctx, &avpkt);
			if (averr)
			{
				FAudio_assert(0 && "avcodec_send_packet failed" && averr);
				break;
			}

			ffmpeg->encOffset += avpkt.size;
			avpkt.data += avpkt.size;

			/* data sent, try receive again */
			continue;
		}

		if (averr)
		{
			LOG_ERROR(
				voice->audio,
				"avcodec_receive_frame failed: %d",
				averr
			)
			FAudio_assert(0 && "avcodec_receive_frame failed" && averr);
			LOG_FUNC_EXIT(voice->audio)
			return;
		}
		else
		{
			break;
		}
	}

	/* copy decoded samples to internal buffer, reordering if necessary */
	total_samples = ffmpeg->av_frame->nb_samples * ffmpeg->av_ctx->channels;

	FAudio_INTERNAL_ResizeConvertCache(voice, total_samples);

	if (av_sample_fmt_is_planar(ffmpeg->av_ctx->sample_fmt))
	{
		int32_t s, c;
		uint8_t **src = ffmpeg->av_frame->data;
		uint32_t *dst = (uint32_t *) ffmpeg->convertCache;

		for(s = 0; s < ffmpeg->av_frame->nb_samples; ++s)
			for(c = 0; c < ffmpeg->av_ctx->channels; ++c)
				*dst++ = ((uint32_t*)(src[c]))[s];
	}
	else
	{
		FAudio_memcpy(
			ffmpeg->convertCache,
			ffmpeg->av_frame->data[0],
			total_samples * sizeof(float)
		);
	}

	ffmpeg->convertSamples = ffmpeg->av_frame->nb_samples;
	ffmpeg->convertOffset = 0;
	LOG_FUNC_EXIT(voice->audio)
}

void FAudio_INTERNAL_DecodeFFMPEG(
	FAudioVoice *voice,
	FAudioBuffer *buffer,
	float *decodeCache,
	uint32_t samples
) {
	FAudioFFmpeg *ffmpeg = voice->src.ffmpeg;
	uint32_t decSampleSize = voice->src.format->nChannels * voice->src.format->wBitsPerSample / 8;
	uint32_t outSampleSize = voice->src.format->nChannels * sizeof(float);
	uint32_t done = 0, available, todo, cumulative;
	uint32_t reseek = 0;

	LOG_FUNC_ENTER(voice->audio)

	/* check if we need to reposition in the stream */
	if (voice->src.curBufferOffset < ffmpeg->decOffset)
	{
		/* If curBufferOffset is behind, it's because we had to do some
		 * padding, which should not affect the stream offset. To fix,
		 * we simply rewind by a couple samples. Pretty safe if it doesn't
		 * cross back into the previous decoded block.
		 */
		uint32_t delta = ffmpeg->decOffset - voice->src.curBufferOffset;

		if (ffmpeg->convertOffset >= delta)
		{
			ffmpeg->convertOffset -= delta;
			ffmpeg->decOffset = voice->src.curBufferOffset;
		}
		else
		{
			reseek = 1;
		}
	}
	else if (voice->src.curBufferOffset > ffmpeg->decOffset)
	{
		/* If we're starting in the middle, we have to seek to the
		 * starting position. AFAIK this shouldn't happen mid-stream.
		 */
		reseek = 1;
	}

	if (reseek)
	{
		FAudioBufferWMA *bufferWMA = &voice->src.bufferList->bufferWMA;
		uint32_t byteOffset = voice->src.curBufferOffset * decSampleSize;
		uint32_t packetIdx = bufferWMA->PacketCount - 1;

		/* figure out in which encoded packet has this position */
		while (packetIdx > 0 && bufferWMA->pDecodedPacketCumulativeBytes[packetIdx] > byteOffset)
		{
			packetIdx -= 1;
		}

		if (packetIdx == 0)
		{
			cumulative = 0;
		}
		else
		{
			cumulative = bufferWMA->pDecodedPacketCumulativeBytes[packetIdx - 1];
		}

		/* seek to the wanted position in the stream */
		ffmpeg->encOffset = packetIdx * voice->src.format->nBlockAlign;
		FAudio_INTERNAL_FillConvertCache(voice, buffer);
		ffmpeg->convertOffset = (byteOffset - cumulative) / outSampleSize;
		ffmpeg->decOffset = voice->src.curBufferOffset;
	}

	while (done < samples)
	{
		/* check for available data in decoded cache, refill if necessary */
		if (ffmpeg->convertOffset >= ffmpeg->convertSamples)
		{
			FAudio_INTERNAL_FillConvertCache(voice, buffer);
		}

		available = ffmpeg->convertSamples - ffmpeg->convertOffset;
		if (available <= 0)
		{
			break;
		}

		todo = FAudio_min(available, samples - done);
		FAudio_memcpy(
			decodeCache + (done * voice->src.format->nChannels),
			ffmpeg->convertCache + (ffmpeg->convertOffset * voice->src.format->nChannels),
			todo * voice->src.format->nChannels * sizeof(float)
		);

		done += todo;
		ffmpeg->convertOffset += todo;
	}

	/* FIXME: This block should not be here! */
	if (done < samples)
	{
		FAudio_zero(
			decodeCache + (done * voice->src.format->nChannels),
			(samples - done) * voice->src.format->nChannels * sizeof(float)
		);
	}

	ffmpeg->decOffset += samples;
	LOG_FUNC_EXIT(voice->audio)
}

#else

extern int this_tu_is_empty;

#endif /* HAVE_FFMPEG */

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */
