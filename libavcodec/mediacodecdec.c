/*
 * Android MediaCodec MPEG-2 / H.264 / H.265 / MPEG-4 / VP8 / VP9 decoders
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_components.h"

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"
#include "libavutil/sha.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "dovi_rpu.h"
#include "h264_parse.h"
#include "h264_ps.h"
#if CONFIG_HEVC_MEDIACODEC_DECODER
#include "h2645_parse.h"
#include "hevc/hevc.h"
#endif
#include "hevc/parse.h"
#include "hwconfig.h"
#include "internal.h"
#include "jni.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

#define MEDIACODEC_DOVI_MAX_PENDING 512
/* MediaCodecInfo.CodecProfileLevel.DolbyVisionProfileDvheStn. */
#define MEDIACODEC_DOVI_PROFILE_DVHE_STN 32

typedef struct MediaCodecDOVIFrame {
    struct MediaCodecDOVIFrame *next;
    AVBufferRef *metadata;
    AVBufferRef *rpu;
    int64_t pts;
    uint64_t input_id;
} MediaCodecDOVIFrame;

typedef struct MediaCodecH264DecContext {

    AVClass *avclass;

    MediaCodecDecContext *ctx;

    AVPacket buffered_pkt;

    int delay_flush;
    int amlogic_mpeg2_api23_workaround;

    int use_ndk_codec;
    // Ref. MediaFormat KEY_OPERATING_RATE
    int operating_rate;

    int dovi_diagnostics;
    struct AVSHA *dovi_sha;
    uint64_t dovi_packet_id;
    int dovi_packet_size;
    int dovi_packet_offset;

    int dovi_p5_metadata;
    int dovi_p5_surface_decoder;
    int dovi_p5_active;
    DOVIContext dovi_ctx;
    MediaCodecDOVIFrame *dovi_head;
    MediaCodecDOVIFrame *dovi_tail;
    unsigned dovi_pending;
    uint64_t dovi_input_id;
} MediaCodecH264DecContext;

#if CONFIG_HEVC_MEDIACODEC_DECODER
static void mediacodec_dovi_frame_free(MediaCodecDOVIFrame *item)
{
    if (!item)
        return;

    av_buffer_unref(&item->metadata);
    av_buffer_unref(&item->rpu);
    av_free(item);
}

static void mediacodec_dovi_queue_clear(MediaCodecH264DecContext *s)
{
    MediaCodecDOVIFrame *item = s->dovi_head;

    while (item) {
        MediaCodecDOVIFrame *next = item->next;
        mediacodec_dovi_frame_free(item);
        item = next;
    }

    s->dovi_head = NULL;
    s->dovi_tail = NULL;
    s->dovi_pending = 0;
}

static void mediacodec_dovi_flush(MediaCodecH264DecContext *s)
{
    if (!s->dovi_p5_metadata && !s->dovi_p5_surface_decoder)
        return;

    mediacodec_dovi_queue_clear(s);
    ff_dovi_ctx_flush(&s->dovi_ctx);
}

static void mediacodec_dovi_set_config(AVCodecContext *avctx,
                                       const uint8_t *data, size_t size,
                                       const char *source)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    AVDOVIDecoderConfigurationRecord cfg;

    if (!s->dovi_p5_metadata && !s->dovi_p5_surface_decoder)
        return;

    if (!data || size < sizeof(cfg)) {
        ff_dovi_ctx_unref(&s->dovi_ctx);
        s->dovi_ctx.logctx = avctx;
        s->dovi_p5_active = 0;
        av_log(avctx, AV_LOG_WARNING,
               "Dolby Vision Profile 5 processing is enabled, "
               "but %s configuration is missing or truncated\n", source);
        return;
    }

    memcpy(&cfg, data, sizeof(cfg));

    if (memcmp(&s->dovi_ctx.cfg, &cfg, sizeof(cfg))) {
        ff_dovi_ctx_flush(&s->dovi_ctx);
        s->dovi_ctx.cfg = cfg;
    }

    s->dovi_p5_active = avctx->codec_id == AV_CODEC_ID_HEVC &&
                        cfg.dv_profile == 5 && cfg.bl_present_flag &&
                        cfg.rpu_present_flag && !cfg.el_present_flag;

    if (!s->dovi_p5_active) {
        av_log(avctx, AV_LOG_VERBOSE,
               "Dolby Vision metadata propagation is limited to "
               "single-layer Profile 5; %s configuration has profile=%u "
               "BL=%u EL=%u RPU=%u\n",
               source, cfg.dv_profile, cfg.bl_present_flag,
               cfg.el_present_flag, cfg.rpu_present_flag);
    }
}

static int64_t mediacodec_dovi_output_pts(const AVCodecContext *avctx,
                                          const AVPacket *pkt)
{
    int64_t pts = pkt->pts;

    if (pts == AV_NOPTS_VALUE)
        return 0;

    if (pts && avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
        pts = av_rescale_q(pts, avctx->pkt_timebase, AV_TIME_BASE_Q);
        pts = av_rescale_q(pts, AV_TIME_BASE_Q, avctx->pkt_timebase);
    }

    return pts;
}

