#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg-2.3/openjpeg.h>
#include <libavutil/opt.h>
#include <pthread.h>
#include <time.h>
#include "asdcp.h"
#include "color.h"
#include "linked_list.h"
#include "mxf_decode.h"

static volatile int keep_running = 1;

pthread_mutex_t decoding_mutex;
pthread_mutex_t writeout_mutex;
pthread_mutex_t vid_packet_mutex;
pthread_mutex_t aud_packet_mutex;

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

static linked_list_t *vid_packet_queue_s = NULL;
static linked_list_t *aud_packet_queue_s = NULL;


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

int image_to_fd(opj_image_t *image, decoding_parameters_t *parameters) 
{
    AVPacket *pkt = NULL;
    int err = 0;
    if (image) {
        
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

        err = 0;
        err = av_frame_make_writable(parameters->video_stream.frame);
        if (err) {
            fprintf(stderr, "error av_frame_make_writeable\n");
            goto err_and_out;
        }


        // To-DO: check if we can use this part and directly encode to r210
        // this way we can skip avcodec_encode_video2 below which does in a way
        // only another loop through the frame
        AVFrame *frame = parameters->video_stream.frame;
        for (int i = 0; i < image->numcomps; i++) {
            int compno = comp_table[i];
            int mask = (1 << image->comps[compno].prec) - 1;
            int *ptr = image->comps[compno].data;
            for (int y = 0; y < h; y++) { // height, y
                for (int x = 0; x < w; x++) { // width, x
                    int curr = *ptr;
                    if (curr > 65535) {
                        curr = 65535;
                    } else if (curr < 0) {
                        curr = 0;
                    }
                    uc16.val = (unsigned short)(curr & mask);
                    //fprintf(stderr, "[%d] %d", compno, frame->linesize[compno]); 
                    //linesize = width*2
                    int offset = 2 * (y * w + x);
                    frame->data[i][offset] = uc16.vals[0];
                    frame->data[i][offset + 1] = uc16.vals[1];

                    ptr++;
                }
            }
        }

        frame->pts = parameters->video_stream.next_pts++;

        pkt = (AVPacket*)malloc(sizeof(AVPacket));
        memset(pkt, 0, sizeof(AVPacket));

        AVCodecContext *c = parameters->video_stream.codec_context;
        AVStream *st = parameters->video_stream.stream;

        av_init_packet(pkt);
        int got_packet = 0;
        err = avcodec_encode_video2(c, pkt, frame, &got_packet);
        if (err) {
            fprintf(stderr, "error avcodec_encode_video2: %s\n", av_err2str(err));
            goto err_and_out;
        }
        if (!got_packet) {
            err = 1;
            fprintf(stderr, "no packet produced\n");
            goto err_and_out;
        }
        //timebase = c->timebase
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;
#if 0
        err = av_interleaved_write_frame(parameters->format_context, &pkt);
        if (err) {
            fprintf(stderr, "error av_interleaved_write_frame: %s\n", av_err2str(err));
            goto err_and_out;
        }
#endif
    }
    
    do {
        pthread_mutex_lock(&vid_packet_mutex);
        int frames_buffered = ll_len(vid_packet_queue_s);
        pthread_mutex_unlock(&vid_packet_mutex);
        if (frames_buffered > 25) {
            usleep(25);
        } else {
            break;
        }
    } while (1);

    pthread_mutex_lock(&vid_packet_mutex);
    if (!vid_packet_queue_s) {
        vid_packet_queue_s = ll_create(pkt);
    } else {
        ll_append(vid_packet_queue_s, pkt);
    }
    pthread_mutex_unlock(&vid_packet_mutex);

err_and_out:
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
            usleep(25);
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
    int ok = 1;
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

        // TO-DO: move this to writeout thread. Tried but doesnt work.
        // Does openjpeg have some internal state that prevents us from
        // calling color_sycc_to_rgb from other thread?
        if (image->color_space != OPJ_CLRSPC_SYCC
            && image->numcomps == 3
            && image->comps[0].dx == image->comps[0].dy
            && image->comps[1].dx != 1) {

            image->color_space = OPJ_CLRSPC_SYCC;
            int ok = color_sycc_to_rgb(image);
            if (!ok) {
                fprintf(stderr, "error converting sycc to rgb\n");
                return 1;
            }

        }

    }

    // throttle in case io queue cant keep up with
    // decoding queue
    do {
        pthread_mutex_lock(&writeout_mutex);
        int frames_buffered = ll_len(writeout_queue_s);
        pthread_mutex_unlock(&writeout_mutex);
        if (frames_buffered > 100) {
            usleep(25);
        } else {
            break;
        }
    } while (1);

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
        fprintf(stderr, "free and out bitch\n");
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
            usleep(25);
            continue;
        }    
        pthread_mutex_unlock(&writeout_mutex);

        writeout_queue_context_t *writeout_queue_context = (writeout_queue_context_t *)head->user_data;

        int err = image_to_fd(writeout_queue_context->image, writeout_queue_context->parameters); 
        if (err) {
            fprintf(stderr, "error image_to_fd [frame: %d]\n", writeout_queue_context->current_frame);
            keep_running = 0;
        }

        if (writeout_queue_context->image) {
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
            usleep(25);
            continue;
        }
        pthread_mutex_unlock(&decoding_mutex);

        decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)head->user_data;
        //if (!decoding_queue_context->frame_buf) {
        //    fprintf(stderr, "decode_frames_consumer done\n");
        //    break;
        //} else {
        int err = decode_frame(
                decoding_queue_context->frame_buf,
                decoding_queue_context->frame_size,
                decoding_queue_context->current_frame,
                decoding_queue_context->parameters);
        if (err) {
            fprintf(stderr, "err decode frame\n");
            keep_running = 0;
        }
        if (decoding_queue_context->frame_buf) {
            free(decoding_queue_context->frame_buf);
        }
        //}
        free(decoding_queue_context);
        free(head);
    }

    fprintf(stderr, "exit decoding thread\n");
    return NULL;
}

