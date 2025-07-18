/*
 * Motion estimation
 * Copyright (c) 2000,2001 Fabrice Bellard
 * Copyright (c) 2002-2004 Michael Niedermayer
 *
 * new motion estimation (X1/EPZS) by Michael Niedermayer <michaelni@gmx.at>
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

/**
 * @file
 * Motion estimation.
 */

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#include "avcodec.h"
#include "h263.h"
#include "mathops.h"
#include "motion_est.h"
#include "mpegutils.h"
#include "mpegvideoenc.h"

#define P_LEFT P[1]
#define P_TOP P[2]
#define P_TOPRIGHT P[3]
#define P_MEDIAN P[4]
#define P_MV1 P[9]

#define ME_MAP_SHIFT 3
#define ME_MAP_MV_BITS 11

static int sad_hpel_motion_search(MPVEncContext *const s,
                                  int *mx_ptr, int *my_ptr, int dmin,
                                  int src_index, int ref_index,
                                  int size, int h);

static inline unsigned update_map_generation(MotionEstContext *c)
{
    c->map_generation+= 1<<(ME_MAP_MV_BITS*2);
    if(c->map_generation==0){
        c->map_generation= 1<<(ME_MAP_MV_BITS*2);
        memset(c->map, 0, sizeof(uint32_t)*ME_MAP_SIZE);
    }
    return c->map_generation;
}

/* shape adaptive search stuff */
typedef struct Minima{
    int height;
    int x, y;
    int checked;
}Minima;

static int minima_cmp(const void *a, const void *b){
    const Minima *da = (const Minima *) a;
    const Minima *db = (const Minima *) b;

    return da->height - db->height;
}

#define FLAG_QPEL   1 //must be 1
#define FLAG_CHROMA 2
#define FLAG_DIRECT 4

static inline void init_ref(MotionEstContext *c, uint8_t *const src[3],
                            uint8_t *const ref[3], uint8_t *const ref2[3],
                            int x, int y, int ref_index)
{
    const int offset[3]= {
          y*c->  stride + x,
        ((y*c->uvstride + x)>>1),
        ((y*c->uvstride + x)>>1),
    };
    int i;
    for(i=0; i<3; i++){
        c->src[0][i]= src [i] + offset[i];
        c->ref[0][i]= ref [i] + offset[i];
    }
    if(ref_index){
        for(i=0; i<3; i++){
            c->ref[ref_index][i]= ref2[i] + offset[i];
        }
    }
}

static int get_flags(MotionEstContext *c, int direct, int chroma){
    return   ((c->avctx->flags&AV_CODEC_FLAG_QPEL) ? FLAG_QPEL : 0)
           + (direct ? FLAG_DIRECT : 0)
           + (chroma ? FLAG_CHROMA : 0);
}

static av_always_inline int cmp_direct_inline(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, int qpel){
    MotionEstContext *const c = &s->me;
    const int stride= c->stride;
    const int hx = subx + x * (1 << (1 + qpel));
    const int hy = suby + y * (1 << (1 + qpel));
    const uint8_t * const * const ref = c->ref[ref_index];
    const uint8_t * const * const src = c->src[src_index];
    int d;
    //FIXME check chroma 4mv, (no crashes ...)
    av_assert2(x >= c->xmin && hx <= c->xmax<<(qpel+1) && y >= c->ymin && hy <= c->ymax<<(qpel+1));
    if (x >= c->xmin && hx <= c->xmax << (qpel + 1) &&
        y >= c->ymin && hy <= c->ymax << (qpel + 1)) {
        const int time_pp = s->c.pp_time;
        const int time_pb = s->c.pb_time;
        const int mask    = 2 * qpel + 1;
        if (s->c.mv_type == MV_TYPE_8X8) {
            for(int i = 0; i < 4; ++i) {
                int fx  = c->direct_basis_mv[i][0] + hx;
                int fy  = c->direct_basis_mv[i][1] + hy;
                int bx  = hx ? fx - c->co_located_mv[i][0] : c->co_located_mv[i][0] * (time_pb - time_pp)/time_pp + ((i &1)<<(qpel+4));
                int by  = hy ? fy - c->co_located_mv[i][1] : c->co_located_mv[i][1] * (time_pb - time_pp)/time_pp + ((i>>1)<<(qpel+4));
                int fxy = (fx & mask) + ((fy & mask) << (qpel + 1));
                int bxy = (bx & mask) + ((by & mask) << (qpel + 1));

                uint8_t *dst = c->temp + 8 * (i & 1) + 8 * stride * (i >> 1);
                if (qpel) {
                    c->qpel_put[1][fxy](dst, ref[0] + (fx >> 2) + (fy >> 2) * stride, stride);
                    c->qpel_avg[1][bxy](dst, ref[8] + (bx >> 2) + (by >> 2) * stride, stride);
                } else {
                    c->hpel_put[1][fxy](dst, ref[0] + (fx >> 1) + (fy >> 1) * stride, stride, 8);
                    c->hpel_avg[1][bxy](dst, ref[8] + (bx >> 1) + (by >> 1) * stride, stride, 8);
                }
            }
        } else {
            int fx  = c->direct_basis_mv[0][0] + hx;
            int fy  = c->direct_basis_mv[0][1] + hy;
            int bx  = hx ? fx - c->co_located_mv[0][0] : (c->co_located_mv[0][0] * (time_pb - time_pp)/time_pp);
            int by  = hy ? fy - c->co_located_mv[0][1] : (c->co_located_mv[0][1] * (time_pb - time_pp)/time_pp);
            int fxy = (fx & mask) + ((fy & mask) << (qpel + 1));
            int bxy = (bx & mask) + ((by & mask) << (qpel + 1));

            if (qpel) {
                c->qpel_put[1][fxy](c->temp               , ref[0] + (fx>>2) + (fy>>2)*stride               , stride);
                c->qpel_put[1][fxy](c->temp + 8           , ref[0] + (fx>>2) + (fy>>2)*stride + 8           , stride);
                c->qpel_put[1][fxy](c->temp     + 8*stride, ref[0] + (fx>>2) + (fy>>2)*stride     + 8*stride, stride);
                c->qpel_put[1][fxy](c->temp + 8 + 8*stride, ref[0] + (fx>>2) + (fy>>2)*stride + 8 + 8*stride, stride);
                c->qpel_avg[1][bxy](c->temp               , ref[8] + (bx>>2) + (by>>2)*stride               , stride);
                c->qpel_avg[1][bxy](c->temp + 8           , ref[8] + (bx>>2) + (by>>2)*stride + 8           , stride);
                c->qpel_avg[1][bxy](c->temp     + 8*stride, ref[8] + (bx>>2) + (by>>2)*stride     + 8*stride, stride);
                c->qpel_avg[1][bxy](c->temp + 8 + 8*stride, ref[8] + (bx>>2) + (by>>2)*stride + 8 + 8*stride, stride);
            } else {
                av_assert2((fx >> 1) + 16 * s->c.mb_x >= -16);
                av_assert2((fy >> 1) + 16 * s->c.mb_y >= -16);
                av_assert2((fx >> 1) + 16 * s->c.mb_x <= s->c.width);
                av_assert2((fy >> 1) + 16 * s->c.mb_y <= s->c.height);
                av_assert2((bx >> 1) + 16 * s->c.mb_x >= -16);
                av_assert2((by >> 1) + 16 * s->c.mb_y >= -16);
                av_assert2((bx >> 1) + 16 * s->c.mb_x <= s->c.width);
                av_assert2((by >> 1) + 16 * s->c.mb_y <= s->c.height);

                c->hpel_put[0][fxy](c->temp, ref[0] + (fx >> 1) + (fy >> 1) * stride, stride, 16);
                c->hpel_avg[0][bxy](c->temp, ref[8] + (bx >> 1) + (by >> 1) * stride, stride, 16);
            }
        }
        d = cmp_func(s, c->temp, src[0], stride, 16);
    } else
        d = 256 * 256 * 256 * 32;
    return d;
}

static av_always_inline int cmp_inline(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, int qpel, int chroma){
    MotionEstContext *const c = &s->me;
    const int stride= c->stride;
    const int uvstride= c->uvstride;
    const int dxy= subx + (suby<<(1+qpel)); //FIXME log2_subpel?
    const int hx= subx + x*(1<<(1+qpel));
    const int hy= suby + y*(1<<(1+qpel));
    const uint8_t * const * const ref = c->ref[ref_index];
    const uint8_t * const * const src = c->src[src_index];
    int d;
    //FIXME check chroma 4mv, (no crashes ...)
    int uvdxy;              /* no, it might not be used uninitialized */
    if (dxy) {
        if (qpel) {
            if (h << size == 16) {
                c->qpel_put[size][dxy](c->temp,  ref[0] + x + y * stride, stride); //FIXME prototype (add h)
            } else if (size == 0 && h == 8) {
                c->qpel_put[1][dxy](c->temp    , ref[0] + x + y * stride    , stride);
                c->qpel_put[1][dxy](c->temp + 8, ref[0] + x + y * stride + 8, stride);
            } else
                av_assert2(0);
            if (chroma) {
                int cx = hx / 2;
                int cy = hy / 2;
                cx = (cx >> 1) | (cx & 1);
                cy = (cy >> 1) | (cy & 1);
                uvdxy = (cx & 1) + 2 * (cy & 1);
                // FIXME x/y wrong, but MPEG-4 qpel is sick anyway, we should drop as much of it as possible in favor for H.264
            }
        } else {
            c->hpel_put[size][dxy](c->temp, ref[0] + x + y * stride, stride, h);
            if (chroma)
                uvdxy = dxy | (x & 1) | (2 * (y & 1));
        }
        d = cmp_func(s, c->temp, src[0], stride, h);
    } else {
        d = cmp_func(s, src[0], ref[0] + x + y * stride, stride, h);
        if (chroma)
            uvdxy = (x & 1) + 2 * (y & 1);
    }
    if (chroma) {
        uint8_t *const uvtemp = c->temp + 16 * stride;
        c->hpel_put[size + 1][uvdxy](uvtemp    , ref[1] + (x >> 1) + (y >> 1) * uvstride, uvstride, h >> 1);
        c->hpel_put[size + 1][uvdxy](uvtemp + 8, ref[2] + (x >> 1) + (y >> 1) * uvstride, uvstride, h >> 1);
        d += chroma_cmp_func(s, uvtemp    , src[1], uvstride, h >> 1);
        d += chroma_cmp_func(s, uvtemp + 8, src[2], uvstride, h >> 1);
    }
    return d;
}

static int cmp_simple(MPVEncContext *const s, const int x, const int y,
                      int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func){
    return cmp_inline(s,x,y,0,0,0,16,ref_index,src_index, cmp_func, chroma_cmp_func, 0, 0);
}

