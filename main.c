#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg-2.3/openjpeg.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "asdcp.h"
#include "color.h"
#include "io.h"
#include "linked_list.h"

static volatile int keep_running = 1;
pthread_mutex_t decoding_mutex;
pthread_mutex_t writeout_mutex;

void SIGINT_handler(int dummy) {
    keep_running = 0;
    signal(SIGINT, 0);
}

typedef struct {
  void *user_data;
  //opj_dparameters_t core;
  int num_threads;
  int print_debug;
  int out_fd;
  unsigned int decode_frame_buffer_size;
} decoding_parameters_t;

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
      usleep(500);
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
      usleep(500);
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
      usleep(100);
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

int main(int argc, char **argv) {

  signal(SIGINT, SIGINT_handler);

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

  decoding_parameters_t parameters;
  opj_dparameters_t core;

  memset(&parameters, 0, sizeof(decoding_parameters_t));
  opj_set_default_decoder_parameters(&core);

  parameters.user_data = &core;
  parameters.num_threads = opj_get_num_cpus() - 2; 
  parameters.print_debug = 0;
  parameters.out_fd = STDOUT_FILENO;
  parameters.decode_frame_buffer_size = 100;

  int err = asdcp_read_mxf(argv[1], &parameters, on_frame_data_mt);
  if (err) {
    fprintf(stderr, "error\n");
  }

  pthread_join(decoding_queue_thread_id, NULL);
  pthread_join(writeout_queue_thread_id, NULL);

  return 0;
}
