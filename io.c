#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "io.h"

#define FRAME_SIZE_BUF 16

int read_frames(
    int fd,
    unsigned int read_buf_size,
    on_frame_buf_func_t on_frame_buf_func,
    void *user_data)
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
        on_frame_buf_func(NULL, 0, current_frame + 1, user_data);
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

          stop = on_frame_buf_func(frame_buf, frame_buf_pos, current_frame, user_data);
          frame_size = -1;
          break;
        }
      }
    }
  }

  free(buf);

  return 0;
}

int image_to_fd(opj_image_t *image, int fd) 
{
  unsigned int compno, numcomps;
  int line, row, curr, mask;
  int *ptr;

  if ((image->numcomps * image->x1 * image->y1) == 0) {
    fprintf(stderr, "\nError: invalid raw image parameters\n");
    return 1;
  }

  numcomps = image->numcomps;

  if (numcomps > 4) {
    numcomps = 4;
  }

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
    mask = (1 << image->comps[compno].prec) - 1;
    ptr = image->comps[compno].data;
    for (line = 0; line < h; line++) {
      for (row = 0; row < w; row++)    {
        curr = *ptr;
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
