#include <signal.h>
#include <openjpeg-2.3/openjpeg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "av_pipeline.h"
#include "asdcp.h"
#include "imf.h"

void SIGINT_handler(int dummy) {
    fprintf(stderr, "got signal\n");
    stop_decoding_signal();
    signal(SIGINT, 0);
}

static void free_asset(asset_t *asset) {
    if (asset) {
        if (asset->essence_descriptor) {
            free(asset->essence_descriptor);
        }
        free(asset);
    }
}

typedef struct {
    linked_list_t *video_assets;
    linked_list_t *audio_assets;
} decoding_assets_t;

int get_audio_assets(const char *cpl_path, const char *assetmap_path, decoding_assets_t *decoding_assets) {
    int err = 0;
    linked_list_t *resources = cpl_get_audio_resources(cpl_path);

    for (linked_list_t *head = resources; head && !err; head = head->next) {
        cpl_resource_t *cpl_res = head->user_data;
        am_chunk_t *chunk = am_get_chunk_for_resource(assetmap_path, cpl_res);
        if (!chunk) {
            fprintf(stderr, "error resolving asset %s for resource %s\n", cpl_res->track_file_id, cpl_res->id);
            err = 1;
        } else {

            cpl_wave_pcm_descriptor *wave_pcm_desc = cpl_get_wave_pcm_descriptor_for_resource(cpl_path, cpl_res);
            if (!wave_pcm_desc) {
                fprintf(stderr, "error resolving essence descriptor [%s] for resource %s [%s]\n", cpl_res->source_encoding, cpl_res->id, chunk->path);
                err = 1;
            } else {
                for (int i = 0; i < cpl_res->repeat_count; ++i) {
                    asset_t* asset = (asset_t*)malloc(sizeof(asset_t));
                    memset(asset, 0, sizeof(asset_t));
                    asset->asset_type = ASSET_TYPE_AUDIO;
                    asset->essence_descriptor = wave_pcm_desc;
                    strcpy(asset->mxf_path, chunk->path);
                    asset->start_frame = cpl_res->entry_point;
                    asset->end_frame = cpl_res->source_duration;

                    decoding_assets->audio_assets = ll_append(decoding_assets->audio_assets, asset);
                }
            }
        }
        am_free_chunk(chunk);
    }

    cpl_free_resources(resources);
    return err;
}

int get_video_assets(const char *cpl_path, const char *assetmap_path, decoding_assets_t *decoding_assets) {
    int err = 0;
    linked_list_t *resources = cpl_get_video_resources(cpl_path);

    for (linked_list_t *head = resources; head && !err; head = head->next) {
        cpl_resource_t *cpl_res = head->user_data;
        am_chunk_t *chunk = am_get_chunk_for_resource(assetmap_path, cpl_res);
        if (!chunk) {
            fprintf(stderr, "error resolving asset %s for resource %s\n", cpl_res->track_file_id, cpl_res->id);
            err = 1;
        } else {
            cpl_cdci_descriptor *cdci_desc = cpl_get_cdci_descriptor_for_resource(cpl_path, cpl_res);
            if (!cdci_desc) {
                // try to get rgba
                fprintf(stderr, "error resolving essence descriptor [%s] for resource %s [%s]\n", cpl_res->source_encoding, cpl_res->id, chunk->path);
                err = 1;
            } else {
                for (int i = 0; i < cpl_res->repeat_count; ++i) {
                    asset_t* asset = (asset_t*)malloc(sizeof(asset_t));
                    memset(asset, 0, sizeof(asset_t));
                    asset->asset_type = ASSET_TYPE_PICTURE;
                    if (cdci_desc) {
                        asset->picture_type = PICTURE_TYPE_CDCI;
                        asset->essence_descriptor = cdci_desc;
                    } else {
                        asset->picture_type = PICTURE_TYPE_RGBA;
                        //asset->essence_descriptor = rgba_desc;
                    }
                    strcpy(asset->mxf_path, chunk->path);
                    asset->start_frame = cpl_res->entry_point;
                    asset->end_frame = cpl_res->source_duration;

                    decoding_assets->video_assets = ll_append(decoding_assets->video_assets, asset);
                }
            }
        }
        am_free_chunk(chunk);
    }

    cpl_free_resources(resources);
    return err;
}