static int cmp_fpel_internal(MPVEncContext *const s, const int x, const int y,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, const int flags){
    if(flags&FLAG_DIRECT){
        return cmp_direct_inline(s,x,y,0,0,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, flags&FLAG_QPEL);
    }else{
        return cmp_inline(s,x,y,0,0,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, 0, flags&FLAG_CHROMA);
    }
}

static int cmp_internal(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, const int flags){
    if(flags&FLAG_DIRECT){
        return cmp_direct_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, flags&FLAG_QPEL);
    }else{
        return cmp_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, flags&FLAG_QPEL, flags&FLAG_CHROMA);
    }
}

/** @brief compares a block (either a full macroblock or a partition thereof)
    against a proposed motion-compensated prediction of that block
 */
static av_always_inline int cmp(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, const int flags){
    if(av_builtin_constant_p(flags) && av_builtin_constant_p(h) && av_builtin_constant_p(size)
       && av_builtin_constant_p(subx) && av_builtin_constant_p(suby)
       && flags==0 && h==16 && size==0 && subx==0 && suby==0){
        return cmp_simple(s,x,y,ref_index,src_index, cmp_func, chroma_cmp_func);
    }else if(av_builtin_constant_p(subx) && av_builtin_constant_p(suby)
       && subx==0 && suby==0){
        return cmp_fpel_internal(s,x,y,size,h,ref_index,src_index, cmp_func, chroma_cmp_func,flags);
    }else{
        return cmp_internal(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, flags);
    }
}

static int cmp_hpel(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, const int flags){
    if(flags&FLAG_DIRECT){
        return cmp_direct_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, 0);
    }else{
        return cmp_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, 0, flags&FLAG_CHROMA);
    }
}

static int cmp_qpel(MPVEncContext *const s, const int x, const int y, const int subx, const int suby,
                      const int size, const int h, int ref_index, int src_index,
                      me_cmp_func cmp_func, me_cmp_func chroma_cmp_func, const int flags){
    if(flags&FLAG_DIRECT){
        return cmp_direct_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, 1);
    }else{
        return cmp_inline(s,x,y,subx,suby,size,h,ref_index,src_index, cmp_func, chroma_cmp_func, 1, flags&FLAG_CHROMA);
    }
}

#include "motion_est_template.c"

static int zero_cmp(MPVEncContext *const s, const uint8_t *a, const uint8_t *b,
                    ptrdiff_t stride, int h)
{
    return 0;
}

static void zero_hpel(uint8_t *a, const uint8_t *b, ptrdiff_t stride, int h){
}

av_cold int ff_me_init(MotionEstContext *c, AVCodecContext *avctx,
                       const MECmpContext *mecc, int mpvenc)
{
    int cache_size = FFMIN(ME_MAP_SIZE>>ME_MAP_SHIFT, 1<<ME_MAP_SHIFT);
    int dia_size   = FFMAX(FFABS(avctx->dia_size) & 255, FFABS(avctx->pre_dia_size) & 255);
    int ret;

    if (FFMIN(avctx->dia_size, avctx->pre_dia_size) < -FFMIN(ME_MAP_SIZE, MAX_SAB_SIZE)) {
        av_log(avctx, AV_LOG_ERROR, "ME_MAP size is too small for SAB diamond\n");
        return AVERROR(EINVAL);
    }

    c->avctx = avctx;

    if (avctx->codec_id == AV_CODEC_ID_H261)
        avctx->me_sub_cmp = avctx->me_cmp;

    if (cache_size < 2 * dia_size)
        av_log(avctx, AV_LOG_INFO, "ME_MAP size may be a little small for the selected diamond size\n");

    ret  = ff_set_cmp(mecc, c->me_pre_cmp, avctx->me_pre_cmp, mpvenc);
    ret |= ff_set_cmp(mecc, c->me_cmp,     avctx->me_cmp,     mpvenc);
    ret |= ff_set_cmp(mecc, c->me_sub_cmp, avctx->me_sub_cmp, mpvenc);
    ret |= ff_set_cmp(mecc, c->mb_cmp,     avctx->mb_cmp,     mpvenc);
    if (ret < 0)
        return ret;

    c->sse = mecc->sse[0];
    memcpy(c->pix_abs, mecc->pix_abs, sizeof(c->pix_abs));

    c->flags     = get_flags(c, 0, avctx->me_cmp     & FF_CMP_CHROMA);
    c->sub_flags = get_flags(c, 0, avctx->me_sub_cmp & FF_CMP_CHROMA);
    c->mb_flags  = get_flags(c, 0, avctx->mb_cmp     & FF_CMP_CHROMA);

    if (avctx->codec_id == AV_CODEC_ID_H261) {
        c->sub_motion_search = no_sub_motion_search;
    } else if (avctx->flags & AV_CODEC_FLAG_QPEL) {
        c->sub_motion_search= qpel_motion_search;
    }else{
        if(c->avctx->me_sub_cmp&FF_CMP_CHROMA)
            c->sub_motion_search= hpel_motion_search;
        else if(   c->avctx->me_sub_cmp == FF_CMP_SAD
                && c->avctx->    me_cmp == FF_CMP_SAD
                && c->avctx->    mb_cmp == FF_CMP_SAD)
            c->sub_motion_search= sad_hpel_motion_search; // 2050 vs. 2450 cycles
        else
            c->sub_motion_search= hpel_motion_search;
    }

    /* 8x8 fullpel search would need a 4x4 chroma compare, which we do
     * not have yet, and even if we had, the motion estimation code
     * does not expect it. */
    if (avctx->codec_id != AV_CODEC_ID_SNOW) {
        if ((avctx->me_cmp & FF_CMP_CHROMA) /* && !c->me_cmp[2] */)
            c->me_cmp[2] = zero_cmp;
        if ((avctx->me_sub_cmp & FF_CMP_CHROMA) && !c->me_sub_cmp[2])
            c->me_sub_cmp[2] = zero_cmp;
    }

    return 0;
}

void ff_me_init_pic(MPVEncContext *const s)
{
    MotionEstContext *const c = &s->me;

/*FIXME s->c.no_rounding b_type*/
    if (c->avctx->flags & AV_CODEC_FLAG_QPEL) {
        c->qpel_avg = s->c.qdsp.avg_qpel_pixels_tab;
        if (s->c.no_rounding)
            c->qpel_put = s->c.qdsp.put_no_rnd_qpel_pixels_tab;
        else
            c->qpel_put = s->c.qdsp.put_qpel_pixels_tab;
    }
    c->hpel_avg = s->c.hdsp.avg_pixels_tab;
    if (s->c.no_rounding)
        c->hpel_put = s->c.hdsp.put_no_rnd_pixels_tab;
    else
        c->hpel_put = s->c.hdsp.put_pixels_tab;

    if (s->c.linesize) {
        c->stride   = s->c.linesize;
        c->uvstride = s->c.uvlinesize;
    }else{
        c->stride   = 16*s->c.mb_width + 32;
        c->uvstride =  8*s->c.mb_width + 16;
    }
    if (s->c.codec_id != AV_CODEC_ID_SNOW) {
        c->hpel_put[2][0]= c->hpel_put[2][1]=
        c->hpel_put[2][2]= c->hpel_put[2][3]= zero_hpel;
    }
    /* Reset the average MB variance and scene change stats */
    c->scene_change_score = 0;
    c->mb_var_sum_temp    =
    c->mc_mb_var_sum_temp = 0;
}

#define CHECK_SAD_HALF_MV(suffix, x, y) \
{\
    d  = c->pix_abs[size][(x ? 1 : 0) + (y ? 2 : 0)](NULL, pix, ptr + ((x) >> 1), stride, h); \
    d += (mv_penalty[pen_x + x] + mv_penalty[pen_y + y])*penalty_factor;\
    COPY3_IF_LT(dminh, d, dx, x, dy, y)\
}

static int sad_hpel_motion_search(MPVEncContext *const s,
                                  int *mx_ptr, int *my_ptr, int dmin,
                                  int src_index, int ref_index,
                                  int size, int h)
{
    MotionEstContext *const c = &s->me;
    const int penalty_factor= c->sub_penalty_factor;
    int mx, my, dminh;
    const uint8_t *pix, *ptr;
    int stride= c->stride;
    LOAD_COMMON

    av_assert2(c->sub_flags == 0);

    if(c->skip){
        *mx_ptr = 0;
        *my_ptr = 0;
        return dmin;
    }

    pix = c->src[src_index][0];

    mx = *mx_ptr;
    my = *my_ptr;
    ptr = c->ref[ref_index][0] + (my * stride) + mx;

    dminh = dmin;

    if (mx > xmin && mx < xmax &&
        my > ymin && my < ymax) {
        int dx=0, dy=0;
        int d, pen_x, pen_y;
        const int index= my*(1<<ME_MAP_SHIFT) + mx;
        const int t= score_map[(index-(1<<ME_MAP_SHIFT))&(ME_MAP_SIZE-1)];
        const int l= score_map[(index- 1               )&(ME_MAP_SIZE-1)];
        const int r= score_map[(index+ 1               )&(ME_MAP_SIZE-1)];
        const int b= score_map[(index+(1<<ME_MAP_SHIFT))&(ME_MAP_SIZE-1)];
        mx += mx;
        my += my;


        pen_x= pred_x + mx;
        pen_y= pred_y + my;

        ptr-= stride;
        if(t<=b){
            CHECK_SAD_HALF_MV(y2 , 0, -1)
            if(l<=r){
                CHECK_SAD_HALF_MV(xy2, -1, -1)
                if(t+r<=b+l){
                    CHECK_SAD_HALF_MV(xy2, +1, -1)
                    ptr+= stride;
                }else{
                    ptr+= stride;
                    CHECK_SAD_HALF_MV(xy2, -1, +1)
                }
                CHECK_SAD_HALF_MV(x2 , -1,  0)
            }else{
                CHECK_SAD_HALF_MV(xy2, +1, -1)
                if(t+l<=b+r){
                    CHECK_SAD_HALF_MV(xy2, -1, -1)
                    ptr+= stride;
                }else{
                    ptr+= stride;
                    CHECK_SAD_HALF_MV(xy2, +1, +1)
                }
                CHECK_SAD_HALF_MV(x2 , +1,  0)
            }
        }else{
            if(l<=r){
                if(t+l<=b+r){
                    CHECK_SAD_HALF_MV(xy2, -1, -1)
                    ptr+= stride;
                }else{
                    ptr+= stride;
                    CHECK_SAD_HALF_MV(xy2, +1, +1)
                }
                CHECK_SAD_HALF_MV(x2 , -1,  0)
                CHECK_SAD_HALF_MV(xy2, -1, +1)
            }else{
                if(t+r<=b+l){
                    CHECK_SAD_HALF_MV(xy2, +1, -1)
                    ptr+= stride;
                }else{
                    ptr+= stride;
                    CHECK_SAD_HALF_MV(xy2, -1, +1)
                }
                CHECK_SAD_HALF_MV(x2 , +1,  0)
                CHECK_SAD_HALF_MV(xy2, +1, +1)
            }
            CHECK_SAD_HALF_MV(y2 ,  0, +1)
        }
        mx+=dx;
        my+=dy;

    }else{
        mx += mx;
        my += my;
    }

    *mx_ptr = mx;
    *my_ptr = my;
    return dminh;
}

