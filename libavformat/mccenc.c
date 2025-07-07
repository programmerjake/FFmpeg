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

#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/rational.h"
#include "libavutil/timecode.h"

#include "libklvanc/vanc-eia_708b.h"

#include <stdint.h>
#include <string.h>

typedef struct MCCContext
{
    const AVClass* class;
    AVTimecode                       timecode;
    int64_t                          twenty_four_hr;
    struct klvanc_packet_eia_708b_s* eia708_cdp;
    unsigned                         cdp_sequence_cntr;
    char*                            override_cdp_frame_rate;
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
    MCCContext* mcc            = avf->priv_data;
    AVRational  cdp_frame_rate = avf->streams[0]->avg_frame_rate;
    AVRational  time_code_rate = avf->streams[0]->avg_frame_rate;
    int         timecode_flags = 0;
    int         ret;
    AVTimecode  twenty_four_hr;

    if (klvanc_create_eia708_cdp(&mcc->eia708_cdp) < 0)
        return AVERROR(ENOMEM);

    if (mcc->override_cdp_frame_rate && (ret = av_parse_video_rate(&cdp_frame_rate, mcc->override_cdp_frame_rate)) < 0)
        goto error;

    if (mcc->override_time_code_rate && (ret = av_parse_video_rate(&time_code_rate, mcc->override_time_code_rate)) < 0)
        goto error;

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
        ret = AVERROR(EINVAL);
        goto error;
    }

    // set values that all our vanc packets have in common:

    // num/denom intentionally swapped since casparcg and ffmpeg uses the reciprocal of libklvanc
    if (klvanc_set_framerate_EIA_708B(mcc->eia708_cdp, cdp_frame_rate.den, cdp_frame_rate.num) < 0) {
        av_log(avf,
               AV_LOG_FATAL,
               "cdp_frame_rate not supported by libklvanc: %d/%d\n",
               cdp_frame_rate.num,
               cdp_frame_rate.den);
        ret = AVERROR(EINVAL);
        goto error;
    }

    mcc->eia708_cdp->header.ccdata_present         = 1;
    mcc->eia708_cdp->header.caption_service_active = 1;

    if (time_code_rate.den == 1001 && time_code_rate.num % 30000 == 0) {
        timecode_flags |= AV_TIMECODE_FLAG_DROPFRAME;
    }

    ret = av_timecode_init(&mcc->timecode, time_code_rate, timecode_flags, 0, avf);
    if (ret < 0)
        goto error;

    // get av_timecode to calculate how many frames are in 24hr
    ret = av_timecode_init_from_components(&twenty_four_hr, time_code_rate, timecode_flags, 24, 0, 0, 0, avf);
    if (ret < 0)
        goto error;

    mcc->twenty_four_hr = twenty_four_hr.start;

    avpriv_set_pts_info(avf->streams[0], 64, time_code_rate.den, time_code_rate.num);
    avio_printf(
        avf->pb, "%s\n", time_code_rate.num == 60000 && time_code_rate.den == 1001 ? mcc_header_v2 : mcc_header_v1);
    avio_printf(
        avf->pb, "Time Code Rate=%u%s\n\n", mcc->timecode.fps, timecode_flags & AV_TIMECODE_FLAG_DROPFRAME ? "DF" : "");

    return 0;

error:
    klvanc_destroy_eia708_cdp(mcc->eia708_cdp);
    mcc->eia708_cdp = NULL;
    return ret;
}

/// convert the least significant 8 bits of the input words to hexadecimal with mcc's aliases
static char* mcc_words_to_hex(const uint16_t* words, size_t words_size, int use_u_alias)
{
    // at most 2 characters for each input word, plus the terminating nul
    char* ret  = av_malloc(2 * words_size + 1);
    char* dest = ret;
    if (!ret)
        return NULL;

    while (words_size != 0) {
        switch (words[0] & 0xFF) {
            case 0xFA:
                *dest = '\0';
                for (unsigned char code = 'G'; code <= (unsigned char)'O'; code++) {
                    if (words_size < 3)
                        break;
                    if ((words[0] & 0xFF) != 0xFA || (words[1] & 0xFF) != 0 || (words[2] & 0xFF) != 0)
                        break;
                    *dest = code;
                    words += 3;
                    words_size -= 3;
                }
                if (*dest) {
                    dest++;
                    continue;
                }
                break;
            case 0xFB:
            case 0xFC:
            case 0xFD:
                if (words_size >= 3 && (words[1] & 0xFF) == 0x80 && (words[2] & 0xFF) == 0x80) {
                    *dest++ = (words[0] & 0xFF) - 0xFB + 'P';
                    words += 3;
                    words_size -= 3;
                    continue;
                }
                break;
            case 0x96:
                if (words_size >= 2 && (words[1] & 0xFF) == 0x69) {
                    *dest++ = 'S';
                    words += 2;
                    words_size -= 2;
                    continue;
                }
                break;
            case 0x61:
                if (words_size >= 2 && (words[1] & 0xFF) == 0x01) {
                    *dest++ = 'T';
                    words += 2;
                    words_size -= 2;
                    continue;
                }
                break;
            case 0xE1:
                if (use_u_alias && words_size >= 4 && (words[1] & 0xFF) == 0 && (words[2] & 0xFF) == 0 &&
                    (words[3] & 0xFF) == 0) {
                    *dest++ = 'U';
                    words += 4;
                    words_size -= 4;
                    continue;
                }
                break;
            case 0:
                *dest++ = 'Z';
                words++;
                words_size--;
                continue;
            default:
                // any other bytes falls through to writing hex
                break;
        }
        for (int shift = 4; shift >= 0; shift -= 4) {
            int v = (words[0] >> shift) & 0xF;
            if (v < 0xA)
                *dest++ = v + '0';
            else
                *dest++ = v - 0xA + 'A';
        }
        words++;
        words_size--;
    }
    *dest = '\0';
    return ret;
}

