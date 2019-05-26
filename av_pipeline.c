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
#include "imf.h"
#include "linked_list.h"
#include "av_pipeline.h"

static volatile int keep_running = 1;

pthread_mutex_t decoding_mutex;
pthread_mutex_t vid_packet_mutex;
pthread_mutex_t aud_packet_mutex;

typedef struct {
    // need to free later when consumed
    unsigned char *frame_buf;
    unsigned int frame_size;
    unsigned int current_frame;
} decoding_queue_context_t;

static linked_list_t *decoding_queue_s = NULL;
static linked_list_t *vid_packet_queue_s = NULL;
static linked_list_t *aud_packet_queue_s = NULL;


// 5 MB read buf
#define MAX_BUF 5*1048576 

#define MAX_QUEUE_LEN   25
#define QUEUE_SLEEP_MS  10

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

void block_until_queue_has_space(pthread_mutex_t *m, linked_list_t **q, int threshold, int sleep_ms) {
    int wait = 1;
    while (keep_running && wait) {
        pthread_mutex_lock(m);
        int frames_buffered = ll_len(*q);
        pthread_mutex_unlock(m);
        if (frames_buffered > threshold) {
            usleep(sleep_ms);
        } else {
            wait = 0;
        }
    }
}

void push_to_queue(pthread_mutex_t *m, linked_list_t **q, void *data) {
    pthread_mutex_lock(m);
    if (!*q) {
        *q = ll_create(data);
    } else {
        ll_append(*q, data);
    }
    pthread_mutex_unlock(m);
}

linked_list_t *blocked_pop_queue(pthread_mutex_t *m, linked_list_t **q, int sleep_ms) {
    int wait = 1;
    linked_list_t *head = NULL;
    while (keep_running && wait) {
        pthread_mutex_lock(m);
        head = ll_poph(q);
        pthread_mutex_unlock(m);
        if (!head) {
            usleep(sleep_ms);
        } else {
            wait = 0;
        }
    }

    return head;
}

int encode_image_to_r210(opj_image_t *image, av_pipeline_context_t *av_context, AVPacket **pkt_ptr) 
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
        err = av_frame_make_writable(av_context->video_stream.frame);
        if (err) {
            fprintf(stderr, "error av_frame_make_writeable\n");
            goto err_and_out;
        }


        // To-DO: check if we can use this part and directly encode to r210
        // this way we can skip avcodec_encode_video2 below which does in a way
        // only another loop through the frame
        AVFrame *frame = av_context->video_stream.frame;
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

        frame->pts = av_context->video_stream.next_pts++;

        pkt = (AVPacket*)malloc(sizeof(AVPacket));
        memset(pkt, 0, sizeof(AVPacket));

        AVCodecContext *c = av_context->video_stream.codec_context;
        AVStream *st = av_context->video_stream.stream;

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
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        *pkt_ptr = pkt;
    }
    
err_and_out:
    return err;
}

int on_jpeg2000_frame(unsigned char* frame_buf, unsigned int frame_size, unsigned int current_frame, void *user_data)
{
    av_pipeline_context_t *av_context = user_data;
    
    decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)malloc(sizeof(decoding_queue_context_t));

    decoding_queue_context->frame_buf = frame_buf;
    decoding_queue_context->frame_size = frame_size;
    decoding_queue_context->current_frame = current_frame;

    block_until_queue_has_space(
            &decoding_mutex,
            &decoding_queue_s,
            MAX_QUEUE_LEN*10,
            QUEUE_SLEEP_MS);

    push_to_queue(
            &decoding_mutex,
            &decoding_queue_s,
            decoding_queue_context);

    return 0;
}

