/*****************************************************************************
 * lwlibav_source.c
 *****************************************************************************
 * Copyright (C) 2013-2014 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL. */

#define NO_PROGRESS_HANDLER

/* Libav (LGPL or GPL) */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavutil/imgutils.h>

/* Dummy definitions.
 * Audio resampler/buffer is NOT used at all in this filter. */
typedef void AVAudioResampleContext;
typedef void audio_samples_t;
int flush_resampler_buffers( AVAudioResampleContext *avr ){ return 0; }
int update_resampler_configuration( AVAudioResampleContext *avr,
                                    uint64_t out_channel_layout, int out_sample_rate, enum AVSampleFormat out_sample_fmt,
                                    uint64_t  in_channel_layout, int  in_sample_rate, enum AVSampleFormat  in_sample_fmt,
                                    int *input_planes, int *input_block_align ){ return 0; }
int resample_audio( AVAudioResampleContext *avr, audio_samples_t *out, audio_samples_t *in ){ return 0; }
void avresample_free( AVAudioResampleContext **avr ){ }
#include "../common/audio_output.h"
uint64_t output_pcm_samples_from_buffer
(
    lw_audio_output_handler_t *aohp,
    AVFrame                   *frame_buffer,
    uint8_t                  **output_buffer,
    enum audio_output_flag    *output_flags
)
{
    return 0;
}

uint64_t output_pcm_samples_from_packet
(
    lw_audio_output_handler_t *aohp,
    AVCodecContext            *ctx,
    AVPacket                  *pkt,
    AVFrame                   *frame_buffer,
    uint8_t                  **output_buffer,
    enum audio_output_flag    *output_flags
)
{
    return 0;
}

void lw_cleanup_audio_output_handler( lw_audio_output_handler_t *aohp ){ };

#include "lsmashsource.h"
#include "video_output.h"

#include "../common/progress.h"
#include "../common/lwlibav_dec.h"
#include "../common/lwlibav_video.h"
#include "../common/lwlibav_audio.h"
#include "../common/lwindex.h"

typedef struct
{
    VSVideoInfo                    vi;
    lwlibav_file_handler_t         lwh;
    lwlibav_video_decode_handler_t vdh;
    lwlibav_video_output_handler_t voh;
} lwlibav_handler_t;

static void VS_CC vs_filter_init( VSMap *in, VSMap *out, void **instance_data, VSNode *node, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    vsapi->setVideoInfo( &hp->vi, 1, node );
}

static void set_frame_properties
(
    lwlibav_video_decode_handler_t *vdhp,
    VSVideoInfo                    *vi,
    AVFrame                        *av_frame,
    VSFrameRef                     *vs_frame,
    const VSAPI                    *vsapi
)
{
    AVCodecContext *ctx   = vdhp->ctx;
    VSMap          *props = vsapi->getFramePropsRW( vs_frame );
    /* Sample aspect ratio */
    vsapi->propSetInt( props, "_SARNum", av_frame->sample_aspect_ratio.num, paReplace );
    vsapi->propSetInt( props, "_SARDen", av_frame->sample_aspect_ratio.den, paReplace );
    /* Sample duration
     * Variable Frame Rate is not supported yet. */
    vsapi->propSetInt( props, "_DurationNum", vi->fpsDen, paReplace );
    vsapi->propSetInt( props, "_DurationDen", vi->fpsNum, paReplace );
    /* Color format */
    if( ctx )
    {
        vsapi->propSetInt( props, "_ColorRange",  ctx->color_range != AVCOL_RANGE_JPEG, paReplace );
        vsapi->propSetInt( props, "_ColorSpace",  ctx->colorspace,                      paReplace );
        int chroma_loc;
        switch( ctx->chroma_sample_location )
        {
            case AVCHROMA_LOC_LEFT       : chroma_loc = 0;  break;
            case AVCHROMA_LOC_CENTER     : chroma_loc = 1;  break;
            case AVCHROMA_LOC_TOPLEFT    : chroma_loc = 2;  break;
            case AVCHROMA_LOC_TOP        : chroma_loc = 3;  break;
            case AVCHROMA_LOC_BOTTOMLEFT : chroma_loc = 4;  break;
            case AVCHROMA_LOC_BOTTOM     : chroma_loc = 5;  break;
            default                      : chroma_loc = -1; break;
        }
        if( chroma_loc != -1 )
            vsapi->propSetInt( props, "_ChromaLocation", chroma_loc, paReplace );
    }
    /* Picture type */
    char pict_type = av_get_picture_type_char( av_frame->pict_type );
    vsapi->propSetData( props, "_PictType", &pict_type, 1, paReplace );
    /* Progressive or Interlaced */
    vsapi->propSetInt( props, "_FieldBased", !!av_frame->interlaced_frame, paReplace );
}