int stop_decoding_signal() {
    keep_running = 0;
}

int on_audio_frame_func(unsigned char *buf, unsigned int length, unsigned int current_frame, void *user_data) {
    int err = 0;
    if (!keep_running) {
        return -1;
    }
    AVPacket *pkt = NULL;
    if (buf) {

        decoding_parameters_t *parameters = user_data;

        OutputStream *ost = &parameters->audio_stream;
        AVCodecContext *c = ost->codec_context;
        AVFrame *frame = ost->frame;

        //fprintf(stderr, "copy\nlength: %d (%d)\nnb_samples %d\n", length, length/(3*ost->codec_context->channels), frame->nb_samples);

        unsigned char *src_ptr = buf;
        int8_t *data_ptr = frame->data[0];
        for (int i = 0; i < length/(3*ost->codec_context->channels); ++i) {
            for (int c = 0; c < ost->codec_context->channels; c++) {
                *data_ptr++ = 0;
                *data_ptr++ = *src_ptr++;
                *data_ptr++ = *src_ptr++;
                *data_ptr++ = *src_ptr++;
            }
        }

        frame->pts = ost->next_pts;
        ost->next_pts += frame->nb_samples;
        
        err = av_frame_make_writable(frame);
        if (err != 0) {
            fprintf(stderr, "error making audio frame writable\n");
            goto err_and_out;
        }

        frame->pts = av_rescale_q(
                ost->samples_count,
                (AVRational) {1, c->sample_rate},
                c->time_base);
        ost->samples_count += frame->nb_samples;

        pkt = (AVPacket*)malloc(sizeof(AVPacket));
        memset(pkt, 0, sizeof(AVPacket));
        av_init_packet(pkt);

        int got_packet = 0;
        err = avcodec_encode_audio2(c, pkt, frame, &got_packet);
        if (err) {
            fprintf(stderr, "error encoding audio %s\n", av_err2str(err));
            goto err_and_out;
        }
        if (!got_packet) {
            fprintf(stderr, "got no audio packet\n");
            err = 1;
            goto err_and_out;
        }
        
        av_packet_rescale_ts(pkt, c->time_base, ost->stream->time_base);
        pkt->stream_index = ost->stream->index;
#if 0
        err = av_interleaved_write_frame(parameters->format_context, pkt);
        if (err) {
            fprintf(stderr, "error av_interleaved_write_frame: %s\n", av_err2str(err));
            goto err_and_out;
        }
        fprintf(stderr, "done with frame %d\n", current_frame);

        free(pkt);
#endif
    }
    do {
        if (!keep_running) {
            return -1;
        }

        pthread_mutex_lock(&aud_packet_mutex);
        int frames_buffered = ll_len(aud_packet_queue_s);
        pthread_mutex_unlock(&aud_packet_mutex);
        if (frames_buffered > 25) {
            usleep(25);
        } else {
            break;
        }
    } while (1);

    pthread_mutex_lock(&aud_packet_mutex);
    if (!aud_packet_queue_s) {
        aud_packet_queue_s = ll_create(pkt);
    } else {
        ll_append(aud_packet_queue_s, pkt);
    }
    pthread_mutex_unlock(&aud_packet_mutex);

err_and_out:
    free(buf);
    return err;
}

