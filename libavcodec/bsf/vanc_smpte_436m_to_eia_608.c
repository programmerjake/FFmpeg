/*
 * MXF VANC SMPTE-436M to EIA-608 bitstream filter
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
#include "libavutil/error.h"

static av_cold int ff_vanc_smpte_436m_to_eia_608_init(AVBSFContext* ctx)
{
    ctx->par_out->codec_type = AVMEDIA_TYPE_SUBTITLE;
    ctx->par_out->codec_id   = AV_CODEC_ID_EIA_608;
    return 0;
}

static int ff_vanc_smpte_436m_to_eia_608_filter(AVBSFContext* ctx, AVPacket* out)
{
    AVPacket* in;
    int       ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    VancSmpte436mIterator iter;
    ret = avpriv_vanc_smpte_436m_iter_init(&iter, in->data, in->size);
    if (ret < 0)
        goto fail;
    Smpte436mCodedAnc coded_anc;
    while ((ret = avpriv_vanc_smpte_436m_iter_next(&iter, &coded_anc)) >= 0) {
        Smpte291mAnc anc;
        ret = avpriv_smpte_291m_decode_anc(
            &anc, coded_anc.payload_sample_coding, coded_anc.payload_sample_count, coded_anc.payload, ctx);
        if (ret < 0)
            goto fail;
        ret = avpriv_smpte_291m_anc_extract_cta_708(&anc, NULL, ctx);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            goto fail;
        int cc_count = ret;

        ret = av_new_packet(out, 3 * cc_count);
        if (ret < 0)
            goto fail;

        ret = av_packet_copy_props(out, in);
        if (ret < 0)
            goto fail;

        // verified it won't fail by running it above
        avpriv_smpte_291m_anc_extract_cta_708(&anc, out->data, ctx);

        return 0;
    }
    if (ret != AVERROR_EOF)
        return ret;
    ret = AVERROR(EAGAIN);

fail:
    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);
    return ret;
}

const FFBitStreamFilter ff_vanc_smpte_436m_to_eia_608_bsf = {
    .p.name      = "vanc_smpte_436m_to_eia_608",
    .p.codec_ids = (const enum AVCodecID[]){AV_CODEC_ID_VANC_SMPTE_436M, AV_CODEC_ID_NONE},
    .init        = ff_vanc_smpte_436m_to_eia_608_init,
    .filter      = ff_vanc_smpte_436m_to_eia_608_filter,
};
