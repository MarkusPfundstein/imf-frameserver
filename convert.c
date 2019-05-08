#include <unistd.h>
#include <stdlib.h>
#include "convert.h"

int image_to_buf(opj_image_t *image, on_image_buf_func_t on_image_buf_func)
{
  size_t res;
  unsigned int compno, numcomps;
  int mask;
  int *ptr;
  unsigned char uc;

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

  int fails = 1;
  fprintf(stderr, "Raw image characteristics: %d components\n", image->numcomps);
  
  int w = (int)image->comps[0].w;
  int h = (int)image->comps[0].h;
  for (compno = 0; compno < image->numcomps; compno++) {
      fprintf(stderr, "Component %u characteristics: %dx%dx%d %s\n", compno,
              image->comps[compno].w,
              image->comps[compno].h,
              image->comps[compno].prec,
              image->comps[compno].sgnd == 1 ? "signed" : "unsigned");
      if (w != image->comps[compno].w || h != image->comps[compno].h) {
        fprintf(stderr, "invalid width and height for comp %d\n", compno);
        goto fin;
      }
  }

  union {
    unsigned short val;
    unsigned char vals[2];
  } uc16;

  int comp_table[3];
  comp_table[0] = 2;
  comp_table[1] = 0;
  comp_table[2] = 1;

  int *buffer = (int *)malloc(w * h * numcomps * sizeof(int));
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      for (compno = 0; compno < image->numcomps; compno++) {
        int compn = comp_table[compno];
        int curr = *(image->comps[compn].data + ((y * w + x)));
        if (curr > 65535) {
          curr = 65535;
        } else if (curr < 0) {
          curr = 0;
        }
        mask = (1 << image->comps[compn].prec) - 1;
        uc16.val = (unsigned short)(curr & mask);
        //write(STDOUT_FILENO, uc16.vals, 2);
        printf("%d", uc16.val);
        printf(" ");
      }
      printf("| ");
    }
    printf("\n");
  }

  free(buffer);
  
  fails = 0;
fin:
  return fails;
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

  unsigned int frame_size = w * h * image->numcomps * 2;
  unsigned char *out_buf = (unsigned char*)malloc(frame_size);
  int out_buf_pos = 0;
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
        /*
        res = write(fd, uc16.vals, 2);
        if (res < 2) {
          fprintf(stderr, "failed to write 2 byte\n");
          goto fin;
        }
        */
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
      break;
    }
    out_buf_pos += written;
  }

  free(out_buf);
  return 0;
}

/*
int image_to_fd(opj_image_t *image, int fd)
{
    size_t res;
    unsigned int compno, numcomps;
    int w, h, fails;
    int line, row, curr, mask;
    int *ptr;
    unsigned char uc;

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

    fails = 1;
    fprintf(stderr, "Raw image characteristics: %d components\n", image->numcomps);

    for (compno = 0; compno < image->numcomps; compno++) {
        fprintf(stderr, "Component %u characteristics: %dx%dx%d %s\n", compno,
                image->comps[compno].w,
                image->comps[compno].h, image->comps[compno].prec,
                image->comps[compno].sgnd == 1 ? "signed" : "unsigned");

        w = (int)image->comps[compno].w;
        h = (int)image->comps[compno].h;

        if (image->comps[compno].prec <= 8) {
            if (image->comps[compno].sgnd == 1) {
                mask = (1 << image->comps[compno].prec) - 1;
                ptr = image->comps[compno].data;
                for (line = 0; line < h; line++) {
                    for (row = 0; row < w; row++)    {
                        curr = *ptr;
                        if (curr > 127) {
                            curr = 127;
                        } else if (curr < -128) {
                            curr = -128;
                        }
                        uc = (unsigned char)(curr & mask);
                        // file desc, buf, n
                        res = write(fd, &uc, 1);

                        // ptr, size, count, stream
                        //res = fwrite(&uc, 1, 1, rawFile);
                        if (res < 1) {
                            fprintf(stderr, "failed to write 1 byte\n");
                            goto fin;
                        }
                        ptr++;
                    }
                }
            } else if (image->comps[compno].sgnd == 0) {
                mask = (1 << image->comps[compno].prec) - 1;
                ptr = image->comps[compno].data;
                for (line = 0; line < h; line++) {
                    for (row = 0; row < w; row++)    {
                        curr = *ptr;
                        if (curr > 255) {
                            curr = 255;
                        } else if (curr < 0) {
                            curr = 0;
                        }
                        uc = (unsigned char)(curr & mask);
                        res = write(fd, &uc, 1);
                        if (res < 1) {
                            fprintf(stderr, "failed to write 1 byte\n");
                            goto fin;
                        }
                        ptr++;
                    }
                }
            }
        } else if (image->comps[compno].prec <= 16) {
            if (image->comps[compno].sgnd == 1) {
                union {
                    signed short val;
                    signed char vals[2];
                } uc16;
                mask = (1 << image->comps[compno].prec) - 1;
                ptr = image->comps[compno].data;
                for (line = 0; line < h; line++) {
                    for (row = 0; row < w; row++)    {
                        curr = *ptr;
                        if (curr > 32767) {
                            curr = 32767;
                        } else if (curr < -32768) {
                            curr = -32768;
                        }
                        uc16.val = (signed short)(curr & mask);
                        res = write(fd, uc16.vals, 2);
                        //res = fwrite(uc16.vals, 1, 2, rawFile);
                        if (res < 2) {
                            fprintf(stderr, "failed to write 2 byte\n");
                            goto fin;
                        }
                        ptr++;
                    }
                }
            } else if (image->comps[compno].sgnd == 0) {
                union {
                    unsigned short val;
                    unsigned char vals[2];
                } uc16;
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
                        res = write(fd, uc16.vals, 2);
                        if (res < 2) {
                            fprintf(stderr, "failed to write 2 byte\n");
                            goto fin;
                        }
                        ptr++;
                    }
                }
            }
        } else if (image->comps[compno].prec <= 32) {
            fprintf(stderr, "More than 16 bits per component not handled yet\n");
            goto fin;
        } else {
            fprintf(stderr, "Error: invalid precision: %d\n", image->comps[compno].prec);
            goto fin;
        }
    }
    fails = 0;
fin:
    return fails;
}
*/
