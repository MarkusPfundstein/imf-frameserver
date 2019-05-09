#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg-2.3/openjpeg.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "color.h"
#include "convert.h"

typedef struct {
  int a;
  int b;
} test_t;

static volatile int keep_running = 1;
pthread_mutex_t decoding_mutex;

void SIGINT_handler(int dummy) {
    keep_running = 0;
}

typedef struct linked_list {
    void *user_data;
    struct linked_list *next;
} linked_list_t;

linked_list_t* ll_create(void *data) {
  linked_list_t *node = (linked_list_t*)malloc(sizeof(linked_list_t));
  node->user_data = data;
  node->next = NULL;
  return node;
}

linked_list_t* ll_append(linked_list_t *head, void *user_data) {
  if (!head) {
    return ll_create(user_data);
  }
  linked_list_t *p = head;
  while (p->next != NULL) {
    p = p->next;
  }
  linked_list_t *ll = ll_create(user_data);
  p->next = ll;

  return ll;
}

linked_list_t *ll_pop(linked_list_t *head) {
  if (!head) {
    return NULL;
  }
  linked_list_t *p = head;

  if (p->next) {
    head = p->next;
  } else {
    head = NULL;
  }

  free(p);
 
  return head;
}

unsigned int ll_len(linked_list_t *head) {
  if (!head) {
    return 0;
  }
  int i = 0;
  linked_list_t *p = head;
  while (p) {
    i++;
    p = p->next;
  }
  return i;
}

typedef struct {
  void *user_data;
  //opj_dparameters_t core;
  int num_threads;
  int print_debug;
  int out_fd;
  unsigned int decode_frame_buffer_size;
} decoding_parameters_t;

// 1 MB read buf
#define MAX_BUF 1048576 
#define FRAME_SIZE_BUF 16

typedef int (*on_frame_buf_func_t)(
    unsigned char* frame_buf,
    unsigned int frame_size,
    unsigned int current_frame,
    decoding_parameters_t *parameters);

int read_frames(
    int fd,
    unsigned int read_buf_size,
    on_frame_buf_func_t on_frame_buf_func,
    decoding_parameters_t *parameters)
{
  // output of read operation
  unsigned char *buf = (unsigned char*)malloc(read_buf_size);
  // size of frame before converting it to int frame_size
  char frame_size_buf[FRAME_SIZE_BUF];

  // current position in buf
  int pos = 0;
  // current position in frame_size_buf
  int frame_size_buf_pos = 0;
  // how big is our frame. -1 => no frame parsed yet
  int frame_size = -1;
  // how much have we read into buf
  int bytes_read = 0;
  // which frame are we processing
  int current_frame = 0;

  // heap allocated storage where we put the whole raw j2k frame
  unsigned char *frame_buf = NULL;
  // position into frame_buf_pos
  int frame_buf_pos = 0;

  int stop = 0;

  while (!stop) {
    // we need more data and fill buf with MAX_BUF bytes. here we also check if we stop the operation
    if (pos == bytes_read) {
      bytes_read = read(fd, buf, read_buf_size);
      if (bytes_read == 0) {
        on_frame_buf_func(NULL, 0, current_frame + 1, parameters);
        break;
      }
      pos = 0;
    }

    // frame_size is -1 and we need to figure out how big the next frame is going to be. read until new-line and
    // store result in frame_size
    if (frame_size < 0) {
      while (pos < bytes_read) {
        char c = buf[pos++];
        if (c == '\n') {
          frame_size_buf[frame_size_buf_pos] = '\0';
          frame_size = atoi(frame_size_buf);
          // allocate buffer large enough to hold our frame
          frame_buf = (unsigned char*)malloc(frame_size);
          frame_buf_pos = 0;
          frame_size_buf_pos = 0;
          current_frame += 1;
          break;
        } 
        frame_size_buf[frame_size_buf_pos++] = c;
      }
    }

    // we still have frame_size bytes left to read
    if (frame_size > 0) {
      while (pos < bytes_read) {
        // try to copy as much as possible into frame_buf. if we try to read more then frame_size, adjust
        int n = bytes_read - pos;
        if (frame_size - n < 0) {
          n = frame_size;
        }
        memcpy(frame_buf + frame_buf_pos, buf + pos, n);
        pos += n;
        frame_buf_pos += n;
        frame_size -= n;
        
        // we are done and completely processed a frame
        if (frame_size == 0) {
          // sanity checks to see if frame_buf is filled completely
          // codestream j2k magic
          if (frame_buf[0] != 0xFF || frame_buf[1] != 0x4f || frame_buf[2] != 0xFF || frame_buf[3] != 0x51 ||
              frame_buf[frame_buf_pos-2] != 0xff || frame_buf[frame_buf_pos-1] != 0xd9) {
            fprintf(stderr, "ERROR in frame-buf %d (%02x %02x)\n",
                current_frame, frame_buf[frame_buf_pos-2], frame_buf[frame_buf_pos-1]);
          }

          stop = on_frame_buf_func(frame_buf, frame_buf_pos, current_frame, parameters);
          frame_size = -1;
          free(frame_buf);
          frame_buf = NULL;
          break;
        }
      }
    }
  }

  free(buf);

  return 0;
}

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

typedef struct {
  // need to free later when consumed
  int keep_going;
  unsigned char *frame_buf;
  unsigned int frame_size;
  unsigned int current_frame;
  decoding_parameters_t *parameters;
} decoding_queue_context_t;

static linked_list_t *decoding_queue_s = NULL;

