#ifndef ANDROID_DRM_BLOB_H
#define ANDROID_DRM_BLOB_H

#include <stddef.h>

/*
 * Returns 1 and transfers a malloc-owned augmented blob through output_data,
 * or returns 0 and leaves both output values cleared. The helper never
 * modifies source_data.
 */
int android_drm_blob_append_xb24_qcom(const void *source_data,
                                      size_t source_length,
                                      void **output_data,
                                      size_t *output_length);

#endif