int main(int argc, char **argv) {

    int err;
    if (argc != 3) {
        fprintf(stderr, "no cpl and assetmap");
        return 1;
    }

    signal(SIGINT, SIGINT_handler);

    av_pipeline_context_t av_context;
    memset(&av_context, 0, sizeof(av_pipeline_context_t));
    av_context.num_threads = opj_get_num_cpus() - 2; 
    av_context.print_debug = 0;
    av_context.decode_frame_buffer_size = 50;

    decoding_assets_t decoding_assets;
    memset(&decoding_assets, 0, sizeof(decoding_assets_t));

    cpl_composition_playlist* cpl = cpl_get_composition_playlist(argv[1]);
    if (!cpl) {
        fprintf(stderr, "couldn't get cpl from %s\n", argv[1]);
        return 1;
    }

    err = get_video_assets(argv[1], argv[2], &decoding_assets);
    if (err) {
        fprintf(stderr, "error getting video assets from CPL\n");
        return 1;
    }
    err = get_audio_assets(argv[1], argv[2], &decoding_assets);
    if (err) {
        fprintf(stderr, "error getting audio assets from CPL\n");
        return 1;
    }

    fprintf(stderr, "loaded CPL:\n");
    fprintf(stderr, "\tEditRate:\t\t%d/%d\n", cpl->edit_rate.num, cpl->edit_rate.denom);

    fprintf(stderr, "loaded resources:\n");
    fprintf(stderr, "VIDEO\n");
    for (linked_list_t *i = decoding_assets.video_assets; i; i = i->next) {
        asset_t *asset = i->user_data;
        fprintf(stderr, "\t%s\n", asset->mxf_path);
        if (asset->picture_type == PICTURE_TYPE_CDCI) {
            cpl_cdci_descriptor *desc = asset->essence_descriptor;
            fprintf(stderr, "\t\tCDCI\n");
            fprintf(stderr, "\t\tVertical Subsampling\t\t%d\n", desc->vertical_subsampling);
            fprintf(stderr, "\t\tHorizontal Subsampling\t\t%d\n", desc->horizontal_subsampling);
            fprintf(stderr, "\t\tStoredWidth\t\t\t%d\n", desc->stored_width);
            fprintf(stderr, "\t\tStoredHeight\t\t\t%d\n", desc->stored_height);
            fprintf(stderr, "\t\tSampleRate\t\t\t%d/%d\n", desc->sample_rate.num, desc->sample_rate.denom);
        }
    }
    fprintf(stderr, "AUDIO\n");
    for (linked_list_t *i = decoding_assets.audio_assets; i; i = i->next) {
        asset_t *asset = i->user_data;
        fprintf(stderr, "\t%s\n", asset->mxf_path);
        cpl_wave_pcm_descriptor *desc = asset->essence_descriptor;
        fprintf(stderr, "\t\tWAVE_PCM\n");
        fprintf(stderr, "\t\tAverageBytesPerSecond\t\t%d\n", desc->average_bytes_per_second);
        fprintf(stderr, "\t\tBlockAlign\t\t\t%d\n", desc->block_align);
        fprintf(stderr, "\t\tChannelCount\t\t\t%d\n", desc->channel_count);
        fprintf(stderr, "\t\tQuantizationBits\t\t%d\n", desc->quantization_bits);
        fprintf(stderr, "\t\tSampleRate\t\t\t%d/%d\n", desc->sample_rate.num, desc->sample_rate.denom);

    }

    av_context.cpl = cpl;

    err = av_pipeline_run(decoding_assets.video_assets, decoding_assets.audio_assets, &av_context);

    ll_free(decoding_assets.video_assets, (free_user_data_func_t)free_asset);
    ll_free(decoding_assets.audio_assets, (free_user_data_func_t)free_asset);
    free(cpl);

    fprintf(stderr, "shutdown imf-fs - bye bye \n");

    return !err;
}
