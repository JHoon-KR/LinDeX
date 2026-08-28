#define _GNU_SOURCE

#include "android_drm_blob.h"
#include "android_drm_bridge.h"

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <link.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drmMode.h>

#ifndef DRM_FORMAT_MOD_QCOM_COMPRESSED
#define DRM_FORMAT_MOD_QCOM_COMPRESSED fourcc_mod_code(QCOM, 1)
#endif

enum {
    MAX_SYNTHETIC_BLOBS = 64,
};

enum preload_policy {
    PRELOAD_POLICY_REJECT = 0,
    PRELOAD_POLICY_APPEND,
    PRELOAD_POLICY_STRICT_XB24_QCOM,
    PRELOAD_POLICY_STRICT_XB24_QCOM_LINEAR,
};

typedef drmModePropertyBlobPtr (*get_property_blob_fn)(int, uint32_t);
typedef void (*free_property_blob_fn)(drmModePropertyBlobPtr);
typedef uint32_t (*get_abi_version_fn)(void);
typedef uint64_t (*get_capabilities_fn)(void);
typedef int (*preload_validate_plane_blob_fn)(int, uint32_t, uint32_t,
                                               uint32_t, uint64_t);

struct synthetic_blob {
    drmModePropertyBlobPtr blob;
    struct synthetic_blob *next;
};

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t synthetic_lock = PTHREAD_MUTEX_INITIALIZER;
static get_property_blob_fn real_get_property_blob;
static free_property_blob_fn real_free_property_blob;
static get_abi_version_fn core_get_abi_version;
static get_capabilities_fn core_get_capabilities;
static preload_validate_plane_blob_fn core_validate_plane_blob;
static struct synthetic_blob *synthetic_blobs;
static unsigned int synthetic_blob_count;
static int augmentation_logged;
static int strict_logged;
static int strict_fallback_logged;
static _Thread_local unsigned int bridge_depth;
static void *libdrm_handle;
static void *core_handle;

struct strict_xb24_qcom_blob {
    struct drm_format_modifier_blob header;
    uint32_t format;
    uint32_t padding;
    struct drm_format_modifier modifier;
};

struct strict_xb24_qcom_linear_blob {
    struct drm_format_modifier_blob header;
    uint32_t format;
    uint32_t padding;
    struct drm_format_modifier modifiers[2];
};

static void log_augmentation_once(uint32_t plane_id, uint32_t blob_id)
{
    int should_log = 0;

    pthread_mutex_lock(&synthetic_lock);
    if (!augmentation_logged) {
        augmentation_logged = 1;
        should_log = 1;
    }
    pthread_mutex_unlock(&synthetic_lock);

    if (should_log) {
        fprintf(stderr,
                "ADBR-PRELOAD IN_FORMATS_AUGMENTED pid=%ld plane=%" PRIu32
                " blob=%" PRIu32 " format=XB24 fourcc=0x%08" PRIx32
                " modifier=0x%016" PRIx64
                " source=exact-device-candidate\n",
                (long)getpid(), plane_id, blob_id, (uint32_t)DRM_FORMAT_XBGR8888,
                (uint64_t)DRM_FORMAT_MOD_QCOM_COMPRESSED);
    }
}

static void log_strict_once(uint32_t plane_id, uint32_t blob_id)
{
    int should_log = 0;

    pthread_mutex_lock(&synthetic_lock);
    if (!strict_logged) {
        strict_logged = 1;
        should_log = 1;
    }
    pthread_mutex_unlock(&synthetic_lock);

    if (should_log) {
        fprintf(stderr,
                "ADBR-PRELOAD IN_FORMATS_STRICT pid=%ld plane=%" PRIu32
                " blob=%" PRIu32 " format=XB24 fourcc=0x%08" PRIx32
                " modifier=0x%016" PRIx64
                " policy=exact-device-xb24-qcom-only\n",
                (long)getpid(), plane_id, blob_id, (uint32_t)DRM_FORMAT_XBGR8888,
                (uint64_t)DRM_FORMAT_MOD_QCOM_COMPRESSED);
    }
}

