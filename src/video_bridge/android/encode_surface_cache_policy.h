#ifndef ADVC_ENCODE_SURFACE_CACHE_POLICY_H
#define ADVC_ENCODE_SURFACE_CACHE_POLICY_H

/*
 * An asynchronous AHardwareBuffer import cache is safe only when EGL can
 * return an explicit completion fence and Android can identify the underlying
 * allocation independently of a recyclable AHardwareBuffer wrapper pointer.
 */
static inline int advc_egl_ahb_async_cache_allowed(
    int native_fence_async_supported, int get_id_available,
    int get_id_result) {
    return native_fence_async_supported && get_id_available &&
           get_id_result == 0;
}

#endif