static int mediacodec_dovi_queue_push(AVCodecContext *avctx, int64_t pts,
                                      AVBufferRef *metadata, AVBufferRef *rpu)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    MediaCodecDOVIFrame *item;

    if (s->dovi_pending >= MEDIACODEC_DOVI_MAX_PENDING) {
        av_log(avctx, AV_LOG_ERROR,
               "Dolby Vision metadata queue reached its %u-frame limit\n",
               MEDIACODEC_DOVI_MAX_PENDING);
        return AVERROR(ENOBUFS);
    }

    item = av_mallocz(sizeof(*item));
    if (!item)
        return AVERROR(ENOMEM);

    item->metadata = metadata;
    item->rpu = rpu;
    item->pts = pts;
    item->input_id = ++s->dovi_input_id;

    if (s->dovi_tail)
        s->dovi_tail->next = item;
    else
        s->dovi_head = item;
    s->dovi_tail = item;
    s->dovi_pending++;

    if (s->dovi_diagnostics) {
        av_log(avctx, AV_LOG_INFO,
               "[dovi-diag] queued metadata input=%"PRIu64
               " pts=%"PRId64" metadata=%zu RPU=%zu pending=%u\n",
               item->input_id, item->pts,
               item->metadata ? item->metadata->size : 0,
               item->rpu ? item->rpu->size : 0, s->dovi_pending);
    }

    return 0;
}

static int mediacodec_dovi_finish_au(AVCodecContext *avctx,
                                     const H2645NAL *rpu_nal, int64_t pts)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    AVDOVIDecoderConfigurationRecord cfg = s->dovi_ctx.cfg;
    AVDOVIMetadata *metadata = NULL;
    AVBufferRef *metadata_ref = NULL;
    AVBufferRef *rpu_ref = NULL;
    int metadata_size;
    int ret;

    if (!rpu_nal)
        return mediacodec_dovi_queue_push(avctx, pts, NULL, NULL);

    if (rpu_nal->size <= 2 || rpu_nal->raw_size <= 2 ||
        rpu_nal->nuh_layer_id || rpu_nal->temporal_id) {
        av_log(avctx, AV_LOG_WARNING,
               "Ignoring malformed Dolby Vision Profile 5 RPU NAL\n");
        return mediacodec_dovi_queue_push(avctx, pts, NULL, NULL);
    }

    rpu_ref = av_buffer_alloc(rpu_nal->raw_size - 2);
    if (!rpu_ref)
        return AVERROR(ENOMEM);
    memcpy(rpu_ref->data, rpu_nal->raw_data + 2, rpu_ref->size);

    ret = ff_dovi_rpu_parse(&s->dovi_ctx, rpu_nal->data + 2,
                            rpu_nal->size - 2, avctx->err_recognition);
    if (ret < 0) {
        av_log(avctx, AV_LOG_WARNING,
               "Ignoring malformed Dolby Vision Profile 5 RPU: %s\n",
               av_err2str(ret));
        av_buffer_unref(&rpu_ref);
        ff_dovi_ctx_unref(&s->dovi_ctx);
        s->dovi_ctx.logctx = avctx;
        s->dovi_ctx.cfg = cfg;
        return mediacodec_dovi_queue_push(avctx, pts, NULL, NULL);
    }

    metadata_size = ff_dovi_get_metadata(&s->dovi_ctx, &metadata);
    if (metadata_size < 0) {
        av_buffer_unref(&rpu_ref);
        return metadata_size;
    }

    if (metadata_size > 0) {
        metadata_ref = av_buffer_create((uint8_t *)metadata, metadata_size,
                                        NULL, NULL, 0);
        if (!metadata_ref) {
            av_free(metadata);
            av_buffer_unref(&rpu_ref);
            return AVERROR(ENOMEM);
        }
    }

    ret = mediacodec_dovi_queue_push(avctx, pts, metadata_ref, rpu_ref);
    if (ret < 0) {
        av_buffer_unref(&metadata_ref);
        av_buffer_unref(&rpu_ref);
    }
    return ret;
}

static int mediacodec_dovi_parse_packet(AVCodecContext *avctx,
                                        const AVPacket *pkt)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    H2645Packet h2645_pkt = { 0 };
    const H2645NAL *rpu_nal = NULL;
    const uint8_t *config;
    size_t config_size = 0;
    int have_vcl = 0;
    int ret = 0;
    int64_t pts;

    if (s->dovi_p5_surface_decoder || !s->dovi_p5_metadata ||
        avctx->codec_id != AV_CODEC_ID_HEVC)
        return 0;

    config = av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF,
                                     &config_size);
    if (config)
        mediacodec_dovi_set_config(avctx, config, config_size, "packet");

    if (!s->dovi_p5_active || pkt->size <= 0)
        return 0;

    ret = ff_h2645_packet_split(&h2645_pkt, pkt->data, pkt->size, avctx, 0,
                                AV_CODEC_ID_HEVC, H2645_FLAG_SMALL_PADDING);
    if (ret < 0) {
        av_log(avctx, AV_LOG_WARNING,
               "Unable to inspect HEVC packet for Dolby Vision metadata: %s\n",
               av_err2str(ret));
        goto done;
    }

    for (int i = 0; i < h2645_pkt.nb_nals; i++) {
        if (h2645_pkt.nals[i].type == HEVC_NAL_UNSPEC63) {
            av_log(avctx, AV_LOG_ERROR,
                   "Profile 5 metadata propagation does not support "
                   "enhancement-layer NAL units\n");
            ret = AVERROR(ENOSYS);
            goto done;
        }
    }

    pts = mediacodec_dovi_output_pts(avctx, pkt);

    for (int i = 0; i < h2645_pkt.nb_nals; i++) {
        const H2645NAL *nal = &h2645_pkt.nals[i];
        const int is_vcl = nal->type >= 0 && nal->type <= 31;
        const int first_slice = is_vcl && nal->size > 2 &&
                                (nal->data[2] & 0x80);

        if (nal->type == HEVC_NAL_AUD) {
            if (have_vcl) {
                ret = mediacodec_dovi_finish_au(avctx, rpu_nal, pts);
                if (ret < 0)
                    goto done;
            }
            have_vcl = 0;
            rpu_nal = NULL;
            continue;
        }

        if (first_slice && have_vcl) {
            ret = mediacodec_dovi_finish_au(avctx, rpu_nal, pts);
            if (ret < 0)
                goto done;
            have_vcl = 0;
            rpu_nal = NULL;
        }

        if (is_vcl)
            have_vcl = 1;

        if (nal->type == HEVC_NAL_UNSPEC62 && nal->size > 2 &&
            nal->raw_size > 2 && !nal->nuh_layer_id && !nal->temporal_id) {
            if (rpu_nal) {
                av_log(avctx, AV_LOG_WARNING,
                       "Multiple Dolby Vision RPUs found in one access unit; "
                       "using the last one\n");
            }
            rpu_nal = nal;
        }
    }

    if (have_vcl)
        ret = mediacodec_dovi_finish_au(avctx, rpu_nal, pts);

