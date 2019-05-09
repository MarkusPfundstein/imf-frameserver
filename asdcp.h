#ifndef ASDCP_H
#define ASDCP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*asdcp_on_j2k_frame_func)(unsigned char *data, unsigned int length, unsigned int frame_count, void *user_data);

extern int asdcp_read_mxf(const char *filename, void *user_data, asdcp_on_j2k_frame_func on_frame);

#ifdef __cplusplus
}
#endif

#endif
