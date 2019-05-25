#ifndef AV_PIPELINE_H
#define AV_PIPELINE_H

#include <libavformat/avformat.h>
#include "linked_list.h"

typedef struct {
    AVStream *stream;
    AVCodecContext *codec_context;
    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;
    AVFrame *frame;
} OutputStream;

typedef struct {
    void *user_data;
    int num_threads;
    int print_debug;
    unsigned int decode_frame_buffer_size;

    // libav related stuff
    OutputStream video_stream;
    OutputStream audio_stream;
    AVFormatContext *format_context;
    AVDictionary *encode_ops;
    AVCodec *video_codec;
    AVCodec *audio_codec;
} av_pipeline_context_t;

extern int stop_decoding_signal();

extern int av_pipeline_run(linked_list_t *video_files, linked_list_t *audio_files, av_pipeline_context_t *parameters);

#endif