int decode_jpeg2000_frame(unsigned char* frame_buf, unsigned int frame_size, unsigned int current_frame, av_pipeline_context_t *av_context, opj_image_t **image_ptr)
{
    int ok = 1;
    opj_image_t *image = NULL;
    opj_stream_t *stream = NULL;
    opj_stream_t *codec = NULL;

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

    if (av_context->print_debug) {
        opj_set_info_handler(codec, info_callback, 00);
        opj_set_warning_handler(codec, warning_callback, 00);
        opj_set_error_handler(codec, error_callback, 00);
    } else {
        opj_set_info_handler(codec, quiet_callback, 00);
        opj_set_warning_handler(codec, quiet_callback, 00);
        opj_set_error_handler(codec, quiet_callback, 00);
    }

    opj_dparameters_t *core = av_context->user_data;

    ok = opj_setup_decoder(codec, core);
    if (!ok) {
        fprintf(stderr, "failed to setup decoder [frame: %d]\n", current_frame);
        goto free_and_out;
    }

    ok = opj_codec_set_threads(codec, av_context->num_threads);
    if (!ok) {
        fprintf(stderr, "failed to setup %d threads [frame: %d]\n",
                av_context->num_threads,
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
        fprintf(stderr, "failedon end decompress [frame: %d]\n", current_frame);
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
        ok = color_sycc_to_rgb(image);
        if (!ok) {
            fprintf(stderr, "error converting sycc to rgb\n");
            goto free_and_out;
        }
    }
   
    *image_ptr = image;

free_and_out:
    if (stream) {
        opj_stream_destroy(stream);
    }
    if (codec) {
        opj_destroy_codec(codec);
    }
    return !ok;
}

void *jpeg2000_to_r210_thread(void *thread_data) {
    av_pipeline_context_t *av_context = thread_data;
    while (keep_running) {
        linked_list_t *head = blocked_pop_queue(
                &decoding_mutex,
                &decoding_queue_s,
                QUEUE_SLEEP_MS);
        if (!head) {
            keep_running = 0;
            break;
        }
        
        decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)head->user_data;

        AVPacket *pkt = NULL;

        if (decoding_queue_context->frame_buf) {
            opj_image_t *image = NULL;

            int err = decode_jpeg2000_frame(
                    decoding_queue_context->frame_buf,
                    decoding_queue_context->frame_size,
                    decoding_queue_context->current_frame,
                    av_context,
                    &image);
            if (err) {
                fprintf(stderr, "err decode frame\n");
                keep_running = 0;
            } else {
                err = encode_image_to_r210(image, av_context, &pkt);
                if (err) {
                    fprintf(stderr, "error encoding image\n");
                    keep_running = 0;
                }
            }
            free(decoding_queue_context->frame_buf);
            if (image) {
                opj_image_destroy(image);
            }
        }

        block_until_queue_has_space(
                &vid_packet_mutex,
                &vid_packet_queue_s,
                MAX_QUEUE_LEN,
                QUEUE_SLEEP_MS);

        push_to_queue(
                &vid_packet_mutex,
                &vid_packet_queue_s,
                pkt);

        free(decoding_queue_context);
        free(head);
    }

    fprintf(stderr, "exit decoding thread\n");
    return NULL;
}

int stop_decoding_signal() {
    keep_running = 0;
}

int encode_pcm24le_audio(unsigned char *buf, unsigned int length, unsigned int current_frame, void *user_data) {
    int err = 0;
    if (!keep_running) {
        return -1;
    }
    AVPacket *pkt = NULL;
    if (buf) {

        av_pipeline_context_t *av_context = user_data;

        OutputStream *ost = &av_context->audio_stream;
        AVCodecContext *c = ost->codec_context;
        AVFrame *frame = ost->frame;

        int8_t *data_ptr = frame->data[0];
        int8_t *src_ptr = buf;
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
            free(pkt);
            pkt = NULL;
            goto err_and_out;
        }
        if (!got_packet) {
            fprintf(stderr, "got no audio packet\n");
            err = 1;
            free(pkt);
            pkt = NULL;
            goto err_and_out;
        }
        
        av_packet_rescale_ts(pkt, c->time_base, ost->stream->time_base);
        pkt->stream_index = ost->stream->index;
    }

    block_until_queue_has_space(
            &aud_packet_mutex,
            &aud_packet_queue_s,
            MAX_QUEUE_LEN,
            QUEUE_SLEEP_MS);

    push_to_queue(
            &aud_packet_mutex,
            &aud_packet_queue_s,
            pkt);

err_and_out:
    if (buf) {
        free(buf);
    }
    return err;
}

typedef struct {
    linked_list_t *files;
    av_pipeline_context_t *av_context;
} audio_thread_args_t;

void* extract_audio_thread(void *data) {
    audio_thread_args_t *args = data;
    linked_list_t *files = args->files;
    av_pipeline_context_t *av_context = args->av_context;
    int err = asdcp_read_audio_files(files, av_context, encode_pcm24le_audio, av_context);
    if (err && !keep_running) {
        fprintf(stderr, "error audio thread\n");
        stop_decoding_signal();
    }

    fprintf(stderr, "exit extract_audio_thread\n");

    return NULL;
}

