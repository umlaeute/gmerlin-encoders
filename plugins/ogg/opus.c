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

#include <gmerlin/translation.h>
#include <gmerlin/plugin.h>
#include <gmerlin/utils.h>
#include <gmerlin/log.h>
#define LOG_DOMAIN "oggopus"

#include <gavl/metatags.h>

#include "ogg_common.h"

#include <opus.h>
#include <opus_multistream.h>

#define BITRATE_VBR   0
#define BITRATE_CVBR  1
#define BITRATE_CBR   2

typedef struct
  {
  uint8_t  version;
  uint8_t  channel_count;
  uint16_t pre_skip;
  uint32_t samplerate;
  int16_t  output_gain;
  uint8_t  channel_mapping;
  
  struct
    {
    uint8_t stream_count;
    uint8_t coupled_count;

    uint8_t map[256]; // 255 actually
    
    } chtab; // Channel mapping table
  } opus_header_t;

#define MAX_HEADER_LEN (8+1+1+2+4+2+1+1+1+256)

static void setup_header(opus_header_t * h, gavl_audio_format_t * format);
static void header_to_packet(opus_header_t * h, gavl_buffer_t * ret);

typedef struct
  {
  /* Config */
  int application;
  int bitrate_mode;
  int complexity;
  int fec;
  int dtx;
  int loss_perc;
  int bandwidth;
  int max_bandwidth;
  int bitrate;
  int frame_size; // in 100 us
  
  /* Encoder */
  
  OpusMSEncoder * enc;
  opus_header_t h;
  opus_int32 lookahead;
  gavl_audio_frame_t * frame;
  gavl_audio_format_t * format;

  int64_t samples_read;

  uint8_t * enc_buffer;
  int enc_buffer_size;

  int64_t pts;

  int to_skip;

  gavl_packet_sink_t * psink;
  
  } opus_t;

static void * create_opus()
  {
  opus_t * ret;
  ret = calloc(1, sizeof(*ret));
  return ret;
  }

