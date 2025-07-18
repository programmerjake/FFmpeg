/*
 * MCC subtitle demuxer
 * Copyright (c) 2020 Paul B Mahol
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

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/codec_id.h"
#include "libavcodec/vanc_smpte_436m.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
#include "libavutil/timecode.h"
#include "subtitles.h"
#include <stdbool.h>

typedef struct MCCContext {
    const AVClass* class;
    int                   eia608_extract;
    unsigned              vanc_line;
    FFDemuxSubtitlesQueue q;
} MCCContext;

static int mcc_probe(const AVProbeData *p)
{
    char buf[28];
    FFTextReader tr;

    ff_text_init_buf(&tr, p->buf, p->buf_size);

    while (ff_text_peek_r8(&tr) == '\r' || ff_text_peek_r8(&tr) == '\n')
        ff_text_r8(&tr);

    ff_text_read(&tr, buf, sizeof(buf));

    if (!memcmp(buf, "File Format=MacCaption_MCC V", 28))
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int convert(uint8_t x)
{
    if (x >= 'a')
        x -= 87;
    else if (x >= 'A')
        x -= 55;
    else
        x -= '0';
    return x;
}

typedef struct alias {
    uint8_t key;
    int len;
    const char *value;
} alias;

static const alias aliases[20] = {
    { .key = 16, .len =  3, .value = "\xFA\x0\x0", },
    { .key = 17, .len =  6, .value = "\xFA\x0\x0\xFA\x0\x0", },
    { .key = 18, .len =  9, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 19, .len = 12, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 20, .len = 15, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 21, .len = 18, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 22, .len = 21, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 23, .len = 24, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 24, .len = 27, .value = "\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0\xFA\x0\x0", },
    { .key = 25, .len =  3, .value = "\xFB\x80\x80", },
    { .key = 26, .len =  3, .value = "\xFC\x80\x80", },
    { .key = 27, .len =  3, .value = "\xFD\x80\x80", },
    { .key = 28, .len =  2, .value = "\x96\x69", },
    { .key = 29, .len =  2, .value = "\x61\x01", },
    { .key = 30, .len =  3, .value = "\xFC\x80\x80", },
    { .key = 31, .len =  3, .value = "\xFC\x80\x80", },
    { .key = 32, .len =  4, .value = "\xE1\x00\x00\x00", },
    { .key = 33, .len =  0, .value = NULL, },
    { .key = 34, .len =  0, .value = NULL, },
    { .key = 35, .len =  1, .value = "\x0", },
};

typedef struct TimeTracker
{
    int64_t    last_ts;
    int64_t    twenty_four_hr;
    AVTimecode timecode;
} TimeTracker;

static int time_tracker_init(TimeTracker* tt, AVStream* st, AVRational rate, bool is_drop_frame, void* log_ctx)
{
    *tt     = (TimeTracker){.last_ts = 0};
    int ret = av_timecode_init(&tt->timecode, rate, is_drop_frame ? AV_TIMECODE_FLAG_DROPFRAME : 0, 0, log_ctx);
    if (ret < 0)
        return ret;
    // wrap pts values at 24hr ourselves since they can be bigger than fits in an int
    AVTimecode twenty_four_hr;
    ret = av_timecode_init_from_components(&twenty_four_hr, rate, tt->timecode.flags, 24, 0, 0, 0, log_ctx);
    if (ret < 0)
        return ret;
    tt->twenty_four_hr = twenty_four_hr.start;
    // timecode uses reciprocal of timebase
    avpriv_set_pts_info(st, 64, rate.den, rate.num);
    return 0;
}

static int time_tracker_set_time(TimeTracker* tt, int hh, int mm, int ss, int ff, void* log_ctx)
{
    AVTimecode last = tt->timecode;
    int        ret  = av_timecode_init_from_components(&tt->timecode, last.rate, last.flags, hh, mm, ss, ff, log_ctx);
    if (ret < 0) {
        tt->timecode = last;
        return ret;
    }
    tt->last_ts -= last.start;
    tt->last_ts += tt->timecode.start;
    if (tt->timecode.start < last.start)
        tt->last_ts += tt->twenty_four_hr;
    return 0;
}

static int mcc_read_header(AVFormatContext* s)
{
    MCCContext*       mcc = s->priv_data;
    AVStream*         st  = avformat_new_stream(s, NULL);
    int64_t           pos;
    Smpte436mCodedAnc coded_anc = {
        .line_number           = mcc->vanc_line,
        .wrapping_type         = SMPTE_436M_WRAPPING_TYPE_VANC_FRAME,
        .payload_sample_coding = SMPTE_436M_PAYLOAD_SAMPLE_CODING_8BIT_LUMA,
    };
    char line[4096];
    FFTextReader tr;
    int ret = 0;

    ff_text_init_avio(s, &tr, s->pb);

    if (!st)
        return AVERROR(ENOMEM);
    if (mcc->eia608_extract) {
        st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st->codecpar->codec_id   = AV_CODEC_ID_EIA_608;
    } else {
        st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        st->codecpar->codec_id   = AV_CODEC_ID_VANC_SMPTE_436M;
        av_dict_set(&st->metadata, "data_type", "vbi_vanc_smpte_436M", 0);
    }

    TimeTracker tt;
    ret = time_tracker_init(&tt, st, (AVRational){.num = 30, .den = 1}, false, s);
    if (ret < 0)
        return ret;

    while (!ff_text_eof(&tr)) {
        ff_subtitles_read_line(&tr, line, sizeof(line));
        if (!strncmp(line, "File Format=MacCaption_MCC V", 28))
            continue;
        if (!strncmp(line, "//", 2))
            continue;
        if (!strncmp(line, "Time Code Rate=", 15)) {
            char *rate_str = line + 15;
            char *df = NULL;

            AVRational rate          = {.num = -1, .den = -1};
            bool       is_drop_frame = false;

            if (rate_str[0]) {
                rate.num = strtol(rate_str, &df, 10);
                rate.den = 1;
                if (df && !av_strncasecmp(df, "DF", 2)) {
                    is_drop_frame = true;
                    rate          = av_mul_q(rate, (AVRational){.num = 1000, .den = 1001});
                }
            }

            if (rate.num > 0 && rate.den > 0) {
                ret = time_tracker_init(&tt, st, rate, is_drop_frame, s);
                if (ret < 0)
                    return ret;
            }
            continue;
        }

        int hh, mm, ss, ff, timestamp_len;
        if (av_sscanf(line, "%d:%d:%d:%d%n", &hh, &mm, &ss, &ff, &timestamp_len) != 4)
            continue;

        ret = time_tracker_set_time(&tt, hh, mm, ss, ff, s);
        if (ret < 0)
            continue;

        char* lline = line + timestamp_len;
        if (*lline != '\t')
            continue;
        lline++;
        pos = ff_text_pos(&tr);

        PutByteContext pb;
        bytestream2_init_writer(&pb, coded_anc.payload, SMPTE_436M_CODED_ANC_PAYLOAD_CAPACITY);

        while (*lline) {
            uint8_t v = convert(*lline++);

            if (v >= 16 && v <= 35) {
                int idx = v - 16;
                bytestream2_put_buffer(&pb, aliases[idx].value, aliases[idx].len);
            } else {
                uint8_t vv;

                if (!*lline)
                    break;
                vv = convert(*lline++);
                bytestream2_put_byte(&pb, vv | (v << 4));
            }
        }
        if (pb.eof)
            continue;
        // remove trailing ANC checksum byte (not to be confused with the CDP checksum byte),
        // it's not included in 8-bit sample encodings. see section 6.2 (page 14) of:
        // https://pub.smpte.org/latest/st436/s436m-2006.pdf
        bytestream2_seek_p(&pb, -1, SEEK_CUR);
        coded_anc.payload_sample_count = bytestream2_tell_p(&pb);
        if (coded_anc.payload_sample_count == 0)
            continue; // ignore if too small
        // add padding to align to 4 bytes
        while (!pb.eof && bytestream2_tell_p(&pb) % 4)
            bytestream2_put_byte(&pb, 0);
        if (pb.eof)
            continue;
        coded_anc.payload_array_length = bytestream2_tell_p(&pb);

        int len;
        if (mcc->eia608_extract) {
            Smpte291mAnc anc;
            if (avpriv_smpte_291m_decode_anc(
                    &anc, coded_anc.payload_sample_coding, coded_anc.payload_sample_count, coded_anc.payload, s) < 0)
                continue;
            // reuse line
            int cc_count = avpriv_smpte_291m_anc_extract_cta_708(&anc, line, s);
            if (cc_count < 0) // continue if error or if it's not a closed captions packet
                continue;
            len = cc_count * 3;
        } else {
            // reuse line
            len = avpriv_vanc_smpte_436m_encode(line, sizeof(line), 1, &coded_anc);
            if (len < 0)
                continue; // continue if error
        }

        AVPacket* sub = ff_subtitles_queue_insert(&mcc->q, line, len, 0);
        if (!sub)
            return AVERROR(ENOMEM);

        sub->pos      = pos;
        sub->pts      = tt.last_ts;
        sub->duration = 1;
    }

    ff_subtitles_queue_finalize(s, &mcc->q);

    return ret;
}

static int mcc_read_packet(AVFormatContext* s, AVPacket* pkt)
{
    MCCContext* mcc = s->priv_data;
    return ff_subtitles_queue_read_packet(&mcc->q, pkt);
}

static int mcc_read_seek(AVFormatContext* s, int stream_index, int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    MCCContext* mcc = s->priv_data;
    return ff_subtitles_queue_seek(&mcc->q, s, stream_index, min_ts, ts, max_ts, flags);
}

static int mcc_read_close(AVFormatContext* s)
{
    MCCContext* mcc = s->priv_data;
    ff_subtitles_queue_clean(&mcc->q);
    return 0;
}

#define OFFSET(x) offsetof(MCCContext, x)
#define SD AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_DECODING_PARAM
// clang-format off
static const AVOption mcc_options[] = {
    { "eia608_extract", "extract EIA-608/708 captions from VANC packets", OFFSET(eia608_extract), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, SD },
    { "vanc_line", "line number of VANC packets", OFFSET(vanc_line), AV_OPT_TYPE_UINT, {.i64 = 9}, 0, 65535, SD },
    { NULL },
};
// clang-format on

static const AVClass mcc_class = {
    .class_name = "mcc demuxer",
    .item_name  = av_default_item_name,
    .option     = mcc_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEMUXER,
};

const FFInputFormat ff_mcc_demuxer = {
    .p.name         = "mcc",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MacCaption"),
    .p.extensions   = "mcc",
    .p.priv_class   = &mcc_class,
    .priv_data_size = sizeof(MCCContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = mcc_probe,
    .read_header    = mcc_read_header,
    .read_packet    = mcc_read_packet,
    .read_seek2     = mcc_read_seek,
    .read_close     = mcc_read_close,
};