typedef struct {
    linked_list_t *files;
    decoding_parameters_t *parameters;
} audio_thread_args_t;

void* extract_audio_thread(void *data) {
    audio_thread_args_t *args = data;
    linked_list_t *files = args->files;
    decoding_parameters_t *parameters = args->parameters;
    int err = asdcp_read_audio_files(files, on_audio_frame_func, parameters);
    if (err && !keep_running) {
        fprintf(stderr, "error audio thread\n");
        stop_decoding_signal();
    }

    fprintf(stderr, "exit extract_audio_thread\n");

    return NULL;
}

void* write_interleaved_consumer(void *data) {
    decoding_parameters_t *parameters = data;
    int audio_done = 0;
    int video_done = 0;

    OutputStream *video_st = &(parameters->video_stream);
    OutputStream *audio_st = &(parameters->audio_stream);

    while (keep_running && (!audio_done || !video_done)) {
        if (!video_done && av_compare_ts(
                    video_st->next_pts,
                    video_st->codec_context->time_base,
                    audio_st->next_pts,
                    audio_st->codec_context->time_base) <= 0) {
            pthread_mutex_lock(&vid_packet_mutex);
            linked_list_t *head = ll_poph(&vid_packet_queue_s);
            if (!head) {
                pthread_mutex_unlock(&vid_packet_mutex);
                usleep(25);
                continue;
            }
            pthread_mutex_unlock(&vid_packet_mutex);

            AVPacket *packet = head->user_data;
            free(head);
            if (!packet) {
                fprintf(stderr, "VIDEO DONE\n");
                video_done = 1;
            } else {
                int err = av_interleaved_write_frame(parameters->format_context, packet);
                if (err) {
                    fprintf(stderr, "error av_interleaved_write_frame: %s\n", av_err2str(err));
                    keep_running = 0;
                    free(head);
                    free(packet);
                    break;
                }

                free(packet);
            }
        } else {
            pthread_mutex_lock(&aud_packet_mutex);
            linked_list_t *head = ll_poph(&aud_packet_queue_s);
            if (!head) {
                pthread_mutex_unlock(&aud_packet_mutex);
                usleep(25);
                continue;
            }
            pthread_mutex_unlock(&aud_packet_mutex);

            AVPacket *packet = head->user_data;
            free(head);
            if (!packet) {
                fprintf(stderr, "AUDIO DONE\n");
                audio_done = 1;
            } else {
                int err = av_interleaved_write_frame(parameters->format_context, packet);
                if (err) {
                    fprintf(stderr, "error av_interleaved_write_frame: %s\n", av_err2str(err));
                    keep_running = 0;
                    free(head);
                    free(packet);
                    break;
                }

                free(packet);
            }
        }
    }
    keep_running = 0;
    fprintf(stderr, "exit write interleaved thread\n");
    return NULL;
}

