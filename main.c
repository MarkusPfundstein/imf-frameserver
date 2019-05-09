#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg-2.3/openjpeg.h>
#include "color.h"
#include "convert.h"

typedef struct {
  void *user_data;
  //opj_dparameters_t core;
  int num_threads;
  int print_debug;
  int out_fd;
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

int on_frame_data(
    unsigned char* frame_buf,
    unsigned int frame_size,
    unsigned int current_frame,
    decoding_parameters_t *parameters)
{
    int ok = 0;
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

    fprintf(stderr, "[on_frame] processed frame %d, %d bytes\n", current_frame, frame_size);

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

int main(int argc, char **argv) {
  decoding_parameters_t parameters;
  opj_dparameters_t core;

  memset(&parameters, 0, sizeof(decoding_parameters_t));
  opj_set_default_decoder_parameters(&core);

  parameters.user_data = &core;
  parameters.num_threads = opj_get_num_cpus();
  parameters.print_debug = 0;
  parameters.out_fd = STDOUT_FILENO;

  int err = read_frames(STDIN_FILENO, MAX_BUF, on_frame_data, &parameters);
  if (err) {
    fprintf(stderr, "error\n");
  }

  return 0;
}
