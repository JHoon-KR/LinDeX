#define _GNU_SOURCE

#include "advc_vaapi_encode.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ADVC_VAAPI_DEFAULT_SOCKET "/run/android-drm/advc-broker-1.1.sock"
#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)

#ifndef DMA_HEAP_IOCTL_ALLOC
struct advc_dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC                                                \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct advc_dma_heap_allocation_data)
#endif

static void merge_dimensions(uint32_t *width, uint32_t *height,
                             const struct advc_codec_capability *codec) {
    if (codec->max_width > *width) *width = codec->max_width;
    if (codec->max_height > *height) *height = codec->max_height;
}

static enum advc_vaapi_encode_codec profile_codec(VAProfile profile) {
    switch (profile) {
        case VAProfileH264ConstrainedBaseline:
            return ADVC_VAAPI_ENCODE_CODEC_H264;
        case VAProfileHEVCMain:
            return ADVC_VAAPI_ENCODE_CODEC_HEVC;
        default:
            return ADVC_VAAPI_ENCODE_CODEC_NONE;
    }
}

int advc_vaapi_encode_policy_from_capabilities(
    const struct advc_capability_set *caps,
    struct advc_vaapi_encode_policy *policy) {
    uint32_t i;
    uint64_t required;
    if (caps == NULL || policy == NULL || caps->count > ADVC_MAX_CAPABILITIES) {
        errno = EINVAL;
        return -1;
    }
    memset(policy, 0, sizeof(*policy));
    required = ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
               ADVC_FEATURE_NATIVE_FENCE;
    policy->prime_input_ready =
        (caps->transport_features & required) == required &&
        (caps->transport_features &
         (ADVC_FEATURE_DMABUF_VULKAN | ADVC_FEATURE_DMABUF_EGL)) != 0;
    if (!policy->prime_input_ready) return 0;

    for (i = 0; i < caps->count; ++i) {
        const struct advc_codec_capability *codec = &caps->codecs[i];
        if (codec->direction != ADVC_DIRECTION_ENCODE ||
            codec->acceleration != ADVC_ACCELERATION_HARDWARE)
            continue;
        if (strcmp(codec->mime, "video/avc") == 0) {
            policy->codecs |= ADVC_VAAPI_ENCODE_H264;
            merge_dimensions(&policy->h264_max_width,
                             &policy->h264_max_height, codec);
        } else if (strcmp(codec->mime, "video/hevc") == 0) {
            policy->codecs |= ADVC_VAAPI_ENCODE_HEVC;
            merge_dimensions(&policy->hevc_max_width,
                             &policy->hevc_max_height, codec);
        }
    }
    if (policy->codecs != 0) {
        /* Protocol v1 exposes only VBR; the Android backend selects VBR too. */
        policy->rate_control = VA_RC_VBR;
        policy->rt_formats = VA_RT_FORMAT_YUV420;
    }
    return 0;
}

int advc_vaapi_encode_profile_supported(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile) {
    enum advc_vaapi_encode_codec codec;
    if (policy == NULL || !policy->prime_input_ready) return 0;
    codec = profile_codec(profile);
    if (codec == ADVC_VAAPI_ENCODE_CODEC_H264)
        return (policy->codecs & ADVC_VAAPI_ENCODE_H264) != 0;
    if (codec == ADVC_VAAPI_ENCODE_CODEC_HEVC)
        return (policy->codecs & ADVC_VAAPI_ENCODE_HEVC) != 0;
    return 0;
}

