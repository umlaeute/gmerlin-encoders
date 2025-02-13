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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <config.h>

#include <gmerlin/translation.h>
#include <gmerlin/log.h>
#include <gmerlin/plugin.h>
#include <gmerlin/pluginfuncs.h>
#include <gmerlin/utils.h>

#include <gavl/metatags.h>

#include "ogg_common.h"

#include <vorbiscomment.h>


#define LOG_DOMAIN "ogg"

void * bg_ogg_encoder_create()
  {
  bg_ogg_encoder_t * ret;
  ret = calloc(1, sizeof(*ret));
  return ret;
  }

static void free_stream(bg_ogg_stream_t * s)
  {
  gavl_compression_info_free(&s->ci);
  gavl_dictionary_free(&s->m_stream);
  if(s->stats_file)
    free(s->stats_file);
  gavl_packet_free(&s->last_packet);
  }

void bg_ogg_encoder_destroy(void * data)
  {
  int i;
  bg_ogg_encoder_t * e = data;
  
  if(e->io)
    bg_ogg_encoder_close(e, 1);

  if(e->io_priv)
    gavf_io_destroy(e->io_priv);
  
  if(e->audio_streams)
    {
    for(i = 0; i < e->num_audio_streams; i++)
      free_stream(&e->audio_streams[i]);
    free(e->audio_streams);
    }

  if(e->video_streams)
    {
    for(i = 0; i < e->num_video_streams; i++)
      free_stream(&e->video_streams[i]);
    free(e->video_streams);
    }
  
  if(e->filename)
    free(e->filename);
  
  if(e->audio_parameters)
    bg_parameter_info_destroy_array(e->audio_parameters);
  if(e->video_parameters)
    bg_parameter_info_destroy_array(e->video_parameters);
  
  free(e);
  }

void bg_ogg_encoder_set_callbacks(void * data, bg_encoder_callbacks_t * cb)
  {
  bg_ogg_encoder_t * e = data;
  e->cb = cb;
  }

int
bg_ogg_encoder_open(void * data, const char * file,
                    gavf_io_t * io,
                    const gavl_dictionary_t * metadata,
                    const char * ext)
  {
  bg_ogg_encoder_t * e = data;

  if(file)
    {
    if(!strcmp(file, "-"))
      {
      /* stdout */
      e->io_priv = gavf_io_create_file(stdout, 1, 0, 0);
      }
    else
      {
      FILE * f;
      
      e->filename = bg_filename_ensure_extension(file, ext);
      
      if(!bg_encoder_cb_create_output_file(e->cb, e->filename))
        return 0;
      
      if(!(f = fopen(e->filename, "w")))
        {
        gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "Cannot open file %s: %s",
               file, strerror(errno));
        return 0;
        }
      e->io_priv = gavf_io_create_file(f, 1, 1, 1);
      }
    e->io = e->io_priv;
    }
  else if(io)
    {
    e->io = io;
    }
  
  if(e->open_callback)
    {
    if(!e->open_callback(e->open_callback_data))
      return 0;
    }
  
  e->serialno = rand();
  if(metadata)
    gavl_dictionary_copy(&e->metadata, metadata);
  return 1;
  }

static int bg_ogg_stream_flush_page(bg_ogg_stream_t * s, int force)
  {
  int result;
  ogg_page og;
  memset(&og, 0, sizeof(og));
  if(force || (s->flags & STREAM_FORCE_FLUSH))
    result = ogg_stream_flush(&s->os,&og);
  else
    result = ogg_stream_pageout(&s->os,&og);
  
  if(result)
    {
    if((gavf_io_write_data(s->enc->io,
                           og.header,og.header_len) < og.header_len) ||
       (gavf_io_write_data(s->enc->io,
                           og.body,og.body_len) < og.body_len))
      return -1;
    else
      return 1;
    }
  return 0;
  }

int bg_ogg_stream_write_header_packet(bg_ogg_stream_t * s, ogg_packet * p)
  {
  if(!s->packetno)
    p->b_o_s = 1;
  else
    p->b_o_s = 0;
  
  p->packetno = s->packetno++;
  ogg_stream_packetin(&s->os, p);
  if(!s->num_headers)
    {
    if(bg_ogg_stream_flush_page(s, 1) <= 0)
      {
      gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN,  "Got no ID page");
      return 0;
      }
    }
  s->num_headers++;
  return 1;
  }