static const bg_parameter_info_t parameters[] =
  {
    {
      .name =        "application",
      .long_name =   TRS("Application"),
      .type =        BG_PARAMETER_STRINGLIST,
      .val_default = GAVL_VALUE_INIT_STRING("audio"),
      .multi_names =  (char const *[]){ "audio", "voip", NULL },
      .multi_labels = (char const *[]){ TRS("Audio"), TRS("VOIP"),
                                        NULL },
    },
    {
      .name =        "bitrate_mode",
      .long_name =   TRS("Bitrate mode"),
      .type =        BG_PARAMETER_STRINGLIST,
      .val_default = GAVL_VALUE_INIT_STRING("vbr"),
      .multi_names =  (char const *[]){ "vbr", "cvbr", "cbr", NULL },
      .multi_labels = (char const *[]){ TRS("VBR"), TRS("Constrained VBR"),
                                        TRS("CBR"),
                                        NULL },
      .help_string = TRS("Bitrate mode")
    },
    {
      .name =        "bitrate",
      .long_name =   TRS("Bitrate"),
      .type =        BG_PARAMETER_INT,
      .val_min =     GAVL_VALUE_INIT_INT(0),
      .val_max =     GAVL_VALUE_INIT_INT(512000),
      .val_default = GAVL_VALUE_INIT_INT(0),
      .help_string = TRS("Bitrate (in bps). 0 means auto."),
    },
    {
      .name =      "complexity",
      .long_name = TRS("Encoding complexity"),
      .type =      BG_PARAMETER_SLIDER_INT,
      .val_min =     GAVL_VALUE_INIT_INT(1),
      .val_max =     GAVL_VALUE_INIT_INT(10),
      .val_default = GAVL_VALUE_INIT_INT(10),
    },
    {
      .name =        "frame_size",
      .long_name =   TRS("Frame size (ms)"),
      .type =        BG_PARAMETER_STRINGLIST,
      /* Multiplied by 10 */
      .val_default = GAVL_VALUE_INIT_STRING("200"),
      .multi_names =  (char const *[]){ "25",
                                        "50",
                                        "100",
                                        "200",
                                        "400",
                                        "600",
                                        NULL },
      .multi_labels = (char const *[]){ "2.5",
                                        "5",
                                        "10",
                                        "20",
                                        "40",
                                        "60",
                                        NULL },
      .help_string = TRS("Smaller framesizes achieve lower latency but less quality at a given bitrate. Sizes greater than 20ms are only interesting at fairly low bitrates. ")
    },
    {
      .name =        "dtx",
      .long_name =   TRS("Enable discontinuous transmission"),
      .type =        BG_PARAMETER_CHECKBUTTON,
    },
    {
      .name =        "inband_fec",
      .long_name =   TRS("Enable inband forward error correction"),
      .type =        BG_PARAMETER_CHECKBUTTON,
    },
    {
      .name =        "bandwidth",
      .long_name =   TRS("Bandwidth"),
      .type =        BG_PARAMETER_STRINGLIST,
      .val_default = GAVL_VALUE_INIT_STRING("auto"),
      .multi_names =  (char const *[]){ "auto", "narrow", "medium", "wide", "superwide", "full", NULL },
      .multi_labels = (char const *[]){ TRS("Automatic"),
                                        TRS("Narrow (4 kHz)"),
                                        TRS("Medium (6 kHz)"),
                                        TRS("Wide (8 kHz)"),
                                        TRS("Superwide (12 kHz)"),
                                        TRS("Full (20 kHz)"),
                                        NULL },
    },
#if 0 // Request not implemented
    {
      .name =        "max_bandwidth",
      .long_name =   TRS("Maximum bandwidth"),
      .type =        BG_PARAMETER_STRINGLIST,
      .val_default = GAVL_VALUE_INIT_STRING("full"),
      .multi_names =  (char const *[]){ "narrow", "medium", "wide", "superwide", "full", NULL },
      .multi_labels = (char const *[]){ TRS("Narrow (4 kHz)"),
                                        TRS("Medium (6 kHz)"),
                                        TRS("Wide (8 kHz)"),
                                        TRS("Superwide (12 kHz)"),
                                        TRS("Full (20 kHz)"),
                                        NULL },
    },
#endif
    {
      .name =      "loss_perc",
      .long_name = TRS("Loss percentage"),
      .type =      BG_PARAMETER_SLIDER_INT,
      .val_min =     GAVL_VALUE_INIT_INT(0),
      .val_max =     GAVL_VALUE_INIT_INT(100),
      .val_default = GAVL_VALUE_INIT_INT(0),
      
    },
    { /* End */ },
  };

static const bg_parameter_info_t * get_parameters_opus()
  {
  return parameters;
  }

static void set_packet_sink(void * data, gavl_packet_sink_t * psink)
  {
  opus_t * opus = data;
  opus->psink = psink;
  }