done:
    ff_h2645_packet_uninit(&h2645_pkt);
    return ret;
}
static MediaCodecDOVIFrame *mediacodec_dovi_queue_take(
    MediaCodecH264DecContext *s, int64_t pts)
{
    MediaCodecDOVIFrame *item = s->dovi_head;
    MediaCodecDOVIFrame *prev = NULL;

    while (item && item->pts != pts) {
        prev = item;
        item = item->next;
    }
    if (!item)
        return NULL;

    if (prev)
        prev->next = item->next;
    else
        s->dovi_head = item->next;
    if (s->dovi_tail == item)
        s->dovi_tail = prev;

    item->next = NULL;
    s->dovi_pending--;
    return item;
}

static int mediacodec_dovi_attach(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    MediaCodecDOVIFrame *item;
    int added_metadata = 0;
    int had_metadata;
    int had_rpu;

    if (s->dovi_p5_surface_decoder || !s->dovi_p5_metadata)
        return 0;

    item = mediacodec_dovi_queue_take(s, frame->pts);
    if (!item) {
        if (s->dovi_p5_active && s->dovi_diagnostics) {
            av_log(avctx, AV_LOG_INFO,
                   "[dovi-diag] no metadata match for output pts=%"PRId64
                   " pending=%u\n", frame->pts, s->dovi_pending);
        }
        return 0;
    }
    had_metadata = !!item->metadata;
    had_rpu = !!item->rpu;

    if (item->metadata &&
        !av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA)) {
        if (!av_frame_new_side_data_from_buf(frame,
                                             AV_FRAME_DATA_DOVI_METADATA,
                                             item->metadata))
            goto nomem;
        item->metadata = NULL;
        added_metadata = 1;
    }

    if (item->rpu &&
        !av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER)) {
        if (!av_frame_new_side_data_from_buf(frame,
                                             AV_FRAME_DATA_DOVI_RPU_BUFFER,
                                             item->rpu))
            goto nomem;
        item->rpu = NULL;
    }

    if (s->dovi_diagnostics) {
        av_log(avctx, AV_LOG_INFO,
               "[dovi-diag] matched metadata input=%"PRIu64
               " output-pts=%"PRId64" metadata=%d RPU=%d pending=%u\n",
               item->input_id, frame->pts, had_metadata,
               had_rpu, s->dovi_pending);
    }

    mediacodec_dovi_frame_free(item);
    return 0;

nomem:
    if (added_metadata)
        av_frame_remove_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    mediacodec_dovi_frame_free(item);
    return AVERROR(ENOMEM);
}
#else
static void mediacodec_dovi_queue_clear(MediaCodecH264DecContext *s)
{
    (void)s;
}

static void mediacodec_dovi_flush(MediaCodecH264DecContext *s)
{
    (void)s;
}

static void mediacodec_dovi_set_config(AVCodecContext *avctx,
                                       const uint8_t *data, size_t size,
                                       const char *source)
{
    (void)avctx;
    (void)data;
    (void)size;
    (void)source;
}

static int mediacodec_dovi_parse_packet(AVCodecContext *avctx,
                                        const AVPacket *pkt)
{
    (void)avctx;
    (void)pkt;
    return 0;
}

static int mediacodec_dovi_attach(AVCodecContext *avctx, AVFrame *frame)
{
    (void)avctx;
    (void)frame;
    return 0;
}
#endif

static void mediacodec_dovi_log_config(AVCodecContext *avctx)
{
    const AVPacketSideData *sd =
        ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
    AVDOVIDecoderConfigurationRecord cfg;

    if (!sd) {
        av_log(avctx, AV_LOG_INFO,
               "[dovi-diag] coded configuration: absent\n");
        return;
    }
    if (sd->size < sizeof(AVDOVIDecoderConfigurationRecord)) {
        av_log(avctx, AV_LOG_WARNING,
               "[dovi-diag] coded configuration: truncated (%zu < %zu)\n",
               sd->size, sizeof(AVDOVIDecoderConfigurationRecord));
        return;
    }

    memcpy(&cfg, sd->data, sizeof(cfg));
    av_log(avctx, AV_LOG_INFO,
           "[dovi-diag] coded configuration: version=%u.%u profile=%u "
           "level=%u compatibility-id=%u BL=%u EL=%u RPU=%u compression=%u\n",
           cfg.dv_version_major, cfg.dv_version_minor, cfg.dv_profile,
           cfg.dv_level, cfg.dv_bl_signal_compatibility_id,
           cfg.bl_present_flag, cfg.el_present_flag,
           cfg.rpu_present_flag, cfg.dv_md_compression);
}

