/*****************************************************************************
 * libavsmash_source.cpp
 *****************************************************************************
 * Copyright (C) 2012-2013 L-SMASH Works project
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

#include "lsmashsource.h"

extern "C"
{
/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "video_output.h"
#include "audio_output.h"
#include "libavsmash_source.h"

static const char func_name_video_source[] = "LSMASHVideoSource";
static const char func_name_audio_source[] = "LSMASHAudioSource";

LSMASHVideoSource::LSMASHVideoSource
(
    const char         *source,
    uint32_t            track_number,
    int                 threads,
    int                 seek_mode,
    uint32_t            forward_seek_threshold,
    int                 direct_rendering,
    int                 stacked_format,
    enum AVPixelFormat  pixel_format,
    const char         *forced_codec_name,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &vdh, 0, sizeof(libavsmash_video_decode_handler_t) );
    memset( &voh, 0, sizeof(libavsmash_video_output_handler_t) );
    format_ctx                   = NULL;
    vdh.seek_mode                = seek_mode;
    vdh.forward_seek_threshold   = forward_seek_threshold;
    vdh.config.forced_codec_name = forced_codec_name;
    as_video_output_handler_t *as_vohp = (as_video_output_handler_t *)lw_malloc_zero( sizeof(as_video_output_handler_t) );
    if( !as_vohp )
        env->ThrowError( "LSMASHVideoSource: failed to allocate the AviSynth video output handler." );
    as_vohp->vi  = &vi;
    as_vohp->env = env;
    voh.private_handler      = as_vohp;
    voh.free_private_handler = as_free_video_output_handler;
    get_video_track( source, track_number, threads, env );
    lsmash_discard_boxes( vdh.root );
    prepare_video_decoding( direct_rendering, stacked_format, pixel_format, env );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    libavsmash_cleanup_video_decode_handler( &vdh );
    libavsmash_cleanup_video_output_handler( &voh );
    if( format_ctx )
        avformat_close_input( &format_ctx );
    lsmash_close_file( &file_param );
    lsmash_destroy_root( vdh.root );
}

uint32_t LSMASHVideoSource::open_file( const char *source, IScriptEnvironment *env )
{
    lw_log_handler_t *lhp = &vdh.config.lh;
    lhp->name     = func_name_video_source;
    lhp->level    = LW_LOG_FATAL;
    lhp->show_log = throw_error;
    lsmash_movie_parameters_t movie_param;
    vdh.root = libavsmash_open_file( &format_ctx, source, &file_param, &movie_param, lhp );
    return movie_param.number_of_tracks;
}

void LSMASHVideoSource::get_video_track( const char *source, uint32_t track_number, int threads, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first video track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            vdh.track_ID = lsmash_get_track_ID( vdh.root, i );
            if( vdh.track_ID == 0 )
                env->ThrowError( "LSMASHVideoSource: failed to find video track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vdh.root, vdh.track_ID, &media_param ) )
                env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
    }
    else
    {
        /* Get the desired video track. */
        vdh.track_ID = lsmash_get_track_ID( vdh.root, track_number );
        if( vdh.track_ID == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vdh.root, vdh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            env->ThrowError( "LSMASHVideoSource: the track you specified is not a video track." );
    }
    if( lsmash_construct_timeline( vdh.root, vdh.track_ID ) )
        env->ThrowError( "LSMASHVideoSource: failed to get construct timeline." );
    if( get_summaries( vdh.root, vdh.track_ID, &vdh.config ) )
        env->ThrowError( "LSMASHVideoSource: failed to get summaries." );
    vi.num_frames = lsmash_get_sample_count_in_media_timeline( vdh.root, vdh.track_ID );
    /* Calculate average framerate. */
    {
        int64_t fps_num = 25;
        int64_t fps_den = 1;
        libavsmash_setup_timestamp_info( &vdh, &fps_num, &fps_den, vi.num_frames );
        vi.fps_numerator   = (unsigned int)fps_num;
        vi.fps_denominator = (unsigned int)fps_den;
    }
    /* libavformat */
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream       *stream = format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    vdh.config.ctx = ctx;
    AVCodec *codec = libavsmash_find_decoder( &vdh.config );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

