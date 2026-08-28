#ifndef ADVC_VAAPI_ENCODE_RUNTIME_H
#define ADVC_VAAPI_ENCODE_RUNTIME_H

#include "advc_vaapi_encode.h"
#include "advc_vaapi_image.h"

#include <va/va_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_VAAPI_ENCODE_CONFIG_TYPE UINT32_C(0x11000000)
#define ADVC_VAAPI_ENCODE_CONTEXT_TYPE UINT32_C(0x12000000)
#define ADVC_VAAPI_ENCODE_SURFACE_TYPE UINT32_C(0x13000000)
#define ADVC_VAAPI_ENCODE_BUFFER_TYPE UINT32_C(0x14000000)
#define ADVC_VAAPI_OBJECT_TYPE_MASK UINT32_C(0xff000000)

struct advc_vaapi_encode_runtime;

struct advc_vaapi_encode_surface_accounting {
    uint64_t cpu_pixel_copies;
    uint64_t writable_prime_exports;
    uint64_t gpu_conversion_submits;
    uint64_t gpu_modifier_repack_submits;
};

/*
 * Runtime contract used by the vendor driver after the exact
 * ADVC_VAAPI_ENABLE_ENCODE=validated-avc-hevc-v1 gate has been satisfied.
 * Object types intentionally do not overlap the decode or image namespaces.
 * The validated subset is H.264 Constrained Baseline and HEVC Main EncSlice,
 * VBR, NV12/YUV420, one slice, no B pictures, no packed headers, and no
 * EncSliceLP.  Unsupported requests fail before a broker session is opened.
 * Writable PRIME2 export for OBS texture input is separately gated by
 * ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1. EndPicture
 * snapshots the producer's implicit dma-buf fence and forwards it to the
 * broker as an explicit acquire fence.
 */
struct advc_vaapi_encode_runtime *advc_vaapi_encode_runtime_create(
    const char *socket_path, const struct advc_vaapi_encode_policy *policy);
void advc_vaapi_encode_runtime_destroy(
    struct advc_vaapi_encode_runtime *runtime);

int advc_vaapi_encode_owns_config(VAConfigID id);
int advc_vaapi_encode_owns_context(VAContextID id);
int advc_vaapi_encode_owns_surface(VASurfaceID id);
int advc_vaapi_encode_owns_buffer(VABufferID id);
int advc_vaapi_encode_buffer_is_coded(
    struct advc_vaapi_encode_runtime *runtime, VABufferID id);
struct advc_vaapi_image_runtime *advc_vaapi_encode_image_runtime(
    struct advc_vaapi_encode_runtime *runtime);

VAStatus advc_vaapi_encode_create_config(
    struct advc_vaapi_encode_runtime *runtime, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes, int num_attributes,
    VAConfigID *config_id);
VAStatus advc_vaapi_encode_destroy_config(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id);
VAStatus advc_vaapi_encode_query_config(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attributes,
    int *num_attributes);

VAStatus advc_vaapi_encode_create_surfaces(
    struct advc_vaapi_encode_runtime *runtime, unsigned int format,
    unsigned int width, unsigned int height, VASurfaceID *surfaces,
    unsigned int num_surfaces, VASurfaceAttrib *attributes,
    unsigned int num_attributes);
VAStatus advc_vaapi_encode_destroy_surfaces(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID *surfaces,
    int num_surfaces);
VAStatus advc_vaapi_encode_query_surface_attributes(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes);

VAStatus advc_vaapi_encode_create_context(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id, int width,
    int height, int flag, VASurfaceID *targets, int num_targets,
    VAContextID *context_id);
VAStatus advc_vaapi_encode_destroy_context(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id);

VAStatus advc_vaapi_encode_create_buffer(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VABufferType type, unsigned int size, unsigned int num_elements, void *data,
    VABufferID *buffer_id);
VAStatus advc_vaapi_encode_buffer_set_num_elements(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id,
    unsigned int num_elements);
VAStatus advc_vaapi_encode_map_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id,
    void **mapped);
VAStatus advc_vaapi_encode_unmap_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id);
VAStatus advc_vaapi_encode_destroy_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id);

VAStatus advc_vaapi_encode_begin_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VASurfaceID target);
VAStatus advc_vaapi_encode_render_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VABufferID *buffers, int num_buffers);
VAStatus advc_vaapi_encode_end_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id);
VAStatus advc_vaapi_encode_sync_surface(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface,
    uint64_t timeout_ns);
VAStatus advc_vaapi_encode_sync_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer,
    uint64_t timeout_ns);
VAStatus advc_vaapi_encode_query_surface_status(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface,
    VASurfaceStatus *status);
VAStatus advc_vaapi_encode_export_surface(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface,
    uint32_t mem_type, uint32_t flags, void *descriptor);
VAStatus advc_vaapi_encode_query_surface_accounting(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface,
    struct advc_vaapi_encode_surface_accounting *accounting);

#ifdef __cplusplus
}
#endif

#endif