static int prepare_video_decoding( lwlibav_handler_t *hp, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    VSVideoInfo                    *vi   = &hp->vi;
    lw_log_handler_t               *lhp  = &vdhp->lh;
    /* Import AVIndexEntrys. */
    if( lwlibav_import_av_index_entry( (lwlibav_decode_handler_t *)vdhp ) < 0 )
        return -1;
    /* Set up output format. */
    vdhp->ctx->width      = vdhp->initial_width;
    vdhp->ctx->height     = vdhp->initial_height;
    vdhp->ctx->pix_fmt    = vdhp->initial_pix_fmt;
    vdhp->ctx->colorspace = vdhp->initial_colorspace;
    if( determine_colorspace_conversion( vohp, vdhp->ctx->pix_fmt ) )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: %s is not supported", av_get_pix_fmt_name( vdhp->ctx->pix_fmt ) );
        return -1;
    }
    if( initialize_scaler_handler( &vohp->scaler, vdhp->ctx, vohp->scaler.enabled, SWS_FAST_BILINEAR, vohp->scaler.output_pixel_format ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to initialize scaler handler." );
        return -1;
    }
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = NULL;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    vdhp->exh.get_buffer = setup_video_rendering( vohp, vdhp->ctx, vi, vdhp->max_width, vdhp->max_height );
    if( !vdhp->exh.get_buffer )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate memory for the background black frame data." );
        return -1;
    }
    /* Find the first valid video frame. */
    if( lwlibav_find_first_valid_video_frame( vdhp ) < 0 )
    {
        set_error( lhp, LW_LOG_FATAL, "lsmas: failed to allocate the first valid video frame." );
        return -1;
    }
    /* Force seeking at the first reading. */
    vdhp->last_frame_number = vi->numFrames + 1;
    return 0;
}

static const VSFrameRef *VS_CC vs_filter_get_frame( int n, int activation_reason, void **instance_data, void **frame_data, VSFrameContext *frame_ctx, VSCore *core, const VSAPI *vsapi )
{
    if( activation_reason != arInitial )
        return NULL;
    lwlibav_handler_t *hp = (lwlibav_handler_t *)*instance_data;
    VSVideoInfo       *vi = &hp->vi;
    uint32_t frame_number = MIN( n + 1, vi->numFrames );    /* frame_number is 1-origin. */
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    if( vdhp->error )
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = NULL;
    vsbh.frame_ctx = frame_ctx;
    vsbh.vsapi     = vsapi;
    vdhp->lh.priv     = &vsbh;
    vdhp->lh.show_log = set_error;
    /* Get and decode the desired video frame. */
    vs_video_output_handler_t *vs_vohp = (vs_video_output_handler_t *)vohp->private_handler;
    vs_vohp->frame_ctx = frame_ctx;
    vs_vohp->core      = core;
    vs_vohp->vsapi     = vsapi;
    vdhp->ctx->opaque = vohp;
    if( lwlibav_get_video_frame( vdhp, vohp, frame_number ) < 0 )
        return NULL;
    /* Output the video frame. */
    AVFrame    *av_frame = vdhp->frame_buffer;
    VSFrameRef *vs_frame = make_frame( vohp, vdhp->ctx, av_frame );
    if( !vs_frame )
    {
        vsapi->setFilterError( "lsmas: failed to output a video frame.", frame_ctx );
        return vsapi->newVideoFrame( vi->format, vi->width, vi->height, NULL, core );
    }
    set_frame_properties( vdhp, vi, av_frame, vs_frame, vsapi );
    return vs_frame;
}

static void VS_CC vs_filter_free( void *instance_data, VSCore *core, const VSAPI *vsapi )
{
    lwlibav_handler_t *hp = (lwlibav_handler_t *)instance_data;
    if( !hp )
        return;
    lwlibav_cleanup_video_decode_handler( &hp->vdh );
    lwlibav_cleanup_video_output_handler( &hp->voh );
    if( hp->lwh.file_path )
        free( hp->lwh.file_path );
    free( hp );
}