static void mediacodec_dovi_hash_rpu(MediaCodecH264DecContext *s,
                                     const uint8_t *data, size_t size,
                                     char hash[17])
{
    uint8_t digest[32];

    av_sha_init(s->dovi_sha, 256);
    av_sha_update(s->dovi_sha, data, size);
    av_sha_final(s->dovi_sha, digest);
    for (int i = 0; i < 8; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
}

static void mediacodec_dovi_log_packet(AVCodecContext *avctx,
                                       const AVPacket *pkt)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
#if CONFIG_HEVC_MEDIACODEC_DECODER
    H2645Packet h2645_pkt = { 0 };
    int au_count = 0;
    int el_count = 0;
    int rpu_count = 0;
    int vcl_count = 0;
    int ret;
#endif

    s->dovi_packet_id++;
    s->dovi_packet_size = pkt->size;
    s->dovi_packet_offset = 0;

    av_log(avctx, AV_LOG_INFO,
           "[dovi-diag] input packet=%"PRIu64" pts=%"PRId64
           " dts=%"PRId64" duration=%"PRId64" size=%d key=%d timebase=%d/%d\n",
           s->dovi_packet_id, pkt->pts, pkt->dts, pkt->duration, pkt->size,
           !!(pkt->flags & AV_PKT_FLAG_KEY), avctx->pkt_timebase.num,
           avctx->pkt_timebase.den);

#if CONFIG_HEVC_MEDIACODEC_DECODER
    if (avctx->codec_id == AV_CODEC_ID_HEVC && pkt->size > 0) {
        ret = ff_h2645_packet_split(&h2645_pkt, pkt->data, pkt->size,
                                    avctx, 0, AV_CODEC_ID_HEVC,
                                    H2645_FLAG_SMALL_PADDING);
        if (ret < 0) {
            av_log(avctx, AV_LOG_WARNING,
                   "[dovi-diag] packet=%"PRIu64
                   " Annex-B NAL split failed: %d\n",
                   s->dovi_packet_id, ret);
            goto done;
        }

        for (int i = 0; i < h2645_pkt.nb_nals; i++) {
            const H2645NAL *nal = &h2645_pkt.nals[i];

            if (nal->type >= 0 && nal->type <= 31) {
                vcl_count++;
                if (nal->size > 2 && (nal->data[2] & 0x80))
                    au_count++;
            } else if (nal->type == HEVC_NAL_UNSPEC63) {
                el_count++;
            } else if (nal->type == HEVC_NAL_UNSPEC62) {
                char hash[17] = "unavailable";

                rpu_count++;
                if (nal->raw_size > 2) {
                    mediacodec_dovi_hash_rpu(s, nal->raw_data + 2,
                                             nal->raw_size - 2, hash);
                    av_log(avctx, AV_LOG_INFO,
                           "[dovi-diag] packet=%"PRIu64" RPU=%d size=%d "
                           "sha256-prefix=%s layer=%d temporal=%d\n",
                           s->dovi_packet_id, rpu_count,
                           nal->raw_size - 2, hash, nal->nuh_layer_id,
                           nal->temporal_id);
                } else {
                    av_log(avctx, AV_LOG_WARNING,
                           "[dovi-diag] packet=%"PRIu64
                           " RPU=%d is truncated (raw-size=%d)\n",
                           s->dovi_packet_id, rpu_count, nal->raw_size);
                }
            }
        }

        if (!au_count && vcl_count)
            au_count = 1;

        av_log(avctx, AV_LOG_INFO,
               "[dovi-diag] packet=%"PRIu64
               " nals=%d access-units=%d VCL=%d RPU=%d EL=%d\n",
               s->dovi_packet_id, h2645_pkt.nb_nals, au_count,
               vcl_count, rpu_count, el_count);

done:
        ff_h2645_packet_uninit(&h2645_pkt);
    }
#endif
}

