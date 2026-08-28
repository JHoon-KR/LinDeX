#ifndef ADVC_VAAPI_DECODE_RUNTIME_H
#define ADVC_VAAPI_DECODE_RUNTIME_H

#include "advc_vaapi_decode_eos.h"
#include "advc_vaapi_policy.h"

#include <va/va_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

struct advc_vaapi_decode_runtime;

struct advc_vaapi_decode_runtime *advc_vaapi_decode_runtime_create(
    const char *socket_path, const struct advc_vaapi_policy *policy);
void advc_vaapi_decode_runtime_destroy(
    struct advc_vaapi_decode_runtime *runtime);

VAStatus advc_vaapi_decode_create_config(
    struct advc_vaapi_decode_runtime *runtime, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes, int num_attributes,
    VAConfigID *config_id);
VAStatus advc_vaapi_decode_destroy_config(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id);
VAStatus advc_vaapi_decode_query_config(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attributes,
    int *num_attributes);

VAStatus advc_vaapi_decode_create_surfaces(
    struct advc_vaapi_decode_runtime *runtime, unsigned int format,
    unsigned int width, unsigned int height, VASurfaceID *surfaces,
    unsigned int num_surfaces, VASurfaceAttrib *attributes,
    unsigned int num_attributes);
VAStatus advc_vaapi_decode_destroy_surfaces(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID *surfaces,
    int num_surfaces);
VAStatus advc_vaapi_decode_query_surface_attributes(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes);

VAStatus advc_vaapi_decode_create_context(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id, int width,
    int height, int flag, VASurfaceID *targets, int num_targets,
    VAContextID *context_id);
VAStatus advc_vaapi_decode_destroy_context(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id);

VAStatus advc_vaapi_decode_create_buffer(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VABufferType type, unsigned int size, unsigned int num_elements, void *data,
    VABufferID *buffer_id);
VAStatus advc_vaapi_decode_buffer_set_num_elements(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id,
    unsigned int num_elements);
VAStatus advc_vaapi_decode_map_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id,
    void **mapped);
VAStatus advc_vaapi_decode_unmap_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id);
VAStatus advc_vaapi_decode_destroy_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id);

VAStatus advc_vaapi_decode_begin_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VASurfaceID target);
VAStatus advc_vaapi_decode_render_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VABufferID *buffers, int num_buffers);
VAStatus advc_vaapi_decode_end_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id);
VAStatus advc_vaapi_decode_sync_surface(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface,
    uint64_t timeout_ns);
VAStatus advc_vaapi_decode_query_surface_status(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface,
    VASurfaceStatus *status);
VAStatus advc_vaapi_decode_export_surface(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface,
    uint32_t mem_type, uint32_t flags, void *descriptor);

int32_t advc_vaapi_decode_signal_eos_private(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    struct advc_vaapi_decode_eos_status_v1 *status);
int32_t advc_vaapi_decode_progress_eos_private(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    uint32_t max_outputs,
    struct advc_vaapi_decode_eos_status_v1 *status);

#ifdef __cplusplus
}
#endif

#endif