static void set_parameter_opus(void * data, const char * name,
                               const gavl_value_t * v)
  {
  opus_t * opus = data;
  
  if(!name)
    return;
  
  if(!strcmp(name, "application"))
    {
    if(!strcmp(v->v.str, "audio"))
      opus->application = OPUS_APPLICATION_AUDIO;
    else if(!strcmp(v->v.str, "voip"))
      opus->application = OPUS_APPLICATION_VOIP;
    }
  else if(!strcmp(name, "bitrate_mode"))
    {
    if(!strcmp(v->v.str, "vbr"))
      opus->bitrate_mode = BITRATE_VBR;
    else if(!strcmp(v->v.str, "cvbr"))
      opus->bitrate_mode = BITRATE_CVBR;
    else if(!strcmp(v->v.str, "cbr"))
      opus->bitrate_mode = BITRATE_CBR;
    }
  else if(!strcmp(name, "bitrate"))
    {
    opus->bitrate = v->v.i;
    }
  else if(!strcmp(name, "complexity"))
    {
    opus->complexity = v->v.i; 
    }
  else if(!strcmp(name, "dtx"))
    {
    opus->dtx = v->v.i; 
    }
  else if(!strcmp(name, "inband_fec"))
    {
    opus->fec = v->v.i; 
    }
  else if(!strcmp(name, "bandwidth"))
    {
    if(!strcmp(v->v.str, "narrow"))
      opus->bandwidth = OPUS_BANDWIDTH_NARROWBAND;
    else if(!strcmp(v->v.str, "medium"))
      opus->bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
    else if(!strcmp(v->v.str, "wide"))
      opus->bandwidth = OPUS_BANDWIDTH_WIDEBAND;
    else if(!strcmp(v->v.str, "superwide"))
      opus->bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
    else if(!strcmp(v->v.str, "full"))
      opus->bandwidth = OPUS_BANDWIDTH_FULLBAND;
    else if(!strcmp(v->v.str, "auto"))
      opus->bandwidth = OPUS_AUTO;
    }
  else if(!strcmp(name, "loss_perc"))
    {
    opus->loss_perc = v->v.i; 
    }
  else if(!strcmp(name, "frame_size"))
    {
    opus->frame_size = atoi(v->v.str); 
    }
  
  }

static int flush_frame(opus_t * opus, int eof)
  {
  gavl_packet_t gp;
  int result;

  //  fprintf(stderr, "Flush frame %d %d\n", opus->frame->valid_samples,
  //          opus->format->samples_per_frame);
  
  if(opus->frame && opus->frame->valid_samples)
    {
    
    if(opus->frame->valid_samples < opus->format->samples_per_frame)
      {
      int block_align = opus->format->num_channels *
        gavl_bytes_per_sample(opus->format->sample_format);
    
      memset(opus->frame->samples.s_8 +
             block_align * opus->frame->valid_samples, 0,
             (opus->format->samples_per_frame - opus->frame->valid_samples) *
             block_align);
      }

    if(opus->format->sample_format == GAVL_SAMPLE_FLOAT)
      {
      result = opus_multistream_encode_float(opus->enc,
                                             opus->frame->samples.f,
                                             opus->format->samples_per_frame,
                                             opus->enc_buffer,
                                             opus->enc_buffer_size);
      }
    else
      {
      result = opus_multistream_encode(opus->enc,
                                       opus->frame->samples.s_16,
                                       opus->format->samples_per_frame,
                                       opus->enc_buffer,
                                       opus->enc_buffer_size);
      }
    
    if(result < 0)
      {
      gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "Encoding failed: %s", opus_strerror(result));
      return 0;
      }
    
    /* Create packet */
    gavl_packet_init(&gp);
    gp.buf.buf = opus->enc_buffer;
    gp.buf.len = result;
    if(eof)
      gp.flags |= GAVL_PACKET_LAST;

    gp.duration = (opus->frame->valid_samples * 48000) / opus->format->samplerate;
    gp.pts = opus->pts;
    opus->pts += gp.duration;
    gavl_packet_sink_put_packet(opus->psink, &gp);
    opus->frame->valid_samples = 0;
    }
  return 1;
  }

