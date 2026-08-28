#include "android_prime_mapper.h"
#include "stable_mapper_metadata.h"
#include "stable_mapper_v5_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <media/NdkImage.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct android_prime_mapper {
    void *binder_library;
    void *android_library;
    void *mapper_library;
    struct advc_ai_mapper *mapper;
    advc_ahb_get_native_handle_fn get_native_handle;
};

static _Thread_local const char *last_status = "not-probed";
static _Thread_local uint32_t last_transport_fds;
static _Thread_local uint32_t last_transport_ints;
static _Thread_local struct advc_android_prime_diagnostics last_diagnostics;

const char *advc_android_prime_mapper_last_status(void) {
    return last_status;
}

uint32_t advc_android_prime_mapper_last_transport_fds(void) {
    return last_transport_fds;
}

uint32_t advc_android_prime_mapper_last_transport_ints(void) {
    return last_transport_ints;
}

void advc_android_prime_mapper_last_diagnostics(
    struct advc_android_prime_diagnostics *diagnostics) {
    if (diagnostics != NULL) *diagnostics = last_diagnostics;
}

static int query_metadata(struct advc_ai_mapper *mapper,
                          advc_buffer_handle handle, int64_t type,
                          uint8_t **data, size_t *size) {
    int32_t required;
    int32_t written;
    uint8_t *allocated;
    if (mapper == NULL || handle == NULL || data == NULL || size == NULL) {
        errno = EINVAL;
        return -1;
    }
    *data = NULL;
    *size = 0;
    required = mapper->v5.get_standard_metadata(handle, type, NULL, 0);
    if (required <= 0 || required > 1024 * 1024) {
        errno = required < 0 ? ENOTSUP : EOVERFLOW;
        return -1;
    }
    allocated = (uint8_t *)malloc((size_t)required);
    if (allocated == NULL) return -1;
    written = mapper->v5.get_standard_metadata(handle, type, allocated,
                                               (size_t)required);
    if (written != required) {
        free(allocated);
        errno = written < 0 ? ENOTSUP : EPROTO;
        return -1;
    }
    *data = allocated;
    *size = (size_t)required;
    return 0;
}

static int query_u32(struct advc_ai_mapper *mapper, advc_buffer_handle handle,
                     int64_t type, uint32_t *value) {
    uint8_t *data = NULL;
    size_t size = 0;
    int result;
    if (query_metadata(mapper, handle, type, &data, &size) < 0) return -1;
    result = advc_mapper_metadata_decode_u32(data, size, type, value);
    free(data);
    return result;
}

static int query_u64(struct advc_ai_mapper *mapper, advc_buffer_handle handle,
                     int64_t type, uint64_t *value) {
    uint8_t *data = NULL;
    size_t size = 0;
    int result;
    if (query_metadata(mapper, handle, type, &data, &size) < 0) return -1;
    result = advc_mapper_metadata_decode_u64(data, size, type, value);
    free(data);
    return result;
}

static int query_layouts(struct advc_ai_mapper *mapper,
                         advc_buffer_handle handle,
                         struct advc_mapper_plane_layouts *layouts) {
    uint8_t *data = NULL;
    size_t size = 0;
    int result;
    if (query_metadata(mapper, handle, ADVC_STANDARD_METADATA_PLANE_LAYOUTS,
                       &data, &size) < 0)
        return -1;
    result = advc_mapper_metadata_decode_plane_layouts(data, size, layouts);
    free(data);
    return result;
}

static int query_crop(struct advc_ai_mapper *mapper, advc_buffer_handle handle,
                      struct advc_mapper_crops *crops) {
    uint8_t *data = NULL;
    size_t size = 0;
    int result;
    if (query_metadata(mapper, handle, ADVC_STANDARD_METADATA_CROP,
                       &data, &size) < 0)
        return -1;
    result = advc_mapper_metadata_decode_crops(data, size, crops);
    free(data);
    return result;
}