int bg_ogg_stream_flush(bg_ogg_stream_t * s, int force)
  {
  int result, ret = 0;
  while((result = bg_ogg_stream_flush_page(s, force)) > 0)
    ret = 1;
  
  if(result < 0)
    return result;
  return ret;
  }

void bg_ogg_packet_to_gavl(ogg_packet * src, gavl_packet_t * dst, int64_t * pts)
  {
  dst->buf.buf = src->packet;
  dst->buf.len = src->bytes;

  if(pts)
    {
    dst->pts = *pts;
    dst->duration = src->granulepos - dst->pts;
    *pts += dst->duration;
    }
  
  if(src->e_o_s)
    dst->flags |= GAVL_PACKET_LAST;
  else
    dst->flags &= ~GAVL_PACKET_LAST;
  }

void bg_ogg_packet_from_gavl(bg_ogg_stream_t * s, gavl_packet_t * src,
                             ogg_packet * dst)
  {
  dst->packet = src->buf.buf;
  dst->bytes = src->buf.len;
  dst->granulepos = src->pts + src->duration;
  if(src->flags & GAVL_PACKET_LAST)
    dst->e_o_s = 1;
  else
    dst->e_o_s = 0;

  if(s->codec->convert_packet)
    s->codec->convert_packet(s, src, dst);
  }

static gavl_sink_status_t write_gavl_packet(void * data, gavl_packet_t * p)
  {
  bg_ogg_stream_t * s = data;

  /* Flush the last packet */
  if(s->last_packet.buf.len)
    {
    ogg_packet op;
    memset(&op, 0, sizeof(op));
    bg_ogg_packet_from_gavl(s, &s->last_packet, &op);
    op.packetno = s->packetno++;
    ogg_stream_packetin(&s->os, &op);
    /* Flush pages if any */
    if(bg_ogg_stream_flush(s, 0) < 0)
      return GAVL_SINK_ERROR;
    }
  /* Save this packet */
  gavl_packet_copy(&s->last_packet, p);
  return GAVL_SINK_OK;
  }

static int flush_stream(bg_ogg_stream_t * s)
  {
  /* Flush the last packet */
  if(s->last_packet.buf.len)
    {
    ogg_packet op;
    memset(&op, 0, sizeof(op));
    bg_ogg_packet_from_gavl(s, &s->last_packet, &op);
    op.packetno = s->packetno++;
    op.e_o_s = 1;
    ogg_stream_packetin(&s->os, &op);
    /* Flush pages if any */
    if(bg_ogg_stream_flush(s, 1) < 0)
      return 0;
    }
  return 1;
  }

static bg_ogg_stream_t * append_stream(bg_ogg_encoder_t * e,
                                       bg_ogg_stream_t ** streams_p,
                                       int * num_streams_p,
                                       const gavl_dictionary_t * m)
  {
  bg_ogg_stream_t * ret;
  bg_ogg_stream_t * streams = *streams_p;
  int num_streams = *num_streams_p;

  streams = realloc(streams, (num_streams+1)*sizeof(*streams));

  ret = streams + num_streams;
  ret->index = num_streams;
  
  memset(ret, 0, sizeof(*ret));
  ogg_stream_init(&ret->os, e->serialno++);
  
  gavl_dictionary_copy(&ret->m_stream, m);
  
  ret->enc = e;
  ret->index = num_streams;
  ret->m_global = &e->metadata;
  
  num_streams++;
  
  *num_streams_p = num_streams;
  *streams_p = streams;
  return ret;
  }

bg_ogg_stream_t *
bg_ogg_encoder_add_audio_stream(void * data,
                                const gavl_dictionary_t * m,
                                const gavl_audio_format_t * format)
  {
  bg_ogg_stream_t * s;
  bg_ogg_encoder_t * e = data;
  s = append_stream(e, &e->audio_streams, &e->num_audio_streams, m);
  gavl_audio_format_copy(&s->afmt, format);
  gavl_metadata_delete_compression_fields(&s->m_stream);
  return s;
  }