static gavl_sink_status_t
write_audio_frame_opus(void * data, gavl_audio_frame_t * frame)
  {
  int result = 1;
  int samples_read = 0;
  int samples_copied;
  
  opus_t * opus = data;

  /* Handle lookahead */
  while(opus->lookahead)
    {
    gavl_audio_frame_mute(opus->frame, opus->format);

    opus->frame->valid_samples = opus->lookahead;
    if(opus->frame->valid_samples > opus->format->samples_per_frame)
      opus->frame->valid_samples = opus->format->samples_per_frame;

    samples_copied = opus->frame->valid_samples;
    
    if(opus->frame->valid_samples == opus->format->samples_per_frame)
      {
      result = flush_frame(opus, 0);
      if(!result)
        break;
      }
    opus->lookahead -= samples_copied;
    }
  
  // fprintf(stderr, "write_audio %d\n", frame->valid_samples);
  while(samples_read < frame->valid_samples)
    {
    samples_copied =
      gavl_audio_frame_copy(opus->format,
                            opus->frame,
                            frame,
                            opus->frame->valid_samples, /* dst_pos */
                            samples_read,                /* src_pos */
                            opus->format->samples_per_frame -
                            opus->frame->valid_samples, /* dst_size */
                            frame->valid_samples - samples_read /* src_size */ );
    opus->frame->valid_samples += samples_copied;
    samples_read += samples_copied;

    if(opus->frame->valid_samples == opus->format->samples_per_frame)
      {
      result = flush_frame(opus, 0);
      if(!result)
        break;
      }
    }
  
  opus->samples_read += frame->valid_samples;
  return result ? GAVL_SINK_OK : GAVL_SINK_ERROR; 
  }


static gavl_audio_sink_t *
init_opus(void * data, gavl_compression_info_t * ci,
          gavl_audio_format_t * format,
          gavl_dictionary_t * stream_metadata)
  {
  int err;
  opus_t * opus = data;
  //  uint8_t header[MAX_HEADER_LEN];
  /* Setup header (also adjusts format) */

  setup_header(&opus->h, format);

  format->samples_per_frame =
    (format->samplerate * opus->frame_size) / 10000;
  
  /* Create encoder */

  opus->enc = opus_multistream_encoder_create(format->samplerate,
                                              opus->h.channel_count,
                                              opus->h.chtab.stream_count,
                                              opus->h.chtab.coupled_count,
                                              opus->h.chtab.map,
                                              opus->application,
                                              &err);
  
  /* Apply config */

  switch(opus->bitrate_mode)
    {
    case BITRATE_VBR:
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR(1));
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR_CONSTRAINT(0));
      break;
    case BITRATE_CVBR:
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR(1));
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR_CONSTRAINT(1));
      break;
    case BITRATE_CBR:
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR(0));
      opus_multistream_encoder_ctl(opus->enc, OPUS_SET_VBR_CONSTRAINT(0));
      break;
    }
  
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_BITRATE(opus->bitrate))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting bitrate failed: %s",
           opus_strerror(err));
    }
  
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_COMPLEXITY(opus->complexity))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting complexity failed: %s",
           opus_strerror(err));
    }
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_DTX(opus->dtx))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting dtx failed: %s",
           opus_strerror(err));
    }
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_INBAND_FEC(opus->fec))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting fec failed: %s",
           opus_strerror(err));
    }
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_PACKET_LOSS_PERC(opus->loss_perc))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting loss percentage failed: %s",
           opus_strerror(err));
    }
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_BANDWIDTH(opus->bandwidth))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting bandwidth failed: %s",
           opus_strerror(err));
    }
  if((err = opus_multistream_encoder_ctl(opus->enc,
                                         OPUS_SET_MAX_BANDWIDTH(opus->max_bandwidth))) != OPUS_OK)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN, "Setting max bandwidth failed: %s",
           opus_strerror(err));
    }
  /* Get preskip */

  err = opus_multistream_encoder_ctl(opus->enc,
                                     OPUS_GET_LOOKAHEAD(&opus->lookahead));
  if(err != OPUS_OK)
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "OPUS_GET_LOOKAHEAD failed: %s",
           opus_strerror(err));
    return 0; 
    }

  opus->h.pre_skip = (opus->lookahead * 48000) / format->samplerate;
  
  /* Save format and create frame */
  opus->format = format;
  opus->frame = gavl_audio_frame_create(opus->format);
  
  /* Output header */

  header_to_packet(&opus->h, &ci->codec_header);
  ci->id = GAVL_CODEC_ID_OPUS;
  ci->pre_skip = opus->h.pre_skip;

  opus->pts = -((int64_t)ci->pre_skip);
  
  
  gavl_dictionary_set_string(stream_metadata, GAVL_META_SOFTWARE,
                    opus_get_version_string());
  
  /* Allocate encoder buffer */

  // Size taken from opusenc.c
  opus->enc_buffer_size = opus->h.chtab.stream_count * (1275*3+7); 
  opus->enc_buffer = malloc(opus->enc_buffer_size);
  
  return gavl_audio_sink_create(NULL, write_audio_frame_opus, opus,
                                opus->format);
  }

