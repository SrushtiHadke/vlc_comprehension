/*****************************************************************************
 * trim.c : Trim video plugin for VLC
 *****************************************************************************
 
 *****************************************************************************/

 #ifdef HAVE_CONFIG_H
 # include "config.h"
 #endif
 
 #include <vlc_common.h>
 #include <vlc_plugin.h>
 #include <vlc_filter.h>
 #include <vlc_picture.h>
 #include <libavformat/avformat.h>
 #include <libavcodec/avcodec.h>
 
 typedef struct {
     int64_t start_pts;
     int64_t start_dts;
     AVRational time_base;
 } StreamContext;
 
 int64_t parse_time(const char *time_str);
 void cleanup(AVFormatContext *in, AVFormatContext *out);
 static int Trim(const char *input, const char *output, const char *start_time, const char *end_time);
 
 /*****************************************************************************
  * Module descriptor
  *****************************************************************************/
 vlc_module_begin()
     set_description(N_("Trim video filter"))
     set_shortname(N_("Video Trimming"))
     set_category(CAT_VIDEO)
     set_subcategory(SUBCAT_VIDEO_VFILTER)
     set_capability("video filter", 0)
     add_shortcut("trim")
     add_integer(FILTER_PREFIX "start", 0, "Trim Start Time", "Start time in seconds", false)
     add_integer(FILTER_PREFIX "end", 0, "Trim End Time", "End time in seconds", false)
     add_string(FILTER_PREFIX "input", NULL, "Input File", "Path to the input video file", false)
     add_string(FILTER_PREFIX "output", NULL, "Output File", "Path to the output video file", false)
     set_callbacks(Create, Destroy)
 vlc_module_end()
 
 /*****************************************************************************
  * Create: allocates Trim video thread output method
  *****************************************************************************/
 static int Create(vlc_object_t *p_this)
 {
     filter_t *p_filter = (filter_t *)p_this;
 
     // Retrieve start and end times from VLC configuration
     int start_time = var_CreateGetIntegerCommand(p_filter, FILTER_PREFIX "start");
     int end_time = var_CreateGetIntegerCommand(p_filter, FILTER_PREFIX "end");
 
     if (start_time >= end_time) {
         msg_Err(p_filter, "Invalid start and end times");
         return VLC_EGENERIC;
     }
 
     // Retrieve input and output file paths from VLC configuration
     char *input = var_CreateGetStringCommand(p_filter, FILTER_PREFIX "input");
     char *output = var_CreateGetStringCommand(p_filter, FILTER_PREFIX "output");
 
     if (!input || !output) {
         msg_Err(p_filter, "Input or output file path not provided");
         free(input);
         free(output);
         return VLC_EGENERIC;
     }
 
     // Convert start and end times to strings (mm:ss format)
     char start_time_str[16], end_time_str[16];
     snprintf(start_time_str, sizeof(start_time_str), "%02d:%02d", start_time / 60, start_time % 60);
     snprintf(end_time_str, sizeof(end_time_str), "%02d:%02d", end_time / 60, end_time % 60);
 
     if (Trim(input, output, start_time_str, end_time_str) != 0) {
         msg_Err(p_filter, "Failed to trim video");
         free(input);
         free(output);
         return VLC_EGENERIC;
     }
 
     msg_Info(p_filter, "Video trimmed successfully");
     free(input);
     free(output);
     return VLC_SUCCESS;
 }
 
 /*****************************************************************************
  * Destroy: destroy Trim video thread output method
  *****************************************************************************/
 static void Destroy(vlc_object_t *p_this)
 {
     (void)p_this;
 }
 
 /*****************************************************************************
  * Trim: Trims the video between start and end times
  *****************************************************************************/
 static int Trim(const char *input, const char *output, const char *start_time, const char *end_time)
 {
     int video_stream_idx = -1;
 
     // Parse times
     int64_t start_sec = parse_time(start_time);
     int64_t end_sec = parse_time(end_time);
 
     if (start_sec >= end_sec) {
         fprintf(stderr, "Error: start must be before end\n");
         return 1;
     }
 
     // Open input
     AVFormatContext *in = NULL;
     if (avformat_open_input(&in, input, NULL, NULL) != 0 || avformat_find_stream_info(in, NULL) < 0) {
         fprintf(stderr, "Could not open input\n");
         return 1;
     }
 
     // Create output
     AVFormatContext *out = NULL;
     if (avformat_alloc_output_context2(&out, NULL, NULL, output) < 0) {
         cleanup(in, NULL);
         return 1;
     }
 
     // Setup streams and contexts
     StreamContext *stream_ctx = av_malloc_array(in->nb_streams, sizeof(*stream_ctx));
     if (!stream_ctx) {
         fprintf(stderr, "Failed to allocate memory for stream context\n");
         cleanup(in, out);
         return 1;
     }
 
     for (unsigned i = 0; i < in->nb_streams; i++) {
         AVStream *in_stream = in->streams[i];
         stream_ctx[i].start_pts = AV_NOPTS_VALUE;
         stream_ctx[i].start_dts = AV_NOPTS_VALUE;
 
         if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
             in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
             continue;
 
         if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
             video_stream_idx = i;
         }
 
         AVStream *out_stream = avformat_new_stream(out, NULL);
         if (!out_stream || avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
             av_free(stream_ctx);
             cleanup(in, out);
             return 1;
         }
         out_stream->time_base = in_stream->time_base;
         stream_ctx[i].time_base = in_stream->time_base;
     }
 
     // Open output file
     if ((avio_open(&out->pb, output, AVIO_FLAG_WRITE) < 0) || (avformat_write_header(out, NULL) < 0)) {
         av_free(stream_ctx);
         cleanup(in, out);
         return 1;
     }
 
     // Seek to start time
     int64_t seek_target = av_rescale_q(start_sec * AV_TIME_BASE, AV_TIME_BASE_Q,
                                        in->streams[video_stream_idx]->time_base);
     if (av_seek_frame(in, video_stream_idx, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
         fprintf(stderr, "Seek failed\n");
         av_free(stream_ctx);
         cleanup(in, out);
         return 1;
     }
 
     // Process packets
     AVPacket pkt;
     int got_video_keyframe = 0;
     int64_t last_video_dts = AV_NOPTS_VALUE;
 
     while (av_read_frame(in, &pkt) >= 0) {
         int stream_idx = pkt.stream_index;
         AVStream *in_stream = in->streams[stream_idx];
 
         if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
             in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
             av_packet_unref(&pkt);
             continue;
         }
 
         double pts_seconds = (pkt.pts == AV_NOPTS_VALUE) ?
                              pkt.dts * av_q2d(in_stream->time_base) :
                              pkt.pts * av_q2d(in_stream->time_base);
 
         if (pts_seconds >= end_sec) {
             av_packet_unref(&pkt);
             break;
         }
 
         if (pts_seconds < start_sec) {
             av_packet_unref(&pkt);
             continue;
         }
 
         if (stream_idx == video_stream_idx && !got_video_keyframe) {
             if (!(pkt.flags & AV_PKT_FLAG_KEY)) {
                 av_packet_unref(&pkt);
                 continue;
             }
             got_video_keyframe = 1;
         }
 
         if (stream_ctx[stream_idx].start_pts == AV_NOPTS_VALUE) {
             stream_ctx[stream_idx].start_pts = pkt.pts;
             stream_ctx[stream_idx].start_dts = pkt.dts;
         }
 
         if (pkt.pts != AV_NOPTS_VALUE) {
             pkt.pts -= stream_ctx[stream_idx].start_pts;
         }
         if (pkt.dts != AV_NOPTS_VALUE) {
             pkt.dts -= stream_ctx[stream_idx].start_dts;
 
             if (stream_idx == video_stream_idx) {
                 if (last_video_dts != AV_NOPTS_VALUE && pkt.dts <= last_video_dts) {
                     pkt.dts = last_video_dts + 1;
                 }
                 last_video_dts = pkt.dts;
             }
         }
 
         if (stream_idx == video_stream_idx && pkt.pts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE) {
             if (pkt.pts < pkt.dts) {
                 pkt.pts = pkt.dts;
             }
         }
 
         pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out->streams[stream_idx]->time_base);
         pkt.pos = -1;
 
         if (av_interleaved_write_frame(out, &pkt) < 0) {
             fprintf(stderr, "Error writing packet\n");
             av_packet_unref(&pkt);
             break;
         }
 
         av_packet_unref(&pkt);
     }
 
     av_write_trailer(out);
     av_free(stream_ctx);
     cleanup(in, out);
 
     printf("Successfully trimmed from %s to %s\n", start_time, end_time);
     return 0;
 }
 
 int64_t parse_time(const char *time_str)
 {
     int m, s;
     if (sscanf(time_str, "%d:%d", &m, &s) != 2) {
         fprintf(stderr, "Invalid time format (use mm:ss)\n");
         exit(1);
     }
     return m * 60 + s;
 }
 
 void cleanup(AVFormatContext *in, AVFormatContext *out)
 {
     if (out) {
         if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
             avio_closep(&out->pb);
         }
         avformat_free_context(out);
     }
     if (in) {
         avformat_close_input(&in);
     }
 }