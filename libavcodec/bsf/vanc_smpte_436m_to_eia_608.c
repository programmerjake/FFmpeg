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
#include "bytestream.h"
#include "codec_id.h"
#include "libavutil/error.h"
#include <stdint.h>

static av_cold int ff_vanc_smpte_436m_to_eia_608_init(AVBSFContext* ctx)
{
    ctx->par_out->codec_type = AVMEDIA_TYPE_SUBTITLE;
    ctx->par_out->codec_id   = AV_CODEC_ID_EIA_608;
    return 0;
}

typedef struct Smpte436mAnc
{
    uint16_t       line_number;
    uint8_t        wrapping_type;
    uint8_t        payload_sample_config;
    uint16_t       payload_sample_count;
    uint32_t       payload_array_length;
    uint32_t       payload_array_element_size;
    GetByteContext payload;
} Smpte436mAnc;

static int read_smpte_436m_anc(GetByteContext* gb, Smpte436mAnc* out)
{
    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    out->line_number                = bytestream2_get_be16(gb);
    out->wrapping_type              = bytestream2_get_byte(gb);
    out->payload_sample_config      = bytestream2_get_byte(gb);
    out->payload_sample_count       = bytestream2_get_be16(gb);
    out->payload_array_length       = bytestream2_get_be32(gb);
    out->payload_array_element_size = bytestream2_get_be32(gb);
    if (out->payload_array_element_size != 1)
        return AVERROR_INVALIDDATA;
    if ((unsigned)bytestream2_get_bytes_left(gb) < out->payload_array_length)
        return AVERROR_INVALIDDATA;
    bytestream2_init(&out->payload, gb->buffer, out->payload_array_length);
    bytestream2_skip(gb, out->payload_array_length);
    return 0;
}

/// returns 1 for ok, 0 for skip, < 0 for error
static int check_smpte_436m_anc(const Smpte436mAnc* anc, void* log_ctx)
{
    switch (anc->payload_sample_config) {
        case 4:  // 8-bit luma samples
        case 5:  // 8-bit color difference samples
        case 6:  // 8-bit luma and color difference samples
        case 10: // 8-bit luma samples -- with parity error
        case 11: // 8-bit color difference samples -- with parity error
        case 12: // 8-bit luma and color difference samples -- with parity error
            break;
        case 7: // 10-bit luma samples
        case 8: // 10-bit color difference samples
        case 9: // 10-bit luma and color difference samples
            av_log(log_ctx, AV_LOG_WARNING, "unsupported 10 bit sample coding\n");
            return 0;
        default:
            return AVERROR_INVALIDDATA;
    }
    if (anc->line_number != 9 && anc->line_number != 11)
        return 0;
    return 1;
}

typedef struct Vanc
{
    uint8_t        did;
    uint8_t        sdid;
    uint8_t        data_length;
    GetByteContext payload;
} Vanc;

static int read_vanc(GetByteContext* gb, Vanc* out, void* log_ctx)
{
    out->did         = bytestream2_get_byte(gb);
    out->sdid        = bytestream2_get_byte(gb);
    out->data_length = bytestream2_get_byte(gb);
    if ((unsigned)bytestream2_get_bytes_left(gb) < out->data_length + 4) {
        av_log(log_ctx, AV_LOG_ERROR, "not enough bytes in vanc packet\n");
        return AVERROR_INVALIDDATA;
    }
    bytestream2_init(&out->payload, gb->buffer, out->data_length);
    return 0;
}

/// returns 1 for ok, 0 for skip, < 0 for error
static int check_vanc(const Vanc* vanc)
{
    // check if it's a CTA-708 packet
    if (vanc->did != 0x61 || vanc->sdid != 1) {
        // ignore other types of vanc packets
        return 0;
    }
    return 1;
}

typedef struct Cdp
{
    uint16_t       cdp_identifier;
    uint8_t        cdp_length;
    uint8_t        cdp_frame_rate_and_reserved;
    uint8_t        flags;
    uint16_t       cdp_hdr_sequence_cntr;
    uint8_t        ccdata_id;
    uint8_t        cc_count;
    GetByteContext payload;
} Cdp;

static int read_cdp(GetByteContext* gb, Cdp* out, void* log_ctx)
{
    // based off of:
    // https://pub.smpte.org/latest/st334-2/st0334-2-2015.pdf
    out->cdp_identifier = bytestream2_get_be16(gb);
    if (out->cdp_identifier != 0x9669) {
        av_log(log_ctx, AV_LOG_ERROR, "wrong cdp identifier %x\n", out->cdp_identifier);
        return AVERROR_INVALIDDATA;
    }
    out->cdp_length                  = bytestream2_get_byte(gb);
    out->cdp_frame_rate_and_reserved = bytestream2_get_byte(gb);
    out->flags                       = bytestream2_get_byte(gb);
    out->cdp_hdr_sequence_cntr       = bytestream2_get_be16(gb);
    unsigned section_id              = bytestream2_get_byte(gb);
    if (section_id == 0x71) {
        bytestream2_skip(gb, 4); // skip time code section
        section_id = bytestream2_get_byte(gb);
    }
    if (section_id == 0x72) {
        out->cc_count        = bytestream2_get_byte(gb) & 0x1F;
        unsigned data_length = (unsigned)out->cc_count * 3;
        if (bytestream2_get_bytes_left(gb) < data_length) {
            av_log(log_ctx, AV_LOG_ERROR, "not enough bytes in cdp\n");
            return AVERROR_INVALIDDATA;
        }
        bytestream2_init(&out->payload, gb->buffer, data_length);
    } else {
        out->cc_count = 0;
        bytestream2_init(&out->payload, NULL, 0);
    }
    return 0;
}

static int ff_vanc_smpte_436m_to_eia_608_filter(AVBSFContext* ctx, AVPacket* out)
{
    AVPacket*      in;
    int            ret;
    Cdp            cdp = {.cc_count = 0};
    GetByteContext gb;
    unsigned       number_of_anc_packets;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    bytestream2_init(&gb, in->data, in->size);

    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    number_of_anc_packets = bytestream2_get_be16(&gb);
    for (unsigned i = 0; i < number_of_anc_packets; i++) {
        Smpte436mAnc anc;
        ret = read_smpte_436m_anc(&gb, &anc);
        if (ret < 0)
            return ret;
        ret = check_smpte_436m_anc(&anc, ctx);
        if (ret < 0)
            return ret;
        if (ret == 0)
            continue;
        Vanc vanc;
        ret = read_vanc(&anc.payload, &vanc, ctx);
        if (ret < 0)
            return ret;
        ret = check_vanc(&vanc);
        if (ret < 0)
            return ret;
        if (ret == 0)
            continue;
        ret = read_cdp(&vanc.payload, &cdp, ctx);
        if (ret < 0)
            return ret;
        if (cdp.cc_count != 0)
            break;
    }

    if (cdp.cc_count == 0)
        return AVERROR(EAGAIN);

    ret = av_new_packet(out, bytestream2_get_bytes_left(&cdp.payload));
    if (ret < 0)
        goto fail;

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

    bytestream2_get_buffer(&cdp.payload, out->data, bytestream2_get_bytes_left(&cdp.payload));

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