static void mediacodec_dovi_log_formats(AVCodecContext *avctx,
                                        const char *mime,
                                        FFAMediaFormat *requested)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    MediaCodecDecContext *ctx = s->ctx;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    char *requested_string = ff_AMediaFormat_toString(requested);
    char *output_string = ctx->format ?
        ff_AMediaFormat_toString(ctx->format) : NULL;
    void *hdr_static = NULL;
    void *hdr_dynamic = NULL;
    size_t hdr_static_size = 0;
    size_t hdr_dynamic_size = 0;
    int32_t dataspace = 0;
    int32_t requested_bit_depth = 0;
    int32_t output_bit_depth = 0;
    int32_t color_range = 0;
    int32_t color_standard = 0;
    int32_t color_transfer = 0;

    ff_AMediaFormat_getInt32(requested, "bit-depth", &requested_bit_depth);

    if (ctx->format) {
        ff_AMediaFormat_getBuffer(ctx->format, "hdr-static-info",
                                  &hdr_static, &hdr_static_size);
        ff_AMediaFormat_getBuffer(ctx->format, "hdr10-plus-info",
                                  &hdr_dynamic, &hdr_dynamic_size);
        if (!ff_AMediaFormat_getInt32(ctx->format, "data-space", &dataspace))
            ff_AMediaFormat_getInt32(ctx->format, "android._dataspace", &dataspace);
        ff_AMediaFormat_getInt32(ctx->format, "bit-depth", &output_bit_depth);
        ff_AMediaFormat_getInt32(ctx->format, "color-range", &color_range);
        ff_AMediaFormat_getInt32(ctx->format, "color-standard", &color_standard);
        ff_AMediaFormat_getInt32(ctx->format, "color-transfer", &color_transfer);
    }

    av_log(avctx, AV_LOG_INFO,
           "[dovi-diag] MediaCodec init: mime=%s decoder=%s API=%s path=%s "
           "profile=%d level=%d bits-per-raw-sample=%d output-pixfmt=%s "
           "components=%d color-format=%#x stride=%d slice-height=%d "
           "crop=%d,%d-%d,%d requested-bit-depth=%d output-bit-depth=%d "
           "range=%d standard=%d transfer=%d dataspace=%d "
           "hdr-static=%zu hdr-dynamic=%zu\n",
           mime, ctx->codec_name, ctx->use_ndk_codec ? "NDK" : "JNI",
           ctx->surface ? "surface" : "copy", avctx->profile, avctx->level,
           avctx->bits_per_raw_sample,
           av_get_pix_fmt_name(avctx->pix_fmt) ?
               av_get_pix_fmt_name(avctx->pix_fmt) : "unknown",
           desc ? desc->nb_components : 0, ctx->color_format, ctx->stride,
           ctx->slice_height, ctx->crop_left, ctx->crop_top,
           ctx->crop_right, ctx->crop_bottom, requested_bit_depth,
           output_bit_depth, color_range, color_standard, color_transfer, dataspace,
           hdr_static_size, hdr_dynamic_size);
    av_log(avctx, AV_LOG_INFO,
           "[dovi-diag] requested MediaFormat: %s\n",
           requested_string ? requested_string : "unavailable");
    av_log(avctx, AV_LOG_INFO,
           "[dovi-diag] output MediaFormat: %s\n",
           output_string ? output_string : "unavailable");

    av_free(requested_string);
    av_free(output_string);
}

static av_cold int mediacodec_decode_close(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    mediacodec_dovi_queue_clear(s);
#if CONFIG_HEVC_MEDIACODEC_DECODER
    ff_dovi_ctx_unref(&s->dovi_ctx);
#endif

    ff_mediacodec_dec_close(avctx, s->ctx);
    s->ctx = NULL;

    av_packet_unref(&s->buffered_pkt);
    av_freep(&s->dovi_sha);

    return 0;
}

#if CONFIG_H264_MEDIACODEC_DECODER || CONFIG_HEVC_MEDIACODEC_DECODER
static int h2645_ps_to_nalu(const uint8_t *src, int src_size, uint8_t **out, int *out_size)
{
    int i;
    int ret = 0;
    uint8_t *p = NULL;
    static const uint8_t nalu_header[] = { 0x00, 0x00, 0x00, 0x01 };

    if (!out || !out_size) {
        return AVERROR(EINVAL);
    }

    p = av_malloc(sizeof(nalu_header) + src_size);
    if (!p) {
        return AVERROR(ENOMEM);
    }

    *out = p;
    *out_size = sizeof(nalu_header) + src_size;

    memcpy(p, nalu_header, sizeof(nalu_header));
    memcpy(p + sizeof(nalu_header), src, src_size);

    /* Escape 0x00, 0x00, 0x0{0-3} pattern */
    for (i = 4; i < *out_size; i++) {
        if (i < *out_size - 3 &&
            p[i + 0] == 0 &&
            p[i + 1] == 0 &&
            p[i + 2] <= 3) {
            uint8_t *new;

            *out_size += 1;
            new = av_realloc(*out, *out_size);
            if (!new) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
            *out = p = new;

            i = i + 2;
            memmove(p + i + 1, p + i, *out_size - (i + 1));
            p[i] = 0x03;
        }
    }
done:
    if (ret < 0) {
        av_freep(out);
        *out_size = 0;
    }

    return ret;
}
#endif

