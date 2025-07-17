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

#ifndef AVCODEC_VANC_SMPTE_436M_HEADER_H
#define AVCODEC_VANC_SMPTE_436M_HEADER_H

#include "bytestream.h"

#include <stdint.h>

/**
 * Iterator over the ANC packets in a single AV_CODEC_ID_VANC_SMPTE_436M AVPacket's data
 */
typedef struct VancSmpte436mIterator
{
    uint16_t       anc_packets_left;
    GetByteContext gb;
} VancSmpte436mIterator;

/**
 * Wrapping Type from Table 7 (page 13) of:
 * https://pub.smpte.org/latest/st436/s436m-2006.pdf
 */
typedef enum Smpte436mWrappingType
{
    SMPTE_436M_WRAPPING_TYPE_VANC_FRAME             = 1,
    SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_1           = 2,
    SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_2           = 3,
    SMPTE_436M_WRAPPING_TYPE_VANC_PROGRESSIVE_FRAME = 4,
    SMPTE_436M_WRAPPING_TYPE_HANC_FRAME             = 0x11,
    SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_1           = 0x12,
    SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_2           = 0x13,
    SMPTE_436M_WRAPPING_TYPE_HANC_PROGRESSIVE_FRAME = 0x14,
    /** not a real wrapping type, just here to guarantee the enum is big enough */
    SMPTE_436M_WRAPPING_TYPE_MAX = 0xFF,
} Smpte436mWrappingType;

// clang-format off
#define SMPTE_436M_WRAPPING_TYPE_VANC_AVOPTIONS(flags, unit_name) \
{ "vanc_frame", "VANC frame (interlaced or segmented progressive frame)", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_WRAPPING_TYPE_VANC_FRAME}, 0, 0xFF, flags, .unit = unit_name }, \
{ "vanc_field_1", "VANC field 1", 0, AV_OPT_TYPE_CONST, {.i64 = SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_1}, 0, 0xFF, flags, .unit = unit_name }, \
{ "vanc_field_2", "VANC field 2", 0, AV_OPT_TYPE_CONST, {.i64 = SMPTE_436M_WRAPPING_TYPE_VANC_FIELD_2}, 0, 0xFF, flags, .unit = unit_name }, \
{ "vanc_progressive_frame", "VANC progressive frame", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_WRAPPING_TYPE_VANC_PROGRESSIVE_FRAME}, 0, 0xFF, flags, .unit = unit_name }

#define SMPTE_436M_WRAPPING_TYPE_HANC_AVOPTIONS(flags, unit_name) \
{ "hanc_frame", "HANC frame (interlaced or segmented progressive frame)", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_WRAPPING_TYPE_HANC_FRAME}, 0, 0xFF, flags, .unit = unit_name }, \
{ "hanc_field_1", "HANC field 1", 0, AV_OPT_TYPE_CONST, {.i64 = SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_1}, 0, 0xFF, flags, .unit = unit_name }, \
{ "hanc_field_2", "HANC field 2", 0, AV_OPT_TYPE_CONST, {.i64 = SMPTE_436M_WRAPPING_TYPE_HANC_FIELD_2}, 0, 0xFF, flags, .unit = unit_name }, \
{ "hanc_progressive_frame", "HANC progressive frame", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_WRAPPING_TYPE_HANC_PROGRESSIVE_FRAME}, 0, 0xFF, flags, .unit = unit_name }

#define SMPTE_436M_WRAPPING_TYPE_AVOPTIONS(flags, unit_name)   \
    SMPTE_436M_WRAPPING_TYPE_VANC_AVOPTIONS(flags, unit_name), \
    SMPTE_436M_WRAPPING_TYPE_HANC_AVOPTIONS(flags, unit_name)
// clang-format on

/**
 * Payload Sample Coding from Table 4 (page 10) and Table 7 (page 13) of:
 * https://pub.smpte.org/latest/st436/s436m-2006.pdf
 */
typedef enum Smpte436mPayloadSampleCoding
{
    /** only used for VBI */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA = 1,
    /** only used for VBI */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_COLOR_DIFF = 2,
    /** only used for VBI */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA_AND_COLOR_DIFF = 3,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA = 4,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF = 5,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF = 6,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA = 7,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF = 8,
    /** used for VBI and ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF = 9,
    /** only used for ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_WITH_PARITY_ERROR = 10,
    /** only used for ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF_WITH_PARITY_ERROR = 11,
    /** only used for ANC */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF_WITH_PARITY_ERROR = 12,
    /** not a real sample coding, just here to guarantee the enum is big enough */
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_MAX = 0xFF,
} Smpte436mPayloadSampleCoding;