bg_ogg_stream_t *
bg_ogg_encoder_add_video_stream(void * data,
                                const gavl_dictionary_t * m,
                                const gavl_video_format_t * format)
  {
  bg_ogg_stream_t * s;
  bg_ogg_encoder_t * e = data;
  s = append_stream(e, &e->video_streams, &e->num_video_streams, m);
  gavl_video_format_copy(&s->vfmt, format);
  gavl_metadata_delete_compression_fields(&s->m_stream);
  return s;
  }

bg_ogg_stream_t *
bg_ogg_encoder_add_audio_stream_compressed(void * data,
                                           const gavl_dictionary_t * m,
                                           const gavl_audio_format_t * format,
                                           const gavl_compression_info_t * ci)
  {
  bg_ogg_stream_t * s;
  bg_ogg_encoder_t * e = data;
  s = append_stream(e, &e->audio_streams, &e->num_audio_streams, m);
  gavl_compression_info_copy(&s->ci, ci);
  gavl_audio_format_copy(&s->afmt, format);
  s->flags |= STREAM_COMPRESSED;
  return s;
  }

bg_ogg_stream_t *
bg_ogg_encoder_add_video_stream_compressed(void * data,
                                           const gavl_dictionary_t * m,
                                           const gavl_video_format_t * format,
                                           const gavl_compression_info_t * ci)
  {
  bg_ogg_stream_t * s;
  bg_ogg_encoder_t * e = data;
  s = append_stream(e, &e->video_streams, &e->num_video_streams, m);
  gavl_compression_info_copy(&s->ci, ci);
  gavl_video_format_copy(&s->vfmt, format);
  s->flags |= STREAM_COMPRESSED;
  return s;
  }

void bg_ogg_encoder_init_stream(void * data, bg_ogg_stream_t * s,
                                const bg_ogg_codec_t * codec)
  {
  s->codec = codec;
  s->codec_priv = s->codec->create();
  }

void
bg_ogg_encoder_set_audio_parameter(void * data, int stream,
                                   const char * name,
                                   const gavl_value_t * val)
  {
  bg_ogg_encoder_t * e = data;
  bg_ogg_stream_t * s = &e->audio_streams[stream];
  s->codec->set_parameter(s->codec_priv, name, val);
  }

void bg_ogg_encoder_set_video_parameter(void * data, int stream,
                                        const char * name,
                                        const gavl_value_t * val)
  {
  bg_ogg_encoder_t * e = data;
  bg_ogg_stream_t * s = &e->video_streams[stream];
  s->codec->set_parameter(s->codec_priv, name, val);
  }

int bg_ogg_encoder_set_video_pass(void * data, int stream,
                                  int pass, int total_passes,
                                  const char * stats_file)
  {
  bg_ogg_encoder_t * e = data;
  bg_ogg_stream_t * s = &e->video_streams[stream];

  s->pass = pass;
  s->total_passes = total_passes;
  s->stats_file = gavl_strrep(s->stats_file, stats_file);
  return 1;
  }

static int start_audio(bg_ogg_encoder_t * e, int stream)
  {
  bg_ogg_stream_t * s = &e->audio_streams[stream];

  if(s->flags & STREAM_COMPRESSED)
    {
    if(!s->codec->init_audio_compressed(s))
      return 0;
    }
  else
    {
    if(!(s->asink = s->codec->init_audio(s->codec_priv, &s->ci,
                                         &s->afmt, &s->m_stream)))
      return 0;
    
    if(s->ci.id != GAVL_CODEC_ID_NONE)
      {
      if(!s->codec->init_audio_compressed(s))
        return 0;
      }
    }
  s->psink_out = gavl_packet_sink_create(NULL, write_gavl_packet, s);
  s->codec->set_packet_sink(s->codec_priv, s->psink_out);
  return 1;
  }

