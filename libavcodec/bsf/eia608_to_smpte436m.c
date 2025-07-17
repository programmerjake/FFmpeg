/*
 * EIA-608 to MXF VANC SMPTE-436M bitstream filter
 * Copyright (c) 2025 Jacob Lifshay
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

#include "bsf.h"
#include "bsf_internal.h"
#include "codec_id.h"
#include "libavcodec/vanc_smpte_436m.h"
#include "libavutil/avassert.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"

#include "libklvanc/vanc-eia_708b.h"

typedef struct EIA608ToSMPTE436MContext
{
    const AVClass* class;
    struct klvanc_packet_eia_708b_s* eia708_cdp;
    unsigned                         line_number;
    unsigned                         cdp_sequence_cntr;
    unsigned                         wrapping_type_opt;
    unsigned                         sample_coding_opt;
    Smpte436mWrappingType            wrapping_type;
    Smpte436mPayloadSampleCoding     sample_coding;
    AVRational                       cdp_frame_rate;
} EIA608ToSMPTE436MContext;

static av_cold int ff_eia608_to_smpte436m_init(AVBSFContext* ctx)
{
    EIA608ToSMPTE436MContext* priv = ctx->priv_data;

    priv->wrapping_type = priv->wrapping_type_opt;
    priv->sample_coding = priv->sample_coding_opt;

    // validate we can handle the selected wrapping type and sample coding

    static const Smpte291mAnc anc = {
        .did         = 0x61,
        .sdid_or_dbn = 0x01,
        .data_count  = 0x49,
        .payload =
            {
                0x96, 0x69, 0x49, 0x7F, 0x43, 0xFA, 0x8D, 0x72, 0xF4, // header
                0xFC, 0x80, 0x80, 0xFD, 0x80, 0x80,                   // 608 triples
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, 0xFA, 0x00, 0x00, // 708 padding
                0x74, 0xFA, 0x8D, 0x81,                               // footer
            },
        .checksum = 0xAB,
    };
    Smpte436mCodedAnc coded_anc;

    int ret = avpriv_smpte_291m_encode_anc(
        &coded_anc, priv->line_number, priv->wrapping_type, priv->sample_coding, &anc, ctx);
    if (ret < 0)
        return ret;

    ctx->par_out->codec_type = AVMEDIA_TYPE_DATA;
    ctx->par_out->codec_id   = AV_CODEC_ID_VANC_SMPTE_436M;
    if (klvanc_create_eia708_cdp(&priv->eia708_cdp) < 0)
        return AVERROR(ENOMEM);

    // set values that all our vanc packets have in common:
    priv->eia708_cdp->header.ccdata_present         = 1;
    priv->eia708_cdp->header.caption_service_active = 1;

    // num/denom intentionally swapped since casparcg and ffmpeg uses the reciprocal of libklvanc
    if (klvanc_set_framerate_EIA_708B(priv->eia708_cdp, priv->cdp_frame_rate.den, priv->cdp_frame_rate.num) < 0) {
        av_log(ctx,
               AV_LOG_FATAL,
               "cdp_frame_rate not supported by libklvanc: %d/%d\n",
               priv->cdp_frame_rate.num,
               priv->cdp_frame_rate.den);
        klvanc_destroy_eia708_cdp(priv->eia708_cdp);
        priv->eia708_cdp = NULL;
        return AVERROR(EINVAL);
    }

    return 0;
}

static int ff_eia608_to_smpte436m_filter(AVBSFContext* ctx, AVPacket* out)
{
    EIA608ToSMPTE436MContext* priv = ctx->priv_data;
    AVPacket*                 in;

    int ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    size_t cc_count = in->size / 3;

    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(ctx,
               AV_LOG_ERROR,
               "cc_count (%zu) is bigger than the maximum supported by libklvanc (%zu), truncating captions packet\n",
               cc_count,
               (size_t)KLVANC_MAX_CC_COUNT);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    struct klvanc_packet_eia_708b_s* eia708_cdp = priv->eia708_cdp;

    eia708_cdp->ccdata.cc_count = (uint8_t)cc_count;
    for (size_t i = 0; i < cc_count; i++) {
        size_t start = i * 3;

        eia708_cdp->ccdata.cc[i].cc_valid   = (in->data[start] & 0x04) != 0;
        eia708_cdp->ccdata.cc[i].cc_type    = in->data[start] & 0x03;
        eia708_cdp->ccdata.cc[i].cc_data[0] = in->data[start + 1];
        eia708_cdp->ccdata.cc[i].cc_data[1] = in->data[start + 2];
    }

    klvanc_finalize_EIA_708B(eia708_cdp, priv->cdp_sequence_cntr++);
    // cdp_sequence_cntr wraps around at 16-bits
    priv->cdp_sequence_cntr &= 0xFFFFU;

    uint8_t* bytes;
    uint16_t byte_count;

    if (klvanc_convert_EIA_708B_to_packetBytes(eia708_cdp, &bytes, &byte_count) < 0) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    Smpte291mAnc anc;
    anc.did         = 0x61;
    anc.sdid_or_dbn = 0x1;
    av_assert0(byte_count <= SMPTE_291M_ANC_PAYLOAD_CAPACITY);
    anc.data_count = byte_count;
    memcpy(anc.payload, bytes, byte_count);
    free(bytes);
    bytes = NULL;
    avpriv_smpte_291m_anc_fill_checksum(&anc);

    Smpte436mCodedAnc coded_anc;
    ret = avpriv_smpte_291m_encode_anc(
        &coded_anc, priv->line_number, (Smpte436mWrappingType)priv->wrapping_type, priv->sample_coding, &anc, ctx);
    if (ret < 0)
        goto fail;

    ret = avpriv_vanc_smpte_436m_encode(NULL, 0, 1, &coded_anc);
    if (ret < 0)
        goto fail;

    ret = av_new_packet(out, ret);
    if (ret < 0)
        goto fail;

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

    ret = avpriv_vanc_smpte_436m_encode(out->data, out->size, 1, &coded_anc);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);
    return ret;
}

static void ff_eia608_to_smpte436m_close(AVBSFContext* ctx)
{
    EIA608ToSMPTE436MContext* priv = ctx->priv_data;
    klvanc_destroy_eia708_cdp(priv->eia708_cdp);
    priv->eia708_cdp = NULL;
}

#define OFFSET(x) offsetof(EIA608ToSMPTE436MContext, x)
#define FLAGS AV_OPT_FLAG_BSF_PARAM
// clang-format off
static const AVOption options[] = {
    { "line_number", "line number -- you probably want 9 or 11", OFFSET(line_number), AV_OPT_TYPE_UINT, { .i64 = 9 }, 0, 0xFFFF, FLAGS },
    { "wrapping_type", "wrapping type", OFFSET(wrapping_type_opt), AV_OPT_TYPE_UINT, { .i64 = SMPTE_436M_WRAPPING_TYPE_VANC_FRAME }, 0, 0xFF, FLAGS, .unit = "wrapping_type" },
    SMPTE_436M_WRAPPING_TYPE_VANC_AVOPTIONS(FLAGS, "wrapping_type"),
    { "sample_coding", "payload sample coding", OFFSET(sample_coding_opt), AV_OPT_TYPE_UINT, { .i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA }, 0, 0xFF, FLAGS, .unit = "sample_coding" },
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_ANC_AVOPTIONS(FLAGS, "sample_coding"),
    { "initial_cdp_sequence_cntr", "initial cdp_*_sequence_cntr value", OFFSET(cdp_sequence_cntr), AV_OPT_TYPE_UINT, { .i64 = 0 }, 0, 0xFFFF, FLAGS },
    { "cdp_frame_rate", "set the `cdp_frame_rate` fields", OFFSET(cdp_frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "30000/1001" }, 0, INT_MAX, FLAGS },
    { NULL },
};
// clang-format on

static const AVClass eia608_to_smpte436m_class = {
    .class_name = "eia608_to_smpte436m bitstream filter",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_eia608_to_smpte436m_bsf = {
    .p.name         = "eia608_to_smpte436m",
    .p.codec_ids    = (const enum AVCodecID[]){AV_CODEC_ID_EIA_608, AV_CODEC_ID_NONE},
    .p.priv_class   = &eia608_to_smpte436m_class,
    .priv_data_size = sizeof(EIA608ToSMPTE436MContext),
    .init           = ff_eia608_to_smpte436m_init,
    .filter         = ff_eia608_to_smpte436m_filter,
    .close          = ff_eia608_to_smpte436m_close,
};
