/*
 * H.263i decoder
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

#include "codec_internal.h"
#include "h263.h"
#include "mpegvideo.h"
#include "mpegvideodec.h"
#include "h263data.h"
#include "h263dec.h"

/* don't understand why they choose a different header ! */
int ff_intel_h263_decode_picture_header(H263DecContext *const h)
{
    int format;

    if (get_bits_left(&h->gb) == 64) { /* special dummy frames */
        return FRAME_SKIPPED;
    }

    /* picture header */
    if (get_bits(&h->gb, 22) != 0x20) {
        av_log(h->c.avctx, AV_LOG_ERROR, "Bad picture start code\n");
        return -1;
    }
    h->picture_number = get_bits(&h->gb, 8); /* picture timestamp */

    if (check_marker(h->c.avctx, &h->gb, "after picture_number") != 1) {
        return -1;      /* marker */
    }
    if (get_bits1(&h->gb) != 0) {
        av_log(h->c.avctx, AV_LOG_ERROR, "Bad H.263 id\n");
        return -1;      /* H.263 id */
    }
    skip_bits1(&h->gb);         /* split screen off */
    skip_bits1(&h->gb);         /* camera  off */
    skip_bits1(&h->gb);         /* freeze picture release off */

    format = get_bits(&h->gb, 3);
    if (format == 0 || format == 6) {
        av_log(h->c.avctx, AV_LOG_ERROR, "Intel H.263 free format not supported\n");
        return -1;
    }

    h->c.pict_type = AV_PICTURE_TYPE_I + get_bits1(&h->gb);

    h->h263_long_vectors = get_bits1(&h->gb);

    if (get_bits1(&h->gb) != 0) {
        av_log(h->c.avctx, AV_LOG_ERROR, "SAC not supported\n");
        return -1;      /* SAC: off */
    }
    h->c.obmc     = get_bits1(&h->gb);
    h->pb_frame = get_bits1(&h->gb);

    if (format < 6) {
        h->c.width  = ff_h263_format[format][0];
        h->c.height = ff_h263_format[format][1];
        h->c.avctx->sample_aspect_ratio.num = 12;
        h->c.avctx->sample_aspect_ratio.den = 11;
    } else {
        format = get_bits(&h->gb, 3);
        if(format == 0 || format == 7){
            av_log(h->c.avctx, AV_LOG_ERROR, "Wrong Intel H.263 format\n");
            return -1;
        }
        if (get_bits(&h->gb, 2))
            av_log(h->c.avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        h->loop_filter = get_bits1(&h->gb) * !h->c.avctx->lowres;
        if (get_bits1(&h->gb))
            av_log(h->c.avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        if (get_bits1(&h->gb))
            h->pb_frame = 2;
        if (get_bits(&h->gb, 5))
            av_log(h->c.avctx, AV_LOG_ERROR, "Bad value for reserved field\n");
        if (get_bits(&h->gb, 5) != 1)
            av_log(h->c.avctx, AV_LOG_ERROR, "Invalid marker\n");
    }
    if(format == 6){
        int ar = get_bits(&h->gb, 4);
        skip_bits(&h->gb, 9); // display width
        check_marker(h->c.avctx, &h->gb, "in dimensions");
        skip_bits(&h->gb, 9); // display height
        if (ar == 15) {
            h->c.avctx->sample_aspect_ratio.num = get_bits(&h->gb, 8); // aspect ratio - width
            h->c.avctx->sample_aspect_ratio.den = get_bits(&h->gb, 8); // aspect ratio - height
        } else {
            h->c.avctx->sample_aspect_ratio = ff_h263_pixel_aspect[ar];
        }
        if (h->c.avctx->sample_aspect_ratio.num == 0)
            av_log(h->c.avctx, AV_LOG_ERROR, "Invalid aspect ratio.\n");
    }

    h->c.chroma_qscale = h->c.qscale = get_bits(&h->gb, 5);
    skip_bits1(&h->gb); /* Continuous Presence Multipoint mode: off */

    if (h->pb_frame) {
        skip_bits(&h->gb, 3); //temporal reference for B-frame
        skip_bits(&h->gb, 2); //dbquant
    }

    /* PEI */
    if (skip_1stop_8data_bits(&h->gb) < 0)
        return AVERROR_INVALIDDATA;

    h->gob_index = H263_GOB_HEIGHT(h->c.height);

    ff_h263_show_pict_info(h, 0);

    return 0;
}

const FFCodec ff_h263i_decoder = {
    .p.name         = "h263i",
    CODEC_LONG_NAME("Intel H.263"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H263I,
    .priv_data_size = sizeof(H263DecContext),
    .init           = ff_h263_decode_init,
    FF_CODEC_DECODE_CB(ff_h263_decode_frame),
    .close          = ff_mpv_decode_close,
    .p.capabilities = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP |
                      FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
};
