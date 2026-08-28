#ifndef ADVC_VAAPI_ENCODE_H
#define ADVC_VAAPI_ENCODE_H

#include "advc/capabilities.h"
#include "advc/client.h"

#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS 8u

enum advc_vaapi_encode_codec {
    ADVC_VAAPI_ENCODE_CODEC_NONE = 0,
    ADVC_VAAPI_ENCODE_CODEC_H264 = 1,
    ADVC_VAAPI_ENCODE_CODEC_HEVC = 2,
};

enum advc_vaapi_encode_codec_bits {
    ADVC_VAAPI_ENCODE_H264 = UINT32_C(1) << 0,
    ADVC_VAAPI_ENCODE_HEVC = UINT32_C(1) << 1,
};

/*
 * This policy describes only the path which has actually been proved by the
 * Android broker: hardware encode with explicit DRM PRIME input.  It does not
 * infer EncSliceLP, B pictures, packed headers, ROI, or multiple slices from a
 * MediaCodec name.
 */
struct advc_vaapi_encode_policy {
    uint32_t codecs;
    /* Zero means the NDK probe did not expose a limit; create is authoritative. */
    uint32_t h264_max_width;
    uint32_t h264_max_height;
    uint32_t hevc_max_width;
    uint32_t hevc_max_height;
    uint32_t rate_control; /* VA_RC_* mask; currently VBR only. */
    uint32_t rt_formats;   /* VA_RT_FORMAT_* mask. */
    int prime_input_ready;
};

struct advc_vaapi_encode_config {
    enum advc_vaapi_encode_codec codec;
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rate_control;
    uint32_t rt_format;
};

struct advc_vaapi_encode_frame_params {
    enum advc_vaapi_encode_codec codec;
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    uint32_t framerate_milli;
    VABufferID coded_buffer;
    uint32_t gop_frames;
    int sequence_seen;
    int picture_seen;
    int slice_seen;
    int force_idr;
};

struct advc_vaapi_encode_coded_output {
    VACodedBufferSegment segments[ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS];
    void *mappings[ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS];
    size_t mapping_sizes[ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS];
    uint32_t count;
    uint64_t pts_ns;
    uint32_t flags;
};

struct advc_vaapi_encode_broker {
    int fd;
    uint32_t session_id;
    uint64_t features;
    struct advc_client_session_config config;
};

int advc_vaapi_encode_policy_from_capabilities(
    const struct advc_capability_set *caps,
    struct advc_vaapi_encode_policy *policy);
int advc_vaapi_encode_profile_supported(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile);
int advc_vaapi_encode_get_attribute(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttribType type, uint32_t *value);
int advc_vaapi_encode_config_init(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile,
    VAEntrypoint entrypoint, const VAConfigAttrib *attributes,
    int num_attributes, struct advc_vaapi_encode_config *config);

void advc_vaapi_encode_frame_init(
    struct advc_vaapi_encode_frame_params *frame,
    enum advc_vaapi_encode_codec codec, uint32_t width, uint32_t height,
    uint32_t default_bitrate, uint32_t default_framerate_milli);
/* Start the next picture while retaining sequence-level parameters. */
void advc_vaapi_encode_frame_begin(
    struct advc_vaapi_encode_frame_params *frame);
int advc_vaapi_encode_frame_consume(
    struct advc_vaapi_encode_frame_params *frame, VABufferType type,
    const void *data, size_t size, unsigned int num_elements);
int advc_vaapi_encode_frame_validate(
    const struct advc_vaapi_encode_frame_params *frame);

/*
 * Thin standard-VA-to-ADVC runtime used by the vendor-driver integration.
 * PRIME descriptors and acquire fences are borrowed.  Registration duplicates
 * object FDs in the broker.  submit returns an owned release sync_file or -1.
 */
int advc_vaapi_encode_broker_open(
    struct advc_vaapi_encode_broker *broker, const char *socket_path,
    enum advc_vaapi_encode_codec codec, uint32_t encode_profile,
    uint32_t width, uint32_t height, uint32_t bitrate,
    uint32_t framerate_milli);
/*
 * Allocate an exportable modifier=0 NV12 dma-buf from Android's system heap.
 * No pixel is mapped or copied by this operation.  The returned descriptor is
 * owned and must be closed with advc_dmabuf_descriptor_close().
 */
int advc_vaapi_encode_surface_allocate_linear(
    uint64_t buffer_id, uint32_t width, uint32_t height,
    struct advc_dmabuf_descriptor *descriptor);
int advc_vaapi_encode_broker_register_surface(
    struct advc_vaapi_encode_broker *broker,
    const struct advc_dmabuf_descriptor *descriptor);
int advc_vaapi_encode_broker_unregister_surface(
    struct advc_vaapi_encode_broker *broker, uint64_t buffer_id);
int advc_vaapi_encode_broker_submit_surface(
    struct advc_vaapi_encode_broker *broker, uint64_t buffer_id,
    uint64_t pts_ns, int acquire_fence_fd, int *release_fence_fd);
int advc_vaapi_encode_broker_signal_eos(
    struct advc_vaapi_encode_broker *broker, uint64_t pts_ns);
int advc_vaapi_encode_broker_receive(
    struct advc_vaapi_encode_broker *broker, uint32_t timeout_ms,
    struct advc_vaapi_encode_coded_output *output);
void advc_vaapi_encode_coded_output_close(
    struct advc_vaapi_encode_coded_output *output);
void advc_vaapi_encode_broker_close(
    struct advc_vaapi_encode_broker *broker);

#ifdef __cplusplus
}
#endif

#endif
