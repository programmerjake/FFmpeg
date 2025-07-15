/*
 * MXF VANC SMPTE-436M parsing functions
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

#include "vanc_smpte_436m.h"
#include "bytestream.h"
#include "libavutil/error.h"

int avpriv_vanc_smpte_436m_iter_init(VancSmpte436mIterator* iter, const uint8_t* buf, int buf_size)
{
    if (buf_size < 2)
        return AVERROR_INVALIDDATA;
    bytestream2_init(&iter->gb, buf, buf_size);
    iter->anc_packets_left = bytestream2_get_be16(&iter->gb);
    if (iter->anc_packets_left > bytestream2_get_bytes_left(&iter->gb))
        return AVERROR_INVALIDDATA;
    return 0;
}

int avpriv_vanc_smpte_436m_iter_next(VancSmpte436mIterator* iter, Smpte436mCodedAnc* anc)
{
    if (iter->anc_packets_left <= 0)
        return AVERROR_EOF;
    iter->anc_packets_left--;
    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    anc->line_number                    = bytestream2_get_be16(&iter->gb);
    anc->wrapping_type                  = bytestream2_get_byte(&iter->gb);
    anc->payload_sample_coding          = bytestream2_get_byte(&iter->gb);
    anc->payload_sample_count           = bytestream2_get_be16(&iter->gb);
    anc->payload_array_length           = bytestream2_get_be32(&iter->gb);
    unsigned payload_array_element_size = bytestream2_get_be32(&iter->gb);
    unsigned payload_space              = bytestream2_get_bytes_left(&iter->gb);
    anc->payload                        = iter->gb.buffer;
    bytestream2_skip(&iter->gb, anc->payload_array_length);
    switch (anc->wrapping_type) {
        case SMPTE_436M_WRAPPING_TYPE_VANC_FRAME:
        case SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_1:
        case SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_2:
        case SMPTE_436M_WRAPPING_TYPE_VANC_PROGRESSIVE_FRAME:
        case SMPTE_436M_WRAPPING_TYPE_HANC_FRAME:
        case SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_1:
        case SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_2:
        case SMPTE_436M_WRAPPING_TYPE_HANC_PROGRESSIVE_FRAME:
            break;
        default:
            return AVERROR_INVALIDDATA;
    }
    switch (anc->payload_sample_coding) {
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA_AND_COLOR_DIFF:
            // not allowed for ANC packets
            return AVERROR_INVALIDDATA;
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF_WITH_PARITY_ERROR:
            break;
        default:
            return AVERROR_INVALIDDATA;
    }
    if (anc->payload_array_length <
        avpriv_smpte_436m_coded_anc_payload_size(anc->payload_sample_coding, anc->payload_sample_count))
        return AVERROR_INVALIDDATA;
    if (payload_array_element_size != 1)
        return AVERROR_INVALIDDATA;
    if (payload_space < anc->payload_array_length)
        return AVERROR_INVALIDDATA;
    return 0;
}

int avpriv_smpte_436m_coded_anc_payload_size(Smpte436mPayloadSampleCoding sample_coding, uint16_t sample_count)
{
    switch (sample_coding) {
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA_AND_COLOR_DIFF:
            return AVERROR_INVALIDDATA;
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF_WITH_PARITY_ERROR:
            // "The Payload Byte Array shall be padded to achieve UInt32 alignment."
            // section 4.4 of https://pub.smpte.org/latest/st436/s436m-2006.pdf
            return (sample_count + 3) & -4;
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF:
            // encoded with 3 10-bit samples in a UInt32.
            // "The Payload Byte Array shall be padded to achieve UInt32 alignment."
            // section 4.4 of https://pub.smpte.org/latest/st436/s436m-2006.pdf
            return 4 * ((sample_count + 2) / 3);
        default:
            return AVERROR_INVALIDDATA;
    }
}

int avpriv_smpte_291m_decode_anc(Smpte291mAnc*                out,
                                 Smpte436mPayloadSampleCoding sample_coding,
                                 uint16_t                     sample_count,
                                 const uint8_t*               payload,
                                 void*                        log_ctx)
{
    switch (sample_coding) {
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA_AND_COLOR_DIFF:
            return AVERROR_INVALIDDATA;
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF_WITH_PARITY_ERROR:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF_WITH_PARITY_ERROR: {
            if (sample_count < 3)
                return AVERROR_INVALIDDATA;
            out->did         = *payload++;
            out->sdid_or_dbn = *payload++;
            out->data_count  = *payload++;
            if (sample_count < out->data_count + 3)
                return AVERROR_INVALIDDATA;
            // the checksum isn't stored in 8-bit mode, so calculate it.
            uint8_t checksum = out->did + out->sdid_or_dbn + out->data_count;
            for (unsigned i = 0; i < out->data_count; i++) {
                checksum += *payload;
                out->payload[i] = *payload++;
            }
            out->checksum = checksum;
            return 0;
        }
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF:
            av_log(log_ctx,
                   AV_LOG_ERROR,
                   "decoding an ANC packet using the 10-bit SMPTE 436M sample coding isn't implemented.\n");
            return AVERROR_PATCHWELCOME;
        default:
            return AVERROR_INVALIDDATA;
    }
}

int avpriv_smpte_291m_anc_extract_cta_708(const Smpte291mAnc* anc, uint8_t* cc_data, void* log_ctx)
{
    if (anc->did != 0x61 || anc->sdid_or_dbn != 1)
        return AVERROR(EAGAIN);
    GetByteContext gb;
    bytestream2_init(&gb, anc->payload, anc->data_count);
    // based on:
    // https://pub.smpte.org/latest/st334-2/st0334-2-2015.pdf
    uint16_t cdp_identifier = bytestream2_get_be16(&gb);
    if (cdp_identifier != 0x9669) {
        av_log(log_ctx, AV_LOG_ERROR, "wrong cdp identifier %x\n", cdp_identifier);
        return AVERROR_INVALIDDATA;
    }
    bytestream2_get_byte(&gb); // cdp_length
    bytestream2_get_byte(&gb); // cdp_frame_rate and reserved
    bytestream2_get_byte(&gb); // flags
    bytestream2_get_be16(&gb); // cdp_hdr_sequence_cntr
    unsigned section_id = bytestream2_get_byte(&gb);
    if (section_id == 0x71) {
        bytestream2_skip(&gb, 4); // skip time code section
        section_id = bytestream2_get_byte(&gb);
    }
    if (section_id == 0x72) {
        if (bytestream2_get_bytes_left(&gb) < 1)
            goto too_short;
        unsigned cc_count    = bytestream2_get_byte(&gb) & 0x1F;
        unsigned data_length = cc_count * 3;
        if (bytestream2_get_bytes_left(&gb) < data_length)
            goto too_short;
        if (cc_data)
            bytestream2_get_bufferu(&gb, cc_data, data_length);
        return cc_count;
    }
    return AVERROR(EAGAIN);

too_short:
    av_log(log_ctx, AV_LOG_ERROR, "not enough bytes in cdp\n");
    return AVERROR_INVALIDDATA;
}