void VS_CC vs_lwlibavsource_create( const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi )
{
    /* Get file path. */
    const char *file_path = vsapi->propGetData( in, "source", 0, NULL );
    if( !file_path )
    {
        vsapi->setError( out, "lsmas: failed to get source file name." );
        return;
    }
    /* Allocate the handler of this filter function. */
    lwlibav_handler_t *hp = lw_malloc_zero( sizeof(lwlibav_handler_t) );
    if( !hp )
    {
        vsapi->setError( out, "lsmas: failed to allocate the LW-Libav handler." );
        return;
    }
    lwlibav_file_handler_t         *lwhp = &hp->lwh;
    lwlibav_video_decode_handler_t *vdhp = &hp->vdh;
    lwlibav_video_output_handler_t *vohp = &hp->voh;
    vs_video_output_handler_t *vs_vohp = vs_allocate_video_output_handler( vohp );
    if( !vs_vohp )
    {
        free( hp );
        vsapi->setError( out, "lsmas: failed to allocate the VapourSynth video output handler." );
        return;
    }
    vohp->private_handler      = vs_vohp;
    vohp->free_private_handler = free;
    /* Set up VapourSynth error handler. */
    vs_basic_handler_t vsbh = { 0 };
    vsbh.out       = out;
    vsbh.frame_ctx = NULL;
    vsbh.vsapi     = vsapi;
    /* Set up log handler. */
    lw_log_handler_t lh = { 0 };
    lh.level    = LW_LOG_FATAL;
    lh.priv     = &vsbh;
    lh.show_log = set_error;
    /* Get options. */
    int64_t stream_index;
    int64_t threads;
    int64_t cache_index;
    int64_t seek_mode;
    int64_t seek_threshold;
    int64_t variable_info;
    int64_t direct_rendering;
    int64_t apply_repeat_flag;
    int64_t field_dominance;
    const char *format;
    set_option_int64 ( &stream_index,     -1,    "stream_index",   in, vsapi );
    set_option_int64 ( &threads,           0,    "threads",        in, vsapi );
    set_option_int64 ( &cache_index,       1,    "cache",          in, vsapi );
    set_option_int64 ( &seek_mode,         0,    "seek_mode",      in, vsapi );
    set_option_int64 ( &seek_threshold,    10,   "seek_threshold", in, vsapi );
    set_option_int64 ( &variable_info,     0,    "variable",       in, vsapi );
    set_option_int64 ( &direct_rendering,  0,    "dr",             in, vsapi );
    set_option_int64 ( &apply_repeat_flag, 0,    "repeat",         in, vsapi );
    set_option_int64 ( &field_dominance,   0,    "dominance",      in, vsapi );
    set_option_string( &format,            NULL, "format",         in, vsapi );
    /* Set options. */
    lwlibav_option_t opt;
    opt.file_path         = file_path;
    opt.threads           = threads >= 0 ? threads : 0;
    opt.av_sync           = 0;
    opt.no_create_index   = !cache_index;
    opt.force_video       = (stream_index >= 0);
    opt.force_video_index = stream_index >= 0 ? stream_index : -1;
    opt.force_audio       = 0;
    opt.force_audio_index = -1;
    opt.apply_repeat_flag = apply_repeat_flag;
    opt.field_dominance   = CLIP_VALUE( field_dominance, 0, 2 );    /* 0: Obey source flags, 1: TFF, 2: BFF */
    vdhp->seek_mode                 = CLIP_VALUE( seek_mode,         0, 2 );
    vdhp->forward_seek_threshold    = CLIP_VALUE( seek_threshold,    1, 999 );
    vs_vohp->variable_info          = CLIP_VALUE( variable_info,     0, 1 );
    vs_vohp->direct_rendering       = CLIP_VALUE( direct_rendering,  0, 1 ) && !format;
    vs_vohp->vs_output_pixel_format = vs_vohp->variable_info ? pfNone : get_vs_output_pixel_format( format );
    /* Set up progress indicator. */
    progress_indicator_t indicator;
    indicator.open   = NULL;
    indicator.update = NULL;
    indicator.close  = NULL;
    /* Construct index. */
    lwlibav_audio_decode_handler_t adh = { 0 };
    lwlibav_audio_output_handler_t aoh = { 0 };
    int ret = lwlibav_construct_index( lwhp, vdhp, vohp, &adh, &aoh, &lh, &opt, &indicator, NULL );
    lwlibav_cleanup_audio_decode_handler( &adh );
    lwlibav_cleanup_audio_output_handler( &aoh );
    if( ret < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        set_error( &lh, LW_LOG_FATAL, "lsmas: failed to construct index." );
        return;
    }
    /* Get the desired video track. */
    vdhp->lh = lh;
    if( lwlibav_get_desired_video_track( lwhp->file_path, vdhp, lwhp->threads ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    /* Set up timestamp info. */
    hp->vi.numFrames = vohp->frame_count;
    hp->vi.fpsNum    = 25;
    hp->vi.fpsDen    = 1;
    lwlibav_setup_timestamp_info( lwhp, vdhp, vohp, &hp->vi.fpsNum, &hp->vi.fpsDen );
    /* Set up decoders for this stream. */
    if( prepare_video_decoding( hp, core, vsapi ) < 0 )
    {
        vs_filter_free( hp, core, vsapi );
        return;
    }
    vsapi->createFilter( in, out, "LWLibavSource", vs_filter_init, vs_filter_get_frame, vs_filter_free, fmSerial, 0, hp, core );
    return;
}
