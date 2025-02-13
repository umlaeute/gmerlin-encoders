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

#include <string.h>
#include <stdlib.h>
#include <config.h>
#include <gmerlin_encoders.h>

#include <bgshout.h>

#include <gmerlin/plugin.h>
#include <gmerlin/utils.h>
#include <gmerlin/translation.h>
#include <gmerlin/cfg_registry.h>

#include <theora/theora.h>

#include "ogg_common.h"

#undef HAVE_FLAC /* Switch off flac */

extern const bg_ogg_codec_t bg_theora_codec;
extern const bg_ogg_codec_t bg_vorbis_codec;


#ifdef HAVE_OPUS
extern const bg_ogg_codec_t bg_opus_codec;
#endif

#ifdef HAVE_FLAC
extern const bg_ogg_codec_t bg_flacogg_codec;
#endif

static bg_ogg_codec_t const * const audio_codecs[] =
  {
    &bg_vorbis_codec,
#ifdef HAVE_OPUS
    &bg_opus_codec,
#endif
#ifdef HAVE_FLAC
    &bg_flacogg_codec,
#endif
    NULL,
  };

static const bg_parameter_info_t * get_audio_parameters_b_ogg(void * data)
  {
  return bg_ogg_encoder_get_audio_parameters(data, audio_codecs);
  }

static const bg_parameter_info_t * get_video_parameters_b_ogg(void * data)
  {
  return bg_theora_codec.get_parameters();
  }

static int add_audio_stream_b_ogg(void * data,
                                  const gavl_dictionary_t * m,
                                  const gavl_audio_format_t * format)
  {
  bg_ogg_stream_t * ret;
  ret = bg_ogg_encoder_add_audio_stream(data, m, format);
  return ret->index;
  }

static int add_video_stream_b_ogg(void * data,
                                  const gavl_dictionary_t * m,
                                  const gavl_video_format_t * format)
  {
  bg_ogg_stream_t * ret;
  ret = bg_ogg_encoder_add_video_stream(data, m, format);
  bg_ogg_encoder_init_stream(data, ret, &bg_theora_codec);
  return ret->index;
  }

static void set_audio_parameter_b_ogg(void * data, int stream,
                                      const char * name,
                                      const gavl_value_t * val)
  {
  int i;
  bg_ogg_encoder_t * e = data;
  bg_ogg_stream_t * s = &e->audio_streams[stream];

  if(!name)
    return;
  if(!strcmp(name, "codec"))
    {
    const char * codec_name;
    i = 0;

    codec_name = bg_multi_menu_get_selected_name(val);
    
    while(audio_codecs[i])
      {
      if(!strcmp(audio_codecs[i]->name, codec_name))
        {
        bg_ogg_encoder_init_stream(data, s, audio_codecs[i]);
        break;
        }
      i++;
      }
    bg_cfg_section_apply(bg_multi_menu_get_selected(val),
                         NULL,
                         s->codec->set_parameter,
                         s->codec_priv);
    }
  }

static int write_callback(void * priv, const uint8_t * data, int len)
  {
  return bg_shout_write(priv, data, len);
  }

static void close_callback(void * data)
  {
  bg_shout_destroy(data);
  }

static int open_callback(void * data)
  {
  return bg_shout_open(data);
  }

#if 0
static void update_metadata(void * data,
                            const gavl_dictionary_t * m)
  {
  bg_ogg_encoder_t * enc = data;
  bg_shout_update_metadata(enc->open_callback_data, m);
  }
#endif

static void * create_b_ogg()
  {
  bg_ogg_encoder_t * ret = bg_ogg_encoder_create();
  ret->open_callback_data = bg_shout_create(SHOUT_FORMAT_OGG);
  ret->open_callback = open_callback;

  ret->io_priv = gavf_io_create(NULL,
                                write_callback,
                                NULL,
                                close_callback,
                                NULL,
                                GAVF_IO_CAN_WRITE,
                                ret->open_callback_data);
  return ret;
  }

static const bg_parameter_info_t * get_parameters_b_ogg(void * data)
  {
  return bg_shout_get_parameters();
  }

static void set_parameter_b_ogg(void * data, const char * name,
                                const gavl_value_t * val)
  {
  bg_ogg_encoder_t * enc = data;
  bg_shout_set_parameter(enc->open_callback_data, name, val);
  }

static int
open_b_ogg(void * data, const char * file,
           const gavl_dictionary_t * metadata)
  {
  bg_ogg_encoder_t * enc = data;
  if(!bg_ogg_encoder_open(enc, NULL, enc->io_priv, metadata,
                          NULL))
    return 0;
  if(metadata)
    bg_shout_update_metadata(enc->open_callback_data, metadata);
  return 1;
  }

const bg_encoder_plugin_t the_plugin =
  {
    .common =
    {
      BG_LOCALE,
      .name =            "b_ogg",       /* Unique short name */
      .long_name =       TRS("Ogg Broadcaster"),
      .description =     TRS("Broadcaster for Ogg streams using libshout. Supports vorbis, theora and speex."),
      .type =            BG_PLUGIN_ENCODER,
      .flags =           BG_PLUGIN_BROADCAST,
      .priority =        5,
      .create =            create_b_ogg,
      .destroy =           bg_ogg_encoder_destroy,

      .get_parameters =    get_parameters_b_ogg,
      .set_parameter =     set_parameter_b_ogg,
    },
    .max_audio_streams =   -1,
    .max_video_streams =   -1,
    
    .set_callbacks =       bg_ogg_encoder_set_callbacks,
    .open =                open_b_ogg,
    
    .get_audio_parameters =    get_audio_parameters_b_ogg,
    .get_video_parameters =    get_video_parameters_b_ogg,

    .add_audio_stream =        add_audio_stream_b_ogg,
    .add_video_stream =        add_video_stream_b_ogg,
    
    .set_audio_parameter =     set_audio_parameter_b_ogg,
    .set_video_parameter =     bg_ogg_encoder_set_video_parameter,
    
    .start =                  bg_ogg_encoder_start,
    
    .get_audio_sink =        bg_ogg_encoder_get_audio_sink,
    .get_video_sink =        bg_ogg_encoder_get_video_sink,
    
    .close =               bg_ogg_encoder_close,
  };

/* Include this into all plugin modules exactly once
   to let the plugin loader obtain the API version */
BG_GET_PLUGIN_API_VERSION;
