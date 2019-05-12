#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg-2.3/openjpeg.h>
#include <pthread.h>
#include <time.h>
#include "asdcp.h"
#include "color.h"
#include "linked_list.h"
#include "mxf_decode.h"

static volatile int keep_running = 1;

pthread_mutex_t decoding_mutex;
pthread_mutex_t writeout_mutex;

typedef struct {
    // need to free later when consumed
    unsigned char *frame_buf;
    unsigned int frame_size;
    unsigned int current_frame;
    decoding_parameters_t *parameters;
} decoding_queue_context_t;

typedef struct {
    int current_frame;
    opj_image_t *image;
    decoding_parameters_t *parameters;
} writeout_queue_context_t;

static linked_list_t *decoding_queue_s = NULL;
static linked_list_t *writeout_queue_s = NULL;

// 5 MB read buf
#define MAX_BUF 5*1048576 

void error_callback(const char *msg, void *client_data)
{
    (void)client_data;
    fprintf(stderr, "[ERROR] %s", msg);
}
void warning_callback(const char *msg, void *client_data)
{
    (void)client_data;
    fprintf(stderr, "[WARNING] %s", msg);
}

void info_callback(const char *msg, void *client_data)
{
    (void)client_data;
    fprintf(stderr, "[INFO] %s", msg);
}

void quiet_callback(const char *msg, void *client_data)
{
    (void)msg;
    (void)client_data;
}

int image_to_fd(opj_image_t *image, int fd) 
{
    if ((image->numcomps * image->x1 * image->y1) == 0) {
        fprintf(stderr, "\nError: invalid raw image parameters\n");
        return 1;
    }

    unsigned int numcomps = image->numcomps;

    if (numcomps > 4) {
        numcomps = 4;
    }

    int compno;
    for (compno = 1; compno < numcomps; ++compno) {
        if (image->comps[0].dx != image->comps[compno].dx) {
            break;
        }
        if (image->comps[0].dy != image->comps[compno].dy) {
            break;
        }
        if (image->comps[0].prec != image->comps[compno].prec) {
            break;
        }
        if (image->comps[0].sgnd != image->comps[compno].sgnd) {
            break;
        }
    }
    if (compno != numcomps) {
        fprintf(stderr,
                "imagetoraw_common: All components shall have the same subsampling, same bit depth, same sign.\n");
        fprintf(stderr, "\tAborting\n");
        return 1;
    }

    int comp_table[3];
    comp_table[0] = 1;   // r->g[1]
    comp_table[1] = 2;   // g->b[2]
    comp_table[2] = 0;   // b->r[0]

    int w = (int)image->comps[0].w;
    int h = (int)image->comps[0].h;

    union {
        unsigned short val;
        unsigned char vals[2];
    } uc16;

    // TO-DO: get out_buf from outside this function and cache it 
    unsigned int frame_size = w * h * image->numcomps * 2;
    unsigned char *out_buf = (unsigned char*)malloc(frame_size);
    int out_buf_pos = 0;

    int err = 0;
    for (int i = 0; i < image->numcomps; i++) {
        int compno = comp_table[i];
        int mask = (1 << image->comps[compno].prec) - 1;
        int *ptr = image->comps[compno].data;
        for (int line = 0; line < h; line++) {
            for (int row = 0; row < w; row++)    {
                int curr = *ptr;
                if (curr > 65535) {
                    curr = 65535;
                } else if (curr < 0) {
                    curr = 0;
                }
                uc16.val = (unsigned short)(curr & mask);
                out_buf[out_buf_pos++] = uc16.vals[0];
                out_buf[out_buf_pos++] = uc16.vals[1];
                ptr++;
            }
        }
    }

    out_buf_pos = 0;
    int written = 0;
    while (1) {
        written = write(fd, out_buf + out_buf_pos, frame_size - out_buf_pos);
        if (written <= 0) {
            if (written < 0) {
                err = 1;
            }
            break;
        }
        out_buf_pos += written;
    }

    free(out_buf);
    return err;
}