static inline void set_p_mv_tables(MPVEncContext *const s, int mx, int my, int mv4)
{
    const int xy = s->c.mb_x + s->c.mb_y * s->c.mb_stride;

    s->p_mv_table[xy][0] = mx;
    s->p_mv_table[xy][1] = my;

    /* has already been set to the 4 MV if 4MV is done */
    if(mv4){
        int mot_xy = s->c.block_index[0];

        s->c.cur_pic.motion_val[0][mot_xy    ][0] = mx;
        s->c.cur_pic.motion_val[0][mot_xy    ][1] = my;
        s->c.cur_pic.motion_val[0][mot_xy + 1][0] = mx;
        s->c.cur_pic.motion_val[0][mot_xy + 1][1] = my;

        mot_xy += s->c.b8_stride;
        s->c.cur_pic.motion_val[0][mot_xy    ][0] = mx;
        s->c.cur_pic.motion_val[0][mot_xy    ][1] = my;
        s->c.cur_pic.motion_val[0][mot_xy + 1][0] = mx;
        s->c.cur_pic.motion_val[0][mot_xy + 1][1] = my;
    }
}

/**
 * get fullpel ME search limits.
 */
static inline void get_limits(MPVEncContext *const s, int x, int y, int bframe)
{
    MotionEstContext *const c = &s->me;
    int range= c->avctx->me_range >> (1 + !!(c->flags&FLAG_QPEL));
    int max_range = MAX_MV >> (1 + !!(c->flags&FLAG_QPEL));
/*
    if(c->avctx->me_range) c->range= c->avctx->me_range >> 1;
    else                   c->range= 16;
*/
    if (c->unrestricted_mv) {
        c->xmin = - x - 16;
        c->ymin = - y - 16;
        c->xmax = - x + s->c.width;
        c->ymax = - y + s->c.height;
    } else if (!(av_builtin_constant_p(bframe) && bframe) && s->c.out_format == FMT_H261){
        // Search range of H.261 is different from other codec standards
        c->xmin = (x > 15) ? - 15 : 0;
        c->ymin = (y > 15) ? - 15 : 0;
        c->xmax = (x < s->c.mb_width * 16 - 16) ? 15 : 0;
        c->ymax = (y < s->c.mb_height * 16 - 16) ? 15 : 0;
    } else {
        c->xmin = - x;
        c->ymin = - y;
        c->xmax = - x + s->c.mb_width *16 - 16;
        c->ymax = - y + s->c.mb_height*16 - 16;
    }
    if(!range || range > max_range)
        range = max_range;
    if(range){
        c->xmin = FFMAX(c->xmin,-range);
        c->xmax = FFMIN(c->xmax, range);
        c->ymin = FFMAX(c->ymin,-range);
        c->ymax = FFMIN(c->ymax, range);
    }
}

static inline void init_mv4_ref(MotionEstContext *c){
    const int stride= c->stride;

    c->ref[1][0] = c->ref[0][0] + 8;
    c->ref[2][0] = c->ref[0][0] + 8*stride;
    c->ref[3][0] = c->ref[2][0] + 8;
    c->src[1][0] = c->src[0][0] + 8;
    c->src[2][0] = c->src[0][0] + 8*stride;
    c->src[3][0] = c->src[2][0] + 8;
}

static inline int h263_mv4_search(MPVEncContext *const s, int mx, int my, int shift)
{
    MotionEstContext *const c = &s->me;
    const int size= 1;
    const int h=8;
    int block;
    int P[10][2];
    int dmin_sum=0, mx4_sum=0, my4_sum=0, i;
    int same=1;
    const int stride= c->stride;
    const uint8_t *mv_penalty = c->current_mv_penalty;
    int safety_clipping = c->unrestricted_mv && (s->c.width&15) && (s->c.height&15);

    init_mv4_ref(c);

    for(block=0; block<4; block++){
        int mx4, my4;
        int pred_x4, pred_y4;
        int dmin4;
        static const int off[4]= {2, 1, 1, -1};
        const int mot_stride = s->c.b8_stride;
        const int mot_xy = s->c.block_index[block];

        if(safety_clipping){
            c->xmax = - 16*s->c.mb_x + s->c.width  - 8*(block &1);
            c->ymax = - 16*s->c.mb_y + s->c.height - 8*(block>>1);
        }

        P_LEFT[0] = s->c.cur_pic.motion_val[0][mot_xy - 1][0];
        P_LEFT[1] = s->c.cur_pic.motion_val[0][mot_xy - 1][1];

        if (P_LEFT[0] > c->xmax * (1 << shift)) P_LEFT[0] = c->xmax * (1 << shift);

        /* special case for first line */
        if (s->c.first_slice_line && block < 2) {
            c->pred_x= pred_x4= P_LEFT[0];
            c->pred_y= pred_y4= P_LEFT[1];
        } else {
            P_TOP[0]      = s->c.cur_pic.motion_val[0][mot_xy - mot_stride             ][0];
            P_TOP[1]      = s->c.cur_pic.motion_val[0][mot_xy - mot_stride             ][1];
            P_TOPRIGHT[0] = s->c.cur_pic.motion_val[0][mot_xy - mot_stride + off[block]][0];
            P_TOPRIGHT[1] = s->c.cur_pic.motion_val[0][mot_xy - mot_stride + off[block]][1];
            if (P_TOP[1]      > c->ymax * (1 << shift)) P_TOP[1]      = c->ymax * (1 << shift);
            if (P_TOPRIGHT[0] < c->xmin * (1 << shift)) P_TOPRIGHT[0] = c->xmin * (1 << shift);
            if (P_TOPRIGHT[0] > c->xmax * (1 << shift)) P_TOPRIGHT[0] = c->xmax * (1 << shift);
            if (P_TOPRIGHT[1] > c->ymax * (1 << shift)) P_TOPRIGHT[1] = c->ymax * (1 << shift);

            P_MEDIAN[0]= mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
            P_MEDIAN[1]= mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);

            c->pred_x= pred_x4 = P_MEDIAN[0];
            c->pred_y= pred_y4 = P_MEDIAN[1];
        }
        P_MV1[0]= mx;
        P_MV1[1]= my;
        if(safety_clipping)
            for(i=1; i<10; i++){
                if (s->c.first_slice_line && block < 2 && i > 1 && i < 9)
                    continue;
                if (i>4 && i<9)
                    continue;
                if (P[i][0] > c->xmax * (1 << shift)) P[i][0] = c->xmax * (1 << shift);
                if (P[i][1] > c->ymax * (1 << shift)) P[i][1] = c->ymax * (1 <<shift );
            }

        dmin4 = epzs_motion_search2(s, &mx4, &my4, P, block, block, s->p_mv_table, (1<<16)>>shift, 1);

        dmin4= c->sub_motion_search(s, &mx4, &my4, dmin4, block, block, size, h);

        if (c->me_sub_cmp[0] != c->mb_cmp[0]) {
            int dxy;
            const int offset= ((block&1) + (block>>1)*stride)*8;
            uint8_t *dest_y = c->scratchpad + offset;
            if (s->c.quarter_sample) {
                const uint8_t *ref = c->ref[block][0] + (mx4>>2) + (my4>>2)*stride;
                dxy = ((my4 & 3) << 2) | (mx4 & 3);

                c->qpel_put[1][dxy](dest_y, ref, stride);
            }else{
                const uint8_t *ref = c->ref[block][0] + (mx4>>1) + (my4>>1)*stride;
                dxy = ((my4 & 1) << 1) | (mx4 & 1);

                c->hpel_put[1][dxy](dest_y, ref, stride, h);
            }
            dmin_sum+= (mv_penalty[mx4-pred_x4] + mv_penalty[my4-pred_y4])*c->mb_penalty_factor;
        }else
            dmin_sum+= dmin4;

        if (s->c.quarter_sample) {
            mx4_sum+= mx4/2;
            my4_sum+= my4/2;
        }else{
            mx4_sum+= mx4;
            my4_sum+= my4;
        }

        s->c.cur_pic.motion_val[0][s->c.block_index[block]][0] = mx4;
        s->c.cur_pic.motion_val[0][s->c.block_index[block]][1] = my4;

        if(mx4 != mx || my4 != my) same=0;
    }

    if(same)
        return INT_MAX;

    if (c->me_sub_cmp[0] != c->mb_cmp[0]) {
        dmin_sum += c->mb_cmp[0](s,
                                 s->new_pic->data[0] +
                                 s->c.mb_x * 16 + s->c.mb_y * 16 * stride,
                                 c->scratchpad, stride, 16);
    }

    if(c->avctx->mb_cmp&FF_CMP_CHROMA){
        int dxy;
        int mx, my;
        int offset;

        mx= ff_h263_round_chroma(mx4_sum);
        my= ff_h263_round_chroma(my4_sum);
        dxy = ((my & 1) << 1) | (mx & 1);

        offset = (s->c.mb_x*8 + (mx>>1)) + (s->c.mb_y*8 + (my>>1))*s->c.uvlinesize;

        c->hpel_put[1][dxy](c->scratchpad    , s->c.last_pic.data[1] + offset, s->c.uvlinesize, 8);
        c->hpel_put[1][dxy](c->scratchpad + 8, s->c.last_pic.data[2] + offset, s->c.uvlinesize, 8);

        dmin_sum += c->mb_cmp[1](s, s->new_pic->data[1] + s->c.mb_x * 8 + s->c.mb_y * 8 * s->c.uvlinesize, c->scratchpad,     s->c.uvlinesize, 8);
        dmin_sum += c->mb_cmp[1](s, s->new_pic->data[2] + s->c.mb_x * 8 + s->c.mb_y * 8 * s->c.uvlinesize, c->scratchpad + 8, s->c.uvlinesize, 8);
    }

    c->pred_x= mx;
    c->pred_y= my;

    switch(c->avctx->mb_cmp&0xFF){
    /*case FF_CMP_SSE:
        return dmin_sum+ 32*s->c.qscale*s->c.qscale;*/
    case FF_CMP_RD:
        return dmin_sum;
    default:
        return dmin_sum+ 11*c->mb_penalty_factor;
    }
}