void LSMASHVideoSource::prepare_video_decoding
(
    int                 direct_rendering,
    int                 stacked_format,
    enum AVPixelFormat  pixel_format,
    IScriptEnvironment *env
)
{
    vdh.frame_buffer = av_frame_alloc();
    if( !vdh.frame_buffer )
        env->ThrowError( "LSMASHVideoSource: failed to allocate video frame buffer." );
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vdh.config;
    config->lh.priv = env;
    if( initialize_decoder_configuration( vdh.root, vdh.track_ID, config ) )
        env->ThrowError( "LSMASHVideoSource: failed to initialize the decoder configuration." );
    /* Set up output format. */
    config->get_buffer = as_setup_video_rendering( &voh, config->ctx, "LSMASHVideoSource",
                                                   direct_rendering, stacked_format, pixel_format,
                                                   config->prefer.width, config->prefer.height );
    /* Find the first valid video sample. */
    if( libavsmash_find_first_valid_video_frame( &vdh, vi.num_frames ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to find the first valid video frame." );
    /* Force seeking at the first reading. */
    vdh.last_sample_number = vi.num_frames + 1;
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    codec_configuration_t *config = &vdh.config;
    config->lh.priv = env;
    if( config->error )
        return env->NewVideoFrame( vi );
    if( libavsmash_get_video_frame( &vdh, sample_number, vi.num_frames ) < 0 )
        return env->NewVideoFrame( vi );
    PVideoFrame as_frame;
    if( make_frame( &voh, config->ctx, vdh.frame_buffer, as_frame, env ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return as_frame;
}

LSMASHAudioSource::LSMASHAudioSource
(
    const char         *source,
    uint32_t            track_number,
    bool                skip_priming,
    uint64_t            channel_layout,
    int                 sample_rate,
    IScriptEnvironment *env
)
{
    memset( &vi,  0, sizeof(VideoInfo) );
    memset( &adh, 0, sizeof(libavsmash_audio_decode_handler_t) );
    memset( &aoh, 0, sizeof(libavsmash_audio_output_handler_t) );
    format_ctx = NULL;
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( adh.root );
    prepare_audio_decoding( channel_layout, sample_rate, env );
}

LSMASHAudioSource::~LSMASHAudioSource()
{
    libavsmash_cleanup_audio_decode_handler( &adh );
    libavsmash_cleanup_audio_output_handler( &aoh );
    if( format_ctx )
        avformat_close_input( &format_ctx );
    lsmash_close_file( &file_param );
    lsmash_destroy_root( adh.root );
}

uint32_t LSMASHAudioSource::open_file( const char *source, IScriptEnvironment *env )
{
    lw_log_handler_t *lhp = &adh.config.lh;
    lhp->name     = func_name_audio_source;
    lhp->level    = LW_LOG_FATAL;
    lhp->show_log = throw_error;
    lsmash_movie_parameters_t movie_param;
    adh.root = libavsmash_open_file( &format_ctx, source, &file_param, &movie_param, lhp );
    return movie_param.number_of_tracks;
}

static int64_t get_start_time( lsmash_root_t *root, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, track_ID, edit_number, &edit ) )
            return 0;
        if( edit.duration == 0 )
            return 0;   /* no edits */
        if( edit.start_time >= 0 )
            return edit.start_time;
    }
    return 0;
}

static char *duplicate_as_string( void *src, size_t length )
{
    char *dst = new char[length + 1];
    if( !dst )
        return NULL;
    memcpy( dst, src, length );
    dst[length] = '\0';
    return dst;
}

void LSMASHAudioSource::get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first audio track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            adh.track_ID = lsmash_get_track_ID( adh.root, i );
            if( adh.track_ID == 0 )
                env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( adh.root, adh.track_ID, &media_param ) )
                env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
    }
    else
    {
        /* Get the desired audio track. */
        adh.track_ID = lsmash_get_track_ID( adh.root, track_number );
        if( adh.track_ID == 0 )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( adh.root, adh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            env->ThrowError( "LSMASHAudioSource: the track you specified is not an audio track." );
    }
    if( lsmash_construct_timeline( adh.root, adh.track_ID ) )
        env->ThrowError( "LSMASHAudioSource: failed to get construct timeline." );
    if( get_summaries( adh.root, adh.track_ID, &adh.config ) )
        env->ThrowError( "LSMASHAudioSource: failed to get summaries." );
    adh.frame_count = lsmash_get_sample_count_in_media_timeline( adh.root, adh.track_ID );
    vi.num_audio_samples = lsmash_get_media_duration_from_media_timeline( adh.root, adh.track_ID );
    if( skip_priming )
    {
        uint32_t itunes_metadata_count = lsmash_count_itunes_metadata( adh.root );
        for( i = 1; i <= itunes_metadata_count; i++ )
        {
            lsmash_itunes_metadata_t metadata;
            if( lsmash_get_itunes_metadata( adh.root, i, &metadata ) < 0 )
                continue;
            if( metadata.item != ITUNES_METADATA_ITEM_CUSTOM
             || (metadata.type != ITUNES_METADATA_TYPE_STRING && metadata.type != ITUNES_METADATA_TYPE_BINARY)
             || !metadata.meaning || !metadata.name
             || memcmp( "com.apple.iTunes", metadata.meaning, strlen( metadata.meaning ) )
             || memcmp( "iTunSMPB", metadata.name, strlen( metadata.name ) ) )
            {
                lsmash_cleanup_itunes_metadata( &metadata );
                continue;
            }
            char *value = NULL;
            if( metadata.type == ITUNES_METADATA_TYPE_STRING )
            {
                int length = strlen( metadata.value.string );
                if( length >= 116 )
                    value = duplicate_as_string( metadata.value.string, length );
            }
            else    /* metadata.type == ITUNES_METADATA_TYPE_BINARY */
            {
                if( metadata.value.binary.size >= 116 )
                    value = duplicate_as_string( metadata.value.binary.data, metadata.value.binary.size );
            }
            lsmash_cleanup_itunes_metadata( &metadata );
            if( !value )
                continue;
            uint32_t dummy[9];
            uint32_t priming_samples;
            uint32_t padding;
            uint64_t duration;
            if( 12 != sscanf( value, " %I32x %I32x %I32x %I64x %I32x %I32x %I32x %I32x %I32x %I32x %I32x %I32x",
                              &dummy[0], &priming_samples, &padding, &duration, &dummy[1], &dummy[2],
                              &dummy[3], &dummy[4], &dummy[5], &dummy[6], &dummy[7], &dummy[8] ) )
            {
                delete [] value;
                continue;
            }
            delete [] value;
            adh.implicit_preroll     = 1;
            aoh.skip_decoded_samples = priming_samples;
            vi.num_audio_samples = duration + priming_samples;
            break;
        }
        if( aoh.skip_decoded_samples == 0 )
        {
            uint32_t ctd_shift;
            if( lsmash_get_composition_to_decode_shift_from_media_timeline( adh.root, adh.track_ID, &ctd_shift ) )
                env->ThrowError( "LSMASHAudioSource: failed to get the timeline shift." );
            aoh.skip_decoded_samples = ctd_shift + get_start_time( adh.root, adh.track_ID );
        }
    }
    /* libavformat */
    for( i = 0; i < format_ctx->nb_streams && format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO; i++ );
    if( i == format_ctx->nb_streams )
        env->ThrowError( "LSMASHAudioSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream       *stream = format_ctx->streams[i];
    AVCodecContext *ctx    = stream->codec;
    adh.config.ctx = ctx;
    AVCodec *codec = libavsmash_find_decoder( &adh.config );
    if( !codec )
        env->ThrowError( "LSMASHAudioSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = 0;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avcodec_open2." );
}

void LSMASHAudioSource::prepare_audio_decoding
(
    uint64_t            channel_layout,
    int                 sample_rate,
    IScriptEnvironment *env
)
{
    adh.frame_buffer = av_frame_alloc();
    if( !adh.frame_buffer )
        env->ThrowError( "LSMASHAudioSource: failed to allocate audio frame buffer." );
    /* Initialize the audio decoder configuration. */
    codec_configuration_t *config = &adh.config;
    config->lh.priv = env;
    if( initialize_decoder_configuration( adh.root, adh.track_ID, config ) )
        env->ThrowError( "LSMASHAudioSource: failed to initialize the decoder configuration." );
    aoh.output_channel_layout  = config->prefer.channel_layout;
    aoh.output_sample_format   = config->prefer.sample_format;
    aoh.output_sample_rate     = config->prefer.sample_rate;
    aoh.output_bits_per_sample = config->prefer.bits_per_sample;
    as_setup_audio_rendering( &aoh, config->ctx, &vi, env, "LSMASHAudioSource", channel_layout, sample_rate );
    /* Count the number of PCM audio samples. */
    vi.num_audio_samples = libavsmash_count_overall_pcm_samples( &adh, aoh.output_sample_rate, &aoh.skip_decoded_samples );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LSMASHAudioSource: no valid audio frame." );
    /* Force seeking at the first reading. */
    adh.next_pcm_sample_number = vi.num_audio_samples + 1;
}

void __stdcall LSMASHAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    adh.config.lh.priv = env;
    return (void)libavsmash_get_pcm_audio_samples( &adh, &aoh, buf, start, wanted_length );
}