static int start_video(bg_ogg_encoder_t * e, int stream)
  {
  bg_ogg_stream_t * s = &e->video_streams[stream];
  
  if(s->flags & STREAM_COMPRESSED)
    {
    if(!s->codec->init_video_compressed(s))
      return 0;
    }
  else
    {
    if(!(s->vsink = s->codec->init_video(s->codec_priv, &s->ci,
                                         &s->vfmt,
                                         &s->m_stream)))
      return 0;
    
    if(s->pass)
      {
      if(!s->codec->set_video_pass)
        return 0;
      
      else 
        if(!s->codec->set_video_pass(s->codec_priv,
                                     s->pass,
                                     s->total_passes,
                                     s->stats_file))
          return 0;
      }
    
    if(!s->codec->init_video_compressed(s))
      return 0;
    }

  s->psink_out = gavl_packet_sink_create(NULL, write_gavl_packet, s);
  s->codec->set_packet_sink(s->codec_priv, s->psink_out);
  return 1;
  }

int bg_ogg_encoder_start(void * data)
  {
  int i;
  bg_ogg_encoder_t * e = data;
  
  /* Start encoders and write identification headers */
  for(i = 0; i < e->num_video_streams; i++)
    {
    if(!start_video(e, i))
      return 0;
    }
  for(i = 0; i < e->num_audio_streams; i++)
    {
    if(!start_audio(e, i))
      return 0;
    }

  /* Write remaining header pages */
  for(i = 0; i < e->num_video_streams; i++)
    {
    bg_ogg_stream_t * s = &e->video_streams[i];
    if(bg_ogg_stream_flush(s, 1) < 0)
      return 0;
    }
  for(i = 0; i < e->num_audio_streams; i++)
    {
    bg_ogg_stream_t * s = &e->audio_streams[i];
    if(bg_ogg_stream_flush(s, 1) < 0)
      return 0;
    }
  e->started = 1;
  return 1;
  }

gavl_audio_sink_t * bg_ogg_encoder_get_audio_sink(void * data, int stream)
  {
  bg_ogg_encoder_t * e = data;
  return e->audio_streams[stream].asink;
  }

gavl_video_sink_t * bg_ogg_encoder_get_video_sink(void * data, int stream)
  {
  bg_ogg_encoder_t * e = data;
  return e->video_streams[stream].vsink;
  }

gavl_packet_sink_t *
bg_ogg_encoder_get_audio_packet_sink(void * data, int stream)
  {
  bg_ogg_encoder_t * e = data;
  return e->audio_streams[stream].psink_out;
  }

gavl_packet_sink_t *
bg_ogg_encoder_get_video_packet_sink(void * data, int stream)
  {
  bg_ogg_encoder_t * e = data;
  return e->video_streams[stream].psink_out;
  }

void bg_ogg_encoder_update_metadata(void * data, const gavl_dictionary_t * new_metadata)
  {
  int i;

  bg_ogg_encoder_t * e = data;

  /* Copy all tags, overriding what's already there */
  gavl_dictionary_copy(&e->metadata, new_metadata);
  
  if(!e->started)
    return;
  
  /* Flush all data */
  for(i = 0; i < e->num_audio_streams; i++)
    {
    bg_ogg_stream_t * s = &e->audio_streams[i];
    bg_ogg_stream_reset(s, e->serialno++);
    }
  for(i = 0; i < e->num_video_streams; i++)
    {
    bg_ogg_stream_t * s = &e->video_streams[i];
    bg_ogg_stream_reset(s, e->serialno++);
    }
  
  /* Reinitialize with new metadata */
  for(i = 0; i < e->num_audio_streams; i++)
    {
    bg_ogg_stream_t * s = &e->audio_streams[i];
    s->codec->init_audio_compressed(s);
    }
  for(i = 0; i < e->num_video_streams; i++)
    {
    bg_ogg_stream_t * s = &e->video_streams[i];
    s->codec->init_video_compressed(s);
    }
  
  /* Re-write header packets */
  for(i = 0; i < e->num_audio_streams; i++)
    {
    bg_ogg_stream_t * s = &e->audio_streams[i];
    bg_ogg_stream_flush(s, 1);
    }
  for(i = 0; i < e->num_video_streams; i++)
    {
    bg_ogg_stream_t * s = &e->video_streams[i];
    bg_ogg_stream_flush(s, 1);
    }
  
  }

