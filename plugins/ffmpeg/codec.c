/*****************************************************************
 * gmerlin-encoders - encoder plugins for gmerlin
 *
 * Copyright (c) 2001 - 2012 Members of the Gmerlin project
 * gmerlin-general@lists.sourceforge.net
 * http://gmerlin.sourceforge.net
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * *****************************************************************/

#include "ffmpeg_common.h"

#include <gmerlin/utils.h>
#include <gmerlin/cfg_registry.h>

#include <gmerlin/translation.h>
#include <gmerlin/log.h>
#define LOG_DOMAIN "ffmpeg_encoder"

#include <gavl/metatags.h>

/*
 *  Standalone codecs
 */

#define FLAG_INITIALIZED (1<<0)
#define FLAG_ERROR       (1<<1)
#define FLAG_FLUSHED     (1<<2)


static void 
get_pixelformat_converter(bg_ffmpeg_codec_context_t * ctx, enum AVPixelFormat fmt,
                          int do_convert);

static int find_encoder(bg_ffmpeg_codec_context_t * ctx)
  {
  if(ctx->id == AV_CODEC_ID_NONE)
    return 0;

  if(ctx->codec)
    return 1;
  
  if(!(ctx->codec = avcodec_find_encoder(ctx->id)))
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
           "Codec %s not available in your libavcodec installation",
           bg_ffmpeg_get_codec_name(ctx->id));
    return 0;
    }
  
  return 1;
  }

/*
 *  Create a codec context.
 *  If avctx is NULL, it will be created and destroyed.
 *  If id is AV_CODEC_ID_NONE, a "codec" parameter will be supported
 */

bg_ffmpeg_codec_context_t * bg_ffmpeg_codec_create(int type,
                                                   AVCodecParameters * params,
                                                   enum AVCodecID id,
                                                   const ffmpeg_format_info_t * format)
  {
  bg_ffmpeg_codec_context_t * ret;

  //  avcodec_register_all();
  
  ret = calloc(1, sizeof(*ret));
  
  ret->format = format;
  ret->id = id;
  ret->type = type;
  
  if(!find_encoder(ret))
    goto fail;
  
  if(!(ret->avctx = avcodec_alloc_context3(ret->codec)))
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
             "Context for Codec %s could not be initialized",
             bg_ffmpeg_get_codec_name(ret->id));
    goto fail;
    }
  
  /* Get codec context from format context */
  if(params)
    avcodec_parameters_to_context(ret->avctx, params);
  
  ret->avctx->codec_id = ret->id;
  
  ret->avctx->codec_type = type;
  ret->frame = av_frame_alloc();
  
  return ret;

  fail:

  if(ret->avctx)
    avcodec_free_context(&ret->avctx);
  free(ret);
  return NULL;
  }

const bg_parameter_info_t * bg_ffmpeg_codec_get_parameters(bg_ffmpeg_codec_context_t * ctx)
  {
  if(!ctx)
    return NULL;
  return bg_ffmpeg_get_codec_parameters(ctx->id, ctx->type);  
  }

static void apply_func(void * priv, const char * name, const gavl_value_t * val)
  {
  bg_ffmpeg_codec_context_t * ctx;
  
  if(!name || !strcmp(name, BG_CFG_TAG_NAME))
    return;

  ctx = priv;
  
  bg_ffmpeg_set_codec_parameter(ctx->avctx,
                                &ctx->options,
                                name, val);
  }

void bg_ffmpeg_codec_set_parameter(bg_ffmpeg_codec_context_t * ctx,
                                   const char * name,
                                   const gavl_value_t * v)
  {
  if(!name)
    return;
  
  if(!strcmp(name, "codec"))
    {
    const char * name = bg_multi_menu_get_selected_name(v);

    if(ctx->type == AVMEDIA_TYPE_VIDEO)
      ctx->id = bg_ffmpeg_find_video_encoder(ctx->format, name);
    else
      ctx->id = bg_ffmpeg_find_audio_encoder(ctx->format, name);
    if(ctx->id == AV_CODEC_ID_NONE)
      {
      gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
             "Codec %s is not available in libavcodec or not supported in the container",
             v->v.str);
      return;
      }
    find_encoder(ctx);

    bg_cfg_section_apply(bg_multi_menu_get_selected(v),
                         NULL,
                         apply_func,
                         ctx);
    
    }
  else if(bg_encoder_set_framerate_parameter(&ctx->fr, name, v))
    return;
  
  }