static inline void init_interlaced_ref(MPVEncContext *const s, int ref_index)
{
    MotionEstContext *const c = &s->me;

    c->ref[1+ref_index][0] = c->ref[0+ref_index][0] + s->c.linesize;
    c->src[1][0] = c->src[0][0] + s->c.linesize;
    if(c->flags & FLAG_CHROMA){
        c->ref[1+ref_index][1] = c->ref[0+ref_index][1] + s->c.uvlinesize;
        c->ref[1+ref_index][2] = c->ref[0+ref_index][2] + s->c.uvlinesize;
        c->src[1][1] = c->src[0][1] + s->c.uvlinesize;
        c->src[1][2] = c->src[0][2] + s->c.uvlinesize;
    }
}

static int interlaced_search(MPVEncContext *const s, int ref_index,
                             int16_t (*mv_tables[2][2])[2], uint8_t *field_select_tables[2], int mx, int my, int user_field_select)
{
    MotionEstContext *const c = &s->me;
    const int size=0;
    const int h=8;
    int block;
    int P[10][2];
    const uint8_t * const mv_penalty = c->current_mv_penalty;
    int same=1;
    const int stride = 2*s->c.linesize;
    int dmin_sum= 0;
    const int mot_stride = s->c.mb_stride;
    const int xy = s->c.mb_x + s->c.mb_y*mot_stride;

    c->ymin>>=1;
    c->ymax>>=1;
    c->stride<<=1;
    c->uvstride<<=1;
    init_interlaced_ref(s, ref_index);

    for(block=0; block<2; block++){
        int field_select;
        int best_dmin= INT_MAX;
        int best_field= -1;

        for(field_select=0; field_select<2; field_select++){
            int dmin, mx_i, my_i;
            int16_t (*mv_table)[2]= mv_tables[block][field_select];

            if(user_field_select){
                av_assert1(field_select==0 || field_select==1);
                av_assert1(field_select_tables[block][xy]==0 || field_select_tables[block][xy]==1);
                if(field_select_tables[block][xy] != field_select)
                    continue;
            }

            P_LEFT[0] = mv_table[xy - 1][0];
            P_LEFT[1] = mv_table[xy - 1][1];
            if(P_LEFT[0]       > (c->xmax<<1)) P_LEFT[0]       = (c->xmax<<1);

            c->pred_x= P_LEFT[0];
            c->pred_y= P_LEFT[1];

            if (!s->c.first_slice_line) {
                P_TOP[0]      = mv_table[xy - mot_stride][0];
                P_TOP[1]      = mv_table[xy - mot_stride][1];
                P_TOPRIGHT[0] = mv_table[xy - mot_stride + 1][0];
                P_TOPRIGHT[1] = mv_table[xy - mot_stride + 1][1];
                if(P_TOP[1]      > (c->ymax<<1)) P_TOP[1]     = (c->ymax<<1);
                if (P_TOPRIGHT[0] < c->xmin * (1 << 1)) P_TOPRIGHT[0] = c->xmin * (1 << 1);
                if(P_TOPRIGHT[0] > (c->xmax<<1)) P_TOPRIGHT[0]= (c->xmax<<1);
                if(P_TOPRIGHT[1] > (c->ymax<<1)) P_TOPRIGHT[1]= (c->ymax<<1);

                P_MEDIAN[0]= mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
                P_MEDIAN[1]= mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);
            }
            P_MV1[0]= mx; //FIXME not correct if block != field_select
            P_MV1[1]= my / 2;

            dmin = epzs_motion_search2(s, &mx_i, &my_i, P, block, field_select+ref_index, mv_table, (1<<16)>>1, 0);

            dmin= c->sub_motion_search(s, &mx_i, &my_i, dmin, block, field_select+ref_index, size, h);

            mv_table[xy][0]= mx_i;
            mv_table[xy][1]= my_i;

            if (c->me_sub_cmp[0] != c->mb_cmp[0]) {
                int dxy;

                //FIXME chroma ME
                const uint8_t *ref = c->ref[field_select+ref_index][0] + (mx_i>>1) + (my_i>>1)*stride;
                dxy = ((my_i & 1) << 1) | (mx_i & 1);

                c->hpel_put[size][dxy](c->scratchpad, ref, stride, h);
                dmin = c->mb_cmp[size](s, c->src[block][0], c->scratchpad, stride, h);
                dmin+= (mv_penalty[mx_i-c->pred_x] + mv_penalty[my_i-c->pred_y] + 1)*c->mb_penalty_factor;
            }else
                dmin+= c->mb_penalty_factor; //field_select bits

            dmin += field_select != block; //slightly prefer same field

            if(dmin < best_dmin){
                best_dmin= dmin;
                best_field= field_select;
            }
        }
        {
            int16_t (*mv_table)[2]= mv_tables[block][best_field];

            if(mv_table[xy][0] != mx) same=0; //FIXME check if these checks work and are any good at all
            if(mv_table[xy][1]&1) same=0;
            if(mv_table[xy][1]*2 != my) same=0;
            if(best_field != block) same=0;
        }

        field_select_tables[block][xy]= best_field;
        dmin_sum += best_dmin;
    }

    c->ymin *= 2;
    c->ymax<<=1;
    c->stride>>=1;
    c->uvstride>>=1;

    if(same)
        return INT_MAX;

    switch(c->avctx->mb_cmp&0xFF){
    /*case FF_CMP_SSE:
        return dmin_sum+ 32*s->c.qscale*s->c.qscale;*/
    case FF_CMP_RD:
        return dmin_sum;
    default:
        return dmin_sum+ 11*c->mb_penalty_factor;
    }
}

static inline int get_penalty_factor(int lambda, int lambda2, int type){
    switch(type&0xFF){
    default:
    case FF_CMP_SAD:
        return lambda>>FF_LAMBDA_SHIFT;
    case FF_CMP_DCT:
        return (3*lambda)>>(FF_LAMBDA_SHIFT+1);
    case FF_CMP_W53:
        return (4*lambda)>>(FF_LAMBDA_SHIFT);
    case FF_CMP_W97:
        return (2*lambda)>>(FF_LAMBDA_SHIFT);
    case FF_CMP_SATD:
    case FF_CMP_DCT264:
        return (2*lambda)>>FF_LAMBDA_SHIFT;
    case FF_CMP_RD:
    case FF_CMP_PSNR:
    case FF_CMP_SSE:
    case FF_CMP_NSSE:
        return lambda2>>FF_LAMBDA_SHIFT;
    case FF_CMP_BIT:
    case FF_CMP_MEDIAN_SAD:
        return 1;
    }
}