int advc_vaapi_encode_get_attribute(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttribType type, uint32_t *value) {
    if (policy == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    *value = VA_ATTRIB_NOT_SUPPORTED;
    if (entrypoint != VAEntrypointEncSlice ||
        !advc_vaapi_encode_profile_supported(policy, profile)) {
        errno = ENOTSUP;
        return -1;
    }
    switch (type) {
        case VAConfigAttribRTFormat:
            *value = policy->rt_formats;
            return 0;
        case VAConfigAttribRateControl:
            *value = policy->rate_control;
            return 0;
        case VAConfigAttribEncPackedHeaders:
        case VAConfigAttribEncInterlaced:
            *value = 0;
            return 0;
        case VAConfigAttribEncMaxRefFrames:
            /* One forward reference, no B-picture references. */
            *value = 1;
            return 0;
        case VAConfigAttribEncMaxSlices:
            *value = 1;
            return 0;
        default:
            return 0;
    }
}

int advc_vaapi_encode_config_init(
    const struct advc_vaapi_encode_policy *policy, VAProfile profile,
    VAEntrypoint entrypoint, const VAConfigAttrib *attributes,
    int num_attributes, struct advc_vaapi_encode_config *config) {
    uint32_t requested_rt = VA_RT_FORMAT_YUV420;
    uint32_t requested_rc = VA_RC_VBR;
    int i;
    if (policy == NULL || config == NULL || num_attributes < 0 ||
        (num_attributes > 0 && attributes == NULL) ||
        entrypoint != VAEntrypointEncSlice ||
        !advc_vaapi_encode_profile_supported(policy, profile)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < num_attributes; ++i) {
        switch (attributes[i].type) {
            case VAConfigAttribRTFormat:
                requested_rt = attributes[i].value;
                break;
            case VAConfigAttribRateControl:
                requested_rc = attributes[i].value;
                break;
            default: {
                uint32_t supported;
                if (advc_vaapi_encode_get_attribute(
                        policy, profile, entrypoint, attributes[i].type,
                        &supported) < 0 ||
                    supported == VA_ATTRIB_NOT_SUPPORTED ||
                    (attributes[i].value & ~supported) != 0) {
                    errno = ENOTSUP;
                    return -1;
                }
                break;
            }
        }
    }
    if (requested_rt != VA_RT_FORMAT_YUV420 || requested_rc != VA_RC_VBR) {
        errno = ENOTSUP;
        return -1;
    }
    memset(config, 0, sizeof(*config));
    config->codec = profile_codec(profile);
    config->profile = profile;
    config->entrypoint = entrypoint;
    config->rate_control = requested_rc;
    config->rt_format = requested_rt;
    return 0;
}

void advc_vaapi_encode_frame_init(
    struct advc_vaapi_encode_frame_params *frame,
    enum advc_vaapi_encode_codec codec, uint32_t width, uint32_t height,
    uint32_t default_bitrate, uint32_t default_framerate_milli) {
    if (frame == NULL) return;
    memset(frame, 0, sizeof(*frame));
    frame->codec = codec;
    frame->width = width;
    frame->height = height;
    frame->bitrate = default_bitrate;
    frame->framerate_milli = default_framerate_milli;
    frame->coded_buffer = VA_INVALID_ID;
}

void advc_vaapi_encode_frame_begin(
    struct advc_vaapi_encode_frame_params *frame) {
    if (frame == NULL) return;
    frame->coded_buffer = VA_INVALID_ID;
    frame->picture_seen = 0;
    frame->slice_seen = 0;
    frame->force_idr = 0;
}

static int consume_misc(struct advc_vaapi_encode_frame_params *frame,
                        const void *data, size_t size) {
    const VAEncMiscParameterBuffer *misc = data;
    if (size < sizeof(*misc)) return -1;
    switch (misc->type) {
        case VAEncMiscParameterTypeRateControl: {
            const VAEncMiscParameterRateControl *rate;
            if (size < sizeof(*misc) + sizeof(*rate)) return -1;
            rate = (const VAEncMiscParameterRateControl *)misc->data;
            if (rate->bits_per_second == 0 ||
                rate->bits_per_second > ADVC_MAX_ENCODE_BITRATE)
                return -1;
            frame->bitrate = rate->bits_per_second;
            return 0;
        }
        case VAEncMiscParameterTypeFrameRate: {
            const VAEncMiscParameterFrameRate *fps;
            uint32_t numerator;
            uint32_t denominator;
            uint64_t milli;
            if (size < sizeof(*misc) + sizeof(*fps)) return -1;
            fps = (const VAEncMiscParameterFrameRate *)misc->data;
            numerator = fps->framerate & UINT32_C(0xffff);
            denominator = fps->framerate >> 16;
            if (denominator == 0) denominator = 1;
            milli = (uint64_t)numerator * 1000u / denominator;
            if (milli < 1000 || milli > 240000) return -1;
            frame->framerate_milli = (uint32_t)milli;
            return 0;
        }
        case VAEncMiscParameterTypeHRD:
            /* Advisory only: MediaCodec owns its internal VBV. */
            return size >= sizeof(*misc) + sizeof(VAEncMiscParameterHRD) ?
                       0 : -1;
        default:
            return -1;
    }
}

static int consume_h264(struct advc_vaapi_encode_frame_params *frame,
                        VABufferType type, const void *data, size_t size,
                        unsigned int num_elements) {
    if (num_elements != 1) return -1;
    if (type == VAEncSequenceParameterBufferType) {
        const VAEncSequenceParameterBufferH264 *seq = data;
        uint32_t mb_width = (frame->width + 15u) / 16u;
        uint32_t mb_height = (frame->height + 15u) / 16u;
        if (size < sizeof(*seq) || seq->picture_width_in_mbs != mb_width ||
            seq->picture_height_in_mbs != mb_height ||
            seq->seq_fields.bits.chroma_format_idc != 1 ||
            seq->bit_depth_luma_minus8 != 0 ||
            seq->bit_depth_chroma_minus8 != 0 ||
            !seq->seq_fields.bits.frame_mbs_only_flag ||
            seq->seq_fields.bits.mb_adaptive_frame_field_flag ||
            seq->seq_fields.bits.seq_scaling_matrix_present_flag)
            return -1;
        if (seq->bits_per_second != 0) frame->bitrate = seq->bits_per_second;
        frame->gop_frames = seq->intra_idr_period != 0 ?
                                seq->intra_idr_period : seq->intra_period;
        frame->sequence_seen = 1;
        return 0;
    }
    if (type == VAEncPictureParameterBufferType) {
        const VAEncPictureParameterBufferH264 *pic = data;
        if (size < sizeof(*pic) || pic->coded_buf == VA_INVALID_ID ||
            pic->pic_fields.bits.pic_scaling_matrix_present_flag)
            return -1;
        frame->coded_buffer = pic->coded_buf;
        frame->force_idr = pic->pic_fields.bits.idr_pic_flag != 0;
        frame->picture_seen = 1;
        return 0;
    }
    if (type == VAEncSliceParameterBufferType) {
        const VAEncSliceParameterBufferH264 *slice = data;
        uint32_t mb_count = ((frame->width + 15u) / 16u) *
                            ((frame->height + 15u) / 16u);
        uint8_t slice_type;
        if (size < sizeof(*slice)) return -1;
        slice_type = slice->slice_type % 5u;
        if (slice->macroblock_address != 0 ||
            slice->num_macroblocks != mb_count || slice_type == 1 ||
            slice_type > 2)
            return -1;
        frame->slice_seen = 1;
        return 0;
    }
    return -1;
}

static int consume_hevc(struct advc_vaapi_encode_frame_params *frame,
                        VABufferType type, const void *data, size_t size,
                        unsigned int num_elements) {
    if (num_elements != 1) return -1;
    if (type == VAEncSequenceParameterBufferType) {
        const VAEncSequenceParameterBufferHEVC *seq = data;
        if (size < sizeof(*seq) || seq->general_profile_idc != 1 ||
            seq->pic_width_in_luma_samples != frame->width ||
            seq->pic_height_in_luma_samples != frame->height ||
            seq->seq_fields.bits.chroma_format_idc != 1 ||
            seq->seq_fields.bits.separate_colour_plane_flag ||
            seq->seq_fields.bits.bit_depth_luma_minus8 != 0 ||
            seq->seq_fields.bits.bit_depth_chroma_minus8 != 0 ||
            seq->seq_fields.bits.scaling_list_enabled_flag ||
            seq->seq_fields.bits.hierachical_flag)
            return -1;
        if (seq->bits_per_second != 0) frame->bitrate = seq->bits_per_second;
        frame->gop_frames = seq->intra_idr_period != 0 ?
                                seq->intra_idr_period : seq->intra_period;
        frame->sequence_seen = 1;
        return 0;
    }
    if (type == VAEncPictureParameterBufferType) {
        const VAEncPictureParameterBufferHEVC *pic = data;
        if (size < sizeof(*pic) || pic->coded_buf == VA_INVALID_ID ||
            pic->pic_fields.bits.tiles_enabled_flag ||
            pic->pic_fields.bits.scaling_list_data_present_flag ||
            pic->pic_fields.bits.coding_type < 1 ||
            pic->pic_fields.bits.coding_type > 2)
            return -1;
        frame->coded_buffer = pic->coded_buf;
        frame->force_idr = pic->pic_fields.bits.idr_pic_flag != 0;
        frame->picture_seen = 1;
        return 0;
    }
    if (type == VAEncSliceParameterBufferType) {
        const VAEncSliceParameterBufferHEVC *slice = data;
        if (size < sizeof(*slice) || slice->slice_segment_address != 0 ||
            !slice->slice_fields.bits.last_slice_of_pic_flag ||
            slice->slice_type == 0 || slice->slice_type > 2)
            return -1;
        frame->slice_seen = 1;
        return 0;
    }
    return -1;
}

int advc_vaapi_encode_frame_consume(
    struct advc_vaapi_encode_frame_params *frame, VABufferType type,
    const void *data, size_t size, unsigned int num_elements) {
    int status;
    if (frame == NULL || data == NULL || size == 0 || num_elements == 0) {
        errno = EINVAL;
        return -1;
    }
    if (type == VAEncMiscParameterBufferType) {
        if (num_elements != 1 || consume_misc(frame, data, size) < 0) {
            errno = ENOTSUP;
            return -1;
        }
        return 0;
    }
    status = frame->codec == ADVC_VAAPI_ENCODE_CODEC_H264
                 ? consume_h264(frame, type, data, size, num_elements)
                 : frame->codec == ADVC_VAAPI_ENCODE_CODEC_HEVC
                       ? consume_hevc(frame, type, data, size, num_elements)
                       : -1;
    if (status < 0) errno = ENOTSUP;
    return status;
}

int advc_vaapi_encode_frame_validate(
    const struct advc_vaapi_encode_frame_params *frame) {
    if (frame == NULL || frame->width < 16 || frame->height < 16 ||
        (frame->width & 1u) != 0 || (frame->height & 1u) != 0 ||
        frame->bitrate == 0 || frame->bitrate > ADVC_MAX_ENCODE_BITRATE ||
        frame->framerate_milli < 1000 || frame->framerate_milli > 240000 ||
        !frame->sequence_seen || !frame->picture_seen || !frame->slice_seen ||
        frame->coded_buffer == VA_INVALID_ID) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int has_hardware_encoder(const struct advc_capability_set *caps,
                                const char *mime, uint32_t width,
                                uint32_t height, uint32_t fps_milli) {
    uint32_t i;
    if (caps == NULL || mime == NULL) return 0;
    for (i = 0; i < caps->count && i < ADVC_MAX_CAPABILITIES; ++i) {
        const struct advc_codec_capability *codec = &caps->codecs[i];
        if (codec->direction == ADVC_DIRECTION_ENCODE &&
            codec->acceleration == ADVC_ACCELERATION_HARDWARE &&
            strcmp(codec->mime, mime) == 0 &&
            (codec->max_width == 0 || width <= codec->max_width) &&
            (codec->max_height == 0 || height <= codec->max_height) &&
            (codec->max_fps_milli == 0 ||
             fps_milli <= codec->max_fps_milli))
            return 1;
    }
    return 0;
}

static int wait_retry(uint64_t deadline_ms) {
    uint64_t now = monotonic_ms();
    int timeout;
    if (now == UINT64_MAX || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    timeout = (int)((deadline_ms - now) > 2 ? 2 : deadline_ms - now);
    while (poll(NULL, 0, timeout) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

int advc_vaapi_encode_broker_open(
    struct advc_vaapi_encode_broker *broker, const char *socket_path,
    enum advc_vaapi_encode_codec codec, uint32_t encode_profile,
    uint32_t width, uint32_t height, uint32_t bitrate,
    uint32_t framerate_milli) {
    struct advc_capability_set caps;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    uint32_t detail = 0;
    const char *mime;
    int status;
    if (broker == NULL || width < 16 || height < 16 || (width & 1u) != 0 ||
        (height & 1u) != 0 || bitrate == 0 ||
        bitrate > ADVC_MAX_ENCODE_BITRATE || framerate_milli < 1000 ||
        framerate_milli > 240000 ||
        (codec == ADVC_VAAPI_ENCODE_CODEC_H264 &&
         encode_profile != ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE) ||
        (codec == ADVC_VAAPI_ENCODE_CODEC_HEVC &&
         encode_profile != ADVC_ENCODE_PROFILE_HEVC_MAIN)) {
        errno = EINVAL;
        return -1;
    }
    mime = codec == ADVC_VAAPI_ENCODE_CODEC_H264 ? "video/avc" :
           codec == ADVC_VAAPI_ENCODE_CODEC_HEVC ? "video/hevc" : NULL;
    if (mime == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    memset(broker, 0, sizeof(*broker));
    broker->fd = -1;
    if (socket_path == NULL || socket_path[0] == '\0')
        socket_path = ADVC_VAAPI_DEFAULT_SOCKET;
    broker->fd = advc_client_connect_bounded(
        socket_path, ADVC_CLIENT_CONNECT_TIMEOUT_MS);
    if (broker->fd < 0) return -1;
    if (advc_client_hello(broker->fd,
                          ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
                              ADVC_FEATURE_NATIVE_FENCE |
                              ADVC_FEATURE_DMABUF_EGL |
                              ADVC_FEATURE_DMABUF_VULKAN |
                              ADVC_FEATURE_ENCODE_QCOM_MODIFIER,
                          &features, &max_payload) < 0 ||
        max_payload < ADVC_REGISTER_DMABUF_SIZE ||
        (features & (ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
                     ADVC_FEATURE_NATIVE_FENCE)) !=
            (ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
             ADVC_FEATURE_NATIVE_FENCE) ||
        (features & (ADVC_FEATURE_DMABUF_EGL |
                     ADVC_FEATURE_DMABUF_VULKAN)) == 0 ||
        advc_client_query_capabilities(broker->fd, &caps) < 0 ||
        !has_hardware_encoder(&caps, mime, width, height, framerate_milli))
        goto fail;

    broker->features = features;

    memset(&broker->config, 0, sizeof(broker->config));
    broker->config.mime = mime;
    broker->config.direction = ADVC_DIRECTION_ENCODE;
    broker->config.width = width;
    broker->config.height = height;
    broker->config.bitrate = bitrate;
    broker->config.framerate_milli = framerate_milli;
    broker->config.transport = ADVC_TRANSPORT_DMABUF;
    broker->config.encode_profile = encode_profile;
    status = advc_client_create_session(broker->fd, &broker->config,
                                        &broker->session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        errno = status < 0 ? errno : ENOTSUP;
        goto fail;
    }
    return 0;
fail:
    advc_vaapi_encode_broker_close(broker);
    return -1;
}

static int allocate_system_heap(uint64_t size) {
    static const char *const heaps[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/system-uncached",
    };
    size_t i;
    for (i = 0; i < sizeof(heaps) / sizeof(heaps[0]); ++i) {
        struct advc_dma_heap_allocation_data request;
        int heap = open(heaps[i], O_RDONLY | O_CLOEXEC);
        int saved;
        if (heap < 0) continue;
        memset(&request, 0, sizeof(request));
        request.len = size;
        request.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request) == 0) {
            close(heap);
            return (int)request.fd;
        }
        saved = errno;
        close(heap);
        errno = saved;
    }
    errno = ENODEV;
    return -1;
}

int advc_vaapi_encode_surface_allocate_linear(
    uint64_t buffer_id, uint32_t width, uint32_t height,
    struct advc_dmabuf_descriptor *descriptor) {
    uint64_t pitch;
    uint64_t slice_height;
    uint64_t y_size;
    uint64_t allocation_size;
    int fd;
    size_t i;
    if (descriptor == NULL || buffer_id == 0 || width < 16 || height < 16 ||
        width > 8192 || height > 8192 || (width & 1u) != 0 ||
        (height & 1u) != 0) {
        errno = EINVAL;
        return -1;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    pitch = ((uint64_t)width + 63u) & ~UINT64_C(63);
    slice_height = ((uint64_t)height + 31u) & ~UINT64_C(31);
    if (pitch > UINT32_MAX || pitch > UINT64_MAX / slice_height) {
        errno = EOVERFLOW;
        return -1;
    }
    y_size = pitch * slice_height;
    if (y_size > UINT64_MAX - y_size / 2u) {
        errno = EOVERFLOW;
        return -1;
    }
    allocation_size = y_size + y_size / 2u;
    fd = allocate_system_heap(allocation_size);
    if (fd < 0) return -1;
    descriptor->buffer_id = buffer_id;
    descriptor->width = width;
    descriptor->height = height;
    descriptor->drm_fourcc = ADVC_DRM_FORMAT_NV12;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = 0;
    descriptor->crop_width = width;
    descriptor->crop_height = height;
    descriptor->object_count = 1;
    descriptor->plane_count = 2;
    descriptor->color_primaries = ADVC_COLOR_PRIMARIES_BT709;
    descriptor->color_transfer = ADVC_COLOR_TRANSFER_BT709;
    descriptor->color_matrix = ADVC_COLOR_MATRIX_BT709;
    descriptor->color_range = ADVC_COLOR_RANGE_LIMITED;
    descriptor->chroma_horizontal = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->chroma_vertical = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = allocation_size;
    descriptor->planes[0].object_index = 0;
    descriptor->planes[0].pitch = (uint32_t)pitch;
    descriptor->planes[1].object_index = 0;
    descriptor->planes[1].offset = y_size;
    descriptor->planes[1].pitch = (uint32_t)pitch;
    if (advc_dmabuf_descriptor_validate(descriptor) < 0) {
        int saved = errno;
        advc_dmabuf_descriptor_close(descriptor);
        errno = saved;
        return -1;
    }
    return 0;
}

int advc_vaapi_encode_broker_register_surface(
    struct advc_vaapi_encode_broker *broker,
    const struct advc_dmabuf_descriptor *descriptor) {
    uint32_t detail = 0;
    int status;
    if (broker == NULL || broker->fd < 0 || broker->session_id == 0 ||
        descriptor == NULL || descriptor->buffer_id == 0 ||
        descriptor->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        descriptor->width != broker->config.width ||
        descriptor->height != broker->config.height) {
        errno = EINVAL;
        return -1;
    }
    status = advc_client_register_dmabuf(broker->fd, broker->session_id,
                                         descriptor, &detail);
    if (status != ADVC_STATUS_OK) {
        if (status >= 0) errno = status == ADVC_STATUS_UNSUPPORTED ?
                                      ENOTSUP : EIO;
        return -1;
    }
    return 0;
}

int advc_vaapi_encode_broker_unregister_surface(
    struct advc_vaapi_encode_broker *broker, uint64_t buffer_id) {
    uint32_t detail = 0;
    int status;
    if (broker == NULL || broker->fd < 0 || broker->session_id == 0 ||
        buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    status = advc_client_unregister_dmabuf(broker->fd, broker->session_id,
                                           buffer_id, &detail);
    if (status != ADVC_STATUS_OK) {
        if (status >= 0) errno = EBUSY;
        return -1;
    }
    return 0;
}

int advc_vaapi_encode_broker_submit_surface(
    struct advc_vaapi_encode_broker *broker, uint64_t buffer_id,
    uint64_t pts_ns, int acquire_fence_fd, int *release_fence_fd) {
    struct advc_dmabuf_submission submission;
    uint32_t detail = 0;
    int status;
    if (release_fence_fd == NULL || broker == NULL || broker->fd < 0 ||
        broker->session_id == 0 || buffer_id == 0 || acquire_fence_fd < -1) {
        errno = EINVAL;
        return -1;
    }
    *release_fence_fd = -1;
    memset(&submission, 0, sizeof(submission));
    submission.buffer_id = buffer_id;
    submission.pts_ns = pts_ns;
    submission.acquire_fence_fd = acquire_fence_fd;
    status = advc_client_queue_dmabuf(broker->fd, broker->session_id,
                                      &submission, &detail);
    if (status != ADVC_STATUS_OK) {
        if (status >= 0) errno = status == ADVC_STATUS_WOULD_BLOCK ?
                                      EAGAIN : EIO;
        return -1;
    }
    status = advc_client_complete_dmabuf(broker->fd, broker->session_id,
                                         buffer_id, release_fence_fd, &detail);
    if (status != ADVC_STATUS_OK) {
        if (*release_fence_fd >= 0) close(*release_fence_fd);
        *release_fence_fd = -1;
        if (status >= 0) errno = status == ADVC_STATUS_WOULD_BLOCK ?
                                      EAGAIN : EIO;
        return -1;
    }
    return 0;
}

int advc_vaapi_encode_broker_signal_eos(
    struct advc_vaapi_encode_broker *broker, uint64_t pts_ns) {
    struct advc_client_input input;
    uint32_t detail = 0;
    int status;
    if (broker == NULL || broker->fd < 0 || broker->session_id == 0 ||
        pts_ns > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    memset(&input, 0, sizeof(input));
    input.data_fd = -1;
    input.pts_ns = pts_ns;
    input.flags = ADVC_FLAG_END_OF_STREAM;
    status = advc_client_queue_input(broker->fd, broker->session_id, &input,
                                     &detail);
    if (status != ADVC_STATUS_OK) {
        if (status >= 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int append_output(struct advc_vaapi_encode_broker *broker,
                         struct advc_vaapi_encode_coded_output *coded,
                         struct advc_client_output *part) {
    VACodedBufferSegment *segment;
    void *mapping = NULL;
    uint64_t buffer_id = part->buffer_id;
    uint32_t detail = 0;
    int status;
    if (coded->count >= ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS ||
        part->transport != ADVC_TRANSPORT_BYTES || part->size > UINT32_MAX ||
        (part->size > 0 && part->data_fd < 0)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (part->size > 0) {
        /* The broker seals coded-output memfds before transfer.  The VA
         * client only reads them, so a private read-only mapping is the
         * exact contract and also avoids Android SELinux rejecting a
         * cross-domain MAP_SHARED mapping of the received memfd. */
        mapping = mmap(NULL, (size_t)part->size, PROT_READ, MAP_PRIVATE,
                       part->data_fd, 0);
        if (mapping == MAP_FAILED) {
            if (getenv("ADVC_VAAPI_TRACE") != NULL)
                fprintf(stderr,
                        "advc-vaapi-encode: coded-mmap size=%llu fd=%d "
                        "errno=%d (%s)\n",
                        (unsigned long long)part->size, part->data_fd, errno,
                        strerror(errno));
            return -1;
        }
    }
    status = advc_client_release_output(broker->fd, broker->session_id,
                                        buffer_id, &detail);
    if (status != ADVC_STATUS_OK) {
        if (mapping != NULL) munmap(mapping, (size_t)part->size);
        if (status >= 0) errno = EIO;
        if (getenv("ADVC_VAAPI_TRACE") != NULL &&
            status != ADVC_STATUS_WOULD_BLOCK)
            fprintf(stderr,
                    "advc-vaapi-encode: coded-release status=%d errno=%d "
                    "(%s)\n",
                    status, errno, strerror(errno));
        return -1;
    }
    segment = &coded->segments[coded->count];
    memset(segment, 0, sizeof(*segment));
    segment->size = (uint32_t)part->size;
    segment->buf = mapping;
    segment->status = 0;
    if (coded->count > 0)
        coded->segments[coded->count - 1].next = segment;
    coded->mappings[coded->count] = mapping;
    coded->mapping_sizes[coded->count] = (size_t)part->size;
    ++coded->count;
    coded->pts_ns = part->pts_ns;
    coded->flags |= part->flags;
    return 0;
}

int advc_vaapi_encode_broker_receive(
    struct advc_vaapi_encode_broker *broker, uint32_t timeout_ms,
    struct advc_vaapi_encode_coded_output *output) {
    uint64_t deadline;
    if (broker == NULL || output == NULL || broker->fd < 0 ||
        broker->session_id == 0 || timeout_ms > 60000) {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    deadline = monotonic_ms();
    if (deadline == UINT64_MAX || UINT64_MAX - deadline < timeout_ms) {
        errno = EOVERFLOW;
        return -1;
    }
    deadline += timeout_ms;
    for (;;) {
        struct advc_client_output part;
        uint32_t detail = 0;
        int status = advc_client_dequeue_output(
            broker->fd, broker->session_id, &part, &detail);
        if (getenv("ADVC_VAAPI_TRACE") != NULL &&
            status != ADVC_STATUS_WOULD_BLOCK)
            fprintf(stderr,
                    "advc-vaapi-encode: dequeue status=%d detail=%u "
                    "transport=%u size=%llu flags=0x%x pts=%llu errno=%d\n",
                    status, detail, part.transport,
                    (unsigned long long)part.size, part.flags,
                    (unsigned long long)part.pts_ns, errno);
        if (status == ADVC_STATUS_WOULD_BLOCK) {
            if (timeout_ms == 0) {
                errno = ETIMEDOUT;
                goto fail;
            }
            if (wait_retry(deadline) < 0) goto fail;
            continue;
        }
        if (status != ADVC_STATUS_OK) {
            if (status >= 0) errno = EIO;
            goto fail;
        }
        if (append_output(broker, output, &part) < 0) {
            advc_client_output_close(&part);
            goto fail;
        }
        {
            int have_frame = part.size > 0 &&
                             (part.flags & ADVC_FLAG_CODEC_CONFIG) == 0;
            int eos = (part.flags & ADVC_FLAG_END_OF_STREAM) != 0;
            advc_client_output_close(&part);
            if (have_frame || eos) return 0;
        }
    }
fail:
    advc_vaapi_encode_coded_output_close(output);
    return -1;
}

void advc_vaapi_encode_coded_output_close(
    struct advc_vaapi_encode_coded_output *output) {
    uint32_t i;
    if (output == NULL) return;
    for (i = 0; i < output->count &&
                i < ADVC_VAAPI_ENCODE_MAX_CODED_SEGMENTS; ++i) {
        if (output->mappings[i] != NULL && output->mapping_sizes[i] > 0)
            munmap(output->mappings[i], output->mapping_sizes[i]);
    }
    memset(output, 0, sizeof(*output));
}

void advc_vaapi_encode_broker_close(
    struct advc_vaapi_encode_broker *broker) {
    uint32_t detail = 0;
    if (broker == NULL) return;
    if (broker->fd >= 0 && broker->session_id != 0)
        (void)advc_client_close_session(broker->fd, broker->session_id,
                                        &detail);
    if (broker->fd >= 0) close(broker->fd);
    memset(broker, 0, sizeof(*broker));
    broker->fd = -1;
}