static int set_compression_info(bg_ffmpeg_codec_context_t * ctx,
                                gavl_compression_info_t * ci,
                                gavl_dictionary_t * m)
  {
  if(ci)
    {
    /* Set up compression info */
  
    if((ci->id = bg_codec_id_ffmpeg_2_gavl(ctx->codec->id)) == GAVL_CODEC_ID_NONE)
      return 0;

    /* Extract extradata */

    if(ctx->avctx->extradata_size)
      gavl_buffer_append_data(&ci->codec_header, ctx->avctx->extradata, ctx->avctx->extradata_size);
    }
  if(m)
    gavl_dictionary_set_string(m, GAVL_META_SOFTWARE, LIBAVCODEC_IDENT);
  return 1;
  }



/*
 *  Audio
 */

static int flush_audio(bg_ffmpeg_codec_context_t * ctx)
  {
  AVPacket pkt;
  AVFrame * f;
  int result;

  av_init_packet(&pkt);
  gavl_packet_reset(&ctx->gp);
  
  pkt.data = ctx->gp.buf.buf;
  pkt.size = ctx->gp.buf.alloc;
  
  if(ctx->aframe->valid_samples)
    {
    ctx->frame->nb_samples = ctx->aframe->valid_samples;
    ctx->frame->pts = ctx->in_pts;
    ctx->in_pts += ctx->aframe->valid_samples;
    f = ctx->frame;
    }
  else
    {
    if(ctx->codec->capabilities & AV_CODEC_CAP_DELAY)
      f = NULL;
    else
      return 0;
    }
  
  if(avcodec_send_frame(ctx->avctx, f) < 0)
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
           "avcodec_send_frame failed");
    ctx->flags |= FLAG_ERROR;
    return -1;
    }

#if 0
  if(avcodec_encode_audio2(ctx->avctx, &pkt, f, &got_packet) < 0)
    {
    ctx->flags |= FLAG_ERROR;
    return 0;
    }
#endif
  
  /* Mute frame */
  gavl_audio_frame_mute(ctx->aframe, &ctx->afmt);
  ctx->aframe->valid_samples = 0;

  while(1)
    {
    av_init_packet(&pkt);
    result = avcodec_receive_packet(ctx->avctx, &pkt);

    if((result == AVERROR(EAGAIN)) || (result == AVERROR_EOF))
      break;
    
    else if(result)
      {
      /* Fail */
      return 0;
      }
    
    ctx->gp.pts      = ctx->out_pts;
    ctx->gp.duration = ctx->afmt.samples_per_frame;

    // fprintf(stderr, "Samples written: %ld\n", ctx->samples_written);
    
    /* Last frame can be smaller */
    
    if(ctx->gp.pts + ctx->gp.duration > ctx->in_pts)
      ctx->gp.duration = ctx->in_pts - ctx->gp.pts;
    
    ctx->out_pts += ctx->gp.duration;
    
    ctx->gp.flags |= GAVL_PACKET_KEYFRAME;
    
    ctx->gp.buf.len = pkt.size;
    ctx->gp.buf.buf = pkt.data;
    
    // fprintf(stderr, "Put audio packet\n");
    // gavl_packet_dump(&ctx->gp);
    
    if(gavl_packet_sink_put_packet(ctx->psink, &ctx->gp) != GAVL_SINK_OK)
      ctx->flags |= FLAG_ERROR;
    
    av_packet_unref(&pkt);
    }
  return 1;
  }

