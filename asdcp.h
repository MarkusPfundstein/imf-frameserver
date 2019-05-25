#ifndef ASDCP_H
#define ASDCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "linked_list.h"

enum asset_type {
    ASSET_TYPE_PICTURE = 1,
    ASSET_TYPE_AUDIO,
};

enum picture_type {
    PICTURE_TYPE_NONE = 0,
    PICTURE_TYPE_RGBA,
    PICTURE_TYPE_CDCI
};

typedef struct {
    enum asset_type asset_type;
    enum picture_type picture_type;
    // which file to decode
    char mxf_path[512];
    // which frame to start
    int start_frame;
    // last frame to decode
    int end_frame;
    // essence descriptor (RGBA, CDCI, WAVEPCM)
    void *essence_descriptor;
} asset_t;

typedef int (*asdcp_on_pcm_frame_func)(unsigned char *data, unsigned int length, unsigned int current_frame, void *user_data);
typedef int (*asdcp_on_j2k_frame_func)(unsigned char *data, unsigned int length, unsigned int frame_count, void *user_data);

extern int asdcp_read_audio_files(linked_list_t *files, asdcp_on_pcm_frame_func on_frame, void *user_data);
extern int asdcp_read_video_files(linked_list_t *files, asdcp_on_j2k_frame_func on_frame, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