static int query_vendor_i32(struct advc_ai_mapper *mapper,
                            advc_buffer_handle handle, const char *name,
                            int64_t type, int32_t *value) {
    struct advc_ai_mapper_metadata_type metadata_type;
    uint8_t *data = NULL;
    size_t size;
    size_t name_size;
    size_t offset;
    int32_t required;
    int64_t encoded_name_size;
    int64_t encoded_type;

    if (mapper == NULL || handle == NULL || name == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    metadata_type.name = name;
    metadata_type.value = type;
    required = mapper->v5.get_metadata(handle, metadata_type, NULL, 0);
    if (required <= 0 || required > 1024 * 1024) {
        errno = required < 0 ? ENOTSUP : EOVERFLOW;
        return -1;
    }
    size = (size_t)required;
    data = (uint8_t *)malloc(size);
    if (data == NULL) return -1;
    if (mapper->v5.get_metadata(handle, metadata_type, data, size) != required) {
        free(data);
        errno = EPROTO;
        return -1;
    }
    /* Vendor metadata owns its encoding. Current QTI Mapper5 returns raw i32. */
    if (size == sizeof(*value)) {
        memcpy(value, data, sizeof(*value));
        free(data);
        return 0;
    }
    if (size < sizeof(encoded_name_size) + sizeof(encoded_type) +
                   sizeof(*value)) {
        free(data);
        errno = EPROTO;
        return -1;
    }
    memcpy(&encoded_name_size, data, sizeof(encoded_name_size));
    name_size = strlen(name);
    offset = sizeof(encoded_name_size);
    if (encoded_name_size < 0 || (uint64_t)encoded_name_size > SIZE_MAX ||
        (size_t)encoded_name_size != name_size ||
        offset + name_size + sizeof(encoded_type) + sizeof(*value) != size ||
        memcmp(data + offset, name, name_size) != 0) {
        free(data);
        errno = EPROTO;
        return -1;
    }
    offset += name_size;
    memcpy(&encoded_type, data + offset, sizeof(encoded_type));
    offset += sizeof(encoded_type);
    if (encoded_type != type) {
        free(data);
        errno = EPROTO;
        return -1;
    }
    memcpy(value, data + offset, sizeof(*value));
    free(data);
    return 0;
}

static int android_export_prime(
    void *userdata, void *hardware_buffer,
    const struct advc_ahb_public_metadata *public_metadata,
    struct advc_dmabuf_descriptor *descriptor) {
    struct android_prime_mapper *provider =
        (struct android_prime_mapper *)userdata;
    const struct advc_native_handle *raw_handle;
    advc_buffer_handle imported = NULL;
    struct advc_mapper_plane_layouts layouts;
    struct advc_mapper_crops crops;
    struct advc_mapper_qti_nv12_layout qti_layout;
    uint64_t allocation_size = 0;
    uint64_t mapper_width = 0;
    uint64_t mapper_height = 0;
    uint64_t mapper_layers = 0;
    uint64_t modifier = UINT64_MAX;
    uint32_t fourcc = 0;
    uint32_t mapper_stride = 0;
    uint32_t transport_fds = 0;
    uint32_t transport_ints = 0;
    enum advc_prime_layout_gate_status layout_gate;
    int duplicate = -1;
    int result = -1;

    last_status = "begin";
    last_transport_fds = 0;
    last_transport_ints = 0;
    memset(&last_diagnostics, 0, sizeof(last_diagnostics));
    last_diagnostics.qti_data_fd_value = -1;
    last_diagnostics.qti_data_fd_transport_index = -1;

    if (provider == NULL || provider->mapper == NULL ||
        provider->get_native_handle == NULL || hardware_buffer == NULL ||
        public_metadata == NULL || descriptor == NULL) {
        errno = EINVAL;
        last_status = "invalid-arguments";
        return -1;
    }
    raw_handle = provider->get_native_handle(hardware_buffer);
    if (raw_handle == NULL) {
        last_status = "native-handle-unavailable";
        goto fail;
    }
    if (provider->mapper->v5.import_buffer(raw_handle, &imported) !=
            ADVC_AIMAPPER_ERROR_NONE || imported == NULL) {
        last_status = "import-buffer-failed";
        goto fail;
    }
    if (provider->mapper->v5.get_transport_size(
            imported, &transport_fds, &transport_ints) !=
        ADVC_AIMAPPER_ERROR_NONE) {
        last_status = "transport-size-query-failed";
        goto fail;
    }
    last_transport_fds = transport_fds;
    last_transport_ints = transport_ints;
    {
        int saved_errno = errno;
        int32_t qti_data_fd = -1;
        if (query_vendor_i32(provider->mapper, imported, "QTI", 10012,
                             &qti_data_fd) == 0) {
            struct stat qti_info;
            last_diagnostics.qti_data_fd_value = qti_data_fd;
            if (qti_data_fd >= 0 && fcntl(qti_data_fd, F_GETFD) >= 0 &&
                fstat(qti_data_fd, &qti_info) == 0) {
                last_diagnostics.qti_data_fd_valid = 1;
                if (qti_info.st_size > 0)
                    last_diagnostics.qti_data_fd_size =
                        (uint64_t)qti_info.st_size;
            }
            for (uint32_t i = 0; i < transport_fds; ++i) {
                struct stat transport_info;
                if (imported->data[i] == qti_data_fd ||
                    (last_diagnostics.qti_data_fd_valid &&
                     fstat(imported->data[i], &transport_info) == 0 &&
                     transport_info.st_dev == qti_info.st_dev &&
                     transport_info.st_ino == qti_info.st_ino)) {
                    last_diagnostics.qti_data_fd_transport_index = (int32_t)i;
                    break;
                }
            }
        } else {
            last_diagnostics.qti_data_fd_query_errno = errno;
        }
        errno = saved_errno;
    }
    for (uint32_t i = 0; i < transport_fds &&
                         i < ADVC_MAX_DMABUF_OBJECTS; ++i) {
        struct stat info;
        if (fstat(imported->data[i], &info) == 0 && info.st_size > 0)
            last_diagnostics.transport_fd_sizes[i] = (uint64_t)info.st_size;
    }
    if (transport_fds == 0 || transport_fds > ADVC_MAX_DMABUF_OBJECTS ||
        imported->num_fds < 1 ||
        (uint32_t)imported->num_fds < transport_fds ||
        imported->num_ints < 0 ||
        (uint32_t)imported->num_ints < transport_ints) {
        /*
         * Standard PLANE_LAYOUTS has no plane-to-FD/object index. More than
         * one transport FD cannot be mapped exactly without a vendor-private
         * contract and must remain unavailable.
        */
        last_status = "invalid-transport-size";
        errno = ENOTSUP;
        goto fail;
    }
#define QUERY_OR_FAIL(expression, status_text) \
    do { \
        if ((expression) < 0) { \
            last_status = (status_text); \
            goto fail; \
        } \
    } while (0)
    QUERY_OR_FAIL(query_u64(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_WIDTH, &mapper_width),
                  "width-metadata-unavailable");
    QUERY_OR_FAIL(query_u64(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_HEIGHT, &mapper_height),
                  "height-metadata-unavailable");
    QUERY_OR_FAIL(query_u64(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_LAYER_COUNT, &mapper_layers),
                  "layer-count-metadata-unavailable");
    QUERY_OR_FAIL(query_u32(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_PIXEL_FORMAT_FOURCC, &fourcc),
                  "fourcc-metadata-unavailable");
    QUERY_OR_FAIL(query_u64(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER,
                            &modifier),
                  "modifier-metadata-unavailable");
    QUERY_OR_FAIL(query_u64(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_ALLOCATION_SIZE,
                            &allocation_size),
                  "allocation-size-metadata-unavailable");
    QUERY_OR_FAIL(query_u32(provider->mapper, imported,
                            ADVC_STANDARD_METADATA_STRIDE, &mapper_stride),
                  "stride-metadata-unavailable");
    QUERY_OR_FAIL(query_layouts(provider->mapper, imported, &layouts),
                  "plane-layout-metadata-unavailable");
    QUERY_OR_FAIL(query_crop(provider->mapper, imported, &crops),
                  "crop-metadata-unavailable");
#undef QUERY_OR_FAIL

    last_diagnostics.transport_fds = transport_fds;
    last_diagnostics.transport_ints = transport_ints;
    last_diagnostics.mapper_width = mapper_width;
    last_diagnostics.mapper_height = mapper_height;
    last_diagnostics.mapper_layers = mapper_layers;
    last_diagnostics.mapper_stride = mapper_stride;
    last_diagnostics.fourcc = fourcc;
    last_diagnostics.modifier = modifier;
    last_diagnostics.allocation_size = allocation_size;
    last_diagnostics.plane_count = layouts.count;
    last_diagnostics.crop_count = crops.count;
    last_diagnostics.crop_left = (uint32_t)crops.crops[0].left;
    last_diagnostics.crop_top = (uint32_t)crops.crops[0].top;
    last_diagnostics.crop_width =
        (uint32_t)(crops.crops[0].right - crops.crops[0].left);
    last_diagnostics.crop_height =
        (uint32_t)(crops.crops[0].bottom - crops.crops[0].top);
    for (uint32_t i = 0; i < layouts.count; ++i) {
        last_diagnostics.plane_offsets[i] = layouts.planes[i].offset_bytes;
        last_diagnostics.plane_strides[i] =
            (uint32_t)layouts.planes[i].stride_bytes;
        if (getenv("ADVC_DEBUG") != NULL) {
            for (uint32_t component = 0;
                 component < layouts.planes[i].component_count; ++component) {
                const struct advc_mapper_plane_component *decoded =
                    &layouts.planes[i].components[component];
                fprintf(stderr,
                        "advc-prime: layout=%u component=%u name=%s "
                        "value=%" PRId64 " offset_bits=%" PRId64
                        " size_bits=%" PRId64 " offset=%" PRIu64
                        " stride=%" PRIu64 " width=%" PRIu64
                        " height=%" PRIu64 " total=%" PRIu64 "\n",
                        i, component, decoded->type_name,
                        decoded->type_value, decoded->offset_bits,
                        decoded->size_bits, layouts.planes[i].offset_bytes,
                        layouts.planes[i].stride_bytes,
                        layouts.planes[i].width_samples,
                        layouts.planes[i].height_samples,
                        layouts.planes[i].total_size_bytes);
            }
        }
    }
    if (mapper_width != public_metadata->width) {
        last_status = "mapper-width-public-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (mapper_height != public_metadata->height) {
        last_status = "mapper-height-public-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (mapper_layers != 1 || mapper_layers != public_metadata->layers) {
        last_status = "mapper-layers-public-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (mapper_stride != public_metadata->stride) {
        last_status = "mapper-stride-public-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (fourcc == 0 || modifier == UINT64_MAX || allocation_size == 0 ||
        allocation_size > ADVC_MAX_DMABUF_OBJECT_BYTES) {
        last_status = "invalid-standard-metadata";
        errno = EPROTO;
        goto fail;
    }
    if ((uint64_t)public_metadata->crop_left + public_metadata->crop_width >
            mapper_width ||
        (uint64_t)public_metadata->crop_top + public_metadata->crop_height >
            mapper_height ||
        public_metadata->crop_left < (uint32_t)crops.crops[0].left ||
        public_metadata->crop_top < (uint32_t)crops.crops[0].top ||
        (uint64_t)public_metadata->crop_left + public_metadata->crop_width >
            (uint32_t)crops.crops[0].right ||
        (uint64_t)public_metadata->crop_top + public_metadata->crop_height >
            (uint32_t)crops.crops[0].bottom) {
        last_status = "logical-crop-mapper-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (advc_mapper_qti_nv12_normalize(
            fourcc, modifier, allocation_size, (uint32_t)mapper_width,
            (uint32_t)mapper_height, mapper_stride, transport_fds,
            last_diagnostics.transport_fd_sizes,
            last_diagnostics.qti_data_fd_transport_index,
            last_diagnostics.qti_data_fd_valid,
            last_diagnostics.qti_data_fd_size, &layouts, &crops,
            &qti_layout) == 0) {
        duplicate = fcntl(imported->data[qti_layout.image_transport_index],
                          F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0) {
            last_status = "qti-image-fd-duplicate-failed";
            goto fail;
        }
        descriptor->width = (uint32_t)mapper_width;
        descriptor->height = (uint32_t)mapper_height;
        descriptor->drm_fourcc = fourcc;
        descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
        descriptor->drm_modifier = modifier;
        descriptor->crop_left = public_metadata->crop_left;
        descriptor->crop_top = public_metadata->crop_top;
        descriptor->crop_width = public_metadata->crop_width;
        descriptor->crop_height = public_metadata->crop_height;
        descriptor->object_count = 1;
        descriptor->plane_count = qti_layout.plane_count;
        descriptor->objects[0].fd = duplicate;
        descriptor->objects[0].size = allocation_size;
        for (uint32_t i = 0; i < qti_layout.plane_count; ++i) {
            descriptor->planes[i].object_index = 0;
            descriptor->planes[i].offset = qti_layout.plane_offsets[i];
            descriptor->planes[i].pitch = qti_layout.plane_strides[i];
        }
        duplicate = -1;
        last_status = qti_layout.kind == ADVC_MAPPER_QTI_NV12_UBWC
                          ? "qti-nv12-ubwc-prime-export-pass"
                          : "qti-nv12-linear-prime-export-pass";
        result = 0;
        goto fail;
    }
    if (fourcc == UINT32_C(0x3231564e) &&
        modifier == UINT64_C(0x0500000000000001)) {
        last_status = "qti-nv12-ubwc-normalization-rejected";
        if (errno == 0) errno = EPROTO;
        goto fail;
    }
    layout_gate = advc_mapper_prime_layout_gate(
        transport_fds, layouts.count, crops.count);
    if (layout_gate == ADVC_PRIME_LAYOUT_GATE_CROP_PLANE_COUNT_MISMATCH) {
        last_status = "crop-plane-count-mismatch";
        errno = EPROTO;
        goto fail;
    }
    if (layout_gate == ADVC_PRIME_LAYOUT_GATE_AMBIGUOUS_MULTI_FD) {
        last_status = "ambiguous-multi-fd-layout";
        errno = ENOTSUP;
        goto fail;
    }
    for (uint32_t i = 0; i < layouts.count; ++i) {
        const struct advc_mapper_plane_layout *plane = &layouts.planes[i];
        if (plane->offset_bytes >= allocation_size ||
            plane->total_size_bytes > allocation_size - plane->offset_bytes ||
            plane->stride_bytes > UINT32_MAX ||
            (uint64_t)crops.crops[i].right > plane->width_samples ||
            (uint64_t)crops.crops[i].bottom > plane->height_samples) {
            last_status = "invalid-plane-extent";
            errno = EPROTO;
            goto fail;
        }
    }
    duplicate = fcntl(imported->data[0], F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        last_status = "prime-fd-duplicate-failed";
        goto fail;
    }

    descriptor->width = (uint32_t)mapper_width;
    descriptor->height = (uint32_t)mapper_height;
    descriptor->drm_fourcc = fourcc;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = modifier;
    descriptor->crop_left = public_metadata->crop_left;
    descriptor->crop_top = public_metadata->crop_top;
    descriptor->crop_width = public_metadata->crop_width;
    descriptor->crop_height = public_metadata->crop_height;
    descriptor->object_count = 1;
    descriptor->plane_count = layouts.count;
    descriptor->objects[0].fd = duplicate;
    descriptor->objects[0].size = allocation_size;
    for (uint32_t i = 0; i < layouts.count; ++i) {
        descriptor->planes[i].object_index = 0;
        descriptor->planes[i].offset = layouts.planes[i].offset_bytes;
        descriptor->planes[i].pitch = (uint32_t)layouts.planes[i].stride_bytes;
    }
    duplicate = -1;
    last_status = "exact-standard-prime-export-pass";
    result = 0;

fail:
    if (duplicate >= 0) close(duplicate);
    if (imported != NULL)
        (void)provider->mapper->v5.free_buffer(imported);
    if (result < 0 && errno == 0) errno = ENOTSUP;
    return result;
}