#if CONFIG_H264_MEDIACODEC_DECODER
static int h264_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int i;
    int ret;

    H264ParamSets ps = {0};
    const PPS *pps = NULL;
    const SPS *sps = NULL;
    int is_avc = 0;
    int nal_length_size = 0;

    ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &is_avc, &nal_length_size, 0, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (pps && sps) {
        uint8_t *data = NULL;
        int data_size = 0;

        avctx->profile = ff_h264_get_profile(sps);
        avctx->level = sps->level_idc;

        if ((ret = h2645_ps_to_nalu(sps->data, sps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-0", (void*)data, data_size);
        av_freep(&data);

        if ((ret = h2645_ps_to_nalu(pps->data, pps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-1", (void*)data, data_size);
        av_freep(&data);
    } else {
        const int warn = is_avc && (avctx->codec_tag == MKTAG('a','v','c','1') ||
                                    avctx->codec_tag == MKTAG('a','v','c','2'));
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_h264_ps_uninit(&ps);

    return ret;
}
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
static int hevc_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int i;
    int ret;

    HEVCParamSets ps = {0};
    HEVCSEI sei = {0};

    const HEVCVPS *vps = NULL;
    const HEVCPPS *pps = NULL;
    const HEVCSPS *sps = NULL;
    int is_nalff = 0;
    int nal_length_size = 0;

    uint8_t *vps_data = NULL;
    uint8_t *sps_data = NULL;
    uint8_t *pps_data = NULL;
    int vps_data_size = 0;
    int sps_data_size = 0;
    int pps_data_size = 0;

    ret = ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &sei, &is_nalff, &nal_length_size, 0, 1, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < HEVC_MAX_VPS_COUNT; i++) {
        if (ps.vps_list[i]) {
            vps = ps.vps_list[i];
            break;
        }
    }

    for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (vps && pps && sps) {
        uint8_t *data;
        int data_size;

        avctx->profile = sps->ptl.general_ptl.profile_idc;
        avctx->level   = sps->ptl.general_ptl.level_idc;

        if ((ret = h2645_ps_to_nalu(vps->data, vps->data_size, &vps_data, &vps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(sps->data, sps->data_size, &sps_data, &sps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(pps->data, pps->data_size, &pps_data, &pps_data_size)) < 0) {
            goto done;
        }

        data_size = vps_data_size + sps_data_size + pps_data_size;
        data = av_mallocz(data_size);
        if (!data) {
            ret = AVERROR(ENOMEM);
            goto done;
        }

        memcpy(data                                , vps_data, vps_data_size);
        memcpy(data + vps_data_size                , sps_data, sps_data_size);
        memcpy(data + vps_data_size + sps_data_size, pps_data, pps_data_size);

        ff_AMediaFormat_setBuffer(format, "csd-0", data, data_size);

        av_freep(&data);
    } else {
        const int warn = is_nalff && avctx->codec_tag == MKTAG('h','v','c','1');
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract VPS/PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_hevc_ps_uninit(&ps);

    av_freep(&vps_data);
    av_freep(&sps_data);
    av_freep(&pps_data);

    return ret;
}
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER || \
    CONFIG_MPEG4_MEDIACODEC_DECODER || \
    CONFIG_VP8_MEDIACODEC_DECODER   || \
    CONFIG_VP9_MEDIACODEC_DECODER   || \
    CONFIG_AV1_MEDIACODEC_DECODER   || \
    CONFIG_AAC_MEDIACODEC_DECODER   || \
    CONFIG_AMRNB_MEDIACODEC_DECODER || \
    CONFIG_AMRWB_MEDIACODEC_DECODER || \
    CONFIG_MP3_MEDIACODEC_DECODER
static int common_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int ret = 0;

    if (avctx->extradata) {
        ff_AMediaFormat_setBuffer(format, "csd-0", avctx->extradata, avctx->extradata_size);
    }

    return ret;
}
#endif

static av_cold int mediacodec_decode_init(AVCodecContext *avctx)
{
    int ret;
    int sdk_int;
    const AVPacketSideData *dovi_config;

    const char *codec_mime = NULL;

    FFAMediaFormat *format = NULL;
    MediaCodecH264DecContext *s = avctx->priv_data;

    if (s->dovi_p5_surface_decoder && avctx->codec_id != AV_CODEC_ID_HEVC) {
        av_log(avctx, AV_LOG_ERROR,
               "The Dolby Vision surface decoder is only available for HEVC\n");
        return AVERROR(EINVAL);
    }

    s->dovi_ctx.logctx = avctx;
    if (s->dovi_p5_metadata || s->dovi_p5_surface_decoder) {
        dovi_config = ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
        mediacodec_dovi_set_config(avctx,
                                   dovi_config ? dovi_config->data : NULL,
                                   dovi_config ? dovi_config->size : 0,
                                   "coded-stream");
    }

    if (s->dovi_diagnostics) {
        s->dovi_sha = av_sha_alloc();
        if (!s->dovi_sha)
            return AVERROR(ENOMEM);
        mediacodec_dovi_log_config(avctx);
    }

    if (s->use_ndk_codec < 0)
        s->use_ndk_codec = !av_jni_get_java_vm(avctx);

    format = ff_AMediaFormat_new(s->use_ndk_codec);
    if (!format) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media format\n");
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    switch (avctx->codec_id) {
#if CONFIG_AV1_MEDIACODEC_DECODER
    case AV_CODEC_ID_AV1:
        codec_mime = "video/av01";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_H264_MEDIACODEC_DECODER
    case AV_CODEC_ID_H264:
        codec_mime = "video/avc";

        ret = h264_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_HEVC_MEDIACODEC_DECODER
    case AV_CODEC_ID_HEVC:
        if (s->dovi_p5_surface_decoder) {
            if (!s->dovi_p5_active) {
                av_log(avctx, AV_LOG_ERROR,
                       "The Dolby Vision surface decoder is limited to "
                       "single-layer Profile 5\n");
                ret = AVERROR(EINVAL);
                goto done;
            }
            codec_mime = "video/dolby-vision";
        } else {
            codec_mime = "video/hevc";
        }

        ret = hevc_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG2_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG2VIDEO:
        codec_mime = "video/mpeg2";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG4_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG4:
        codec_mime = "video/mp4v-es",

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP8_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP8:
        codec_mime = "video/x-vnd.on2.vp8";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP9_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP9:
        codec_mime = "video/x-vnd.on2.vp9";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AAC_MEDIACODEC_DECODER
    case AV_CODEC_ID_AAC:
        codec_mime = "audio/mp4a-latm";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRNB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_NB:
        codec_mime = "audio/3gpp";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRWB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_WB:
        codec_mime = "audio/amr-wb";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MP3_MEDIACODEC_DECODER
    case AV_CODEC_ID_MP3:
        codec_mime = "audio/mpeg";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
    default:
        av_assert0(0);
    }

    ff_AMediaFormat_setString(format, "mime", codec_mime);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ff_AMediaFormat_setInt32(format, "width", avctx->width);
        ff_AMediaFormat_setInt32(format, "height", avctx->height);
    } else {
        ff_AMediaFormat_setInt32(format, "channel-count", avctx->ch_layout.nb_channels);
        ff_AMediaFormat_setInt32(format, "sample-rate", avctx->sample_rate);
    }
    if (s->operating_rate > 0)
        ff_AMediaFormat_setInt32(format, "operating-rate", s->operating_rate);

    s->ctx = av_mallocz(sizeof(*s->ctx));
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate MediaCodecDecContext\n");
        ret = AVERROR(ENOMEM);
        goto done;
    }

    s->ctx->delay_flush = s->delay_flush;
    s->ctx->use_ndk_codec = s->use_ndk_codec;
    s->ctx->codec_profile = -1;
    if (s->dovi_p5_surface_decoder) {
        s->ctx->codec_profile = MEDIACODEC_DOVI_PROFILE_DVHE_STN;
        ff_AMediaFormat_setInt32(format, "profile",
                                 MEDIACODEC_DOVI_PROFILE_DVHE_STN);
        av_log(avctx, AV_LOG_INFO,
               "Requesting the Android Dolby Vision Profile 5 surface decoder\n");
    }

    if ((ret = ff_mediacodec_dec_init(avctx, s->ctx, codec_mime, format)) < 0) {
        s->ctx = NULL;
        goto done;
    }

    if (s->dovi_p5_surface_decoder && !s->ctx->surface) {
        av_log(avctx, AV_LOG_ERROR,
               "The Dolby Vision decoder option requires a MediaCodec "
               "output surface\n");
        ret = AVERROR(EINVAL);
        goto done;
    }

    if (s->dovi_diagnostics)
        mediacodec_dovi_log_formats(avctx, codec_mime, format);

    av_log(avctx, AV_LOG_INFO,
           "MediaCodec started successfully: codec = %s, ret = %d\n",
           s->ctx->codec_name, ret);

    sdk_int = ff_Build_SDK_INT(avctx);
    /* ff_Build_SDK_INT can fail when target API < 24 and JVM isn't available.
     * If we don't check sdk_int > 0, the workaround might be enabled by
     * mistake.
     * JVM is required to make the workaround works reliably. On the other hand,
     * missing a workaround should not be a serious issue, we do as best we can.
     */
    if (sdk_int > 0 && sdk_int <= 23 &&
        strcmp(s->ctx->codec_name, "OMX.amlogic.mpeg2.decoder.awesome") == 0) {
        av_log(avctx, AV_LOG_INFO, "Enabling workaround for %s on API=%d\n",
               s->ctx->codec_name, sdk_int);
        s->amlogic_mpeg2_api23_workaround = 1;
    }

done:
    if (format) {
        ff_AMediaFormat_delete(format);
    }

    if (ret < 0) {
        mediacodec_decode_close(avctx);
    }

    return ret;
}

static int mediacodec_receive_output(AVCodecContext *avctx, AVFrame *frame,
                                     bool wait)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret = ff_mediacodec_dec_receive(avctx, s->ctx, frame, wait);

    if (ret == 0) {
        ret = mediacodec_dovi_attach(avctx, frame);
        if (ret < 0) {
            av_frame_unref(frame);
            mediacodec_dovi_flush(s);
        }
    } else if (ret != AVERROR(EAGAIN)) {
        mediacodec_dovi_flush(s);
    }

    return ret;
}

static int mediacodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    int ret;
    ssize_t index;

    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_mediacodec_dec_is_flushing(avctx, s->ctx)) {
        if (!ff_mediacodec_dec_flush(avctx, s->ctx)) {
            return AVERROR(EAGAIN);
        }
    }

    /* poll for new frame */
    ret = mediacodec_receive_output(avctx, frame, false);
    if (ret != AVERROR(EAGAIN))
        return ret;

    /* feed decoder */
    while (1) {
        if (s->ctx->current_input_buffer < 0 && !s->ctx->draining) {
            /* poll for input space */
            index = ff_AMediaCodec_dequeueInputBuffer(s->ctx->codec, 0);
            if (index < 0) {
                /* no space, block for an output frame to appear */
                ret = mediacodec_receive_output(avctx, frame, true);
                /* Try again if both input port and output port return EAGAIN.
                 * If no data is consumed and no frame in output, it can make
                 * both avcodec_send_packet() and avcodec_receive_frame()
                 * return EAGAIN, which violate the design.
                 */
                if (ff_AMediaCodec_infoTryAgainLater(s->ctx->codec, index) &&
                    ret == AVERROR(EAGAIN))
                    continue;
                return ret;
            }
            s->ctx->current_input_buffer = index;
        }

        /* try to flush any buffered packet data */
        if (s->buffered_pkt.size > 0) {
            const int packet_size_before_send = s->buffered_pkt.size;

            ret = ff_mediacodec_dec_send(avctx, s->ctx, &s->buffered_pkt, false);
            if (ret >= 0) {
                if (s->dovi_diagnostics) {
                    av_log(avctx, AV_LOG_INFO,
                           "[dovi-diag] packet=%"PRIu64" input chunk "
                           "offset=%d size=%d remaining=%d split=%d\n",
                           s->dovi_packet_id, s->dovi_packet_offset, ret,
                           packet_size_before_send - ret,
                           ret < packet_size_before_send ||
                           s->dovi_packet_offset > 0);
                    s->dovi_packet_offset += ret;
                }
                s->buffered_pkt.size -= ret;
                s->buffered_pkt.data += ret;
                if (s->buffered_pkt.size <= 0) {
                    av_packet_unref(&s->buffered_pkt);
                } else {
                    av_log(avctx, AV_LOG_WARNING,
                           "could not send entire packet in single input buffer (%d < %d)\n",
                           ret, s->buffered_pkt.size+ret);
                }
            } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return ret;
            }

            if (s->amlogic_mpeg2_api23_workaround && s->buffered_pkt.size <= 0) {
                /* fallthrough to fetch next packet regardless of input buffer space */
            } else {
                /* poll for space again */
                continue;
            }
        }

        /* fetch new packet or eof */
        ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
        if (ret == AVERROR_EOF) {
            AVPacket null_pkt = { 0 };
            if (s->dovi_diagnostics)
                av_log(avctx, AV_LOG_INFO,
                       "[dovi-diag] end of input after packet=%"PRIu64"\n",
                       s->dovi_packet_id);
            ret = ff_mediacodec_dec_send(avctx, s->ctx, &null_pkt, true);
            if (ret < 0)
                return ret;
            return mediacodec_receive_output(avctx, frame, true);
        } else if (ret == AVERROR(EAGAIN) && s->ctx->current_input_buffer < 0) {
            return mediacodec_receive_output(avctx, frame, true);
        } else if (ret < 0) {
            return ret;
        }

        if (s->dovi_diagnostics)
            mediacodec_dovi_log_packet(avctx, &s->buffered_pkt);

        ret = mediacodec_dovi_parse_packet(avctx, &s->buffered_pkt);
        if (ret < 0)
            return ret;
    }

    return AVERROR(EAGAIN);
}