static gavl_sink_status_t
write_audio_func(void * data, gavl_audio_frame_t * frame)
  {
  int samples_written = 0;
  int samples_copied;
  bg_ffmpeg_codec_context_t * ctx = data;

  if(ctx->in_pts == GAVL_TIME_UNDEFINED)
    {
    ctx->in_pts = frame->timestamp;
    ctx->out_pts = ctx->in_pts - ctx->avctx->delay;
    }

  while(samples_written < frame->valid_samples)
    {
    samples_copied =
      gavl_audio_frame_copy(&ctx->afmt,
                            ctx->aframe, // dst frame
                            frame,     // src frame
                            ctx->aframe->valid_samples, // dst_pos
                            samples_written,          // src_pos,
                            ctx->afmt.samples_per_frame - ctx->aframe->valid_samples, // dst_size,
                            frame->valid_samples - samples_written);
    
    ctx->aframe->valid_samples += samples_copied;
    if(ctx->aframe->valid_samples == ctx->afmt.samples_per_frame)
      {
      flush_audio(ctx);
      
      if(ctx->flags & FLAG_ERROR)
        return GAVL_SINK_ERROR;
      }
    samples_written += samples_copied;
    }
  return GAVL_SINK_OK;
  }


void bg_ffmpeg_set_audio_format_avctx(AVCodecContext * avctx,
                                      const gavl_audio_format_t * fmt)
  {
  /* Set format for codec */
  avctx->sample_rate = fmt->samplerate;
  
  /* Channel setup */
  avctx->channels    = fmt->num_channels;
  }

void bg_ffmpeg_set_audio_format_params(AVCodecParameters * avctx,
                                       const gavl_audio_format_t * fmt)
  {
  /* Set format for codec */
  avctx->sample_rate = fmt->samplerate;
  
  /* Channel setup */
  avctx->channels    = fmt->num_channels;
  }

gavl_audio_sink_t * bg_ffmpeg_codec_open_audio(bg_ffmpeg_codec_context_t * ctx,
                                               gavl_compression_info_t * ci,
                                               gavl_audio_format_t * fmt,
                                               gavl_dictionary_t * m)
  {
  AVOutputFormat * ofmt;
  //  if(!find_encoder(ctx))
  //    return NULL;
  
  /* Set format for codec */

  if(!ctx->codec)
    return NULL;

  bg_ffmpeg_set_audio_format_avctx(ctx->avctx, fmt);

  
  ctx->avctx->channel_layout =
    bg_ffmpeg_get_channel_layout(fmt);

  
  /* Sample format */
  ctx->avctx->sample_fmt = ctx->codec->sample_fmts[0];
  fmt->sample_format =
    bg_sample_format_ffmpeg_2_gavl(ctx->avctx->sample_fmt, &fmt->interleave_mode);

  /* Set codec specific stuff */
  switch(ctx->avctx->codec_id)
    {
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S16LE:
      ctx->avctx->bit_rate =
        ctx->afmt.samplerate * ctx->afmt.num_channels * 16;
      break;
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_PCM_MULAW:
      ctx->avctx->bit_rate =
        ctx->afmt.samplerate * ctx->afmt.num_channels * 8;
      break;
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_VORBIS:
      if(!ctx->avctx->bit_rate)
        ctx->avctx->flags |= AV_CODEC_FLAG_QSCALE;
      break;
    default:
      break;
    }

  /* Decide whether we need a global header */
  if(!ctx->format ||
     ((ofmt = guess_format(ctx->format->short_name, NULL, NULL)) &&
      (ofmt->flags & AVFMT_GLOBALHEADER)))
    ctx->avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  
  /* Open encoder */
  if(avcodec_open2(ctx->avctx, ctx->codec, &ctx->options) < 0)
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "avcodec_open2 failed for audio");
    return NULL;
    }

  if(ctx->avctx->frame_size <= 1)
    fmt->samples_per_frame = 1024; // Frame size for uncompressed codecs
  else
    fmt->samples_per_frame = ctx->avctx->frame_size;
  
  ctx->aframe = gavl_audio_frame_create(fmt);

  /* Set up AVFrame */
  if(fmt->interleave_mode == GAVL_INTERLEAVE_ALL)
    {
    ctx->frame->extended_data = ctx->frame->data;
    ctx->frame->linesize[0] = ctx->aframe->channel_stride * fmt->num_channels;
    ctx->frame->extended_data[0] = ctx->aframe->samples.u_8;
    }
  else
    {
    int i;
    if(fmt->num_channels > AV_NUM_DATA_POINTERS)
      ctx->frame->extended_data = av_mallocz(fmt->num_channels *
                                             sizeof(*ctx->frame->extended_data));
    else
      ctx->frame->extended_data = ctx->frame->data;
    
    for(i = 0; i < fmt->num_channels; i++)
      ctx->frame->extended_data[i] = ctx->aframe->channels.u_8[i];
    ctx->frame->linesize[0] = ctx->aframe->channel_stride;
    }
  
  /* Mute frame */
  gavl_audio_frame_mute(ctx->aframe, fmt);
  ctx->aframe->valid_samples = 0;
  
  ctx->asink = gavl_audio_sink_create(NULL, write_audio_func, ctx, fmt);
  
  /* Copy format for later use */
  gavl_audio_format_copy(&ctx->afmt, fmt);

  set_compression_info(ctx, ci, m);

  if(ci)
    {
    switch(ctx->avctx->codec_id)
      {
      case AV_CODEC_ID_MP2:
      case AV_CODEC_ID_AC3:
        ci->bitrate = ctx->avctx->bit_rate;
        break;
      default:
        break;
      }
    ci->pre_skip = ctx->avctx->delay;
    }
  
  ctx->in_pts = GAVL_TIME_UNDEFINED;
  ctx->out_pts = GAVL_TIME_UNDEFINED;
  
  ctx->flags |= FLAG_INITIALIZED;
  return ctx->asink;
  }