static void log_strict_fallback_once(uint32_t plane_id, uint32_t blob_id)
{
    int should_log = 0;

    pthread_mutex_lock(&synthetic_lock);
    if (!strict_fallback_logged) {
        strict_fallback_logged = 1;
        should_log = 1;
    }
    pthread_mutex_unlock(&synthetic_lock);

    if (should_log) {
        fprintf(stderr,
                "ADBR-PRELOAD IN_FORMATS_STRICT_FALLBACK pid=%ld plane=%" PRIu32
                " blob=%" PRIu32 " format=XB24 fourcc=0x%08" PRIx32
                " modifiers=0x%016" PRIx64 ",0x%016" PRIx64
                " policy=exact-device-xb24-qcom-preferred-linear-fallback\n",
                (long)getpid(), plane_id, blob_id,
                (uint32_t)DRM_FORMAT_XBGR8888,
                (uint64_t)DRM_FORMAT_MOD_QCOM_COMPRESSED,
                (uint64_t)DRM_FORMAT_MOD_LINEAR);
    }
}

static int symbol_belongs_to_object(void *symbol,
                                    const struct link_map *object_map)
{
    Dl_info info;

    return symbol != NULL && object_map != NULL && dladdr(symbol, &info) != 0 &&
           info.dli_fbase == (void *)(uintptr_t)object_map->l_addr;
}

