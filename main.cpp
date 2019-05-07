#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// 1 MB read buf
#define MAX_BUF 1048576 
#define FRAME_SIZE_BUF 256

int main(int argc, char **argv) {

  // output of read operation
  unsigned char buf[MAX_BUF];
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

  while (1) {
    // we need more data and fill buf with MAX_BUF bytes. here we also check if we stop the operation
    if (pos == bytes_read) {
      bytes_read = read(STDIN_FILENO, buf, MAX_BUF);
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
          frame_buf = (unsigned char*)malloc(frame_size * sizeof(unsigned char));
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
          if (frame_buf[0] != 0xFF || frame_buf[1] != 0x4f || frame_buf[2] != 0xFF || frame_buf[3] != 0x51 ||
              frame_buf[frame_buf_pos-2] != 0xff || frame_buf[frame_buf_pos-1] != 0xd9) {
            printf("ERROR in frame-buf %d (%02x %02x)\n",
                current_frame, frame_buf[frame_buf_pos-2], frame_buf[frame_buf_pos-1]);
          }

          // emit frame_buf (n bytes = frame_buf_pos, [0, frame_buf_pos-1]
          printf("processed frame %d, %d bytes\n", current_frame-1, frame_buf_pos);

          frame_size = -1;
          free(frame_buf);
          frame_buf = NULL;
          break;
        }
      }
    }
  }

  return 0;
}