/*
 *  Video
 */

int bg_ffmpeg_codec_set_video_pass(bg_ffmpeg_codec_context_t * ctx,
                                   int pass,
                                   int total_passes,
                                   const char * stats_filename)
  {
  ctx->pass           = pass;
  ctx->total_passes   = total_passes;
  ctx->stats_filename = gavl_strrep(ctx->stats_filename, stats_filename);
  return 1;
  }

static int flush_video(bg_ffmpeg_codec_context_t * ctx,
                       AVFrame * frame)
  {
  int result;
  AVPacket pkt;
  
  if(avcodec_send_frame(ctx->avctx, frame) < 0)
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
           "avcodec_send_frame failed");
    ctx->flags |= FLAG_ERROR;
    return 0;
    }
  
  while(1)
    {
    av_init_packet(&pkt);
    result = avcodec_receive_packet(ctx->avctx, &pkt);

    if((result == AVERROR(EAGAIN)) || (result == AVERROR_EOF))
      break;
    
    else if(result)
      {
      /* Fail */
      return 0;
      }
    
    /* Got packet */

    gavl_packet_reset(&ctx->gp);

    ctx->gp.pts = pkt.pts;

    if(pkt.flags & AV_PKT_FLAG_KEY)
      ctx->gp.flags |= GAVL_PACKET_KEYFRAME;
    
    ctx->gp.buf.len = pkt.size;
    ctx->gp.buf.buf = pkt.data;
    
    if(ctx->vfmt.framerate_mode == GAVL_FRAMERATE_CONSTANT)
      ctx->gp.pts *= ctx->vfmt.frame_duration;
    
    /* Detect VP8 alternate reference frames */
    if((ctx->id == AV_CODEC_ID_VP8) &&
       !(ctx->gp.buf.buf[0] & 0x10))
      ctx->gp.flags |= GAVL_PACKET_NOOUTPUT; 
    else
      {
      /* Decide frame type */
      if(ctx->gp.pts < ctx->out_pts)
        ctx->gp.flags |= GAVL_PACKET_TYPE_B;
      else
        {
        if(ctx->gp.flags & GAVL_PACKET_KEYFRAME)
          ctx->gp.flags |= GAVL_PACKET_TYPE_I;
        else
          ctx->gp.flags |= GAVL_PACKET_TYPE_P;
        ctx->out_pts = ctx->gp.pts;
        }

      if(!bg_encoder_pts_cache_pop_packet(ctx->pc, &ctx->gp, -1, ctx->gp.pts))
        {
        ctx->flags |= FLAG_ERROR;
        gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
               "Got no packet in cache for pts %"PRId64, ctx->gp.pts);
        //     fprintf(stderr, "Got no packet in cache for pts %"PRId64"\n", ctx->gp.pts);
        }
      //      else
        //        fprintf(stderr, "pop packet: %"PRId64"\n", ctx->gp.pts);
      }
    /* Write frame */

    //    fprintf(stderr, "Put video packet\n");
    //    gavl_packet_dump(&ctx->gp);
    
    if(gavl_packet_sink_put_packet(ctx->psink, &ctx->gp) != GAVL_SINK_OK)
      {
      ctx->flags |= FLAG_ERROR;
      gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,
             "Writing packet failed");
      }
    /* Write stats */
    if((ctx->pass == 1) && ctx->avctx->stats_out && ctx->stats_file)
      fprintf(ctx->stats_file, "%s", ctx->avctx->stats_out);

    ctx->gp.buf.buf = NULL;
    
    av_packet_unref(&pkt);
    }
  
  return 1;
  }