void ff_estimate_p_frame_motion(MPVEncContext *const s,
                                int mb_x, int mb_y)
{
    MotionEstContext *const c = &s->me;
    const uint8_t *pix, *ppix;
    int sum, mx = 0, my = 0, dmin = 0;
    int varc;            ///< the variance of the block (sum of squared (p[y][x]-average))
    int vard;            ///< sum of squared differences with the estimated motion vector
    int P[10][2];
    const int shift = 1 + s->c.quarter_sample;
    int mb_type=0;

    init_ref(c, s->new_pic->data, s->c.last_pic.data, NULL, 16*mb_x, 16*mb_y, 0);

    av_assert0(s->c.quarter_sample == 0 || s->c.quarter_sample == 1);
    av_assert0(s->c.linesize == c->stride);
    av_assert0(s->c.uvlinesize == c->uvstride);

    c->penalty_factor     = get_penalty_factor(s->lambda, s->lambda2, c->avctx->me_cmp);
    c->sub_penalty_factor = get_penalty_factor(s->lambda, s->lambda2, c->avctx->me_sub_cmp);
    c->mb_penalty_factor  = get_penalty_factor(s->lambda, s->lambda2, c->avctx->mb_cmp);
    c->current_mv_penalty = c->mv_penalty[s->f_code] + MAX_DMV;

    get_limits(s, 16*mb_x, 16*mb_y, 0);
    c->skip=0;

    /* intra / predictive decision */
    pix = c->src[0][0];
    sum  = s->mpvencdsp.pix_sum(pix, s->c.linesize);
    varc = s->mpvencdsp.pix_norm1(pix, s->c.linesize) -
           (((unsigned) sum * sum) >> 8) + 500;

    s->mb_mean[s->c.mb_stride * mb_y + mb_x] = (sum  + 128) >> 8;
    s->mb_var [s->c.mb_stride * mb_y + mb_x] = (varc + 128) >> 8;
    c->mb_var_sum_temp += (varc+128)>>8;

    if (c->motion_est != FF_ME_ZERO) {
        const int mot_stride = s->c.b8_stride;
        const int mot_xy = s->c.block_index[0];

        P_LEFT[0] = s->c.cur_pic.motion_val[0][mot_xy - 1][0];
        P_LEFT[1] = s->c.cur_pic.motion_val[0][mot_xy - 1][1];

        if (P_LEFT[0] > (c->xmax << shift))
            P_LEFT[0] =  c->xmax << shift;

        if (!s->c.first_slice_line) {
            P_TOP[0]      = s->c.cur_pic.motion_val[0][mot_xy - mot_stride    ][0];
            P_TOP[1]      = s->c.cur_pic.motion_val[0][mot_xy - mot_stride    ][1];
            P_TOPRIGHT[0] = s->c.cur_pic.motion_val[0][mot_xy - mot_stride + 2][0];
            P_TOPRIGHT[1] = s->c.cur_pic.motion_val[0][mot_xy - mot_stride + 2][1];
            if (P_TOP[1] > (c->ymax << shift))
                P_TOP[1] =  c->ymax << shift;
            if (P_TOPRIGHT[0] < (c->xmin * (1 << shift)))
                P_TOPRIGHT[0] =  c->xmin * (1 << shift);
            if (P_TOPRIGHT[1] > (c->ymax * (1 << shift)))
                P_TOPRIGHT[1] =  c->ymax * (1 << shift);

            P_MEDIAN[0] = mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
            P_MEDIAN[1] = mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);

            if (s->c.out_format == FMT_H263) {
                c->pred_x = P_MEDIAN[0];
                c->pred_y = P_MEDIAN[1];
            } else { /* MPEG-1 at least */
                c->pred_x = P_LEFT[0];
                c->pred_y = P_LEFT[1];
            }
        } else {
            c->pred_x = P_LEFT[0];
            c->pred_y = P_LEFT[1];
        }
        dmin = ff_epzs_motion_search(s, &mx, &my, P, 0, 0, s->p_mv_table, (1<<16)>>shift, 0, 16);
    }

    /* At this point (mx,my) are full-pell and the relative displacement */
    ppix = c->ref[0][0] + (my * s->c.linesize) + mx;

    vard = c->sse(NULL, pix, ppix, s->c.linesize, 16);

    s->mc_mb_var[s->c.mb_stride * mb_y + mb_x] = (vard+128)>>8;
    c->mc_mb_var_sum_temp += (vard+128)>>8;

    if (c->avctx->mb_decision > FF_MB_DECISION_SIMPLE) {
        int p_score = FFMIN(vard, varc - 500 + (s->lambda2 >> FF_LAMBDA_SHIFT)*100);
        int i_score = varc - 500 + (s->lambda2 >> FF_LAMBDA_SHIFT)*20;
        c->scene_change_score+= ff_sqrt(p_score) - ff_sqrt(i_score);

        if (vard*2 + 200*256 > varc && !s->intra_penalty)
            mb_type|= CANDIDATE_MB_TYPE_INTRA;
        if (varc*2 + 200*256 > vard || s->c.qscale > 24){
//        if (varc*2 + 200*256 + 50*(s->lambda2>>FF_LAMBDA_SHIFT) > vard){
            mb_type|= CANDIDATE_MB_TYPE_INTER;
            c->sub_motion_search(s, &mx, &my, dmin, 0, 0, 0, 16);
            if (s->mpv_flags & FF_MPV_FLAG_MV0)
                if(mx || my)
                    mb_type |= CANDIDATE_MB_TYPE_SKIPPED; //FIXME check difference
        }else{
            mx *= 1 << shift;
            my *= 1 << shift;
        }
        if ((c->avctx->flags & AV_CODEC_FLAG_4MV)
           && !c->skip && varc>50<<8 && vard>10<<8){
            if(h263_mv4_search(s, mx, my, shift) < INT_MAX)
                mb_type|=CANDIDATE_MB_TYPE_INTER4V;

            set_p_mv_tables(s, mx, my, 0);
        }else
            set_p_mv_tables(s, mx, my, 1);
        if ((c->avctx->flags & AV_CODEC_FLAG_INTERLACED_ME)
           && !c->skip){ //FIXME varc/d checks
            if(interlaced_search(s, 0, s->c.p_field_mv_table, s->p_field_select_table, mx, my, 0) < INT_MAX)
                mb_type |= CANDIDATE_MB_TYPE_INTER_I;
        }
    }else{
        int intra_score, i;
        mb_type= CANDIDATE_MB_TYPE_INTER;

        dmin= c->sub_motion_search(s, &mx, &my, dmin, 0, 0, 0, 16);
        if(c->avctx->me_sub_cmp != c->avctx->mb_cmp && !c->skip)
            dmin= get_mb_score(s, mx, my, 0, 0, 0, 16, 1);

        if ((c->avctx->flags & AV_CODEC_FLAG_4MV)
           && !c->skip && varc>50<<8 && vard>10<<8){
            int dmin4= h263_mv4_search(s, mx, my, shift);
            if(dmin4 < dmin){
                mb_type= CANDIDATE_MB_TYPE_INTER4V;
                dmin=dmin4;
            }
        }
        if ((c->avctx->flags & AV_CODEC_FLAG_INTERLACED_ME)
           && !c->skip){ //FIXME varc/d checks
            int dmin_i= interlaced_search(s, 0, s->c.p_field_mv_table, s->p_field_select_table, mx, my, 0);
            if(dmin_i < dmin){
                mb_type = CANDIDATE_MB_TYPE_INTER_I;
                dmin= dmin_i;
            }
        }

        set_p_mv_tables(s, mx, my, mb_type!=CANDIDATE_MB_TYPE_INTER4V);

        /* get intra luma score */
        if((c->avctx->mb_cmp&0xFF)==FF_CMP_SSE){
            intra_score= varc - 500;
        }else{
            unsigned mean = (sum+128)>>8;
            mean*= 0x01010101;

            for(i=0; i<16; i++){
                *(uint32_t*)(&c->scratchpad[i*s->c.linesize+ 0]) = mean;
                *(uint32_t*)(&c->scratchpad[i*s->c.linesize+ 4]) = mean;
                *(uint32_t*)(&c->scratchpad[i*s->c.linesize+ 8]) = mean;
                *(uint32_t*)(&c->scratchpad[i*s->c.linesize+12]) = mean;
            }

            intra_score= c->mb_cmp[0](s, c->scratchpad, pix, s->c.linesize, 16);
        }
        intra_score += c->mb_penalty_factor*16 + s->intra_penalty;

        if(intra_score < dmin){
            mb_type= CANDIDATE_MB_TYPE_INTRA;
            s->c.cur_pic.mb_type[mb_y*s->c.mb_stride + mb_x] = CANDIDATE_MB_TYPE_INTRA; //FIXME cleanup
        }else
            s->c.cur_pic.mb_type[mb_y*s->c.mb_stride + mb_x] = 0;

        {
            int p_score = FFMIN(vard, varc-500+(s->lambda2>>FF_LAMBDA_SHIFT)*100);
            int i_score = varc-500+(s->lambda2>>FF_LAMBDA_SHIFT)*20;
            c->scene_change_score+= ff_sqrt(p_score) - ff_sqrt(i_score);
        }
    }

    s->mb_type[mb_y*s->c.mb_stride + mb_x] = mb_type;
}

int ff_pre_estimate_p_frame_motion(MPVEncContext *const s,
                                    int mb_x, int mb_y)
{
    MotionEstContext *const c = &s->me;
    int mx, my, dmin;
    int P[10][2];
    const int shift = 1 + s->c.quarter_sample;
    const int xy    = mb_x + mb_y*s->c.mb_stride;
    init_ref(c, s->new_pic->data, s->c.last_pic.data, NULL, 16*mb_x, 16*mb_y, 0);

    av_assert0(s->c.quarter_sample == 0 || s->c.quarter_sample == 1);

    c->pre_penalty_factor = get_penalty_factor(s->lambda, s->lambda2, c->avctx->me_pre_cmp);
    c->current_mv_penalty = c->mv_penalty[s->f_code] + MAX_DMV;

    get_limits(s, 16*mb_x, 16*mb_y, 0);
    c->skip=0;

    P_LEFT[0]       = s->p_mv_table[xy + 1][0];
    P_LEFT[1]       = s->p_mv_table[xy + 1][1];

    if(P_LEFT[0]       < (c->xmin<<shift)) P_LEFT[0]       = (c->xmin<<shift);

    /* special case for first line */
    if (s->c.first_slice_line) {
        c->pred_x= P_LEFT[0];
        c->pred_y= P_LEFT[1];
        P_TOP[0]= P_TOPRIGHT[0]= P_MEDIAN[0]=
        P_TOP[1]= P_TOPRIGHT[1]= P_MEDIAN[1]= 0; //FIXME
    } else {
        P_TOP[0]      = s->p_mv_table[xy + s->c.mb_stride    ][0];
        P_TOP[1]      = s->p_mv_table[xy + s->c.mb_stride    ][1];
        P_TOPRIGHT[0] = s->p_mv_table[xy + s->c.mb_stride - 1][0];
        P_TOPRIGHT[1] = s->p_mv_table[xy + s->c.mb_stride - 1][1];
        if(P_TOP[1]      < (c->ymin<<shift)) P_TOP[1]     = (c->ymin<<shift);
        if(P_TOPRIGHT[0] > (c->xmax<<shift)) P_TOPRIGHT[0]= (c->xmax<<shift);
        if(P_TOPRIGHT[1] < (c->ymin<<shift)) P_TOPRIGHT[1]= (c->ymin<<shift);

        P_MEDIAN[0]= mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
        P_MEDIAN[1]= mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);

        c->pred_x = P_MEDIAN[0];
        c->pred_y = P_MEDIAN[1];
    }

    dmin = ff_epzs_motion_search(s, &mx, &my, P, 0, 0, s->p_mv_table, (1<<16)>>shift, 0, 16);

    s->p_mv_table[xy][0] = mx<<shift;
    s->p_mv_table[xy][1] = my<<shift;

    return dmin;
}

static int estimate_motion_b(MPVEncContext *const s, int mb_x, int mb_y,
                             int16_t (*mv_table)[2], int ref_index, int f_code)
{
    MotionEstContext *const c = &s->me;
    int mx = 0, my = 0, dmin = 0;
    int P[10][2];
    const int shift= 1+s->c.quarter_sample;
    const int mot_stride = s->c.mb_stride;
    const int mot_xy = mb_y*mot_stride + mb_x;
    const uint8_t * const mv_penalty = c->mv_penalty[f_code] + MAX_DMV;
    int mv_scale;

    c->current_mv_penalty= mv_penalty;

    get_limits(s, 16*mb_x, 16*mb_y, 1);

    if (c->motion_est != FF_ME_ZERO) {
        P_LEFT[0] = mv_table[mot_xy - 1][0];
        P_LEFT[1] = mv_table[mot_xy - 1][1];

        if (P_LEFT[0] > (c->xmax << shift)) P_LEFT[0] = (c->xmax << shift);

        /* special case for first line */
        if (!s->c.first_slice_line) {
            P_TOP[0]      = mv_table[mot_xy - mot_stride    ][0];
            P_TOP[1]      = mv_table[mot_xy - mot_stride    ][1];
            P_TOPRIGHT[0] = mv_table[mot_xy - mot_stride + 1][0];
            P_TOPRIGHT[1] = mv_table[mot_xy - mot_stride + 1][1];
            if (P_TOP[1] > (c->ymax << shift)) P_TOP[1] = (c->ymax << shift);
            if (P_TOPRIGHT[0] < c->xmin * (1 << shift)) P_TOPRIGHT[0] = c->xmin * (1 << shift);
            if (P_TOPRIGHT[1] > (c->ymax << shift)) P_TOPRIGHT[1] = (c->ymax << shift);

            P_MEDIAN[0] = mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
            P_MEDIAN[1] = mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);
        }
        c->pred_x = P_LEFT[0];
        c->pred_y = P_LEFT[1];

        if(mv_table == s->b_forw_mv_table){
            mv_scale= (s->c.pb_time<<16) / (s->c.pp_time<<shift);
        }else{
            mv_scale = ((s->c.pb_time - s->c.pp_time) * (1 << 16)) / (s->c.pp_time<<shift);
        }

        dmin = ff_epzs_motion_search(s, &mx, &my, P, 0, ref_index, s->p_mv_table, mv_scale, 0, 16);
    }

    dmin= c->sub_motion_search(s, &mx, &my, dmin, 0, ref_index, 0, 16);

    if(c->avctx->me_sub_cmp != c->avctx->mb_cmp && !c->skip)
        dmin= get_mb_score(s, mx, my, 0, ref_index, 0, 16, 1);

