#ifndef __IMF_H__
#define __IMF_H__

#include "linked_list.h"

typedef struct {
  int num;
  int denom;
} fraction_t;

typedef struct {
  char id[64];
  fraction_t edit_rate; // not parsed yet
  unsigned int intrinsic_duration;
  unsigned int entry_point;
  unsigned int source_duration;
  unsigned int repeat_count;
  char track_file_id[64];
  char source_encoding[64];
} cpl_resource_t;

typedef struct {
  char path[512];
  int volume_index;
  int offset;
  int length;
} am_chunk_t;

extern am_chunk_t* am_get_chunk_for_resource(const char *filename, cpl_resource_t *resource);
extern void am_free_chunk(am_chunk_t *chunk);
extern linked_list_t* cpl_get_video_resources(const char *filename);
extern linked_list_t* cpl_get_audio_resources(const char *filename);

extern void cpl_free_resources(linked_list_t *resources);
//urn:uuid:532e4f0b-852a-4d7d-a52f-aae87deb836a<

#endif