void* write_output_file_thread(void *data) {
    av_pipeline_context_t *av_context = data;
    int audio_done = 0;
    int video_done = 0;

    OutputStream *video_st = &(av_context->video_stream);
    OutputStream *audio_st = &(av_context->audio_stream);

    while (keep_running && (!audio_done || !video_done)) {
        AVPacket *packet = NULL;
        if (!video_done && av_compare_ts(
                    video_st->next_pts,
                    video_st->codec_context->time_base,
                    audio_st->next_pts,
                    audio_st->codec_context->time_base) <= 0) {
            linked_list_t *head = blocked_pop_queue(
                    &vid_packet_mutex,
                    &vid_packet_queue_s,
                    QUEUE_SLEEP_MS);
            if (!head) {
                keep_running = 0;
                continue;
            }

            packet = head->user_data;
            free(head);
            if (!packet) {
                fprintf(stderr, "VIDEO DONE\n");
                video_done = 1;
                continue;
            } 
        } else {
            linked_list_t *head = blocked_pop_queue(
                    &aud_packet_mutex,
                    &aud_packet_queue_s,
                    QUEUE_SLEEP_MS);
            if (!head) {
                keep_running = 0;
                continue;
            }

            packet = head->user_data;
            free(head);
            if (!packet) {
                fprintf(stderr, "AUDIO DONE\n");
                audio_done = 1;
                continue;
            }
        }

        if (packet) {
            int err = av_interleaved_write_frame(av_context->format_context, packet);
            if (err) {
                fprintf(stderr, "error av_interleaved_write_frame: %s\n", av_err2str(err));
                keep_running = 0;
            }

            free(packet);
        }
    }
    keep_running = 0;
    fprintf(stderr, "exit write interleaved thread\n");
    return NULL;
}