static gavl_sink_status_t
write_video_func(void * data, gavl_video_frame_t * frame)
  {
  bg_ffmpeg_codec_context_t * ctx = data;

  if(!bg_encoder_pts_cache_push_frame(ctx->pc, frame))
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "PTS cache full");
    return GAVL_SINK_ERROR;
    }
  
  //  fprintf(stderr, "push frame: %"PRId64"\n", frame->timestamp);
  
  if(ctx->convert_frame)
    ctx->convert_frame(ctx, frame);

  ctx->frame->pts = frame->timestamp;
  if(ctx->vfmt.framerate_mode == GAVL_FRAMERATE_CONSTANT)
    ctx->frame->pts /= ctx->vfmt.frame_duration;
  
  ctx->frame->data[0]     = frame->planes[0];
  ctx->frame->data[1]     = frame->planes[1];
  ctx->frame->data[2]     = frame->planes[2];
  ctx->frame->linesize[0] = frame->strides[0];
  ctx->frame->linesize[1] = frame->strides[1];
  ctx->frame->linesize[2] = frame->strides[2];
 
//  ctx->frame->width  = ctx->vfmt.image_width;
//  ctx->frame->height = ctx->vfmt.image_height;
 
  flush_video(ctx, ctx->frame);

  if(ctx->flags & FLAG_ERROR)
    return GAVL_SINK_ERROR;
  
  return GAVL_SINK_OK;
  }

static gavl_video_frame_t * get_video_func(void * data)
  {
  bg_ffmpeg_codec_context_t * ctx = data;
  return ctx->vframe;
  }

void bg_ffmpeg_set_video_dimensions_avctx(AVCodecContext * avctx,
                                          const gavl_video_format_t * fmt)
  {
  avctx->width  = fmt->image_width;
  avctx->height = fmt->image_height;
  
  avctx->sample_aspect_ratio.num = fmt->pixel_width;
  avctx->sample_aspect_ratio.den = fmt->pixel_height;
  }

void bg_ffmpeg_set_video_dimensions_params(AVCodecParameters * avctx,
                                             const gavl_video_format_t * fmt)
  {
  avctx->width  = fmt->image_width;
  avctx->height = fmt->image_height;
  
  avctx->sample_aspect_ratio.num = fmt->pixel_width;
  avctx->sample_aspect_ratio.den = fmt->pixel_height;
  }