static void resolve_symbols(void)
{
    struct link_map *libdrm_map = NULL;
    struct link_map *core_map = NULL;
    void *candidate_libdrm;
    void *candidate_core;
    union {
        void *object;
        get_property_blob_fn function;
    } get_symbol = { NULL };
    union {
        void *object;
        free_property_blob_fn function;
    } free_symbol = { NULL };
    union {
        void *object;
        get_abi_version_fn function;
    } abi_symbol = { NULL };
    union {
        void *object;
        get_capabilities_fn function;
    } capabilities_symbol = { NULL };
    union {
        void *object;
        preload_validate_plane_blob_fn function;
    } validate_symbol = { NULL };

    /*
     * Do not use RTLD_NEXT here. Another preload may interpose dlsym() and
     * unintentionally change RTLD_NEXT's caller to itself. That previously
     * resolved both "real" pointers back to this frontend and caused an
     * unbounded drmModeGetPropertyBlob() recursion during Vulkan startup.
     * Resolve from libdrm's exact link map and reject dependency/interposed
     * results instead.
     */
    candidate_libdrm = dlopen("libdrm.so.2",
                              RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
    if (candidate_libdrm == NULL) {
        candidate_libdrm = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    }
    if (candidate_libdrm == NULL ||
        dlinfo(candidate_libdrm, RTLD_DI_LINKMAP, &libdrm_map) != 0) {
        if (candidate_libdrm != NULL) {
            dlclose(candidate_libdrm);
        }
        return;
    }
    get_symbol.object = dlsym(candidate_libdrm, "drmModeGetPropertyBlob");
    free_symbol.object = dlsym(candidate_libdrm, "drmModeFreePropertyBlob");
    if (!symbol_belongs_to_object(get_symbol.object, libdrm_map) ||
        !symbol_belongs_to_object(free_symbol.object, libdrm_map) ||
        get_symbol.function == drmModeGetPropertyBlob ||
        free_symbol.function == drmModeFreePropertyBlob) {
        dlclose(candidate_libdrm);
        return;
    }
    libdrm_handle = candidate_libdrm;
    real_get_property_blob = get_symbol.function;
    real_free_property_blob = free_symbol.function;

    candidate_core = dlopen("libandroid-drm-bridge.so.1",
                            RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
    if (candidate_core == NULL ||
        dlinfo(candidate_core, RTLD_DI_LINKMAP, &core_map) != 0) {
        if (candidate_core != NULL) {
            dlclose(candidate_core);
        }
        return;
    }
    abi_symbol.object = dlsym(candidate_core, "adbr_get_abi_version");
    capabilities_symbol.object = dlsym(candidate_core,
                                       "adbr_get_capabilities");
    validate_symbol.object = dlsym(candidate_core,
                                   "adbr_preload_validate_plane_blob_v1");
    if (!symbol_belongs_to_object(abi_symbol.object, core_map) ||
        !symbol_belongs_to_object(capabilities_symbol.object, core_map) ||
        !symbol_belongs_to_object(validate_symbol.object, core_map)) {
        dlclose(candidate_core);
        return;
    }
    core_handle = candidate_core;
    core_get_abi_version = abi_symbol.function;
    core_get_capabilities = capabilities_symbol.function;
    core_validate_plane_blob = validate_symbol.function;
}

static int parse_unsigned_env(const char *name, uint64_t maximum,
                              uint64_t *value_out)
{
    const char *text = getenv(name);
    uint64_t value = 0;
    size_t index;

    if (text == NULL || text[0] == '\0' || text[0] == '+' || text[0] == '-') {
        return 0;
    }
    for (index = 0; text[index] != '\0'; ++index) {
        uint64_t digit;
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        digit = (uint64_t)(text[index] - '0');
        if (value > (maximum - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    *value_out = value;
    return 1;
}

static enum preload_policy load_exact_mapping(int fd, uint32_t blob_id,
                                              uint32_t *plane_out)
{
    const char *enabled = getenv("ANDROID_DRM_PRELOAD_ENABLE");
    const char *candidate_ack = getenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK");
    const char *blob_pin = getenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB");
    const char *policy = getenv("ANDROID_DRM_PRELOAD_POLICY");
    const char *strict_ack = getenv("ANDROID_DRM_PRELOAD_STRICT_ACK");
    uint64_t configured_plane;
    uint64_t configured_blob;
    enum preload_policy selected_policy;

    (void)fd;
    if (enabled == NULL || strcmp(enabled, "1") != 0 ||
        candidate_ack == NULL ||
        strcmp(candidate_ack, "exact-device-candidate-only") != 0 ||
        !parse_unsigned_env("ANDROID_DRM_PRELOAD_PRIMARY_PLANE", UINT32_MAX,
                            &configured_plane) || configured_plane == 0) {
        return PRELOAD_POLICY_REJECT;
    }
    if (blob_pin != NULL && blob_pin[0] != '\0' &&
        strcmp(blob_pin, "auto") != 0 &&
        (!parse_unsigned_env("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", UINT32_MAX,
                            &configured_blob) || configured_blob == 0 ||
         blob_id != (uint32_t)configured_blob)) {
        return PRELOAD_POLICY_REJECT;
    }
    if (policy == NULL || policy[0] == '\0' || strcmp(policy, "append") == 0) {
        selected_policy = PRELOAD_POLICY_APPEND;
    } else if (strcmp(policy, "strict-xb24-qcom") == 0 &&
               strict_ack != NULL &&
               strcmp(strict_ack,
                      "exact-device-strict-xb24-qcom-no-fallback") == 0) {
        selected_policy = PRELOAD_POLICY_STRICT_XB24_QCOM;
    } else if (strcmp(policy, "strict-xb24-qcom-linear") == 0 &&
               strict_ack != NULL &&
               strcmp(strict_ack,
                      "exact-device-strict-xb24-qcom-linear-fallback") == 0) {
        selected_policy = PRELOAD_POLICY_STRICT_XB24_QCOM_LINEAR;
    } else {
        return PRELOAD_POLICY_REJECT;
    }

    *plane_out = (uint32_t)configured_plane;
    return selected_policy;
}

static int core_validates_xb24_qcom(int fd, uint32_t plane_id,
                                    uint32_t blob_id)
{
    const uint64_t required = ADBR_CAP_PLANE_FORMATS |
                              ADBR_CAP_QCOM_CANDIDATES |
                              ADBR_CAP_PRELOAD_VALIDATE;

    if (core_get_abi_version == NULL || core_get_capabilities == NULL ||
        core_validate_plane_blob == NULL ||
        core_get_abi_version() != ADBR_ABI_VERSION_1 ||
        (core_get_capabilities() & required) != required) {
        return 0;
    }
    return core_validate_plane_blob(fd, plane_id, blob_id,
                                    DRM_FORMAT_XBGR8888,
                                    DRM_FORMAT_MOD_QCOM_COMPRESSED) == 0;
}

static int remember_synthetic_blob(drmModePropertyBlobPtr blob)
{
    struct synthetic_blob *entry = malloc(sizeof(*entry));

    if (entry == NULL) {
        return 0;
    }
    entry->blob = blob;
    pthread_mutex_lock(&synthetic_lock);
    if (synthetic_blob_count >= MAX_SYNTHETIC_BLOBS) {
        pthread_mutex_unlock(&synthetic_lock);
        free(entry);
        return 0;
    }
    entry->next = synthetic_blobs;
    synthetic_blobs = entry;
    ++synthetic_blob_count;
    pthread_mutex_unlock(&synthetic_lock);
    return 1;
}

static int build_strict_xb24_qcom_blob(const void *source_data,
                                       size_t source_length,
                                       void **output_data,
                                       size_t *output_length)
{
    struct strict_xb24_qcom_blob *strict;
    void *validation_copy = NULL;
    size_t validation_length = 0;

    if (output_data == NULL || output_length == NULL) {
        return 0;
    }
    *output_data = NULL;
    *output_length = 0;

    /*
     * Reuse the append helper as the structural and source-state validator.
     * It accepts exactly one XB24 entry with LINEAR present and QCOM absent,
     * and rejects malformed, ambiguous, or already-modified blobs.
     */
    if (android_drm_blob_append_xb24_qcom(source_data, source_length,
                                          &validation_copy,
                                          &validation_length) != 1) {
        return 0;
    }
    free(validation_copy);

    strict = calloc(1, sizeof(*strict));
    if (strict == NULL) {
        return 0;
    }
    strict->header.version = 1;
    strict->header.count_formats = 1;
    strict->header.formats_offset =
        (uint32_t)offsetof(struct strict_xb24_qcom_blob, format);
    strict->header.count_modifiers = 1;
    strict->header.modifiers_offset =
        (uint32_t)offsetof(struct strict_xb24_qcom_blob, modifier);
    strict->format = DRM_FORMAT_XBGR8888;
    strict->modifier.formats = UINT64_C(1);
    strict->modifier.offset = 0;
    strict->modifier.modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;

    *output_data = strict;
    *output_length = sizeof(*strict);
    return 1;
}

static int build_strict_xb24_qcom_linear_blob(const void *source_data,
                                               size_t source_length,
                                               void **output_data,
                                               size_t *output_length)
{
    struct strict_xb24_qcom_linear_blob *strict;
    void *validation_copy = NULL;
    size_t validation_length = 0;

    if (output_data == NULL || output_length == NULL) {
        return 0;
    }
    *output_data = NULL;
    *output_length = 0;
    if (android_drm_blob_append_xb24_qcom(source_data, source_length,
                                          &validation_copy,
                                          &validation_length) != 1) {
        return 0;
    }
    free(validation_copy);

    strict = calloc(1, sizeof(*strict));
    if (strict == NULL) {
        return 0;
    }
    strict->header.version = 1;
    strict->header.count_formats = 1;
    strict->header.formats_offset =
        (uint32_t)offsetof(struct strict_xb24_qcom_linear_blob, format);
    strict->header.count_modifiers = 2;
    strict->header.modifiers_offset =
        (uint32_t)offsetof(struct strict_xb24_qcom_linear_blob, modifiers);
    strict->format = DRM_FORMAT_XBGR8888;
    strict->modifiers[0].formats = UINT64_C(1);
    strict->modifiers[0].offset = 0;
    strict->modifiers[0].modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
    strict->modifiers[1].formats = UINT64_C(1);
    strict->modifiers[1].offset = 0;
    strict->modifiers[1].modifier = DRM_FORMAT_MOD_LINEAR;

    *output_data = strict;
    *output_length = sizeof(*strict);
    return 1;
}

drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
    uint32_t primary_plane = 0;
    drmModePropertyBlobPtr original;
    drmModePropertyBlobPtr augmented;
    void *augmented_data = NULL;
    size_t augmented_length = 0;
    enum preload_policy policy;
    int validated;
    int saved_errno;

    pthread_once(&resolve_once, resolve_symbols);
    if (real_get_property_blob == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    original = real_get_property_blob(fd, blob_id);
    saved_errno = errno;
    policy = load_exact_mapping(fd, blob_id, &primary_plane);
    if (original == NULL || bridge_depth != 0 ||
        real_free_property_blob == NULL ||
        policy == PRELOAD_POLICY_REJECT) {
        errno = saved_errno;
        return original;
    }

    ++bridge_depth;
    validated = core_validates_xb24_qcom(fd, primary_plane, blob_id);
    --bridge_depth;
    if (validated != 1 ||
        (policy == PRELOAD_POLICY_STRICT_XB24_QCOM
             ? build_strict_xb24_qcom_blob(original->data, original->length,
                                            &augmented_data,
                                            &augmented_length)
         : policy == PRELOAD_POLICY_STRICT_XB24_QCOM_LINEAR
             ? build_strict_xb24_qcom_linear_blob(original->data,
                                                   original->length,
                                                   &augmented_data,
                                                   &augmented_length)
             : android_drm_blob_append_xb24_qcom(original->data,
                                                 original->length,
                                                 &augmented_data,
                                                 &augmented_length)) != 1 ||
        augmented_length > UINT32_MAX) {
        free(augmented_data);
        errno = saved_errno;
        return original;
    }

    augmented = calloc(1, sizeof(*augmented));
    if (augmented == NULL) {
        free(augmented_data);
        errno = saved_errno;
        return original;
    }
    augmented->id = original->id;
    augmented->length = (uint32_t)augmented_length;
    augmented->data = augmented_data;
    if (!remember_synthetic_blob(augmented)) {
        free(augmented->data);
        free(augmented);
        errno = saved_errno;
        return original;
    }

    real_free_property_blob(original);
    if (policy == PRELOAD_POLICY_STRICT_XB24_QCOM) {
        log_strict_once(primary_plane, blob_id);
    } else if (policy == PRELOAD_POLICY_STRICT_XB24_QCOM_LINEAR) {
        log_strict_fallback_once(primary_plane, blob_id);
    } else {
        log_augmentation_once(primary_plane, blob_id);
    }
    errno = saved_errno;
    return augmented;
}

void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob)
{
    struct synthetic_blob **link;
    struct synthetic_blob *entry = NULL;

    if (blob == NULL) {
        return;
    }
    pthread_once(&resolve_once, resolve_symbols);
    pthread_mutex_lock(&synthetic_lock);
    for (link = &synthetic_blobs; *link != NULL; link = &(*link)->next) {
        if ((*link)->blob == blob) {
            entry = *link;
            *link = entry->next;
            --synthetic_blob_count;
            break;
        }
    }
    pthread_mutex_unlock(&synthetic_lock);

    if (entry != NULL) {
        free(blob->data);
        free(blob);
        free(entry);
    } else if (real_free_property_blob != NULL) {
        real_free_property_blob(blob);
    }
}
