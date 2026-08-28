/* SPDX-License-Identifier: MIT */
#ifndef ANDROID_DRM_BRIDGE_H
#define ANDROID_DRM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define ADBR_PUBLIC __attribute__((visibility("default")))
#else
#define ADBR_PUBLIC
#endif

#define ADBR_ABI_VERSION_1 UINT32_C(0x00010000)
#define ADBR_MAX_PLANES 4u
#define ADBR_MAX_LEASE_OBJECTS 256u
#define ADBR_MAX_FORMAT_MODIFIERS 1024u

#define ADBR_CAP_LEASE_VALIDATE UINT64_C(1)
#define ADBR_CAP_PLANE_FORMATS UINT64_C(2)
#define ADBR_CAP_QCOM_CANDIDATES UINT64_C(4)
#define ADBR_CAP_PRIME_FB_IMPORT UINT64_C(8)
#define ADBR_CAP_GETFB2_OBSERVE UINT64_C(16)
#define ADBR_CAP_PRELOAD_VALIDATE UINT64_C(32)

#define ADBR_FORMAT_SOURCE_IN_FORMATS UINT32_C(1)
/* A bounded, exact vendor-blob token. This is an advertisement, not proof. */
#define ADBR_FORMAT_SOURCE_QCOM_CANDIDATE UINT32_C(2)

#define ADBR_PLANE_QUERY_QCOM_CANDIDATES UINT32_C(1)

/* Public representation of DRM_FORMAT_MOD_INVALID without requiring libdrm. */
#define ADBR_MODIFIER_INVALID UINT64_C(0x00ffffffffffffff)
#define ADBR_MODIFIER_LINEAR UINT64_C(0)
#define ADBR_MODIFIER_QCOM_COMPRESSED UINT64_C(0x0500000000000001)

struct adbr_lease_info_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t object_count;
    uint32_t reserved;
};

struct adbr_format_modifier_v1 {
    uint32_t fourcc;
    uint32_t source_flags;
    uint64_t modifier;
};

struct adbr_dmabuf_plane_v1 {
    int32_t fd; /* borrowed */
    uint32_t stride;
    uint32_t offset;
    uint32_t reserved;
    uint64_t modifier;
};

struct adbr_dmabuf_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint32_t flags;
    uint32_t reserved;
    struct adbr_dmabuf_plane_v1 planes[ADBR_MAX_PLANES];
};

struct adbr_fb_observation_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t plane_count;
    uint32_t flags;
    uint32_t strides[ADBR_MAX_PLANES];
    uint32_t offsets[ADBR_MAX_PLANES];
    uint64_t modifiers[ADBR_MAX_PLANES];
};

struct adbr_fb;

ADBR_PUBLIC uint32_t adbr_get_abi_version(void);
ADBR_PUBLIC uint64_t adbr_get_capabilities(void);

/*
 * Validate that borrowed_fd is a KMS-capable lease, enumerate its exact object
 * IDs and return a CLOEXEC duplicate. If expected_count is non-zero, the
 * leased object set must exactly equal expected_objects (order-independent).
 * The caller owns *out_owned_fd on success.
 */
ADBR_PUBLIC int adbr_lease_dup_validate_v1(
    int borrowed_fd,
    const uint32_t *expected_objects,
    size_t expected_count,
    uint32_t flags,
    int *out_owned_fd,
    struct adbr_lease_info_v1 *out_info);

/* Two-call enumeration: objects=NULL reports the exact required count. */
ADBR_PUBLIC int adbr_lease_get_objects_v1(
    int borrowed_fd,
    uint32_t *objects,
    size_t *inout_count);

/*
 * Query the standard IN_FORMATS blob. Optional QCOM parsing returns only
 * exact bounded tokens as ADBR_FORMAT_SOURCE_QCOM_CANDIDATE. It never upgrades
 * a candidate to proof. entries=NULL reports the required count.
 */
ADBR_PUBLIC int adbr_plane_query_formats_v1(
    int borrowed_fd,
    uint32_t plane_id,
    uint32_t query_flags,
    struct adbr_format_modifier_v1 *entries,
    size_t *inout_count);

/*
 * Narrow preload gate: validates that plane_id belongs to the exact lease,
 * is PRIMARY, owns the supplied BLOB-typed IN_FORMATS property, advertises
 * standard LINEAR XB24 and has an exact bounded XB24/5/1 vendor candidate.
 * This remains candidate validation, not active scanout proof.
 */
ADBR_PUBLIC int adbr_preload_validate_plane_blob_v1(
    int borrowed_fd,
    uint32_t plane_id,
    uint32_t in_formats_blob_id,
    uint32_t fourcc,
    uint64_t modifier);

ADBR_PUBLIC int adbr_dmabuf_validate_v1(const struct adbr_dmabuf_v1 *dmabuf);

/*
 * PRIME-import every plane into a duplicate of borrowed_kms_fd and register an
 * exact AddFB2WithModifiers framebuffer. No format or modifier fallback exists.
 */
ADBR_PUBLIC int adbr_fb_import_v1(
    int borrowed_kms_fd,
    const struct adbr_dmabuf_v1 *dmabuf,
    uint32_t flags,
    struct adbr_fb **out_fb);

ADBR_PUBLIC uint32_t adbr_fb_get_id_v1(const struct adbr_fb *fb);
/*
 * Remove the framebuffer, close imported GEM handles, and free fb. If RMFB
 * fails, ownership is retained and the caller may retry with the same fb.
 */
ADBR_PUBLIC int adbr_fb_destroy_v1(struct adbr_fb *fb);

/* GETFB2 observation. Returned GEM handles are closed internally. */
ADBR_PUBLIC int adbr_fb_observe_v1(
    int borrowed_kms_fd,
    uint32_t fb_id,
    struct adbr_fb_observation_v1 *out_observation);

#ifdef __cplusplus
}
#endif

#endif