gavl_video_sink_t * bg_ffmpeg_codec_open_video(bg_ffmpeg_codec_context_t * ctx,
                                               gavl_compression_info_t * ci,
                                               gavl_video_format_t * fmt,
                                               gavl_dictionary_t * m)
  {
  int do_convert = 0;
  const ffmpeg_codec_info_t * info;
  gavl_video_sink_get_func get_func = NULL;
  AVOutputFormat * ofmt;

  //  if(!find_encoder(ctx))
  //    return NULL;

  if(!ctx->codec)
    return NULL;
  
  info = bg_ffmpeg_get_codec_info(ctx->id,
                                  AVMEDIA_TYPE_VIDEO);
  
  /* Set format for codec */

  bg_ffmpeg_set_video_dimensions_avctx(ctx->avctx, fmt);
  
  
  ctx->avctx->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->avctx->codec_id = ctx->id;
  
  bg_ffmpeg_choose_pixelformat(ctx->codec->pix_fmts,
                               &ctx->avctx->pix_fmt,
                               &fmt->pixelformat, &do_convert);
  
  /* Framerate */
  if(info->flags & FLAG_CONSTANT_FRAMERATE ||
     (ctx->format && (ctx->format->flags & FLAG_CONSTANT_FRAMERATE)))
    {
    if(info->framerates)
      bg_encoder_set_framerate_nearest(&ctx->fr,
                                       info->framerates,
                                       fmt);
    else
      bg_encoder_set_framerate(&ctx->fr, fmt);
    }
  
  if(fmt->framerate_mode == GAVL_FRAMERATE_CONSTANT)
    {
    ctx->avctx->time_base.den = fmt->timescale;
    ctx->avctx->time_base.num = fmt->frame_duration;
    }
  else
    {
    ctx->avctx->time_base.den = fmt->timescale;
    ctx->avctx->time_base.num = 1;
    }
  
  /* Set up multipass encoding */
  
  if(ctx->total_passes)
    {
    int stats_len;
    
    if(ctx->pass == 1)
      {
      ctx->stats_file = fopen(ctx->stats_filename, "w");
      ctx->avctx->flags |= AV_CODEC_FLAG_PASS1;
      }
    else if(ctx->pass == ctx->total_passes)
      {
      ctx->stats_file = fopen(ctx->stats_filename, "r");
      fseek(ctx->stats_file, 0, SEEK_END);
      stats_len = ftell(ctx->stats_file);
      fseek(ctx->stats_file, 0, SEEK_SET);
      
      ctx->avctx->stats_in = av_malloc(stats_len + 1);
      if(fread(ctx->avctx->stats_in, 1,
               stats_len, ctx->stats_file) < stats_len)
        {
        av_free(ctx->avctx->stats_in);
        ctx->avctx->stats_in = NULL;
        }
      else
        ctx->avctx->stats_in[stats_len] = '\0';
      
      fclose(ctx->stats_file);
      ctx->stats_file = NULL;
      
      ctx->avctx->flags |= AV_CODEC_FLAG_PASS2;
      }
    }

  
  /* Decide whether we need a global header */
  if(!ctx->format ||
     ((ofmt = guess_format(ctx->format->short_name, NULL, NULL)) &&
      (ofmt->flags & AVFMT_GLOBALHEADER)))
    ctx->avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  
  if(avcodec_open2(ctx->avctx, ctx->codec, &ctx->options) < 0)
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "avcodec_open2 failed for video");
    return NULL;
    }
  
  ctx->pc = bg_encoder_pts_cache_create();
  
  gavl_video_format_copy(&ctx->vfmt, fmt);

  /* Create temporary frame if necessary */
  if(do_convert)
    {
    fprintf(stderr, "Need colorspace conversion\n");
    ctx->vframe = gavl_video_frame_create(&ctx->vfmt);
    get_func = get_video_func;

    get_pixelformat_converter(ctx, ctx->avctx->pix_fmt, do_convert);
    
    }

  ctx->vsink = gavl_video_sink_create(get_func, write_video_func, ctx, &ctx->vfmt);
  
  /* Set up compression info */
  set_compression_info(ctx, ci, m);

  if(ci)
    {
    if(!(info->flags & FLAG_INTRA_ONLY))
      {
      if((ctx->avctx->gop_size > 1) ||
         (ctx->avctx->gop_size < 0))
        {
        ci->flags |= GAVL_COMPRESSION_HAS_P_FRAMES;
        }
      if((info->flags & FLAG_B_FRAMES) &&
         ((ctx->avctx->max_b_frames > 0) || ctx->avctx->has_b_frames))
        ci->flags |= GAVL_COMPRESSION_HAS_B_FRAMES |
          GAVL_COMPRESSION_HAS_P_FRAMES;
      }
    }

  ctx->frame->width  = ctx->vfmt.image_width;
  ctx->frame->height = ctx->vfmt.image_height;
 
  ctx->frame->format = ctx->avctx->pix_fmt;
 
  ctx->flags |= FLAG_INITIALIZED;
  
  return ctx->vsink;
  }

