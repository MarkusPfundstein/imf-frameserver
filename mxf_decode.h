#ifndef MXF_DECODE_H
#define MXF_DECODE_H

#include "linked_list.h"

typedef struct {
    void *user_data;
    int num_threads;
    int print_debug;
    int out_fd;
    unsigned int decode_frame_buffer_size;
} decoding_parameters_t;

extern int stop_decoding_signal();

extern int decode_video_files(linked_list_t *files, decoding_parameters_t *parameters);

#endif