//    s->mb_type[mb_y*s->c.mb_width + mb_x]= mb_type;
    mv_table[mot_xy][0]= mx;
    mv_table[mot_xy][1]= my;

    return dmin;
}

static inline int check_bidir_mv(MPVEncContext *const s,
                   int motion_fx, int motion_fy,
                   int motion_bx, int motion_by,
                   int pred_fx, int pred_fy,
                   int pred_bx, int pred_by,
                   int size, int h)
{
    //FIXME optimize?
    //FIXME better f_code prediction (max mv & distance)
    //FIXME pointers
    MotionEstContext *const c = &s->me;
    const uint8_t * const mv_penalty_f = c->mv_penalty[s->f_code] + MAX_DMV; // f_code of the prev frame
    const uint8_t * const mv_penalty_b = c->mv_penalty[s->b_code] + MAX_DMV; // f_code of the prev frame
    int stride= c->stride;
    uint8_t *dest_y = c->scratchpad;
    const uint8_t *ptr;
    int dxy;
    int src_x, src_y;
    int fbmin;
    const uint8_t *const *src_data  = c->src[0];
    const uint8_t *const *ref_data  = c->ref[0];
    const uint8_t *const *ref2_data = c->ref[2];

    if(s->c.quarter_sample){
        dxy = ((motion_fy & 3) << 2) | (motion_fx & 3);
        src_x = motion_fx >> 2;
        src_y = motion_fy >> 2;

        ptr = ref_data[0] + (src_y * stride) + src_x;
        s->c.qdsp.put_qpel_pixels_tab[0][dxy](dest_y, ptr, stride);

        dxy = ((motion_by & 3) << 2) | (motion_bx & 3);
        src_x = motion_bx >> 2;
        src_y = motion_by >> 2;

        ptr = ref2_data[0] + (src_y * stride) + src_x;
        s->c.qdsp.avg_qpel_pixels_tab[size][dxy](dest_y, ptr, stride);
    }else{
        dxy = ((motion_fy & 1) << 1) | (motion_fx & 1);
        src_x = motion_fx >> 1;
        src_y = motion_fy >> 1;

        ptr = ref_data[0] + (src_y * stride) + src_x;
        s->c.hdsp.put_pixels_tab[size][dxy](dest_y    , ptr    , stride, h);

        dxy = ((motion_by & 1) << 1) | (motion_bx & 1);
        src_x = motion_bx >> 1;
        src_y = motion_by >> 1;

        ptr = ref2_data[0] + (src_y * stride) + src_x;
        s->c.hdsp.avg_pixels_tab[size][dxy](dest_y    , ptr    , stride, h);
    }

    fbmin = (mv_penalty_f[motion_fx-pred_fx] + mv_penalty_f[motion_fy-pred_fy])*c->mb_penalty_factor
           +(mv_penalty_b[motion_bx-pred_bx] + mv_penalty_b[motion_by-pred_by])*c->mb_penalty_factor
           + c->mb_cmp[size](s, src_data[0], dest_y, stride, h); // FIXME new_pic

    if(c->avctx->mb_cmp&FF_CMP_CHROMA){
    }
    //FIXME CHROMA !!!

    return fbmin;
}

/* refine the bidir vectors in hq mode and return the score in both lq & hq mode*/
static inline int bidir_refine(MPVEncContext *const s, int mb_x, int mb_y)
{
    MotionEstContext *const c = &s->me;
    const int mot_stride = s->c.mb_stride;
    const int xy = mb_y *mot_stride + mb_x;
    int fbmin;
    int pred_fx= s->b_bidir_forw_mv_table[xy-1][0];
    int pred_fy= s->b_bidir_forw_mv_table[xy-1][1];
    int pred_bx= s->b_bidir_back_mv_table[xy-1][0];
    int pred_by= s->b_bidir_back_mv_table[xy-1][1];
    int motion_fx= s->b_bidir_forw_mv_table[xy][0]= s->b_forw_mv_table[xy][0];
    int motion_fy= s->b_bidir_forw_mv_table[xy][1]= s->b_forw_mv_table[xy][1];
    int motion_bx= s->b_bidir_back_mv_table[xy][0]= s->b_back_mv_table[xy][0];
    int motion_by= s->b_bidir_back_mv_table[xy][1]= s->b_back_mv_table[xy][1];
    const int flags= c->sub_flags;
    const int qpel= flags&FLAG_QPEL;
    const int shift= 1+qpel;
    const int xmin= c->xmin * (1 << shift);
    const int ymin= c->ymin * (1 << shift);
    const int xmax= c->xmax<<shift;
    const int ymax= c->ymax<<shift;
#define HASH(fx,fy,bx,by) ((fx)+17*(fy)+63*(bx)+117*(by))
#define HASH8(fx,fy,bx,by) ((uint8_t)HASH(fx,fy,bx,by))
    int hashidx= HASH(motion_fx,motion_fy, motion_bx, motion_by);
    uint8_t map[256] = { 0 };

    map[hashidx&255] = 1;

    fbmin= check_bidir_mv(s, motion_fx, motion_fy,
                          motion_bx, motion_by,
                          pred_fx, pred_fy,
                          pred_bx, pred_by,
                          0, 16);

    if (c->avctx->bidir_refine) {
        int end;
        static const uint8_t limittab[5]={0,8,32,64,80};
        const int limit = limittab[c->avctx->bidir_refine];
        static const int8_t vect[][4]={
{ 0, 0, 0, 1}, { 0, 0, 0,-1}, { 0, 0, 1, 0}, { 0, 0,-1, 0}, { 0, 1, 0, 0}, { 0,-1, 0, 0}, { 1, 0, 0, 0}, {-1, 0, 0, 0},

{ 0, 0, 1, 1}, { 0, 0,-1,-1}, { 0, 1, 1, 0}, { 0,-1,-1, 0}, { 1, 1, 0, 0}, {-1,-1, 0, 0}, { 1, 0, 0, 1}, {-1, 0, 0,-1},
{ 0, 1, 0, 1}, { 0,-1, 0,-1}, { 1, 0, 1, 0}, {-1, 0,-1, 0},
{ 0, 0,-1, 1}, { 0, 0, 1,-1}, { 0,-1, 1, 0}, { 0, 1,-1, 0}, {-1, 1, 0, 0}, { 1,-1, 0, 0}, { 1, 0, 0,-1}, {-1, 0, 0, 1},
{ 0,-1, 0, 1}, { 0, 1, 0,-1}, {-1, 0, 1, 0}, { 1, 0,-1, 0},

{ 0, 1, 1, 1}, { 0,-1,-1,-1}, { 1, 1, 1, 0}, {-1,-1,-1, 0}, { 1, 1, 0, 1}, {-1,-1, 0,-1}, { 1, 0, 1, 1}, {-1, 0,-1,-1},
{ 0,-1, 1, 1}, { 0, 1,-1,-1}, {-1, 1, 1, 0}, { 1,-1,-1, 0}, { 1, 1, 0,-1}, {-1,-1, 0, 1}, { 1, 0,-1, 1}, {-1, 0, 1,-1},
{ 0, 1,-1, 1}, { 0,-1, 1,-1}, { 1,-1, 1, 0}, {-1, 1,-1, 0}, {-1, 1, 0, 1}, { 1,-1, 0,-1}, { 1, 0, 1,-1}, {-1, 0,-1, 1},
{ 0, 1, 1,-1}, { 0,-1,-1, 1}, { 1, 1,-1, 0}, {-1,-1, 1, 0}, { 1,-1, 0, 1}, {-1, 1, 0,-1}, {-1, 0, 1, 1}, { 1, 0,-1,-1},

{ 1, 1, 1, 1}, {-1,-1,-1,-1},
{ 1, 1, 1,-1}, {-1,-1,-1, 1}, { 1, 1,-1, 1}, {-1,-1, 1,-1}, { 1,-1, 1, 1}, {-1, 1,-1,-1}, {-1, 1, 1, 1}, { 1,-1,-1,-1},
{ 1, 1,-1,-1}, {-1,-1, 1, 1}, { 1,-1,-1, 1}, {-1, 1, 1,-1}, { 1,-1, 1,-1}, {-1, 1,-1, 1},
        };
        static const uint8_t hash[]={
HASH8( 0, 0, 0, 1), HASH8( 0, 0, 0,-1), HASH8( 0, 0, 1, 0), HASH8( 0, 0,-1, 0), HASH8( 0, 1, 0, 0), HASH8( 0,-1, 0, 0), HASH8( 1, 0, 0, 0), HASH8(-1, 0, 0, 0),

HASH8( 0, 0, 1, 1), HASH8( 0, 0,-1,-1), HASH8( 0, 1, 1, 0), HASH8( 0,-1,-1, 0), HASH8( 1, 1, 0, 0), HASH8(-1,-1, 0, 0), HASH8( 1, 0, 0, 1), HASH8(-1, 0, 0,-1),
HASH8( 0, 1, 0, 1), HASH8( 0,-1, 0,-1), HASH8( 1, 0, 1, 0), HASH8(-1, 0,-1, 0),
HASH8( 0, 0,-1, 1), HASH8( 0, 0, 1,-1), HASH8( 0,-1, 1, 0), HASH8( 0, 1,-1, 0), HASH8(-1, 1, 0, 0), HASH8( 1,-1, 0, 0), HASH8( 1, 0, 0,-1), HASH8(-1, 0, 0, 1),
HASH8( 0,-1, 0, 1), HASH8( 0, 1, 0,-1), HASH8(-1, 0, 1, 0), HASH8( 1, 0,-1, 0),

HASH8( 0, 1, 1, 1), HASH8( 0,-1,-1,-1), HASH8( 1, 1, 1, 0), HASH8(-1,-1,-1, 0), HASH8( 1, 1, 0, 1), HASH8(-1,-1, 0,-1), HASH8( 1, 0, 1, 1), HASH8(-1, 0,-1,-1),
HASH8( 0,-1, 1, 1), HASH8( 0, 1,-1,-1), HASH8(-1, 1, 1, 0), HASH8( 1,-1,-1, 0), HASH8( 1, 1, 0,-1), HASH8(-1,-1, 0, 1), HASH8( 1, 0,-1, 1), HASH8(-1, 0, 1,-1),
HASH8( 0, 1,-1, 1), HASH8( 0,-1, 1,-1), HASH8( 1,-1, 1, 0), HASH8(-1, 1,-1, 0), HASH8(-1, 1, 0, 1), HASH8( 1,-1, 0,-1), HASH8( 1, 0, 1,-1), HASH8(-1, 0,-1, 1),
HASH8( 0, 1, 1,-1), HASH8( 0,-1,-1, 1), HASH8( 1, 1,-1, 0), HASH8(-1,-1, 1, 0), HASH8( 1,-1, 0, 1), HASH8(-1, 1, 0,-1), HASH8(-1, 0, 1, 1), HASH8( 1, 0,-1,-1),

HASH8( 1, 1, 1, 1), HASH8(-1,-1,-1,-1),
HASH8( 1, 1, 1,-1), HASH8(-1,-1,-1, 1), HASH8( 1, 1,-1, 1), HASH8(-1,-1, 1,-1), HASH8( 1,-1, 1, 1), HASH8(-1, 1,-1,-1), HASH8(-1, 1, 1, 1), HASH8( 1,-1,-1,-1),
HASH8( 1, 1,-1,-1), HASH8(-1,-1, 1, 1), HASH8( 1,-1,-1, 1), HASH8(-1, 1, 1,-1), HASH8( 1,-1, 1,-1), HASH8(-1, 1,-1, 1),
};

#define CHECK_BIDIR(fx,fy,bx,by)\
    if( !map[(hashidx+HASH(fx,fy,bx,by))&255]\
       &&(fx<=0 || motion_fx+fx<=xmax) && (fy<=0 || motion_fy+fy<=ymax) && (bx<=0 || motion_bx+bx<=xmax) && (by<=0 || motion_by+by<=ymax)\
       &&(fx>=0 || motion_fx+fx>=xmin) && (fy>=0 || motion_fy+fy>=ymin) && (bx>=0 || motion_bx+bx>=xmin) && (by>=0 || motion_by+by>=ymin)){\
        int score;\
        map[(hashidx+HASH(fx,fy,bx,by))&255] = 1;\
        score= check_bidir_mv(s, motion_fx+fx, motion_fy+fy, motion_bx+bx, motion_by+by, pred_fx, pred_fy, pred_bx, pred_by, 0, 16);\
        if(score < fbmin){\
            hashidx += HASH(fx,fy,bx,by);\
            fbmin= score;\
            motion_fx+=fx;\
            motion_fy+=fy;\
            motion_bx+=bx;\
            motion_by+=by;\
            end=0;\
        }\
    }
#define CHECK_BIDIR2(a,b,c,d)\
CHECK_BIDIR(a,b,c,d)\
CHECK_BIDIR(-(a),-(b),-(c),-(d))

        do{
            int i;
            int borderdist=0;
            end=1;

            CHECK_BIDIR2(0,0,0,1)
            CHECK_BIDIR2(0,0,1,0)
            CHECK_BIDIR2(0,1,0,0)
            CHECK_BIDIR2(1,0,0,0)

            for(i=8; i<limit; i++){
                int fx= motion_fx+vect[i][0];
                int fy= motion_fy+vect[i][1];
                int bx= motion_bx+vect[i][2];
                int by= motion_by+vect[i][3];
                if(borderdist<=0){
                    int a= (xmax - FFMAX(fx,bx))|(FFMIN(fx,bx) - xmin);
                    int b= (ymax - FFMAX(fy,by))|(FFMIN(fy,by) - ymin);
                    if((a|b) < 0)
                        map[(hashidx+hash[i])&255] = 1;
                }
                if(!map[(hashidx+hash[i])&255]){
                    int score;
                    map[(hashidx+hash[i])&255] = 1;
                    score= check_bidir_mv(s, fx, fy, bx, by, pred_fx, pred_fy, pred_bx, pred_by, 0, 16);
                    if(score < fbmin){
                        hashidx += hash[i];
                        fbmin= score;
                        motion_fx=fx;
                        motion_fy=fy;
                        motion_bx=bx;
                        motion_by=by;
                        end=0;
                        borderdist--;
                        if(borderdist<=0){
                            int a= FFMIN(xmax - FFMAX(fx,bx), FFMIN(fx,bx) - xmin);
                            int b= FFMIN(ymax - FFMAX(fy,by), FFMIN(fy,by) - ymin);
                            borderdist= FFMIN(a,b);
                        }
                    }
                }
            }
        }while(!end);
    }

    s->b_bidir_forw_mv_table[xy][0]= motion_fx;
    s->b_bidir_forw_mv_table[xy][1]= motion_fy;
    s->b_bidir_back_mv_table[xy][0]= motion_bx;
    s->b_bidir_back_mv_table[xy][1]= motion_by;

    return fbmin;
}

