#ifndef MXF_DECODE_H
#define MXF_DECODE_H

#include <libavformat/avformat.h>
#include "linked_list.h"

typedef struct OutputStream {
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
    int video_out_fd;
    int audio_out_fd;
    unsigned int decode_frame_buffer_size;

    // libav related stuff
    OutputStream video_stream;
    OutputStream audio_stream;
    AVFormatContext *format_context;
    AVDictionary *encode_ops;
    AVCodec *video_codec;
    AVCodec *audio_codec;
} decoding_parameters_t;

extern int stop_decoding_signal();

extern int mxf_decode_files(linked_list_t *video_files, linked_list_t *audio_files, decoding_parameters_t *parameters);

#endif
