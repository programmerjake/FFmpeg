/*
 * MXF SMPTE-436M VBI/ANC parsing functions
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

#include "smpte_436m.h"
#include "bytestream.h"
#include "libavcodec/packet.h"
#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"

typedef struct TryWrite
{
    uint8_t* buf;
    int initial_size_left;
    int size_left_or_error;
} TryWrite;

static av_always_inline void try_write_init(TryWrite* tw, uint8_t* buf, int buf_size)
{
    *tw = (TryWrite){.buf = buf, .initial_size_left = buf ? buf_size : INT_MAX};
    if (buf && buf_size < 0)
        tw->size_left_or_error = AVERROR_BUFFER_TOO_SMALL;
    else
        tw->size_left_or_error = tw->initial_size_left;
}

static av_always_inline int try_write_finish(TryWrite* tw)
{
    if (tw->size_left_or_error < 0)
        return tw->size_left_or_error;
    return tw->initial_size_left - tw->size_left_or_error;
}

static av_always_inline void try_write(TryWrite* tw, const uint8_t* buf, int buf_size)
{
    av_assert0(buf_size >= 0);
    if (buf_size > tw->size_left_or_error) {
        tw->size_left_or_error = AVERROR_INVALIDDATA;
        return;
    }
    tw->size_left_or_error -= buf_size;
    if (tw->buf) {
        memcpy(tw->buf, buf, buf_size);
        tw->buf += buf_size;
    }
}

static av_always_inline void try_write_byte(TryWrite* tw, uint8_t v) { try_write(tw, &v, 1); }

static av_always_inline void try_write_be16(TryWrite* tw, uint16_t v) { try_write(tw, (uint8_t[2]){v >> 8, v}, 2); }

static av_always_inline void try_write_be32(TryWrite* tw, uint32_t v)
{
    try_write(tw, (uint8_t[4]){v >> 24, v >> 16, v >> 8, v}, 4);
}

static int try_write_smpte_436m_entry(TryWrite* tw, const Smpte436mCodedAnc* anc)
{
    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    if (anc->payload_array_length > SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY)
        return AVERROR_INVALIDDATA;
    try_write_be16(tw, anc->line_number);
    try_write_byte(tw, anc->wrapping_type);
    try_write_byte(tw, anc->payload_sample_coding);
    try_write_be16(tw, anc->payload_sample_count);
    try_write_be32(tw, anc->payload_array_length);
    try_write_be32(tw, 1); // payload_array_element_size
    try_write(tw, anc->payload, anc->payload_array_length);
    return 0;
}

int avpriv_smpte_436m_anc_encode(uint8_t* out, int size, int anc_packet_count, const Smpte436mCodedAnc* anc_packets)
{
    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    if (anc_packet_count < 0 || anc_packet_count >= (1L << 16) || size < 0)
        return AVERROR_INVALIDDATA;

    TryWrite tw;
    try_write_init(&tw, out, size);
    try_write_be16(&tw, anc_packet_count);
    for (int i = 0; i < anc_packet_count; i++) {
        int ret = try_write_smpte_436m_entry(&tw, &anc_packets[i]);
        if (ret < 0)
            return ret;
    }
    return try_write_finish(&tw);
}

int avpriv_smpte_436m_anc_append(AVPacket* pkt, int anc_packet_count, const Smpte436mCodedAnc* anc_packets)
{
    int final_packet_count = 0;
    int write_start = 2;
    if (pkt->size >= 2) {
        final_packet_count = AV_RB16(pkt->data);
        write_start = pkt->size;
    } else if (pkt->size != 0) // if packet isn't empty
        return AVERROR_INVALIDDATA;
    if (anc_packet_count < 0 || anc_packet_count >= (1L << 16))
        return AVERROR_INVALIDDATA;
    final_packet_count += anc_packet_count;
    if (final_packet_count >= (1L << 16))
        return AVERROR_INVALIDDATA;
    TryWrite tw;
    int ret;
    try_write_init(&tw, NULL, 0);
    for (int i = 0; i < anc_packet_count; i++) {
        ret = try_write_smpte_436m_entry(&tw, &anc_packets[i]);
        if (ret < 0)
            return ret;
    }
    int additional_size = ret = try_write_finish(&tw);
    if (ret < 0)
        return ret;
    additional_size += write_start - pkt->size;
    ret = av_grow_packet(pkt, additional_size);
    if (ret < 0)
        return ret;
    try_write_init(&tw, pkt->data + write_start, pkt->size - write_start);
    for (int i = 0; i < anc_packet_count; i++) {
        ret = try_write_smpte_436m_entry(&tw, &anc_packets[i]);
        av_assert0(ret >= 0);
    }
    ret = try_write_finish(&tw);
    av_assert0(ret >= 0);
    AV_WB16(pkt->data, final_packet_count);
    return 0;
}

int avpriv_smpte_436m_anc_iter_init(Smpte436mAncIterator* iter, const uint8_t* buf, int buf_size)
{
    if (buf_size < 2)
        return AVERROR_INVALIDDATA;
    bytestream2_init(&iter->gb, buf, buf_size);
    iter->anc_packets_left = bytestream2_get_be16(&iter->gb);
    if (iter->anc_packets_left > bytestream2_get_bytes_left(&iter->gb))
        return AVERROR_INVALIDDATA;
    return 0;
}

int avpriv_smpte_436m_anc_iter_next(Smpte436mAncIterator* iter, Smpte436mCodedAnc* anc)
{
    if (iter->anc_packets_left <= 0)
        return AVERROR_EOF;
    iter->anc_packets_left--;
    // Based off Table 7 (page 13) of:
    // https://pub.smpte.org/latest/st436/s436m-2006.pdf
    anc->line_number = bytestream2_get_be16(&iter->gb);
    anc->wrapping_type = bytestream2_get_byte(&iter->gb);
    anc->payload_sample_coding = bytestream2_get_byte(&iter->gb);
    anc->payload_sample_count = bytestream2_get_be16(&iter->gb);
    anc->payload_array_length = bytestream2_get_be32(&iter->gb);
    unsigned payload_array_element_size = bytestream2_get_be32(&iter->gb);
    unsigned payload_space = bytestream2_get_bytes_left(&iter->gb);
    if (anc->payload_array_length > SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY) {
        bytestream2_skip(&iter->gb, anc->payload_array_length);
        return AVERROR_INVALIDDATA;
    }
    bytestream2_get_buffer(&iter->gb, anc->payload, anc->payload_array_length);
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
    if (sample_count > SMPTE_436M_CODED_ANC_SAMPLE_CAPACITY)
        return AVERROR_INVALIDDATA;
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

int avpriv_smpte_291m_anc_decode(Smpte291mAnc* out,
                                 Smpte436mPayloadSampleCoding sample_coding,
                                 uint16_t sample_count,
                                 const uint8_t* payload,
                                 void* log_ctx)
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
            out->did = *payload++;
            out->sdid_or_dbn = *payload++;
            out->data_count = *payload++;
            if (sample_count < out->data_count + 3)
                return AVERROR_INVALIDDATA;
            memcpy(out->payload, payload, out->data_count);
            // the checksum isn't stored in 8-bit mode, so calculate it.
            avpriv_smpte_291m_anc_fill_checksum(out);
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

void avpriv_smpte_291m_anc_fill_checksum(Smpte291mAnc* anc)
{
    uint8_t checksum = anc->did + anc->sdid_or_dbn + anc->data_count;
    for (unsigned i = 0; i < anc->data_count; i++) {
        checksum += anc->payload[i];
    }
    anc->checksum = checksum;
}

int avpriv_smpte_291m_anc_get_sample_count(const Smpte291mAnc* anc,
                                           Smpte436mPayloadSampleCoding sample_coding,
                                           void* log_ctx)
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
            return 3 + anc->data_count;
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF:
            av_log(log_ctx,
                   AV_LOG_ERROR,
                   "encoding an ANC packet using the 10-bit SMPTE 436M sample coding isn't implemented.\n");
            return AVERROR_PATCHWELCOME;
        default:
            return AVERROR_INVALIDDATA;
    }
}

int avpriv_smpte_291m_anc_encode(Smpte436mCodedAnc* out,
                                 uint16_t line_number,
                                 Smpte436mWrappingType wrapping_type,
                                 Smpte436mPayloadSampleCoding sample_coding,
                                 const Smpte291mAnc* payload,
                                 void* log_ctx)
{
    out->line_number = line_number;
    out->wrapping_type = wrapping_type;
    out->payload_sample_coding = sample_coding;

    int ret = avpriv_smpte_291m_anc_get_sample_count(payload, sample_coding, log_ctx);
    if (ret < 0)
        return ret;

    out->payload_sample_count = ret;

    ret = avpriv_smpte_436m_coded_anc_payload_size(sample_coding, out->payload_sample_count);
    if (ret < 0)
        return ret;

    out->payload_array_length = ret;

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
            // fill trailing padding with zeros
            av_assert0(out->payload_array_length >= 4);
            memset(out->payload + out->payload_array_length - 4, 0, 4);

            out->payload[0] = payload->did;
            out->payload[1] = payload->sdid_or_dbn;
            out->payload[2] = payload->data_count;

            memcpy(out->payload + 3, payload->payload, payload->data_count);
            return 0;
        }
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF:
        case SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF:
            av_log(log_ctx,
                   AV_LOG_ERROR,
                   "encoding an ANC packet using the 10-bit SMPTE 436M sample coding isn't implemented.\n");
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
        unsigned cc_count = bytestream2_get_byte(&gb) & 0x1F;
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
