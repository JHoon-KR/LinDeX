#ifndef ADVC_SURFACE_ENCODE_PROBE_H
#define ADVC_SURFACE_ENCODE_PROBE_H

/* Cached bounded broker-local MediaCodec + EGL/GLES render probe. */
int advc_probe_broker_egl_surface(void);
int advc_probe_android_ahb_surface(void);
/* Full dma-heap -> importer -> codec Surface -> encoded-frame probe. */
int advc_probe_android_dmabuf_surface(void);
enum advc_dmabuf_surface_route {
    ADVC_DMABUF_SURFACE_NONE = 0,
    ADVC_DMABUF_SURFACE_EGL = 1,
    ADVC_DMABUF_SURFACE_VULKAN = 2,
};
/* Each backend probe creates, drains, and destroys an independent codec session. */
int advc_probe_android_dmabuf_surface_backend(int route);
/* Valid only after the cached full probe; calls it when necessary. */
int advc_android_dmabuf_surface_route(void);
const char *advc_android_dmabuf_surface_status(void);
const char *advc_android_dmabuf_surface_backend_status(int route);

#endif