int init_audio_output(av_pipeline_context_t *av_context, asset_t *asset) {
    int err = 0;
    
    av_context->audio_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
    if (!av_context->audio_codec) {
        fprintf(stderr, "error finding codec for pcm_s24le\n");
        err = 1;
        goto err_and_out;
    }

    av_context->audio_stream.stream = avformat_new_stream(av_context->format_context, NULL);
    if (!av_context->audio_stream.stream) {
        fprintf(stderr, "error allocating audio stream\n");
        err = 1;
        goto err_and_out;
    }

    av_context->audio_stream.stream->id = av_context->format_context->nb_streams - 1;
    av_context->audio_stream.codec_context = avcodec_alloc_context3(av_context->audio_codec);
    if (!av_context->audio_stream.codec_context) {
        fprintf(stderr, "error allocating audio codec context\n");
        err = 1;
        goto err_and_out;
    }

    AVCodecContext *c = av_context->audio_stream.codec_context;
    OutputStream *ost = &av_context->audio_stream;

    cpl_composition_playlist *cpl = av_context->cpl;
    if (!cpl) {
        fprintf(stderr, "no cpl\n");
        err = 1;
        goto err_and_out;
    }

    cpl_wave_pcm_descriptor *pcm_desc = asset->essence_descriptor;
    if (!pcm_desc) {
        fprintf(stderr, "no pcm descriptor\n");
        err = 1;
        goto err_and_out;
    }

    c->codec_id = AV_CODEC_ID_PCM_S24LE;
    // TO-DO: Get this from CPL
    c->sample_rate = (int)(pcm_desc->sample_rate.num / pcm_desc->sample_rate.denom);
    if (pcm_desc->channel_count == 2) {
        c->channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        fprintf(stderr, "ONLY STEREO SUPPORTED (YET)\n");
        err = 1;
        goto err_and_out;
    }
    c->sample_fmt = AV_SAMPLE_FMT_S32;
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    ost->stream->time_base = (AVRational){ 1, c->sample_rate };
    c->time_base = ost->stream->time_base;
    
    err = avcodec_open2(ost->codec_context, av_context->audio_codec, &av_context->encode_ops);
    if (err) {
        fprintf(stderr, "error opening audio codec %s\n", av_err2str(err));
        goto err_and_out;
    }
    
    ost->frame = av_frame_alloc();
    ost->frame->format = c->sample_fmt;
    ost->frame->channel_layout = c->channel_layout;
    ost->frame->sample_rate = c->sample_rate;
    // TO-DO: CHECK IF THIS CALC IS CORRECT. WORKS FOR 25 fps
    float fps = (float)cpl->edit_rate.num / (float)cpl->edit_rate.denom;
    // or ceil?
    int nb_samples = floor(pcm_desc->average_bytes_per_second / (fps * pcm_desc->block_align));
    ost->frame->nb_samples = nb_samples;
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

int init_video_output(av_pipeline_context_t *av_context, asset_t *asset) {
    // set libav
    int averr = 0;

    av_context->video_codec = avcodec_find_encoder(AV_CODEC_ID_R210);
    if (!av_context->video_codec) {
        fprintf(stderr, "error finding codec for r210\n");
        averr = 1;
        goto err_and_out;
    }
    av_context->video_stream.stream = avformat_new_stream(av_context->format_context, NULL);
    if (!av_context->video_stream.stream) {
        fprintf(stderr, "error allocating av video stream\n");
        averr = 1;
        goto err_and_out;
    }
    av_context->video_stream.stream->id = av_context->format_context->nb_streams - 1;
    av_context->video_stream.codec_context = avcodec_alloc_context3(av_context->video_codec);
    if (!av_context->video_stream.codec_context) {
        fprintf(stderr, "error allocating video codec\n");
        averr = 1;
        goto err_and_out;
    }

    cpl_composition_playlist *cpl = av_context->cpl;
    if (!cpl) {
        fprintf(stderr, "no cpl\n");
        averr = 1;
        goto err_and_out;
    }

    cpl_cdci_descriptor *cdci_desc = NULL;
    cpl_rgba_descriptor *rgba_desc = NULL;

    fraction_t edit_rate = cpl->edit_rate;

    int stored_width;
    int stored_height;
    float fps;
    if (asset->picture_type == PICTURE_TYPE_CDCI) {
        cdci_desc = asset->essence_descriptor;
        stored_width = cdci_desc->stored_width;
        stored_height = cdci_desc->stored_height;
        fps = (float)edit_rate.num / (float)edit_rate.denom;
    } else if (asset->picture_type == PICTURE_TYPE_RGBA) {
        //rgba_desc = asset->essence_descriptor;
    }
    if (!cdci_desc && !rgba_desc) {
        fprintf(stderr, "couldnt find essence descriptor to init codecs\n");
        averr = 1;
        goto err_and_out;
    }

    fprintf(stderr, "init with w: %d, h: %d, r: %d/%d, fps: %f\n", stored_width, stored_height, edit_rate.num, edit_rate.denom, fps);

    av_context->video_stream.codec_context->codec_id = AV_CODEC_ID_R210;
    // TODO: get this from CPL
    av_context->video_stream.codec_context->width = stored_width;
    av_context->video_stream.codec_context->height = stored_height;
    // 1 / fps
    av_context->video_stream.stream->time_base = (AVRational){ 1, fps };
    av_context->video_stream.codec_context->time_base = av_context->video_stream.stream->time_base;
    // set framerate
    av_context->format_context->streams[0]->r_frame_rate = (AVRational){ edit_rate.num, edit_rate.denom};
    av_context->video_stream.codec_context->pix_fmt = AV_PIX_FMT_GBRP10;

    // allocate codec
    averr = avcodec_open2(av_context->video_stream.codec_context, av_context->video_codec, &av_context->encode_ops);
    if (averr != 0) {
        fprintf(stderr, "error avcodec_open2\n");
        goto err_and_out;
    }
    // allocate frame that we reuse for encoding
    av_context->video_stream.frame = av_frame_alloc();
    av_context->video_stream.frame->format = av_context->video_stream.codec_context->pix_fmt;
    av_context->video_stream.frame->width = av_context->video_stream.codec_context->width;
    av_context->video_stream.frame->height = av_context->video_stream.codec_context->height;
    averr = av_frame_get_buffer(av_context->video_stream.frame, 0);
    if (averr != 0) {
        fprintf(stderr, "error allocating encoding frame\n");
        goto err_and_out;
    }

    if (av_context->format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        av_context->video_stream.codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    averr = avcodec_parameters_from_context(av_context->video_stream.stream->codecpar, av_context->video_stream.codec_context);
    if (averr != 0) {
        fprintf(stderr, "error copying avcodec_parameters\n");
        goto err_and_out;
    }

err_and_out:
    return averr;
}

void close_stream(OutputStream *ost) {
    if (ost->codec_context) {
        avcodec_free_context(&ost->codec_context);
    }
    if (ost->frame) {
        av_frame_free(&ost->frame);
    }
}

int av_pipeline_run(linked_list_t *video_files, linked_list_t *audio_files, av_pipeline_context_t *av_context) {
        // setup openjpeg2000
    opj_dparameters_t core;
    opj_set_default_decoder_parameters(&core);

    av_context->user_data = &core;

    int err = 0;
    err = av_dict_set(&av_context->encode_ops, NULL, NULL, 0);
    if (err != 0) {
        fprintf(stderr, "error allocating encode ops dict\n");
        goto free_and_out;
    }
    
    err = avformat_alloc_output_context2(&av_context->format_context, NULL, "nut", NULL);
    if (err < 0 || !&av_context->format_context) {
        fprintf(stderr, "error creating output context\n");
        goto free_and_out;
    }

    // TO-DO: do this for EVERY asset so that we in theory could mix RGBA & CDCI etc.
    // For now, assume all essence in one CPL have same type
    asset_t *first_vid_asset = video_files->user_data;
    asset_t *first_aud_asset = audio_files->user_data;

    // To-DO: enable encoding of only video and only audio
    if (!first_vid_asset || !first_aud_asset) {
        fprintf(stderr, "can only encode CPLs with both picture and video stream\n");
        err = 1;
        goto free_and_out;
    }

    err = init_video_output(av_context, first_vid_asset);
    if (err != 0) {
        goto free_and_out;
    }

    err = init_audio_output(av_context, first_aud_asset);
    if (err != 0) {
        goto free_and_out;
    }

    // open output
    const char *output_file = "pipe:1";
    av_dump_format(av_context->format_context, 0, output_file, 1);

    err = avio_open(&av_context->format_context->pb, output_file, AVIO_FLAG_WRITE);
    if (err != 0) {
        fprintf(stderr, "error opening %s for output %s\n", output_file, av_err2str(err));
        goto close_and_out;
    }

    err = avformat_write_header(av_context->format_context, &av_context->encode_ops);
    if (err != 0) {
        fprintf(stderr, "error writing header\n");
        goto close_and_out;
    }

    keep_running = 1;

    pthread_t extract_audio_thread_id;
    pthread_t decoding_queue_thread_id;
    pthread_t write_interleaved_thread_id;

    pthread_mutex_init(&decoding_mutex, NULL);
    pthread_mutex_init(&vid_packet_mutex, NULL);
    pthread_mutex_init(&aud_packet_mutex, NULL);
    // start jpeg2000 decoding thread
    pthread_create(&decoding_queue_thread_id, NULL, jpeg2000_to_r210_thread, av_context);
    // start encoding thread for avcodec
    pthread_create(&write_interleaved_thread_id, NULL, write_output_file_thread, av_context);

    // start audio extracting on thread
    audio_thread_args_t audio_thread_args;
    audio_thread_args.files = audio_files;
    audio_thread_args.av_context = av_context;

    pthread_create(&extract_audio_thread_id, NULL, extract_audio_thread, &audio_thread_args);
    
    // start decoding pipeline    
    err = asdcp_read_video_files(video_files, av_context, on_jpeg2000_frame, av_context);

    pthread_join(extract_audio_thread_id, NULL);
    fprintf(stderr, "extract_audio done\n");
    pthread_join(decoding_queue_thread_id, NULL);
    fprintf(stderr, "decoding_queue done\n");
    pthread_join(write_interleaved_thread_id, NULL);
    fprintf(stderr, "write_interleaved done\n");
    fprintf(stderr, "all threads done\n");
    av_write_trailer(av_context->format_context);

close_and_out:
    fprintf(stderr, "close output\n");
    avio_closep(&av_context->format_context->pb);

free_and_out:
    close_stream(&av_context->video_stream);
    close_stream(&av_context->audio_stream);

    if (av_context->format_context) {
        avformat_free_context(av_context->format_context);
    }
    pthread_mutex_destroy(&decoding_mutex);
    pthread_mutex_destroy(&vid_packet_mutex);
    pthread_mutex_destroy(&aud_packet_mutex);

    return err;
}