// clang-format off
#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_VBI_AVOPTIONS(flags, unit_name) \
{ "1bit_luma", "1-bit component luma samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA}, 0, 0xFF, flags, .unit = unit_name }, \
{ "1bit_color_diff", "1-bit component color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }, \
{ "1bit_luma_and_color_diff", "1-bit component luma and color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_1BIT_LUMA_AND_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }

#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_SHARED_AVOPTIONS(flags, unit_name) \
{ "8bit_luma", "8-bit component luma samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA}, 0, 0xFF, flags, .unit = unit_name }, \
{ "8bit_color_diff", "8-bit component color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }, \
{ "8bit_luma_and_color_diff", "8-bit component luma and color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }, \
{ "10bit_luma", "10-bit component luma samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA}, 0, 0xFF, flags, .unit = unit_name }, \
{ "10bit_color_diff", "10-bit component color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }, \
{ "10bit_luma_and_color_diff", "10-bit component luma and color difference samples", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_10BIT_LUMA_AND_COLOR_DIFF}, 0, 0xFF, flags, .unit = unit_name }

#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_ANC_AVOPTIONS(flags, unit_name) \
{ "8bit_luma_parity_error", "8-bit component luma samples with parity error", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_WITH_PARITY_ERROR}, 0, 0xFF, flags, .unit = unit_name }, \
{ "8bit_color_diff_parity_error", "8-bit component color difference samples with parity error", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_COLOR_DIFF_WITH_PARITY_ERROR}, 0, 0xFF, flags, .unit = unit_name }, \
{ "8bit_luma_and_color_diff_parity_error", "8-bit component luma and color difference samples with parity error", 0, AV_OPT_TYPE_CONST, \
    {.i64 = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA_AND_COLOR_DIFF_WITH_PARITY_ERROR}, 0, 0xFF, flags, .unit = unit_name }

#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_VBI_AVOPTIONS(flags, unit_name)      \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_VBI_AVOPTIONS(flags, unit_name), \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_SHARED_AVOPTIONS(flags, unit_name)

#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_ANC_AVOPTIONS(flags, unit_name)    \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_SHARED_AVOPTIONS(flags, unit_name),    \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_ANC_AVOPTIONS(flags, unit_name)

#define SMPTE_436M_PAYLOAD_SAMPLE_CODING_AVOPTIONS(flags, unit_name)          \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_VBI_AVOPTIONS(flags, unit_name), \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_SHARED_AVOPTIONS(flags, unit_name),      \
    SMPTE_436M_PAYLOAD_SAMPLE_CODING_ONLY_IN_ANC_AVOPTIONS(flags, unit_name)
// clang-format on

#define SMPTE_291M_ANC_PAYLOAD_CAPACITY 0xFF

/**
 * An ANC packet as decoded from the payload of Smpte436mAncCoded
 */
typedef struct Smpte291mAnc
{
    uint8_t did;
    uint8_t sdid_or_dbn;
    uint8_t data_count;
    uint8_t payload[SMPTE_291M_ANC_PAYLOAD_CAPACITY];
    uint8_t checksum;
} Smpte291mAnc;

/** max number of samples that can be stored in the payload of Smpte436mCodedAnc */
#define SMPTE_436M_CODED_ANC_SAMPLE_CAPACITY (SMPTE_291M_ANC_PAYLOAD_CAPACITY + 3)
/** max number of bytes that can be stored in the payload of Smpte436mCodedAnc */
#define SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY (((SMPTE_436M_CODED_ANC_SAMPLE_CAPACITY + 2) / 3) * 4)

/**
 * An encoded ANC packet within a single AV_CODEC_ID_VANC_SMPTE_436M AVPacket's data.
 * The repeated section of Table 7 (page 13) of:
 * https://pub.smpte.org/latest/st436/s436m-2006.pdf
 */
typedef struct Smpte436mCodedAnc
{
    uint16_t                     line_number;
    Smpte436mWrappingType        wrapping_type;
    Smpte436mPayloadSampleCoding payload_sample_coding;
    uint16_t                     payload_sample_count;
    uint32_t                     payload_array_length;
    /** the payload, has size payload_array_length.
     * can be decoded into Smpte291mAnc
     */
    uint8_t payload[SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY];
} Smpte436mCodedAnc;

/**
 * Encode ANC packets into a single AV_CODEC_ID_VANC_SMPTE_436M AVPacket's data.
 * @param[in]  anc_packet_count number of ANC packets to encode
 * @param[in]  anc_packets      the ANC packets to encode
 * @param[in]  size             the size of out. ignored if out is NULL.
 * @param[out] out              Output bytes. Doesn't write anything if out is NULL.
 * @return the number of bytes written on success, AVERROR codes otherwise.
 *         If out is NULL, returns the number of bytes it would have written.
 */
int avpriv_vanc_smpte_436m_encode(uint8_t* out, int size, int anc_packet_count, const Smpte436mCodedAnc* anc_packets);