int on_frame_data_mt(
    unsigned char* frame_buf,
    unsigned int frame_size,
    unsigned int current_frame,
    decoding_parameters_t *parameters)
{
  int wait = 1;
  do {
    pthread_mutex_lock(&decoding_mutex);
    int frames_buffered = ll_len(decoding_queue_s);
    pthread_mutex_unlock(&decoding_mutex);
    if (frames_buffered > parameters->decode_frame_buffer_size) {
      usleep(500);
    } else {
      fprintf(stderr, "break out\n");
      wait = 0;
    }
  } while (wait);

  decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)malloc(sizeof(decoding_queue_context_t));

  if (!frame_buf) {
    decoding_queue_context->frame_buf = NULL;
    decoding_queue_context->frame_size = 0;
    decoding_queue_context->current_frame = current_frame;
    decoding_queue_context->parameters = parameters;
    decoding_queue_context->keep_going = 0;

  } else {
    unsigned char *copy_buf = malloc(frame_size);
    memcpy(copy_buf, frame_buf, frame_size);

    decoding_queue_context->frame_buf = copy_buf;
    decoding_queue_context->frame_size = frame_size;
    decoding_queue_context->current_frame = current_frame;
    decoding_queue_context->parameters = parameters;
    decoding_queue_context->keep_going = 1;
  }

  pthread_mutex_lock(&decoding_mutex);
  fprintf(stderr, "push %d\n", current_frame - 1);
  if (!decoding_queue_s) {
    decoding_queue_s = ll_append(NULL, decoding_queue_context);
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
  if (!frame_buf) {
    return 1;
  }
  opj_buffer_info_t buffer_info;
  buffer_info.buf = frame_buf;
  buffer_info.cur = frame_buf;
  buffer_info.len = frame_size;

  opj_image_t *image = NULL;
  opj_stream_t *stream = NULL;
  opj_stream_t *codec = NULL;

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

  if (parameters->out_fd >= 0) {
    int err = image_to_fd(image, parameters->out_fd); 
    if (err) {
      fprintf(stderr, "error image_to_fd [frame: %d]\n", current_frame);
      ok = 0;
      goto free_and_out;
    }
  }

  if (parameters->print_debug) {
    fprintf(stderr, "[on_frame] processed frame %d, %d bytes\n", current_frame, frame_size);
  }

free_and_out:
  if (image) {
    opj_image_destroy(image);
  }
  if (stream) {
    opj_stream_destroy(stream);
  }
  if (codec) {
    opj_destroy_codec(codec);
  }
  return !ok;
}

void *decode_frames_consumer(void *thread_data) {
  //int ID = *((int *) id_ptr);
  //static int nextConsumed = 0;

  while (keep_running) {

    pthread_mutex_lock(&decoding_mutex);
    decoding_queue_s = ll_pop(decoding_queue_s);
    pthread_mutex_unlock(&decoding_mutex);
    if (!decoding_queue_s) {
      usleep(100);
      continue;
    }

    decoding_queue_context_t *decoding_queue_context = (decoding_queue_context_t *)decoding_queue_s->user_data;
    if (!decoding_queue_context->keep_going) {
      break;
    }

    int err = decode_frame(
        decoding_queue_context->frame_buf,
        decoding_queue_context->frame_size,
        decoding_queue_context->current_frame,
        decoding_queue_context->parameters);
    if (err) {
      keep_running = 0;
    }

    free(decoding_queue_context->frame_buf);
    free(decoding_queue_context);
  }

  fprintf(stderr, "exit thread\n");
  return NULL;
}

int main(int argc, char **argv) {
  
  /*
  test_t* t1 = (test_t*)malloc(sizeof(test_t));
  printf("malloc %p\n", t1);
  t1->a = 5;
  t1->b = 10;

  test_t* t2 = (test_t*)malloc(sizeof(test_t));
  printf("malloc %p\n", t2);

  t2->a = 15;
  t2->b = 20;

  test_t* t3 = (test_t*)malloc(sizeof(test_t));
  printf("malloc %p\n", t3);

  t3->a = 25;
  t3->b = 30;


  linked_list_t *ll = ll_create(t1);
  ll_append(ll, t2);
  ll_append(ll, t3);

  linked_list_t *head = ll;
  do {
    printf("it, %p\n", head);
    test_t *t = (test_t*)head->user_data;
    printf("%d %d\n", t->a, t->b);
    printf("free %p\n", t);
    free(t);
  }
  while ((head = ll_pop(head)) != NULL);

  return 0;
  */
  
  signal(SIGINT, SIGINT_handler);

  pthread_t decoding_queue_thread_id;

  if (pthread_mutex_init(&decoding_mutex, NULL) != 0) {
    fprintf(stderr, "decoding mutex init has failed\n");
    return 1;
  }

  pthread_create(&decoding_queue_thread_id, NULL, decode_frames_consumer, NULL);

  decoding_parameters_t parameters;
  opj_dparameters_t core;

  memset(&parameters, 0, sizeof(decoding_parameters_t));
  opj_set_default_decoder_parameters(&core);

  parameters.user_data = &core;
  parameters.num_threads = opj_get_num_cpus();
  parameters.print_debug = 0;
  parameters.out_fd = STDOUT_FILENO;
  parameters.decode_frame_buffer_size = 25;

  int err = read_frames(STDIN_FILENO, MAX_BUF, on_frame_data_mt, &parameters);
  if (err) {
    fprintf(stderr, "error\n");
  }

  pthread_join(decoding_queue_thread_id, NULL);
  fprintf(stderr, "bye bye\n");

  return 0;
}