int init_audio_output(decoding_parameters_t *parameters) {
    int err = 0;
    
    parameters->audio_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
    if (!parameters->audio_codec) {
        fprintf(stderr, "error finding codec for pcm_s24le\n");
        err = 1;
        goto err_and_out;
    }

    parameters->audio_stream.stream = avformat_new_stream(parameters->format_context, NULL);
    if (!parameters->audio_stream.stream) {
        fprintf(stderr, "error allocating audio stream\n");
        err = 1;
        goto err_and_out;
    }

    parameters->audio_stream.stream->id = parameters->format_context->nb_streams - 1;
    parameters->audio_stream.codec_context = avcodec_alloc_context3(parameters->audio_codec);
    if (!parameters->audio_stream.codec_context) {
        fprintf(stderr, "error allocating audio codec context\n");
        err = 1;
        goto err_and_out;
    }

    AVCodecContext *c = parameters->audio_stream.codec_context;
    OutputStream *ost = &parameters->audio_stream;

    c->codec_id = AV_CODEC_ID_PCM_S24LE;
    // TO-DO: Get this from CPL
    c->sample_rate = 48000;
    c->channel_layout = AV_CH_LAYOUT_STEREO;
    c->sample_fmt = AV_SAMPLE_FMT_S32;
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    ost->stream->time_base = (AVRational){ 1, c->sample_rate };
    c->time_base = ost->stream->time_base;
    
    err = avcodec_open2(ost->codec_context, parameters->audio_codec, &parameters->encode_ops);
    if (err) {
        fprintf(stderr, "error opening audio codec %s\n", av_err2str(err));
        goto err_and_out;
    }
    
    ost->frame = av_frame_alloc();
    ost->frame->format = c->sample_fmt;
    ost->frame->channel_layout = c->channel_layout;
    ost->frame->sample_rate = c->sample_rate;
    ost->frame->nb_samples = 1920;//1960;//1920; //288000/25 = 11520; 11520/6=192    fprintf(stderr, "%d\n", ost->frame->nb_samples);
    err = av_frame_get_buffer(ost->frame, 0);
    if (err) {
        fprintf(stderr, "error allocating audio frame %s\n", av_err2str(err));
        goto err_and_out;
    }

    err = avcodec_parameters_from_context(ost->stream->codecpar, c);
    if (err) {
        fprintf(stderr, "error copying audio codec parameters\n");
        goto err_and_out;
    }
    
err_and_out:
    return err;
}