int on_frame_data_mt(
        unsigned char* frame_buf,
        unsigned int frame_size,
        unsigned int current_frame,
        void *user_data)
{
    decoding_parameters_t *parameters = user_data;
    do {
        pthread_mutex_lock(&decoding_mutex);
        int frames_buffered = ll_len(decoding_queue_s);
        pthread_mutex_unlock(&decoding_mutex);
        if (frames_buffered > parameters->decode_frame_buffer_size) {
            usleep(50);
        } else {
            break;
        }
    } while (1);

    decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)malloc(sizeof(decoding_queue_context_t));

    decoding_queue_context->frame_buf = frame_buf;
    decoding_queue_context->frame_size = frame_size;
    decoding_queue_context->current_frame = current_frame;
    decoding_queue_context->parameters = parameters;

    pthread_mutex_lock(&decoding_mutex);
    if (!decoding_queue_s) {
        decoding_queue_s = ll_create(decoding_queue_context);
    } else {
        ll_append(decoding_queue_s, decoding_queue_context);
    }
    pthread_mutex_unlock(&decoding_mutex);

    return 0;
}


int decode_frame(
        unsigned char* frame_buf,
        unsigned int frame_size,
        unsigned int current_frame,
        decoding_parameters_t *parameters)
{
    int ok = 0;
    opj_image_t *image = NULL;
    opj_stream_t *stream = NULL;
    opj_stream_t *codec = NULL;

    if (frame_buf) {
        opj_buffer_info_t buffer_info;
        buffer_info.buf = frame_buf;
        buffer_info.cur = frame_buf;
        buffer_info.len = frame_size;


        stream = opj_stream_create_buffer_stream(&buffer_info, 1);
        if (!stream) {
            fprintf(stderr, "error creating stream [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        codec = opj_create_decompress(OPJ_CODEC_J2K);

        if (parameters->print_debug) {
            opj_set_info_handler(codec, info_callback, 00);
            opj_set_warning_handler(codec, warning_callback, 00);
            opj_set_error_handler(codec, error_callback, 00);
        } else {
            opj_set_info_handler(codec, quiet_callback, 00);
            opj_set_warning_handler(codec, quiet_callback, 00);
            opj_set_error_handler(codec, quiet_callback, 00);
        }

        opj_dparameters_t *core = parameters->user_data;

        ok = opj_setup_decoder(codec, core);
        if (!ok) {
            fprintf(stderr, "failed to setup decoder [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        ok = opj_codec_set_threads(codec, parameters->num_threads);
        if (!ok) {
            fprintf(stderr, "failed to setup %d threads [frame: %d]\n",
                    parameters->num_threads,
                    current_frame);
            goto free_and_out;
        }

        ok = opj_read_header(
                stream,
                codec,
                &image);
        if (!ok) {
            fprintf(stderr, "failed to read header [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        ok = opj_set_decode_area(codec, image, 0, 0, 0, 0);
        if (!ok) {
            fprintf(stderr, "failed to set decode area [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        ok = opj_decode(
                codec,
                stream,
                image);
        if (!ok) {
            fprintf(stderr, "failed on decoding image [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        ok = opj_end_decompress(codec, stream);
        if (!ok) {
            fprintf(stderr, "failed on end decompress [frame: %d]\n", current_frame);
            goto free_and_out;
        }

        if (image->comps[0].data == NULL) {
            fprintf(stderr, "error getting image [frame: %d]\n", current_frame);
            ok = 0;
            goto free_and_out;
        }

        // detect SYCC or RGB color space -> TODO: this is coming from CPL, so we can
        // read that out when parsing CPL and pass it to here
        if (image->color_space != OPJ_CLRSPC_SYCC
                && image->numcomps == 3
                && image->comps[0].dx == image->comps[0].dy
                && image->comps[1].dx != 1) {
            image->color_space = OPJ_CLRSPC_SYCC;
            ok = color_sycc_to_rgb(image);
            if (!ok) {
                fprintf(stderr, "error converting sycc to rgb [frame: %d]\n", current_frame);
                goto free_and_out;
            }
        }
    }

    writeout_queue_context_t *writeout_queue_context = (writeout_queue_context_t *)malloc(sizeof(writeout_queue_context_t));
    writeout_queue_context->parameters = parameters;
    writeout_queue_context->image = image;
    writeout_queue_context->current_frame = current_frame;

    pthread_mutex_lock(&writeout_mutex);
    if (!writeout_queue_s) {
        writeout_queue_s = ll_create(writeout_queue_context);
    } else {
        ll_append(writeout_queue_s, writeout_queue_context);
    }
    pthread_mutex_unlock(&writeout_mutex);

    goto success_out;

free_and_out:
    if (image) {
        opj_image_destroy(image);
    }
success_out:
    if (stream) {
        opj_stream_destroy(stream);
    }
    if (codec) {
        opj_destroy_codec(codec);
    }
    return !ok;
}

void *writeout_frames_consumer(void *thread_data) {
    while (keep_running) {
        pthread_mutex_lock(&writeout_mutex);
        linked_list_t *head = ll_poph(&writeout_queue_s);
        if (!head) {
            pthread_mutex_unlock(&writeout_mutex);
            usleep(50);
            continue;
        }    
        pthread_mutex_unlock(&writeout_mutex);

        writeout_queue_context_t *writeout_queue_context = (writeout_queue_context_t *)head->user_data;
        if (!writeout_queue_context->image) {
            keep_running = 0;
        } else {
            if (writeout_queue_context->parameters->out_fd >= 0) {
                int err = image_to_fd(writeout_queue_context->image, writeout_queue_context->parameters->out_fd); 
                if (err) {
                    fprintf(stderr, "error image_to_fd [frame: %d]\n", writeout_queue_context->current_frame);
                    keep_running = 0;
                }
            }

            opj_image_destroy(writeout_queue_context->image);
        }
        free(writeout_queue_context);
        free(head);
    }
    fprintf(stderr, "exit writeout thread\n");
    return NULL;
}

void *decode_frames_consumer(void *thread_data) {
    while (keep_running) {
        pthread_mutex_lock(&decoding_mutex);
        linked_list_t *head = ll_poph(&decoding_queue_s);
        if (!head) {
            pthread_mutex_unlock(&decoding_mutex);
            usleep(50);
            continue;
        }
        pthread_mutex_unlock(&decoding_mutex);

        decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)head->user_data;
        if (!decoding_queue_context->frame_buf) {
            keep_running = 0;
        } else {
            int err = decode_frame(
                    decoding_queue_context->frame_buf,
                    decoding_queue_context->frame_size,
                    decoding_queue_context->current_frame,
                    decoding_queue_context->parameters);
            if (err) {
                keep_running = 0;
            }
            free(decoding_queue_context->frame_buf);
        }
        free(decoding_queue_context);
        free(head);
    }

    fprintf(stderr, "exit decoding thread\n");
    return NULL;
}

int stop_decoding_signal() {
    keep_running = 0;
}

int decode_video_files(linked_list_t *files, decoding_parameters_t *parameters) {
    pthread_t decoding_queue_thread_id;
    pthread_t writeout_queue_thread_id;

    if (pthread_mutex_init(&decoding_mutex, NULL) != 0) {
        fprintf(stderr, "decoding mutex init has failed\n");
        return 1;
    }
    if (pthread_mutex_init(&writeout_mutex, NULL) != 0) {
        fprintf(stderr, "writeout mutex init has failed\n");
        return 1;
    }

    pthread_create(&decoding_queue_thread_id, NULL, decode_frames_consumer, NULL);
    pthread_create(&writeout_queue_thread_id, NULL, writeout_frames_consumer, NULL);

    opj_dparameters_t core;
    opj_set_default_decoder_parameters(&core);

    parameters->user_data = &core;

    keep_running = 1;
    int err = 0;

    asdcp_read_mxf_list(files, on_frame_data_mt, parameters);

    pthread_join(decoding_queue_thread_id, NULL);
    pthread_join(writeout_queue_thread_id, NULL);

    pthread_mutex_destroy(&decoding_mutex);
    pthread_mutex_destroy(&writeout_mutex);

    return err;

}

int decode_mxf_file(const char *filename, decoding_parameters_t *parameters) {
    pthread_t decoding_queue_thread_id;
    pthread_t writeout_queue_thread_id;

    if (pthread_mutex_init(&decoding_mutex, NULL) != 0) {
        fprintf(stderr, "decoding mutex init has failed\n");
        return 1;
    }
    if (pthread_mutex_init(&writeout_mutex, NULL) != 0) {
        fprintf(stderr, "writeout mutex init has failed\n");
        return 1;
    }

    pthread_create(&decoding_queue_thread_id, NULL, decode_frames_consumer, NULL);
    pthread_create(&writeout_queue_thread_id, NULL, writeout_frames_consumer, NULL);

    opj_dparameters_t core;
    opj_set_default_decoder_parameters(&core);

    parameters->user_data = &core;

    keep_running = 1;
    int err = 0;
    //int err = asdcp_read_mxf(filename, parameters, on_frame_data_mt);

    pthread_join(decoding_queue_thread_id, NULL);
    pthread_join(writeout_queue_thread_id, NULL);

    pthread_mutex_destroy(&decoding_mutex);
    pthread_mutex_destroy(&writeout_mutex);

    return err;
}