static void mediacodec_decode_flush(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    if (s->dovi_diagnostics)
        av_log(avctx, AV_LOG_INFO,
               "[dovi-diag] decoder flush after packet=%"PRIu64
               " buffered=%d offset=%d\n",
               s->dovi_packet_id, s->buffered_pkt.size,
               s->dovi_packet_offset);
    av_packet_unref(&s->buffered_pkt);
    s->dovi_packet_size = 0;
    s->dovi_packet_offset = 0;
    mediacodec_dovi_flush(s);

    ff_mediacodec_dec_flush(avctx, s->ctx);
}

static const AVCodecHWConfigInternal *const mediacodec_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public          = {
            .pix_fmt     = AV_PIX_FMT_MEDIACODEC,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_AD_HOC |
                           AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
        },
        .hwaccel         = NULL,
    },
    NULL
};

#define OFFSET(x) offsetof(MediaCodecH264DecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_vdec_options[] = {
    { "delay_flush", "Delay flush until hw output buffers are returned to the decoder",
                     OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { "operating_rate", "The desired operating rate that the codec will need to operate at, zero for unspecified",
            OFFSET(operating_rate), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, VD },
    { "dovi_diagnostics", "Log Dolby Vision and MediaCodec packet/frame diagnostics",
            OFFSET(dovi_diagnostics), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "dovi_p5_metadata", "Propagate single-layer Dolby Vision Profile 5 metadata",
            OFFSET(dovi_p5_metadata), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "dovi_p5_surface_decoder", "Use Android's Dolby Vision decoder for "
            "single-layer Profile 5 surface output",
            OFFSET(dovi_p5_surface_decoder), AV_OPT_TYPE_BOOL,
            {.i64 = 0}, 0, 1, VD },
    { NULL }
};