static int init_compressed_opus(bg_ogg_stream_t * s)
  {
  ogg_packet op;
  const char * vendor;
  //  opus_t * opus = data;
  
  memset(&op, 0, sizeof(op));

  op.packet = s->ci.codec_header.buf;
  op.bytes = s->ci.codec_header.len;
  
  /* And stream them out */

  if(!bg_ogg_stream_write_header_packet(s, &op))
    {
    gavl_log(GAVL_LOG_ERROR, LOG_DOMAIN, "Got no Opus header page");
    return 0;
    }
  
  /* Build comment */

  vendor = gavl_dictionary_get_string(&s->m_stream, GAVL_META_SOFTWARE);

  if(!vendor)
    {
    gavl_log(GAVL_LOG_WARNING, LOG_DOMAIN,
           "Got no vendor string, using probably wrong value from codec library");
    vendor = opus_get_version_string();
    }
  
  bg_ogg_create_comment_packet((uint8_t*)"OpusTags", 8,
                               &s->m_stream, s->m_global, 0, &op);
  
  op.b_o_s = 0;
  op.e_o_s = 0;
  op.granulepos = 0;
  
  if(!bg_ogg_stream_write_header_packet(s, &op))
    return 0;
  
  bg_ogg_free_comment_packet(&op);
  
  return 1;
  }

static int close_opus(void * data)
  {
  int result = 1;
  opus_t * opus = data;

  /* Flush */
  result = flush_frame(opus, 1);
  
  if(opus->frame)
    gavl_audio_frame_destroy(opus->frame);
  if(opus->enc_buffer)
    free(opus->enc_buffer);
  
  opus_multistream_encoder_destroy(opus->enc);
  free(opus);
  return result;
  }

const bg_ogg_codec_t bg_opus_codec =
  {
    .name =      "opus",
    .long_name = TRS("Opus encoder"),
    .create = create_opus,

    .get_parameters = get_parameters_opus,
    .set_parameter =  set_parameter_opus,
    
    .init_audio  =     init_opus,
    .init_audio_compressed =     init_compressed_opus,
    .set_packet_sink = set_packet_sink,
    
    //    .write_packet = write_audio_packet_opus,

    .close = close_opus,
  };


/* Header stuff */

static const int samplerates[] =
  {
    8000, 12000, 16000, 24000, 48000, 0
  };