int bg_ogg_encoder_close(void * data, int do_delete)
  {
  int ret = 1;
  int i;
  bg_ogg_encoder_t * e = data;

  if(!e->io)
    return 1;
  
  for(i = 0; i < e->num_audio_streams; i++)
    {
    bg_ogg_stream_t * s = &e->audio_streams[i];
    
    if(!s->codec->close(e->audio_streams[i].codec_priv))
      {
      ret = 0;
      break;
      }

    flush_stream(s);
    ogg_stream_clear(&s->os);
    
    if(s->asink)
      {
      gavl_audio_sink_destroy(s->asink);
      s->asink = NULL;
      }
    if(s->psink_out)
      {
      gavl_packet_sink_destroy(s->psink_out);
      s->psink_out = NULL;
      }
    }
  for(i = 0; i < e->num_video_streams; i++)
    {
    bg_ogg_stream_t * s = &e->video_streams[i];
    if(!s->codec->close(s->codec_priv))
      {
      ret = 0;
      break;
      }
    flush_stream(s);
    ogg_stream_clear(&s->os);

    if(s->vsink)
      {
      gavl_video_sink_destroy(s->vsink);
      s->vsink = NULL;
      }
    if(s->psink_out)
      {
      gavl_packet_sink_destroy(s->psink_out);
      s->psink_out = NULL;
      }
    }

  if(e->io_priv)
    gavf_io_destroy(e->io_priv);
  
  e->io_priv = NULL;
  e->io = NULL;

  if(do_delete && e->filename)
    remove(e->filename);
  return ret;
  }

static const bg_parameter_info_t codec_parameters[] =
  {
    {
      .name =      "codec",
      .long_name = TRS("Codec"),
      .type =      BG_PARAMETER_MULTI_MENU,
    },
    { /* End of parameters */ }
  };

static bg_parameter_info_t *
create_codec_parameters(bg_ogg_codec_t const * const * codecs)
  {
  int num_codecs;
  int i;
  bg_parameter_info_t * ret = 0;

  num_codecs = 0;
  while(codecs[num_codecs])
    num_codecs++;
    
  ret = bg_parameter_info_copy_array(codec_parameters);
  ret[0].multi_names_nc =
    calloc(num_codecs+1, sizeof(*ret[0].multi_names));
  ret[0].multi_labels_nc =
    calloc(num_codecs+1, sizeof(*ret[0].multi_labels));
  ret[0].multi_parameters_nc =
    calloc(num_codecs+1, sizeof(*ret[0].multi_parameters));
  for(i = 0; i < num_codecs; i++)
    {
    ret[0].multi_names_nc[i]  =
      gavl_strdup(codecs[i]->name);
    ret[0].multi_labels_nc[i] =
      gavl_strdup(codecs[i]->long_name);
      
    if(codecs[i]->get_parameters)
      ret[0].multi_parameters_nc[i] =
        bg_parameter_info_copy_array(codecs[i]->get_parameters());
    }
  gavl_value_set_string(&ret[0].val_default, codecs[0]->name);
  bg_parameter_info_set_const_ptrs(&ret[0]);
  return ret;
  }
  

bg_parameter_info_t *
bg_ogg_encoder_get_audio_parameters(bg_ogg_encoder_t * e,
                                    bg_ogg_codec_t const * const * audio_codecs)
  {
  if(!e->audio_parameters)
    e->audio_parameters = create_codec_parameters(audio_codecs);
  return e->audio_parameters;
  }

bg_parameter_info_t *
bg_ogg_encoder_get_video_parameters(bg_ogg_encoder_t * e,
                                    bg_ogg_codec_t const * const * video_codecs)
  {
  if(!e->video_parameters)
    e->video_parameters = create_codec_parameters(video_codecs);
  return e->video_parameters;
  }

/* Comment building stuff */