/**
 * Set up iteration over the ANC packets in a single AV_CODEC_ID_VANC_SMPTE_436M AVPacket's data.
 * @param[in]  buf      Pointer to the data from a AV_CODEC_ID_VANC_SMPTE_436M AVPacket.
 * @param[in]  buf_size Size of the data from a AV_CODEC_ID_VANC_SMPTE_436M AVPacket.
 * @param[out] iter     Pointer to the iterator.
 * @return 0 on success, AVERROR codes otherwise.
 */
int avpriv_vanc_smpte_436m_iter_init(VancSmpte436mIterator* iter, const uint8_t* buf, int buf_size);

/**
 * Get the next ANC packet from the iterator, advancing the iterator.
 * @param[in,out] iter Pointer to the iterator.
 * @param[out]    anc  The returned ANC packet.
 * @return 0 on success, AVERROR_EOF when the iterator has reached the end, AVERROR codes otherwise.
 */
int avpriv_vanc_smpte_436m_iter_next(VancSmpte436mIterator* iter, Smpte436mCodedAnc* anc);

/**
 * Get the minimum number of bytes needed to store a Smpte436mCodedAnc payload.
 * @param sample_coding the payload sample coding
 * @param sample_count  the number of samples stored in the payload
 * @return returns the minimum number of bytes needed, on error returns < 0.
 *         always <= SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY
 */
int avpriv_smpte_436m_coded_anc_payload_size(Smpte436mPayloadSampleCoding sample_coding, uint16_t sample_count);

/**
 * Decode a Smpte436mCodedAnc payload into Smpte291mAnc
 * @param[in]  sample_coding the payload sample coding
 * @param[in]  sample_count  the number of samples stored in the payload
 * @param[in]  payload       the bytes storing the payload,
 *                           the needed size can be obtained from
                             avpriv_smpte_436m_coded_anc_payload_size
 * @param[in]  log_ctx       context pointer for av_log
 * @param[out] out           The decoded ANC packet.
 * @return returns 0 on success, otherwise < 0.
 */
int avpriv_smpte_291m_decode_anc(Smpte291mAnc*                out,
                                 Smpte436mPayloadSampleCoding sample_coding,
                                 uint16_t                     sample_count,
                                 const uint8_t*               payload,
                                 void*                        log_ctx);

/**
 * Fill in the correct checksum for a Smpte291mAnc
 * @param[in,out] anc The ANC packet.
 */
void avpriv_smpte_291m_anc_fill_checksum(Smpte291mAnc* anc);

/**
 * Compute the sample count needed to encode a Smpte291mAnc into a Smpte436mCodedAnc payload
 * @param[in] anc           The ANC packet.
 * @param[in] sample_coding The sample coding.
 * @param[in] log_ctx       context pointer for av_log
 * @return returns the sample count on success, otherwise < 0.
 */
int avpriv_smpte_291m_anc_get_sample_count(const Smpte291mAnc*          anc,
                                           Smpte436mPayloadSampleCoding sample_coding,
                                           void*                        log_ctx);

/**
 * Encode a Smpte291mAnc into a Smpte436mCodedAnc
 * @param[in]  line_number   the line number the ANC packet is on
 * @param[in]  wrapping_type the wrapping type
 * @param[in]  sample_coding the payload sample coding
 * @param[in]  payload       the ANC packet to encode.
 * @param[in]  log_ctx       context pointer for av_log
 * @param[out] out           The encoded ANC packet.
 * @return returns 0 on success, otherwise < 0.
 */
int avpriv_smpte_291m_encode_anc(Smpte436mCodedAnc*           out,
                                 uint16_t                     line_number,
                                 Smpte436mWrappingType        wrapping_type,
                                 Smpte436mPayloadSampleCoding sample_coding,
                                 const Smpte291mAnc*          payload,
                                 void*                        log_ctx);

/**
 * Try to decode an ANC packet into EIA-608/CTA-708 data (AV_CODEC_ID_EIA_608).
 * @param[in]  anc     The ANC packet.
 * @param[in]  log_ctx context pointer for av_log
 * @param[out] cc_data the buffer to store the extracted EIA-608/CTA-708 data,
 *                     you can pass NULL to not store the data.
 *                     the required size is 3 * cc_count bytes.
 *                     SMPTE_291M_ANC_PAYLOAD_CAPACITY is always enough size.
 * @return returns cc_count (>= 0) on success, AVERROR(EAGAIN) if it wasn't a CTA-708 ANC packet, < 0 on error.
 */
int avpriv_smpte_291m_anc_extract_cta_708(const Smpte291mAnc* anc, uint8_t* cc_data, void* log_ctx);

#endif /* AVCODEC_VANC_SMPTE_436M_HEADER_H */