static int android_release_image(void *userdata, void *lifetime_token,
                                 int release_fence_fd) {
    (void)userdata;
    AImage_deleteAsync((AImage *)lifetime_token, release_fence_fd);
    return 0;
}

static void android_mapper_destroy(void *userdata) {
    struct android_prime_mapper *provider =
        (struct android_prime_mapper *)userdata;
    if (provider == NULL) return;
    if (provider->mapper_library != NULL) dlclose(provider->mapper_library);
    if (provider->android_library != NULL) dlclose(provider->android_library);
    if (provider->binder_library != NULL) dlclose(provider->binder_library);
    free(provider);
}

static int suffix_from_path(const char *path, char *suffix, size_t suffix_size) {
    static const char prefix[] = "mapper.";
    const char *name = strrchr(path, '/');
    const char *end;
    size_t size;
    name = name == NULL ? path : name + 1;
    if (strncmp(name, prefix, sizeof(prefix) - 1u) != 0) return -1;
    name += sizeof(prefix) - 1u;
    end = strstr(name, ".so");
    if (end == NULL || end[3] != '\0') return -1;
    size = (size_t)(end - name);
    if (size == 0 || size >= suffix_size) return -1;
    memcpy(suffix, name, size);
    suffix[size] = '\0';
    return 0;
}

