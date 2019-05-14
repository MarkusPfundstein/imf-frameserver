#include <signal.h>
#include <openjpeg-2.3/openjpeg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "mxf_decode.h"
#include "asdcp.h"
#include "imf.h"

void SIGINT_handler(int dummy) {
    fprintf(stderr, "got signal\n");
    stop_decoding_signal();
    signal(SIGINT, 0);
}

static void free_asset(asset_t *asset) {
    if (asset) {
        free(asset);
    } }

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
            for (int i = 0; i < cpl_res->repeat_count; ++i) {
                asset_t* asset = (asset_t*)malloc(sizeof(asset_t));
                memset(asset, 0, sizeof(asset_t));
                strcpy(asset->mxf_path, chunk->path);
                asset->start_frame = cpl_res->entry_point;
                asset->end_frame = cpl_res->source_duration;

                decoding_assets->audio_assets = ll_append(decoding_assets->audio_assets, asset);
            }
        }
        am_free_chunk(chunk);
    }

    cpl_free_resources(resources);
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
            for (int i = 0; i < cpl_res->repeat_count; ++i) {
                asset_t* asset = (asset_t*)malloc(sizeof(asset_t));
                memset(asset, 0, sizeof(asset_t));
                strcpy(asset->mxf_path, chunk->path);
                asset->start_frame = cpl_res->entry_point;
                asset->end_frame = cpl_res->source_duration;

                decoding_assets->video_assets = ll_append(decoding_assets->video_assets, asset);
            }
        }
        am_free_chunk(chunk);
    }

    cpl_free_resources(resources);
}

int main(int argc, char **argv) {

    int err;
    if (argc != 3) {
        fprintf(stderr, "no cpl and assetmap");
        return 1;
    }

    const char *video_fifo_path = "/tmp/imf-fs-rgb444.fifo";
    const char *audio_fifo_path = "/tmp/imf-fs-pcm.fifo";

    if (access(video_fifo_path, F_OK) == -1) {
        fprintf(stderr, "create fifo %s\n", video_fifo_path);
        err = mkfifo(video_fifo_path, 0666);
        if (err) {
            fprintf(stderr, "error making %s\n", video_fifo_path);
            return 1;
        }
    }
    if (access(audio_fifo_path, F_OK) == -1) {
        fprintf(stderr, "create fifo %s\n", audio_fifo_path);
        err = mkfifo(audio_fifo_path, 0666);
        if (err) {
            fprintf(stderr, "error making %s\n", audio_fifo_path);
            return 1;
        }
    }

    int video_fd = open(video_fifo_path, O_WRONLY);
    if (video_fd < 0) {
        fprintf(stderr, "error opening %s\n", video_fifo_path);
        return 1;
    }
    int audio_fd = open(audio_fifo_path, O_WRONLY);
    if (audio_fd < 0) {
        fprintf(stderr, "error opening %s\n", audio_fifo_path);
        return 1;
    }

    signal(SIGINT, SIGINT_handler);

    decoding_parameters_t parameters;
    memset(&parameters, 0, sizeof(decoding_parameters_t));
    parameters.num_threads = opj_get_num_cpus() - 2; 
    parameters.print_debug = 0;
    parameters.video_out_fd = video_fd;
    parameters.audio_out_fd = audio_fd;
    parameters.decode_frame_buffer_size = 50;

    decoding_assets_t decoding_assets;
    memset(&decoding_assets, 0, sizeof(decoding_assets_t));

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

    fprintf(stderr, "loaded resources:\n");
    fprintf(stderr, "VIDEO\n");
    for (linked_list_t *i = decoding_assets.video_assets; i; i = i->next) {
        fprintf(stderr, "\t%s\n", ((asset_t*)i->user_data)->mxf_path);
    }
    fprintf(stderr, "AUDIO\n");
    for (linked_list_t *i = decoding_assets.audio_assets; i; i = i->next) {
        fprintf(stderr, "\t%s\n", ((asset_t*)i->user_data)->mxf_path);
    }

    err = mxf_decode_files(
            decoding_assets.video_assets,
            decoding_assets.audio_assets,
            &parameters);

    ll_free(decoding_assets.video_assets, (free_user_data_func_t)free_asset);
    ll_free(decoding_assets.audio_assets, (free_user_data_func_t)free_asset);
    
    close(audio_fd);
    close(video_fd);

    return !err;
}