static char* mcc_cc_data_to_hex(AVFormatContext* avf, const uint8_t* cc_data, size_t cc_data_size)
{
    MCCContext*                      mcc        = avf->priv_data;
    struct klvanc_packet_eia_708b_s* eia708_cdp = mcc->eia708_cdp;
    size_t                           cc_count   = cc_data_size / 3;
    uint16_t*                        words      = NULL;
    uint16_t                         words_size = 0;
    char*                            ret;

    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(avf,
               AV_LOG_ERROR,
               "cc_count (%zu) is bigger than the maximum supported by libklvanc (%zu), truncating captions packet\n",
               cc_count,
               (size_t)KLVANC_MAX_CC_COUNT);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    eia708_cdp->ccdata.cc_count = (uint8_t)cc_count;
    for (size_t i = 0; i < cc_count; i++) {
        size_t start = i * 3;

        eia708_cdp->ccdata.cc[i].cc_valid   = (cc_data[start] & 0x04) != 0;
        eia708_cdp->ccdata.cc[i].cc_type    = cc_data[start] & 0x03;
        eia708_cdp->ccdata.cc[i].cc_data[0] = cc_data[start + 1];
        eia708_cdp->ccdata.cc[i].cc_data[1] = cc_data[start + 2];
    }

    klvanc_finalize_EIA_708B(eia708_cdp, mcc->cdp_sequence_cntr++);
    // cdp_sequence_cntr wraps around at 16-bits
    mcc->cdp_sequence_cntr &= 0xFFFFU;

    if (klvanc_convert_EIA_708B_to_words(eia708_cdp, &words, &words_size) < 0)
        return NULL;

    // first 3 words are always 0x000 0x3ff 0x3ff and are skipped in the mcc format
    av_assert0(words_size >= 3);
    ret = mcc_words_to_hex(words + 3, words_size - 3, mcc->use_u_alias);
    free(words);
    return ret;
}

static int mcc_write_packet(AVFormatContext* avf, AVPacket* pkt)
{
    MCCContext* mcc = avf->priv_data;
    int64_t     pts = pkt->pts;
    char        timecode_str[AV_TIMECODE_STR_SIZE];
    char*       hex;

    if (pts == AV_NOPTS_VALUE) {
        av_log(avf, AV_LOG_WARNING, "Insufficient timestamps.\n");
        return 0;
    }

    // wrap pts values at 24hr ourselves since they can be bigger than fits in an int
    av_timecode_make_string(&mcc->timecode, timecode_str, pts % mcc->twenty_four_hr);

    for (char* p = timecode_str; *p; p++) {
        // .mcc doesn't use ; for drop-frame time codes
        if (*p == ';')
            *p = ':';
    }

    hex = mcc_cc_data_to_hex(avf, pkt->data, pkt->size);
    if (!hex)
        return AVERROR(ENOMEM);

    avio_printf(avf->pb, "%s\t%s\n", timecode_str, hex);
    av_freep(&hex);
    return 0;
}

static void mcc_deinit(AVFormatContext* avf)
{
    MCCContext* mcc = avf->priv_data;
    klvanc_destroy_eia708_cdp(mcc->eia708_cdp);
    mcc->eia708_cdp = NULL;
}

#define OFFSET(x) offsetof(MCCContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
// clang-format off
static const AVOption options[] = {
    { "initial_cdp_sequence_cntr", "initial cdp_*_sequence_cntr value", OFFSET(cdp_sequence_cntr), AV_OPT_TYPE_UINT, { .i64 = 0 }, 0, 0xFFFF, ENC },
    { "override_cdp_frame_rate", "override only the `cdp_frame_rate` fields, defaults to the packet frame rate", OFFSET(override_cdp_frame_rate), AV_OPT_TYPE_STRING, { .str = NULL }, 0, INT_MAX, ENC },
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
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH | FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
    .priv_data_size   = sizeof(MCCContext),
    .deinit           = mcc_deinit,
    .write_header     = mcc_write_header,
    .write_packet     = mcc_write_packet,
};