static int load_mapper(struct android_prime_mapper *provider) {
    static const char *patterns[] = {
        "/vendor/lib64/hw/mapper.*.so",
        "/vendor/lib/hw/mapper.*.so",
    };
    advc_open_passthrough_hal_fn open_hal;
    provider->binder_library = dlopen("libbinder_ndk.so", RTLD_LOCAL | RTLD_NOW);
    if (provider->binder_library == NULL) {
        last_status = "binder-ndk-unavailable";
        return -1;
    }
    open_hal = (advc_open_passthrough_hal_fn)dlsym(
        provider->binder_library,
        "AServiceManager_openDeclaredPassthroughHal");
    if (open_hal == NULL) {
        last_status = "passthrough-hal-loader-unavailable";
        errno = ENOTSUP;
        return -1;
    }
    for (size_t pattern = 0;
         pattern < sizeof(patterns) / sizeof(patterns[0]); ++pattern) {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));
        if (glob(patterns[pattern], 0, NULL, &matches) != 0) {
            globfree(&matches);
            continue;
        }
        for (size_t i = 0; i < matches.gl_pathc; ++i) {
            char suffix[128];
            advc_load_ai_mapper_fn load;
            uint32_t *version;
            if (suffix_from_path(matches.gl_pathv[i], suffix,
                                 sizeof(suffix)) < 0)
                continue;
            provider->mapper_library =
                open_hal("mapper", suffix, RTLD_LOCAL | RTLD_NOW);
            if (provider->mapper_library == NULL) continue;
            version = (uint32_t *)dlsym(provider->mapper_library,
                                        "ANDROID_HAL_STABLEC_VERSION");
            if (version == NULL)
                version = (uint32_t *)dlsym(provider->mapper_library,
                                            "ANDROID_HAL_MAPPER_VERSION");
            load = (advc_load_ai_mapper_fn)dlsym(provider->mapper_library,
                                                  "AIMapper_loadIMapper");
            if (version != NULL && *version >= ADVC_AIMAPPER_VERSION_5 &&
                load != NULL && load(&provider->mapper) ==
                                    ADVC_AIMAPPER_ERROR_NONE &&
                provider->mapper != NULL &&
                provider->mapper->version >= ADVC_AIMAPPER_VERSION_5) {
                last_status = "stable-mapper-loaded";
                globfree(&matches);
                return 0;
            }
            dlclose(provider->mapper_library);
            provider->mapper_library = NULL;
            provider->mapper = NULL;
        }
        globfree(&matches);
    }
    errno = ENOTSUP;
    last_status = "declared-stable-mapper-unavailable";
    return -1;
}

struct advc_ahb_prime_mapper *advc_android_prime_mapper_create(void) {
    static const struct advc_ahb_prime_mapper_ops ops = {
        .export_prime = android_export_prime,
        .release = android_release_image,
        .destroy = android_mapper_destroy,
    };
    struct android_prime_mapper *provider;
    struct advc_ahb_prime_mapper *mapper;
    provider = (struct android_prime_mapper *)calloc(1, sizeof(*provider));
    if (provider == NULL) return NULL;
    provider->android_library = dlopen("libandroid.so", RTLD_LOCAL | RTLD_NOW);
    if (provider->android_library != NULL)
        provider->get_native_handle = (advc_ahb_get_native_handle_fn)dlsym(
            provider->android_library, "AHardwareBuffer_getNativeHandle");
    if (provider->get_native_handle == NULL || load_mapper(provider) < 0) {
        if (provider->get_native_handle == NULL)
            last_status = "ahardwarebuffer-native-handle-unavailable";
        int saved = errno == 0 ? ENOTSUP : errno;
        android_mapper_destroy(provider);
        errno = saved;
        return NULL;
    }
    mapper = advc_ahb_prime_mapper_create(&ops, provider);
    if (mapper == NULL) android_mapper_destroy(provider);
    return mapper;
}