void bg_ffmpeg_codec_set_packet_sink(bg_ffmpeg_codec_context_t * ctx,
                                     gavl_packet_sink_t * psink)
  {
  ctx->psink = psink;
  }

void bg_ffmpeg_codec_flush(bg_ffmpeg_codec_context_t * ctx)
  {
  int result;

  /* Flush */
  if(!(ctx->flags & FLAG_INITIALIZED))
    return;

  if(ctx->type == AVMEDIA_TYPE_VIDEO)
    flush_video(ctx, NULL);
  else // Audio
    {
    while(1)
      {
      // fprintf(stderr, "Flush audio %d\n", ctx->aframe->valid_samples);
      result = flush_audio(ctx);
      if(result <= 0)
        break;
      }
    }
  
  ctx->flags |= FLAG_FLUSHED;
  }

void bg_ffmpeg_codec_destroy(bg_ffmpeg_codec_context_t * ctx)
  {
  if(!(ctx->flags & FLAG_FLUSHED))
    bg_ffmpeg_codec_flush(ctx);
  
  /* Destroy */
  if(ctx->avctx)
    avcodec_free_context(&ctx->avctx);

  if(ctx->pc)
    bg_encoder_pts_cache_destroy(ctx->pc);
  
  if(ctx->aframe)
    gavl_audio_frame_destroy(ctx->aframe);
  if(ctx->vframe)
    gavl_video_frame_destroy(ctx->vframe);
  
  if(ctx->asink)
    gavl_audio_sink_destroy(ctx->asink);
  if(ctx->vsink)
    gavl_video_sink_destroy(ctx->vsink);

  if(ctx->frame->extended_data != ctx->frame->data)
    av_freep(&ctx->frame->extended_data);
  
  if(ctx->frame)
    free(ctx->frame);

  if(ctx->stats_filename)
    free(ctx->stats_filename);
  
  if(ctx->stats_file)
    fclose(ctx->stats_file);

  /* Prevent the buffer from being free()d */
  gavl_buffer_init(&ctx->gp.buf);
  gavl_packet_free(&ctx->gp);
  free(ctx);
  }

/*
 *  Pixelformat conversion stuff
 */

static void convert_frame_bgra(bg_ffmpeg_codec_context_t * ctx, gavl_video_frame_t * f)
  {
  int i, j;
  uint8_t * ptr;
  uint8_t swp;
  
  for(i = 0; i < ctx->vfmt.image_height; i++)
    {
    ptr = f->planes[0] + i * f->strides[0];
    for(j = 0; j < ctx->vfmt.image_width; j++)
      {
      /* RGBA -> BGRA */
      swp = ptr[0];
      ptr[0] = ptr[2];
      ptr[2] = swp;
      ptr += 4;
      }
    }
  }

static void 
get_pixelformat_converter(bg_ffmpeg_codec_context_t * ctx,
                          enum AVPixelFormat fmt, int do_convert)
  {
  if(do_convert & CONVERT_OTHER)
    {
    if(fmt == AV_PIX_FMT_BGRA)
      {
      ctx->convert_frame = convert_frame_bgra;
      }
    }
  
  }