int init_video_output(decoding_parameters_t *parameters) {
    // set libav
    int averr = 0;

    parameters->video_codec = avcodec_find_encoder(AV_CODEC_ID_R210);
    if (!parameters->video_codec) {
        fprintf(stderr, "error finding codec for r210\n");
        averr = 1;
        goto err_and_out;
    }
    parameters->video_stream.stream = avformat_new_stream(parameters->format_context, NULL);
    if (!parameters->video_stream.stream) {
        fprintf(stderr, "error allocating av video stream\n");
        averr = 1;
        goto err_and_out;
    }
    parameters->video_stream.stream->id = parameters->format_context->nb_streams - 1;
    parameters->video_stream.codec_context = avcodec_alloc_context3(parameters->video_codec);
    if (!parameters->video_stream.codec_context) {
        fprintf(stderr, "error allocating video codec\n");
        averr = 1;
        goto err_and_out;
    }

    parameters->video_stream.codec_context->codec_id = AV_CODEC_ID_R210;
    // TODO: get this from CPL
    parameters->video_stream.codec_context->width = 1920;
    parameters->video_stream.codec_context->height = 1080;
    // 1 / fps
    parameters->video_stream.stream->time_base = (AVRational){ 1, 25 };
    parameters->video_stream.codec_context->time_base = parameters->video_stream.stream->time_base;
    // set framerate
    parameters->format_context->streams[0]->r_frame_rate = (AVRational){ 25, 1};
    parameters->video_stream.codec_context->pix_fmt = AV_PIX_FMT_GBRP10;

    // allocate codec
    averr = avcodec_open2(parameters->video_stream.codec_context, parameters->video_codec, &parameters->encode_ops);
    if (averr != 0) {
        fprintf(stderr, "error avcodec_open2\n");
        goto err_and_out;
    }
    // allocate frame that we reuse for encoding
    parameters->video_stream.frame = av_frame_alloc();
    parameters->video_stream.frame->format = parameters->video_stream.codec_context->pix_fmt;
    parameters->video_stream.frame->width = parameters->video_stream.codec_context->width;
    parameters->video_stream.frame->height = parameters->video_stream.codec_context->height;
    averr = av_frame_get_buffer(parameters->video_stream.frame, 0);
    if (averr != 0) {
        fprintf(stderr, "error allocating encoding frame\n");
        goto err_and_out;
    }

    if (parameters->format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        parameters->video_stream.codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    averr = avcodec_parameters_from_context(parameters->video_stream.stream->codecpar, parameters->video_stream.codec_context);
    if (averr != 0) {
        fprintf(stderr, "error copying avcodec_parameters\n");
        goto err_and_out;
    }

err_and_out:
    return averr;
}

int mxf_decode_files(linked_list_t *video_files, linked_list_t *audio_files, decoding_parameters_t *parameters) {
        // setup openjpeg2000
    opj_dparameters_t core;
    opj_set_default_decoder_parameters(&core);

    parameters->user_data = &core;

    int err = 0;
    err = av_dict_set(&parameters->encode_ops, NULL, NULL, 0);
    if (err != 0) {
        fprintf(stderr, "error allocating encode ops dict\n");
        goto free_and_out;
    }
    
    err = avformat_alloc_output_context2(&parameters->format_context, NULL, "nut", NULL);
    if (err < 0 || !&parameters->format_context) {
        fprintf(stderr, "error creating output context\n");
        goto free_and_out;
    }

    err = init_video_output(parameters);
    if (err != 0) {
        goto free_and_out;
    }

    err = init_audio_output(parameters);
    if (err != 0) {
        goto free_and_out;
    }

    // open output
    const char *output_file = "pipe:1";
    av_dump_format(parameters->format_context, 0, output_file, 1);

    err = avio_open(&parameters->format_context->pb, output_file, AVIO_FLAG_WRITE);
    if (err != 0) {
        fprintf(stderr, "error opening %s for output %s\n", output_file, av_err2str(err));
        goto free_and_out;
    }

    err = avformat_write_header(parameters->format_context, &parameters->encode_ops);
    if (err != 0) {
        fprintf(stderr, "error writing header\n");
        goto free_and_out;
    }

    keep_running = 1;

    pthread_t extract_audio_thread_id;
    pthread_t decoding_queue_thread_id;
    pthread_t writeout_queue_thread_id;
    pthread_t write_interleaved_thread_id;

    if (pthread_mutex_init(&decoding_mutex, NULL) != 0) {
        fprintf(stderr, "decoding mutex init has failed\n");
        return 1;
    }
    if (pthread_mutex_init(&writeout_mutex, NULL) != 0) {
        fprintf(stderr, "writeout mutex init has failed\n");
        return 1;
    }
    if (pthread_mutex_init(&vid_packet_mutex, NULL) != 0) {
        fprintf(stderr, "vid packet mutex init has failed\n");
        return 1;
    }
    if (pthread_mutex_init(&aud_packet_mutex, NULL) != 0) {
        fprintf(stderr, "aud packet mutex init has failed\n");
        return 1;
    }
    // start jpeg2000 decoding thread
    //
    pthread_create(&decoding_queue_thread_id, NULL, decode_frames_consumer, NULL);
    // start encoding thread for avcodec
    pthread_create(&writeout_queue_thread_id, NULL, writeout_frames_consumer, &parameters);
    pthread_create(&write_interleaved_thread_id, NULL, write_interleaved_consumer, parameters);

    // start audio extracting on thread
    audio_thread_args_t audio_thread_args;
    audio_thread_args.files = audio_files;
    audio_thread_args.parameters = parameters;

    pthread_create(&extract_audio_thread_id, NULL, extract_audio_thread, &audio_thread_args);
    
    // start decoding pipeline    
    err = asdcp_read_video_files(video_files, on_frame_data_mt, parameters);

    pthread_join(extract_audio_thread_id, NULL);
    fprintf(stderr, "extract_audio done\n");
    pthread_join(decoding_queue_thread_id, NULL);
    fprintf(stderr, "decoding_queue done\n");
    pthread_join(writeout_queue_thread_id, NULL);
    fprintf(stderr, "writeout_queue done\n");
    pthread_join(write_interleaved_thread_id, NULL);
    fprintf(stderr, "write_interleaved done\n");

    fprintf(stderr, "all threads done\n");
    av_write_trailer(parameters->format_context);

free_and_out:
    // TODO: Free all livav related stuff
    pthread_mutex_destroy(&decoding_mutex);
    pthread_mutex_destroy(&writeout_mutex);
    pthread_mutex_destroy(&vid_packet_mutex);
    pthread_mutex_destroy(&aud_packet_mutex);


    return err;

}
