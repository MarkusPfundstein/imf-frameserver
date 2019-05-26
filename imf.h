#ifndef __IMF_H__
#define __IMF_H__

#include "linked_list.h"

typedef struct {
    int num;
    int denom;
} fraction_t;

typedef struct {
    fraction_t edit_rate;
} cpl_composition_playlist;

// EssenceDescriptor->WavePCMDescriptor
typedef struct {
    char channel_assignment[64];
    unsigned int average_bytes_per_second;
    unsigned int block_align;
    fraction_t reference_image_edit_rate;
    unsigned int channel_count;
    unsigned int quantization_bits;
    //unsigned int locked;
    fraction_t audio_sample_rate;
    char sound_compression[64];
    unsigned int essence_length;
    fraction_t sample_rate;
    char container_format[64];
    char instance_id[64];
} cpl_wave_pcm_descriptor;

typedef struct {
    unsigned int vertical_subsampling;
    //<ns10:ColorSiting>CoSiting</ns10:ColorSiting>
    unsigned int horizontal_subsampling;
    unsigned int color_range;
    unsigned int component_depth;
    unsigned int black_ref_level;
    unsigned int white_ref_level;
    //unsigned int display_height;
    //unsigned int display_x_offset;
    char color_primaries[64];
    unsigned int stored_height;
    //unsigned int active_height;
    //<ns10:VideoLineMap>
    //<ns14:Int32>42</ns14:Int32>
    //<ns14:Int32>0</ns14:Int32>
    //</ns10:VideoLineMap>
    //unsigned int sampled_width;
    //unsigned int sampled_y_offset;
    char coding_equations[64];
    unsigned int active_format_descriptor;
    //unsigned int display_f2_offset;
    char picture_compression[64];
    char frame_layout[32];
   // unsigned int display_y_offset;
    //unsigned int display_width;
    unsigned int stored_width;
    //unsigned int active_width;
    fraction_t image_aspect_ratio;
    //char signal_standard[32];
    //unsigned int sampled_height;
    //unsigned int sampled_x_offset;
    char transfer_characteristic[64];
    unsigned int essence_length;
    fraction_t sample_rate;
    unsigned char container_format;
} cpl_cdci_descriptor;

typedef struct {
} cpl_rgba_descriptor;

typedef struct {
    char id[64];
    fraction_t edit_rate;
    unsigned int intrinsic_duration;
    unsigned int entry_point;
    unsigned int source_duration;
    unsigned int repeat_count;
    char track_file_id[64];
    char source_encoding[64]; // refers to EssenceDescriptor
} cpl_resource_t;

typedef struct {
    char path[512];
    int volume_index;
    int offset;
    int length;
} am_chunk_t;

extern cpl_composition_playlist* cpl_get_composition_playlist(const char *filename);
extern cpl_cdci_descriptor* cpl_get_cdci_descriptor_for_resource(const char *filename, cpl_resource_t *resource);
extern cpl_wave_pcm_descriptor* cpl_get_wave_pcm_descriptor_for_resource(const char *filename, cpl_resource_t *resource);
extern am_chunk_t* am_get_chunk_for_resource(const char *filename, cpl_resource_t *resource);
extern void am_free_chunk(am_chunk_t *chunk);
extern linked_list_t* cpl_get_video_resources(const char *filename);
extern linked_list_t* cpl_get_audio_resources(const char *filename);
extern void cpl_free_resources(linked_list_t *resources);

#endif
