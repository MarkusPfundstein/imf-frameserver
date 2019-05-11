#ifndef ASDCP_H
#define ASDCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "linked_list.h"

typedef struct {
  // which file to decode
  char mxf_path[512];
  // which frame to start
  int start_frame;
  // last frame to decode
  int end_frame;
} asset_t;

typedef int (*asdcp_on_j2k_frame_func)(unsigned char *data, unsigned int length, unsigned int frame_count, void *user_data);

extern int asdcp_read_mxf_list(linked_list_t *files, asdcp_on_j2k_frame_func on_frame, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
