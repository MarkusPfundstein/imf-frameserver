#include <unistd.h>
#include <stdlib.h>
#include "convert.h"

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
