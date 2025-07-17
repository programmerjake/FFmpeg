/*
 * MCC subtitle demuxer
 * Copyright (c) 2025 Jacob Lifshay
 * Copyright (c) 2017 Paul B Mahol
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

#include "avformat.h"
#include "internal.h"
#include "mux.h"

#include "libavcodec/codec_id.h"
#include "libavcodec/vanc_smpte_436m.h"

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/rational.h"
#include "libavutil/timecode.h"

typedef struct MCCContext
{
    const AVClass* class;
    AVTimecode                       timecode;
    int64_t                          twenty_four_hr;
    char*                            override_time_code_rate;
    int                              use_u_alias;
} MCCContext;

static const char mcc_header_v1[] =
    "File Format=MacCaption_MCC V1.0\n"
    "\n"
    "///////////////////////////////////////////////////////////////////////////////////\n"
    "// Computer Prompting and Captioning Company\n"
    "// Ancillary Data Packet Transfer File\n"
    "//\n"
    "// Permission to generate this format is granted provided that\n"
    "//   1. This ANC Transfer file format is used on an as-is basis and no warranty is given, and\n"
    "//   2. This entire descriptive information text is included in a generated .mcc file.\n"
    "//\n"
    "// General file format:\n"
    "//   HH:MM:SS:FF(tab)[Hexadecimal ANC data in groups of 2 characters]\n"
    "//     Hexadecimal data starts with the Ancillary Data Packet DID (Data ID defined in S291M)\n"
    "//       and concludes with the Check Sum following the User Data Words.\n"
    "//     Each time code line must contain at most one complete ancillary data packet.\n"
    "//     To transfer additional ANC Data successive lines may contain identical time code.\n"
    "//     Time Code Rate=[24, 25, 30, 30DF, 50, 60]\n"
    "//\n"
    "//   ANC data bytes may be represented by one ASCII character according to the following schema:\n"
    "//     G  FAh 00h 00h\n"
    "//     H  2 x (FAh 00h 00h)\n"
    "//     I  3 x (FAh 00h 00h)\n"
    "//     J  4 x (FAh 00h 00h)\n"
    "//     K  5 x (FAh 00h 00h)\n"
    "//     L  6 x (FAh 00h 00h)\n"
    "//     M  7 x (FAh 00h 00h)\n"
    "//     N  8 x (FAh 00h 00h)\n"
    "//     O  9 x (FAh 00h 00h)\n"
    "//     P  FBh 80h 80h\n"
    "//     Q  FCh 80h 80h\n"
    "//     R  FDh 80h 80h\n"
    "//     S  96h 69h\n"
    "//     T  61h 01h\n"
    "//     U  E1h 00h 00h 00h\n"
    "//     Z  00h\n"
    "//\n"
    "///////////////////////////////////////////////////////////////////////////////////\n";

static const char mcc_header_v2[] =
    "File Format=MacCaption_MCC V2.0\n"
    "\n"
    "///////////////////////////////////////////////////////////////////////////////////\n"
    "// Computer Prompting and Captioning Company\n"
    "// Ancillary Data Packet Transfer File\n"
    "//\n"
    "// Permission to generate this format is granted provided that\n"
    "//   1. This ANC Transfer file format is used on an as-is basis and no warranty is given, and\n"
    "//   2. This entire descriptive information text is included in a generated .mcc file.\n"
    "//\n"
    "// General file format:\n"
    "//   HH:MM:SS:FF(tab)[Hexadecimal ANC data in groups of 2 characters]\n"
    "//     Hexadecimal data starts with the Ancillary Data Packet DID (Data ID defined in S291M)\n"
    "//       and concludes with the Check Sum following the User Data Words.\n"
    "//     Each time code line must contain at most one complete ancillary data packet.\n"
    "//     To transfer additional ANC Data successive lines may contain identical time code.\n"
    "//     Time Code Rate=[24, 25, 30, 30DF, 50, 60, 60DF]\n"
    "//     Time Code Rate=[24, 25, 30, 30DF, 50, 60]\n"
    "//\n"
    "//   ANC data bytes may be represented by one ASCII character according to the following schema:\n"
    "//     G  FAh 00h 00h\n"
    "//     H  2 x (FAh 00h 00h)\n"
    "//     I  3 x (FAh 00h 00h)\n"
    "//     J  4 x (FAh 00h 00h)\n"
    "//     K  5 x (FAh 00h 00h)\n"
    "//     L  6 x (FAh 00h 00h)\n"
    "//     M  7 x (FAh 00h 00h)\n"
    "//     N  8 x (FAh 00h 00h)\n"
    "//     O  9 x (FAh 00h 00h)\n"
    "//     P  FBh 80h 80h\n"
    "//     Q  FCh 80h 80h\n"
    "//     R  FDh 80h 80h\n"
    "//     S  96h 69h\n"
    "//     T  61h 01h\n"
    "//     U  E1h 00h 00h 00h\n"
    "//     Z  00h\n"
    "//\n"
    "///////////////////////////////////////////////////////////////////////////////////\n";

static AVRational valid_time_code_rates[] = {
    {.num = 24, .den = 1},
    {.num = 25, .den = 1},
    {.num = 30000, .den = 1001},
    {.num = 30, .den = 1},
    {.num = 50, .den = 1},
    {.num = 60000, .den = 1001},
    {.num = 60, .den = 1},
};

static int mcc_write_header(AVFormatContext* avf)
{
    MCCContext* mcc = avf->priv_data;
    avio_printf(avf->pb,
                "%s\n",
                mcc->timecode.fps == 60 && (mcc->timecode.flags & AV_TIMECODE_FLAG_DROPFRAME) ? mcc_header_v2
                                                                                              : mcc_header_v1);
    avio_printf(avf->pb,
                "Time Code Rate=%u%s\n\n",
                mcc->timecode.fps,
                mcc->timecode.flags & AV_TIMECODE_FLAG_DROPFRAME ? "DF" : "");

    return 0;
}

/// convert the input bytes to hexadecimal with mcc's aliases
static void mcc_bytes_to_hex(char* dest, const uint8_t* bytes, size_t bytes_size, int use_u_alias)
{
    while (bytes_size != 0) {
        switch (bytes[0]) {
            case 0xFA:
                *dest = '\0';
                for (unsigned char code = 'G'; code <= (unsigned char)'O'; code++) {
                    if (bytes_size < 3)
                        break;
                    if (bytes[0] != 0xFA || bytes[1] != 0 || bytes[2] != 0)
                        break;
                    *dest = code;
                    bytes += 3;
                    bytes_size -= 3;
                }
                if (*dest) {
                    dest++;
                    continue;
                }
                break;
            case 0xFB:
            case 0xFC:
            case 0xFD:
                if (bytes_size >= 3 && bytes[1] == 0x80 && bytes[2] == 0x80) {
                    *dest++ = bytes[0] - 0xFB + 'P';
                    bytes += 3;
                    bytes_size -= 3;
                    continue;
                }
                break;
            case 0x96:
                if (bytes_size >= 2 && bytes[1] == 0x69) {
                    *dest++ = 'S';
                    bytes += 2;
                    bytes_size -= 2;
                    continue;
                }
                break;
            case 0x61:
                if (bytes_size >= 2 && bytes[1] == 0x01) {
                    *dest++ = 'T';
                    bytes += 2;
                    bytes_size -= 2;
                    continue;
                }
                break;
            case 0xE1:
                if (use_u_alias && bytes_size >= 4 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) {
                    *dest++ = 'U';
                    bytes += 4;
                    bytes_size -= 4;
                    continue;
                }
                break;
            case 0:
                *dest++ = 'Z';
                bytes++;
                bytes_size--;
                continue;
            default:
                // any other bytes falls through to writing hex
                break;
        }
        for (int shift = 4; shift >= 0; shift -= 4) {
            int v = (bytes[0] >> shift) & 0xF;
            if (v < 0xA)
                *dest++ = v + '0';
            else
                *dest++ = v - 0xA + 'A';
        }
        bytes++;
        bytes_size--;
    }
    *dest = '\0';
}

static int mcc_write_packet(AVFormatContext* avf, AVPacket* pkt)
{
    MCCContext* mcc = avf->priv_data;
    int64_t     pts = pkt->pts;
    int         ret;

    if (pts == AV_NOPTS_VALUE) {
        av_log(avf, AV_LOG_WARNING, "Insufficient timestamps.\n");
        return 0;
    }

    char timecode_str[AV_TIMECODE_STR_SIZE];

    // wrap pts values at 24hr ourselves since they can be bigger than fits in an int
    av_timecode_make_string(&mcc->timecode, timecode_str, pts % mcc->twenty_four_hr);

    for (char* p = timecode_str; *p; p++) {
        // .mcc doesn't use ; for drop-frame time codes
        if (*p == ';')
            *p = ':';
    }

    VancSmpte436mIterator iter;
    ret = avpriv_vanc_smpte_436m_iter_init(&iter, pkt->data, pkt->size);
    if (ret < 0)
        return ret;
    Smpte436mCodedAnc coded_anc;
    while ((ret = avpriv_vanc_smpte_436m_iter_next(&iter, &coded_anc)) >= 0) {
        Smpte291mAnc anc;
        ret = avpriv_smpte_291m_decode_anc(
            &anc, coded_anc.payload_sample_coding, coded_anc.payload_sample_count, coded_anc.payload, avf);
        if (ret < 0)
            return ret;
        // 4 for did, sdid_or_dbn, data_count, and checksum fields.
        uint8_t mcc_anc[4 + SMPTE_291M_ANC_PAYLOAD_CAPACITY];
        size_t  mcc_anc_len = 0;

        mcc_anc[mcc_anc_len++] = anc.did;
        mcc_anc[mcc_anc_len++] = anc.sdid_or_dbn;
        mcc_anc[mcc_anc_len++] = anc.data_count;
        memcpy(mcc_anc + mcc_anc_len, anc.payload, anc.data_count);
        mcc_anc_len += anc.data_count;
        mcc_anc[mcc_anc_len++] = anc.checksum;

        // 1 for terminating nul. 2 since there's 2 hex digits per byte.
        char hex[1 + 2 * sizeof(mcc_anc)];
        mcc_bytes_to_hex(hex, mcc_anc, mcc_anc_len, mcc->use_u_alias);
        avio_print(avf->pb, timecode_str, "\t", hex, "\n");
    }
    if (ret != AVERROR_EOF)
        return ret;
    return 0;
}

static int mcc_init(AVFormatContext* avf)
{
    MCCContext* mcc = avf->priv_data;
    int         ret;

    if (avf->nb_streams != 1) {
        av_log(avf, AV_LOG_ERROR, "mcc muxer supports at most one stream\n");
        return AVERROR(EINVAL);
    }

    AVStream*  st             = avf->streams[0];
    AVRational time_code_rate = st->avg_frame_rate;
    int        timecode_flags = 0;
    AVTimecode twenty_four_hr;

    if (mcc->override_time_code_rate && (ret = av_parse_video_rate(&time_code_rate, mcc->override_time_code_rate)) < 0)
        return ret;

    ret = AVERROR(EINVAL);

    for (size_t i = 0; i < FF_ARRAY_ELEMS(valid_time_code_rates); i++) {
        if (time_code_rate.num == valid_time_code_rates[i].num && time_code_rate.den == valid_time_code_rates[i].den) {
            ret = 0;
            break;
        }
    }

    if (ret != 0) {
        av_log(
            avf, AV_LOG_FATAL, "time code rate not supported by mcc: %d/%d\n", time_code_rate.num, time_code_rate.den);
        return AVERROR(EINVAL);
    }

    if (time_code_rate.den == 1001 && time_code_rate.num % 30000 == 0) {
        timecode_flags |= AV_TIMECODE_FLAG_DROPFRAME;
    }

    ret = av_timecode_init(&mcc->timecode, time_code_rate, timecode_flags, 0, avf);
    if (ret < 0)
        return ret;

    // get av_timecode to calculate how many frames are in 24hr
    ret = av_timecode_init_from_components(&twenty_four_hr, time_code_rate, timecode_flags, 24, 0, 0, 0, avf);
    if (ret < 0)
        return ret;

    mcc->twenty_four_hr = twenty_four_hr.start;

    if (st->codecpar->codec_id == AV_CODEC_ID_EIA_608) {
        char args[64];
        snprintf(args, sizeof(args), "cdp_frame_rate=%d/%d", time_code_rate.num, time_code_rate.den);
        ret = ff_stream_add_bitstream_filter(st, "eia608_to_smpte436m", args);
        if (ret < 0)
            return ret;
    } else if (st->codecpar->codec_id != AV_CODEC_ID_VANC_SMPTE_436M) {
        av_log(avf,
               AV_LOG_ERROR,
               "mcc muxer supports only codec %s or codec %s\n",
               avcodec_get_name(AV_CODEC_ID_VANC_SMPTE_436M),
               avcodec_get_name(AV_CODEC_ID_EIA_608));
        return AVERROR(EINVAL);
    }

    avpriv_set_pts_info(st, 64, time_code_rate.den, time_code_rate.num);
    return 0;
}

static int mcc_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    (void)std_compliance;
    if (codec_id == AV_CODEC_ID_EIA_608 || codec_id == AV_CODEC_ID_VANC_SMPTE_436M)
        return 1;
    return 0;
}

#define OFFSET(x) offsetof(MCCContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
// clang-format off
static const AVOption options[] = {
    { "override_time_code_rate", "override the `Time Code Rate` value in the output", OFFSET(override_time_code_rate), AV_OPT_TYPE_STRING, { .str = NULL }, 0, INT_MAX, ENC },
    { "use_u_alias", "use the U alias for E1h 00h 00h 00h, disabled by default because some .mcc files disagree on whether it has 2 or 3 zero bytes", OFFSET(use_u_alias), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { NULL },
};
// clang-format on

static const AVClass mcc_muxer_class = {
    .class_name = "mcc muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_mcc_muxer = {
    .p.name           = "mcc",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MacCaption"),
    .p.extensions     = "mcc",
    .p.flags          = AVFMT_GLOBALHEADER,
    .p.video_codec    = AV_CODEC_ID_NONE,
    .p.audio_codec    = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_EIA_608,
    .p.priv_class     = &mcc_muxer_class,
    .priv_data_size   = sizeof(MCCContext),
    .init             = mcc_init,
    .query_codec      = mcc_query_codec,
    .write_header     = mcc_write_header,
    .write_packet     = mcc_write_packet,
};
