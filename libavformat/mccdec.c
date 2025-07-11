/*
 * MCC subtitle demuxer
 * Copyright (c) 2020 Paul B Mahol
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
#include "libavcodec/codec_id.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "subtitles.h"

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

static int mcc_read_header(AVFormatContext *s)
{
    MCCContext *mcc = s->priv_data;
    AVStream *st = avformat_new_stream(s, NULL);
    AVRational rate = {0};
    int64_t ts, pos;
    uint8_t out[4096];
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
    avpriv_set_pts_info(st, 64, 1, 30);

    while (!ff_text_eof(&tr)) {
        int hh, mm, ss, fs, i = 0, j = 0;
        int start = 12, count = 0;
        AVPacket *sub;
        char *lline;

        ff_subtitles_read_line(&tr, line, sizeof(line));
        if (!strncmp(line, "File Format=MacCaption_MCC V", 28))
            continue;
        if (!strncmp(line, "//", 2))
            continue;
        if (!strncmp(line, "Time Code Rate=", 15)) {
            char *rate_str = line + 15;
            char *df = NULL;
            int num = -1, den = -1;

            if (rate_str[0]) {
                num = strtol(rate_str, &df, 10);
                den = 1;
                if (df && !av_strncasecmp(df, "DF", 2)) {
                    av_reduce(&num, &den, num * 1000LL, 1001, INT_MAX);
                }
            }

            if (num > 0 && den > 0) {
                rate = av_make_q(num, den);
                avpriv_set_pts_info(st, 64, rate.den, rate.num);
            }
            continue;
        }

        if (av_sscanf(line, "%d:%d:%d:%d", &hh, &mm, &ss, &fs) != 4 || rate.den <= 0)
            continue;

        ts = av_sat_add64(av_rescale(hh * 3600LL + mm * 60LL + ss, rate.num, rate.den), fs);

        lline = (char *)&line;
        lline += 12;
        pos = ff_text_pos(&tr);

        while (lline[i]) {
            uint8_t v = convert(lline[i]);

            if (v >= 16 && v <= 35) {
                int idx = v - 16;
                if (aliases[idx].len) {
                    if (j >= sizeof(out) - 1 - aliases[idx].len) {
                        j = 0;
                        break;
                    }
                    memcpy(out + j, aliases[idx].value, aliases[idx].len);
                    j += aliases[idx].len;
                }
            } else {
                uint8_t vv;

                if (i + 13 >= sizeof(line) - 1)
                    break;
                vv = convert(lline[i + 1]);
                if (j >= sizeof(out) - 1) {
                    j = 0;
                    break;
                }
                out[j++] = vv | (v << 4);
                i++;
            }

            i++;
        }
        out[j] = 0;

        if (mcc->eia608_extract) {
            if (out[0] != 0x61 || out[1] != 0x1 || out[3] != 0x96 || out[4] != 0x69)
                continue;
            if (out[7] & 0x80)
                start += 4;
            count = (out[11] & 0x1f) * 3;
            if (j < start + count + 1)
                continue;
        } else {
            // add structure for AV_CODEC_ID_VANC_SMPTE_436M
            static const uint8_t header[] = {
                // Based off Table 7 (page 13) of:
                // https://pub.smpte.org/latest/st436/s436m-2006.pdf
                // clang-format off
                0, 1, // number of ANC packets
                0, 9, // line number -- overwritten later
                4, // wrapping type -- just use vanc progressive frame
                4, // sample coding -- 8-bit luma samples
                0, 0, // payload sample count -- overwritten later
                0, 0, 0, 0, // payload array length -- overwritten later
                0, 0, 0, 1, // payload element size -- always 1
                // clang-format on
            };
            int padding_len = 0, padded_len = j;
            if (j == 0)
                continue;
            start = 0;
            memmove(&out[sizeof(header)], out, j);
            count = j + sizeof(header);
            if (count % 4) {
                padding_len = 4 - count % 4;
                memset(out + count, 0, padding_len);
                count += padding_len;
                padded_len += padding_len;
            }
            memcpy(out, header, sizeof(header));
            // line number
            out[2] = (uint8_t)(mcc->vanc_line >> 8);
            out[3] = (uint8_t)mcc->vanc_line;
            // payload sample count
            out[6] = (uint8_t)(j >> 8);
            out[7] = (uint8_t)j;
            // payload array length
            out[8]  = (uint8_t)(padded_len >> 24);
            out[9]  = (uint8_t)(padded_len >> 16);
            out[10] = (uint8_t)(padded_len >> 8);
            out[11] = (uint8_t)padded_len;
        }

        if (!count)
            continue;
        sub = ff_subtitles_queue_insert(&mcc->q, out + start, count, 0);
        if (!sub)
            return AVERROR(ENOMEM);

        sub->pos = pos;
        sub->pts = ts;
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