void
bg_ogg_create_comment_packet(const uint8_t * prefix,
                             int prefix_len,
                             const gavl_dictionary_t * m_stream,
                             const gavl_dictionary_t * m_global,
                             int framing, ogg_packet * op)
  {
  int len;
  gavf_io_t * io;
  uint8_t * ptr;
  
  io = gavf_io_create_mem_write();
  bg_vorbis_comment_write(io, m_stream, m_global, framing);
  
  ptr = gavf_io_mem_get_buf(io, &len);
  
  /* Write mandatory fields */
  
  op->packet = malloc(len + prefix_len);
  op->bytes = len + prefix_len;
  
  if(prefix_len)
    memcpy(op->packet, prefix, prefix_len);

  memcpy(op->packet + prefix_len, ptr, len);

  fprintf(stderr, "Built comment packet:\n");
  gavl_hexdump(op->packet, op->bytes, 16);
  
  free(ptr);
  gavf_io_destroy(io);
  }

void bg_ogg_free_comment_packet(ogg_packet * op)
  {
  free(op->packet);
  }

void bg_ogg_set_vorbis_channel_setup(gavl_audio_format_t * format)
  {
  if(format->channel_locations[0] == GAVL_CHID_AUX)
    return;
  
  switch(format->num_channels)
    {
    case 1:
      format->channel_locations[0] = GAVL_CHID_FRONT_CENTER;
      break;
    case 2:
      format->channel_locations[0] = GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] = GAVL_CHID_FRONT_RIGHT;
      break;
    case 3:
      format->channel_locations[0] = GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] = GAVL_CHID_FRONT_CENTER;
      format->channel_locations[2] = GAVL_CHID_FRONT_RIGHT;
      break;
    case 4:
      format->channel_locations[0] = GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] = GAVL_CHID_FRONT_RIGHT;
      format->channel_locations[2] = GAVL_CHID_REAR_LEFT;
      format->channel_locations[3] = GAVL_CHID_REAR_RIGHT;
      break;
    case 5:
      format->channel_locations[0] =  GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] =  GAVL_CHID_FRONT_CENTER;
      format->channel_locations[2] =  GAVL_CHID_FRONT_RIGHT;
      format->channel_locations[3] =  GAVL_CHID_REAR_LEFT;
      format->channel_locations[4] =  GAVL_CHID_REAR_RIGHT;
      break;
    case 6:
      format->channel_locations[0] =  GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] =  GAVL_CHID_FRONT_CENTER;
      format->channel_locations[2] =  GAVL_CHID_FRONT_RIGHT;
      format->channel_locations[3] =  GAVL_CHID_REAR_LEFT;
      format->channel_locations[4] =  GAVL_CHID_REAR_RIGHT;
      format->channel_locations[5] =  GAVL_CHID_LFE;
      break;
    case 7:
      format->channel_locations[0] =  GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] =  GAVL_CHID_FRONT_CENTER;
      format->channel_locations[2] =  GAVL_CHID_FRONT_RIGHT;
      format->channel_locations[3] =  GAVL_CHID_SIDE_LEFT;
      format->channel_locations[4] =  GAVL_CHID_SIDE_RIGHT;
      format->channel_locations[5] =  GAVL_CHID_REAR_CENTER;
      format->channel_locations[6] =  GAVL_CHID_LFE;
      break;
    case 8:
      format->channel_locations[0] =  GAVL_CHID_FRONT_LEFT;
      format->channel_locations[1] =  GAVL_CHID_FRONT_CENTER;
      format->channel_locations[2] =  GAVL_CHID_FRONT_RIGHT;
      format->channel_locations[3] =  GAVL_CHID_SIDE_LEFT;
      format->channel_locations[4] =  GAVL_CHID_SIDE_RIGHT;
      format->channel_locations[5] =  GAVL_CHID_REAR_LEFT;
      format->channel_locations[6] =  GAVL_CHID_REAR_RIGHT;
      format->channel_locations[7] =  GAVL_CHID_LFE;
      break;
    }
  }

void bg_ogg_stream_reset(bg_ogg_stream_t * s, long serialno)
  {
  flush_stream(s);
  s->packetno = 0;
  s->num_headers = 0;
  ogg_stream_clear(&s->os);
  ogg_stream_init(&s->os, serialno);
  
  }