static inline int direct_search(MPVEncContext *const s, int mb_x, int mb_y)
{
    MotionEstContext *const c = &s->me;
    int P[10][2];
    const int mot_stride = s->c.mb_stride;
    const int mot_xy = mb_y*mot_stride + mb_x;
    const int shift= 1+s->c.quarter_sample;
    int dmin, i;
    const int time_pp= s->c.pp_time;
    const int time_pb= s->c.pb_time;
    int mx, my, xmin, xmax, ymin, ymax;
    int16_t (*mv_table)[2]= s->b_direct_mv_table;

    c->current_mv_penalty= c->mv_penalty[1] + MAX_DMV;
    ymin= xmin=(-32)>>shift;
    ymax= xmax=   31>>shift;

    if (IS_8X8(s->c.next_pic.mb_type[mot_xy])) {
        s->c.mv_type = MV_TYPE_8X8;
    }else{
        s->c.mv_type = MV_TYPE_16X16;
    }

    for(i=0; i<4; i++){
        int index= s->c.block_index[i];
        int min, max;

        c->co_located_mv[i][0] = s->c.next_pic.motion_val[0][index][0];
        c->co_located_mv[i][1] = s->c.next_pic.motion_val[0][index][1];
        c->direct_basis_mv[i][0]= c->co_located_mv[i][0]*time_pb/time_pp + ((i& 1)<<(shift+3));
        c->direct_basis_mv[i][1]= c->co_located_mv[i][1]*time_pb/time_pp + ((i>>1)<<(shift+3));
//        c->direct_basis_mv[1][i][0]= c->co_located_mv[i][0]*(time_pb - time_pp)/time_pp + ((i &1)<<(shift+3);
//        c->direct_basis_mv[1][i][1]= c->co_located_mv[i][1]*(time_pb - time_pp)/time_pp + ((i>>1)<<(shift+3);

        max= FFMAX(c->direct_basis_mv[i][0], c->direct_basis_mv[i][0] - c->co_located_mv[i][0])>>shift;
        min= FFMIN(c->direct_basis_mv[i][0], c->direct_basis_mv[i][0] - c->co_located_mv[i][0])>>shift;
        max+= 16*mb_x + 1; // +-1 is for the simpler rounding
        min+= 16*mb_x - 1;
        xmax= FFMIN(xmax, s->c.width - max);
        xmin= FFMAX(xmin, - 16     - min);

        max= FFMAX(c->direct_basis_mv[i][1], c->direct_basis_mv[i][1] - c->co_located_mv[i][1])>>shift;
        min= FFMIN(c->direct_basis_mv[i][1], c->direct_basis_mv[i][1] - c->co_located_mv[i][1])>>shift;
        max+= 16*mb_y + 1; // +-1 is for the simpler rounding
        min+= 16*mb_y - 1;
        ymax= FFMIN(ymax, s->c.height - max);
        ymin= FFMAX(ymin, - 16      - min);

        if(s->c.mv_type == MV_TYPE_16X16) break;
    }

    av_assert2(xmax <= 15 && ymax <= 15 && xmin >= -16 && ymin >= -16);

    if(xmax < 0 || xmin >0 || ymax < 0 || ymin > 0){
        s->b_direct_mv_table[mot_xy][0]= 0;
        s->b_direct_mv_table[mot_xy][1]= 0;

        return 256*256*256*64-1;
    }

    c->xmin= xmin;
    c->ymin= ymin;
    c->xmax= xmax;
    c->ymax= ymax;
    c->flags     |= FLAG_DIRECT;
    c->sub_flags |= FLAG_DIRECT;
    c->pred_x=0;
    c->pred_y=0;

    P_LEFT[0] = av_clip(mv_table[mot_xy - 1][0], xmin * (1 << shift), xmax << shift);
    P_LEFT[1] = av_clip(mv_table[mot_xy - 1][1], ymin * (1 << shift), ymax << shift);

    /* special case for first line */
    if (!s->c.first_slice_line) { //FIXME maybe allow this over thread boundary as it is clipped
        P_TOP[0]      = av_clip(mv_table[mot_xy - mot_stride    ][0], xmin * (1 << shift), xmax << shift);
        P_TOP[1]      = av_clip(mv_table[mot_xy - mot_stride    ][1], ymin * (1 << shift), ymax << shift);
        P_TOPRIGHT[0] = av_clip(mv_table[mot_xy - mot_stride + 1][0], xmin * (1 << shift), xmax << shift);
        P_TOPRIGHT[1] = av_clip(mv_table[mot_xy - mot_stride + 1][1], ymin * (1 << shift), ymax << shift);

        P_MEDIAN[0]= mid_pred(P_LEFT[0], P_TOP[0], P_TOPRIGHT[0]);
        P_MEDIAN[1]= mid_pred(P_LEFT[1], P_TOP[1], P_TOPRIGHT[1]);
    }

    dmin = ff_epzs_motion_search(s, &mx, &my, P, 0, 0, mv_table, 1<<(16-shift), 0, 16);
    if(c->sub_flags&FLAG_QPEL)
        dmin = qpel_motion_search(s, &mx, &my, dmin, 0, 0, 0, 16);
    else
        dmin = hpel_motion_search(s, &mx, &my, dmin, 0, 0, 0, 16);

    if(c->avctx->me_sub_cmp != c->avctx->mb_cmp && !c->skip)
        dmin= get_mb_score(s, mx, my, 0, 0, 0, 16, 1);

    get_limits(s, 16*mb_x, 16*mb_y, 1); //restore c->?min/max, maybe not needed

    mv_table[mot_xy][0]= mx;
    mv_table[mot_xy][1]= my;
    c->flags     &= ~FLAG_DIRECT;
    c->sub_flags &= ~FLAG_DIRECT;

    return dmin;
}