static void setup_header(opus_header_t * h, gavl_audio_format_t * format)
  {
  int rate;
  format->interleave_mode = GAVL_INTERLEAVE_ALL;

  if(gavl_bytes_per_sample(format->sample_format) >= 4)
    format->sample_format = GAVL_SAMPLE_FLOAT;
  else
    format->sample_format = GAVL_SAMPLE_S16;
    
  
  rate = gavl_nearest_samplerate(format->samplerate, samplerates);
  if(rate != format->samplerate)
    {
    gavl_log(GAVL_LOG_INFO, LOG_DOMAIN, "Resampling from %d to %d",
           format->samplerate, rate);
    format->samplerate = rate;
    }
  
  h->version = 1;
  h->channel_count = format->num_channels;
  h->pre_skip = 0;
  h->samplerate = format->samplerate;
  h->output_gain = 0;
  
  if(format->channel_locations[0] == GAVL_CHID_AUX)
    {
    int i;
    h->channel_mapping = 255;
    h->chtab.stream_count = format->num_channels;
    h->chtab.coupled_count = 0;
    for(i = 0; i < format->num_channels; i++)
      h->chtab.map[i] = i;
    return;
    }
  
  bg_ogg_set_vorbis_channel_setup(format);
  switch(format->num_channels)
    {
    case 1:
      h->channel_mapping = 0;
      h->chtab.stream_count = 1;
      h->chtab.coupled_count = 0;
      h->chtab.map[0] = 0;
      break;
    case 2:
      h->channel_mapping = 0;
      h->chtab.stream_count = 1;
      h->chtab.coupled_count = 1;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 1;
      break;
    case 3: // Left, Center, Right
      h->channel_mapping = 1;
      h->chtab.stream_count = 2;
      h->chtab.coupled_count = 1;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 2;
      h->chtab.map[2] = 1;
      break;
    case 4: // Left, Right, Rear left, Rear right
      h->channel_mapping = 1;
      h->chtab.stream_count = 2;
      h->chtab.coupled_count = 2;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 1;
      h->chtab.map[2] = 2;
      h->chtab.map[3] = 3;
      break;
    case 5: // Left, Center, Right, Rear left, Rear right
      h->channel_mapping = 1;
      h->chtab.stream_count = 3;
      h->chtab.coupled_count = 2;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 4;
      h->chtab.map[2] = 1;
      h->chtab.map[3] = 2;
      h->chtab.map[4] = 3;
      break;
    case 6: // Left, Center, Right, Rear left, Rear right, LFE
      h->channel_mapping = 1;
      h->chtab.stream_count = 4;
      h->chtab.coupled_count = 2;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 4;
      h->chtab.map[2] = 1;
      h->chtab.map[3] = 2;
      h->chtab.map[4] = 3;
      h->chtab.map[5] = 5;
      break;
    case 7: // Left, Center, Right, Side left, Side right, Rear Center, LFE
      h->channel_mapping = 1;
      h->chtab.stream_count = 5;
      h->chtab.coupled_count = 2;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 4;
      h->chtab.map[2] = 1;
      h->chtab.map[3] = 2;
      h->chtab.map[4] = 3;
      h->chtab.map[5] = 5;
      h->chtab.map[6] = 6;
      break;
    case 8: // Left, Center, Right, Side left, Side right, Rear left, Rear right, LFE
      h->channel_mapping = 1;
      h->chtab.stream_count = 5;
      h->chtab.coupled_count = 2;
      h->chtab.map[0] = 0;
      h->chtab.map[1] = 6;
      h->chtab.map[2] = 1;
      h->chtab.map[3] = 2;
      h->chtab.map[4] = 3;
      h->chtab.map[5] = 4;
      h->chtab.map[6] = 5;
      h->chtab.map[7] = 7;
      break;
    }
  }

#define write32(buf, base, val) \
  buf[base+3]=((val)>>24)&0xff;  \
  buf[base+2]=((val)>>16)&0xff;  \
  buf[base+1]=((val)>>8)&0xff;   \
  buf[base]=(val)&0xff;

#define write16(buf, base, val) \
  buf[base+1]=((val)>>8)&0xff;   \
  buf[base]=(val)&0xff;


static void header_to_packet(opus_header_t * h, gavl_buffer_t * buf)
  {
  int len = 0;
  uint8_t * ret;
  
  gavl_buffer_alloc(buf, MAX_HEADER_LEN);
  
  ret = buf->buf;
  
  memcpy(ret, "OpusHead", 8);        len+=8;
  ret[len] = h->version;             len++;
  ret[len] = h->channel_count;       len++;
  write16(ret, len, h->pre_skip);    len+=2;
  write32(ret, len, h->samplerate);  len+=4;
  write16(ret, len, h->output_gain); len+=2;
  ret[len] = h->channel_mapping;     len++;
  if(h->channel_mapping != 0)
    {
    ret[len] = h->chtab.stream_count; len++;
    ret[len] = h->chtab.coupled_count; len++;
    memcpy(ret + len, h->chtab.map, h->channel_count); len += h->channel_count;
    }
  
  buf->len = len;
  }