#define DECLARE_MEDIACODEC_VCLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_vdec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_VDEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_VCLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,                                        \
    .bsfs           = bsf,                                                                     \
    .hw_configs     = mediacodec_hw_configs,                                                   \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_H264_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg2, "MPEG-2", AV_CODEC_ID_MPEG2VIDEO, NULL)
#endif

#if CONFIG_MPEG4_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg4, "MPEG-4", AV_CODEC_ID_MPEG4, NULL)
#endif

#if CONFIG_VP8_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp8, "VP8", AV_CODEC_ID_VP8, NULL)
#endif

#if CONFIG_VP9_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp9, "VP9", AV_CODEC_ID_VP9, NULL)
#endif

#if CONFIG_AV1_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(av1, "AV1", AV_CODEC_ID_AV1, NULL)
#endif

#define AD AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_adec_options[] = {
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, AD },
    { "operating_rate", "The desired operating rate that the codec will need to operate at, zero for unspecified",
            OFFSET(operating_rate), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AD },
    { NULL }
};

#define DECLARE_MEDIACODEC_ACLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_adec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_ADEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_ACLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_AUDIO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,                              \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE,                                        \
    .bsfs           = bsf,                                                                     \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_AAC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(aac, "AAC", AV_CODEC_ID_AAC, "aac_adtstoasc")
#endif

#if CONFIG_AMRNB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrnb, "AMR-NB", AV_CODEC_ID_AMR_NB, NULL)
#endif

#if CONFIG_AMRWB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrwb, "AMR-WB", AV_CODEC_ID_AMR_WB, NULL)
#endif

#if CONFIG_MP3_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(mp3, "MP3", AV_CODEC_ID_MP3, NULL)
#endif