void ff_estimate_b_frame_motion(MPVEncContext *const s,
                             int mb_x, int mb_y)
{
    MotionEstContext *const c = &s->me;
    int fmin, bmin, dmin, fbmin, bimin, fimin;
    int type=0;
    const int xy = mb_y*s->c.mb_stride + mb_x;
    init_ref(c, s->new_pic->data, s->c.last_pic.data,
             s->c.next_pic.data, 16 * mb_x, 16 * mb_y, 2);

    get_limits(s, 16*mb_x, 16*mb_y, 1);

    c->skip=0;

    if (s->c.codec_id == AV_CODEC_ID_MPEG4 && s->c.next_pic.mbskip_table[xy]) {
        int score= direct_search(s, mb_x, mb_y); //FIXME just check 0,0

        score= ((unsigned)(score*score + 128*256))>>16;
        c->mc_mb_var_sum_temp += score;
        s->mc_mb_var[mb_y*s->c.mb_stride + mb_x] = score; //FIXME use SSE
        s->mb_type[mb_y*s->c.mb_stride + mb_x]= CANDIDATE_MB_TYPE_DIRECT0;

        return;
    }

    c->penalty_factor    = get_penalty_factor(s->lambda, s->lambda2, c->avctx->me_cmp);
    c->sub_penalty_factor= get_penalty_factor(s->lambda, s->lambda2, c->avctx->me_sub_cmp);
    c->mb_penalty_factor = get_penalty_factor(s->lambda, s->lambda2, c->avctx->mb_cmp);

    if (s->c.codec_id == AV_CODEC_ID_MPEG4)
        dmin= direct_search(s, mb_x, mb_y);
    else
        dmin= INT_MAX;

// FIXME penalty stuff for non-MPEG-4
    c->skip=0;
    fmin = estimate_motion_b(s, mb_x, mb_y, s->b_forw_mv_table, 0, s->f_code) +
           3 * c->mb_penalty_factor;

    c->skip=0;
    bmin = estimate_motion_b(s, mb_x, mb_y, s->b_back_mv_table, 2, s->b_code) +
           2 * c->mb_penalty_factor;
    ff_dlog(c->avctx, " %d %d ", s->b_forw_mv_table[xy][0], s->b_forw_mv_table[xy][1]);

    c->skip=0;
    fbmin= bidir_refine(s, mb_x, mb_y) + c->mb_penalty_factor;
    ff_dlog(c->avctx, "%d %d %d %d\n", dmin, fmin, bmin, fbmin);

    if (c->avctx->flags & AV_CODEC_FLAG_INTERLACED_ME) {
//FIXME mb type penalty
        c->skip=0;
        c->current_mv_penalty = c->mv_penalty[s->f_code] + MAX_DMV;
        fimin= interlaced_search(s, 0,
                                 s->b_field_mv_table[0], s->b_field_select_table[0],
                                 s->b_forw_mv_table[xy][0], s->b_forw_mv_table[xy][1], 0);
        c->current_mv_penalty = c->mv_penalty[s->b_code] + MAX_DMV;
        bimin= interlaced_search(s, 2,
                                 s->b_field_mv_table[1], s->b_field_select_table[1],
                                 s->b_back_mv_table[xy][0], s->b_back_mv_table[xy][1], 0);
    }else
        fimin= bimin= INT_MAX;

    {
        int score= fmin;
        type = CANDIDATE_MB_TYPE_FORWARD;

        if (dmin <= score){
            score = dmin;
            type = CANDIDATE_MB_TYPE_DIRECT;
        }
        if(bmin<score){
            score=bmin;
            type= CANDIDATE_MB_TYPE_BACKWARD;
        }
        if(fbmin<score){
            score=fbmin;
            type= CANDIDATE_MB_TYPE_BIDIR;
        }
        if(fimin<score){
            score=fimin;
            type= CANDIDATE_MB_TYPE_FORWARD_I;
        }
        if(bimin<score){
            score=bimin;
            type= CANDIDATE_MB_TYPE_BACKWARD_I;
        }

        score= ((unsigned)(score*score + 128*256))>>16;
        c->mc_mb_var_sum_temp += score;
        s->mc_mb_var[mb_y*s->c.mb_stride + mb_x] = score; //FIXME use SSE
    }

    if(c->avctx->mb_decision > FF_MB_DECISION_SIMPLE){
        type= CANDIDATE_MB_TYPE_FORWARD | CANDIDATE_MB_TYPE_BACKWARD | CANDIDATE_MB_TYPE_BIDIR | CANDIDATE_MB_TYPE_DIRECT;
        if(fimin < INT_MAX)
            type |= CANDIDATE_MB_TYPE_FORWARD_I;
        if(bimin < INT_MAX)
            type |= CANDIDATE_MB_TYPE_BACKWARD_I;
        if(fimin < INT_MAX && bimin < INT_MAX){
            type |= CANDIDATE_MB_TYPE_BIDIR_I;
        }
         //FIXME something smarter
        if(dmin>256*256*16) type&= ~CANDIDATE_MB_TYPE_DIRECT; //do not try direct mode if it is invalid for this MB
        if (s->c.codec_id == AV_CODEC_ID_MPEG4 && type&CANDIDATE_MB_TYPE_DIRECT &&
            s->mpv_flags & FF_MPV_FLAG_MV0 && *(uint32_t*)s->b_direct_mv_table[xy])
            type |= CANDIDATE_MB_TYPE_DIRECT0;
    }

    s->mb_type[mb_y*s->c.mb_stride + mb_x]= type;
}

/* find best f_code for ME which do unlimited searches */
int ff_get_best_fcode(MPVMainEncContext *const m, const int16_t (*mv_table)[2], int type)
{
    MPVEncContext *const s = &m->s;
    MotionEstContext *const c = &s->me;

    if (c->motion_est != FF_ME_ZERO) {
        int score[8];
        int i, range = c->avctx->me_range ? c->avctx->me_range : (INT_MAX/2);
        const uint8_t * fcode_tab = m->fcode_tab;
        int best_fcode=-1;
        int best_score=-10000000;

        if (s->c.msmpeg4_version != MSMP4_UNUSED)
            range= FFMIN(range, 16);
        else if (s->c.codec_id == AV_CODEC_ID_MPEG2VIDEO &&
                 c->avctx->strict_std_compliance >= FF_COMPLIANCE_NORMAL)
            range= FFMIN(range, 256);

        for(i=0; i<8; i++) score[i]= s->c.mb_num*(8-i);

        for (int y = 0; y < s->c.mb_height; y++) {
            int x;
            int xy= y*s->c.mb_stride;
            for(x=0; x<s->c.mb_width; x++, xy++){
                if(s->mb_type[xy] & type){
                    int mx= mv_table[xy][0];
                    int my= mv_table[xy][1];
                    int fcode = FFMAX(fcode_tab[mx], fcode_tab[my]);
                    int j;

                    if (mx >= range || mx < -range ||
                        my >= range || my < -range)
                        continue;

                    for(j=0; j<fcode && j<8; j++){
                        if (s->c.pict_type == AV_PICTURE_TYPE_B ||
                            s->mc_mb_var[xy] < s->mb_var[xy])
                            score[j]-= 170;
                    }
                }
            }
        }

        for(i=1; i<8; i++){
            if(score[i] > best_score){
                best_score= score[i];
                best_fcode= i;
            }
        }

        return best_fcode;
    }else{
        return 1;
    }
}

void ff_fix_long_p_mvs(MPVEncContext *const s, int type)
{
    MotionEstContext *const c = &s->me;
    const int f_code= s->f_code;
    int y, range;
    av_assert0(s->c.pict_type == AV_PICTURE_TYPE_P);

    range = (((s->c.out_format == FMT_MPEG1 || s->c.msmpeg4_version != MSMP4_UNUSED) ? 8 : 16) << f_code);

    av_assert0(range <= 16 || s->c.msmpeg4_version == MSMP4_UNUSED);
    av_assert0(range <=256 || !(s->c.codec_id == AV_CODEC_ID_MPEG2VIDEO && c->avctx->strict_std_compliance >= FF_COMPLIANCE_NORMAL));

    if(c->avctx->me_range && range > c->avctx->me_range) range= c->avctx->me_range;

    if (c->avctx->flags & AV_CODEC_FLAG_4MV) {
        const int wrap= s->c.b8_stride;

        /* clip / convert to intra 8x8 type MVs */
        for(y=0; y<s->c.mb_height; y++){
            int xy= y*2*wrap;
            int i= y*s->c.mb_stride;
            int x;

            for(x=0; x<s->c.mb_width; x++){
                if(s->mb_type[i]&CANDIDATE_MB_TYPE_INTER4V){
                    int block;
                    for(block=0; block<4; block++){
                        int off= (block& 1) + (block>>1)*wrap;
                        int mx = s->c.cur_pic.motion_val[0][ xy + off ][0];
                        int my = s->c.cur_pic.motion_val[0][ xy + off ][1];

                        if(   mx >=range || mx <-range
                           || my >=range || my <-range){
                            s->mb_type[i] &= ~CANDIDATE_MB_TYPE_INTER4V;
                            s->mb_type[i] |= type;
                            s->c.cur_pic.mb_type[i] = type;
                        }
                    }
                }
                xy+=2;
                i++;
            }
        }
    }
}

/**
 * @param truncate 1 for truncation, 0 for using intra
 */
void ff_fix_long_mvs(MPVEncContext *const s, uint8_t *field_select_table, int field_select,
                     int16_t (*mv_table)[2], int f_code, int type, int truncate)
{
    MotionEstContext *const c = &s->me;
    int y, h_range, v_range;

    // RAL: 8 in MPEG-1, 16 in MPEG-4
    int range = (((s->c.out_format == FMT_MPEG1 || s->c.msmpeg4_version != MSMP4_UNUSED) ? 8 : 16) << f_code);

    if(c->avctx->me_range && range > c->avctx->me_range) range= c->avctx->me_range;

    h_range= range;
    v_range= field_select_table ? range>>1 : range;

    /* clip / convert to intra 16x16 type MVs */
    for(y=0; y<s->c.mb_height; y++){
        int x;
        int xy= y*s->c.mb_stride;
        for(x=0; x<s->c.mb_width; x++){
            if (s->mb_type[xy] & type){    // RAL: "type" test added...
                if (!field_select_table || field_select_table[xy] == field_select) {
                    if(   mv_table[xy][0] >=h_range || mv_table[xy][0] <-h_range
                       || mv_table[xy][1] >=v_range || mv_table[xy][1] <-v_range){

                        if(truncate){
                            if     (mv_table[xy][0] > h_range-1) mv_table[xy][0]=  h_range-1;
                            else if(mv_table[xy][0] < -h_range ) mv_table[xy][0]= -h_range;
                            if     (mv_table[xy][1] > v_range-1) mv_table[xy][1]=  v_range-1;
                            else if(mv_table[xy][1] < -v_range ) mv_table[xy][1]= -v_range;
                        }else{
                            s->mb_type[xy] &= ~type;
                            s->mb_type[xy] |= CANDIDATE_MB_TYPE_INTRA;
                            mv_table[xy][0]=
                            mv_table[xy][1]= 0;
                        }
                    }
                }
            }
            xy++;
        }
    }
}