AVSValue __cdecl CreateLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source                 = args[0].AsString();
    uint32_t    track_number           = args[1].AsInt( 0 );
    int         threads                = args[2].AsInt( 0 );
    int         seek_mode              = args[3].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[4].AsInt( 10 );
    int         direct_rendering       = args[5].AsBool( false ) ? 1 : 0;
    int         stacked_format         = args[6].AsBool( false ) ? 1 : 0;
    enum AVPixelFormat pixel_format    = get_av_output_pixel_format( args[7].AsString( NULL ) );
    const char *forced_codec_name      = args[8].AsString( NULL );
    threads                = threads >= 0 ? threads : 0;
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    direct_rendering      &= (pixel_format == AV_PIX_FMT_NONE);
    return new LSMASHVideoSource( source, track_number, threads, seek_mode, forward_seek_threshold,
                                  direct_rendering, stacked_format, pixel_format, forced_codec_name, env );
}

AVSValue __cdecl CreateLSMASHAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
#ifdef NDEBUG
    av_log_set_level( AV_LOG_QUIET );
#endif
    const char *source        = args[0].AsString();
    uint32_t    track_number  = args[1].AsInt( 0 );
    bool        skip_priming  = args[2].AsBool( true );
    const char *layout_string = args[3].AsString( NULL );
    int         sample_rate   = args[4].AsInt( 0 );
    uint64_t channel_layout = layout_string ? av_get_channel_layout( layout_string ) : 0;
    return new LSMASHAudioSource( source, track_number, skip_priming, channel_layout, sample_rate, env );
}
