#include "encode_surface_egl.h"
#include "encode_surface_cache_policy.h"

#if !defined(__ANDROID__)
#error "encode_surface_egl.c is Android-only"
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT 0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT 0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT 0x327A
#endif
#ifndef EGL_DMA_BUF_PLANE3_FD_EXT
#define EGL_DMA_BUF_PLANE3_FD_EXT 0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT 0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT 0x3442
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#endif
#ifndef EGL_YUV_COLOR_SPACE_HINT_EXT
#define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
#define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
#define EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT 0x327D
#define EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT 0x327E
#define EGL_ITU_REC709_EXT 0x3280
#define EGL_YUV_NARROW_RANGE_EXT 0x3283
#define EGL_YUV_CHROMA_SITING_0_5_EXT 0x3285
#endif

#define ADVC_DRM_FORMAT_ABGR8888 UINT32_C(0x34324241) /* AB24 */
#define ADVC_DRM_FORMAT_XBGR8888 UINT32_C(0x34324258) /* XB24 */
#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)     /* NV12 */
#define ADVC_EGL_IMPORT_CACHE_SIZE ADVC_MAX_REGISTERED_DMABUFS
#define ADVC_EGL_AHB_CACHE_SIZE ADVC_MAX_INFLIGHT_DMABUFS
#define ADVC_EGL_FENCE_TIMEOUT_MS 5000

_Static_assert(ADVC_EGL_IMPORT_CACHE_SIZE >= ADVC_MAX_INFLIGHT_DMABUFS,
               "every in-flight dma-buf needs a bounded cache entry");
_Static_assert(ADVC_EGL_AHB_CACHE_SIZE > 0,
               "at least one in-flight AHardwareBuffer is required");

typedef int (*advc_ahardwarebuffer_get_id_fn)(
    const AHardwareBuffer *buffer, uint64_t *out_id);

_Static_assert(sizeof(advc_ahardwarebuffer_get_id_fn) == sizeof(void *),
               "Android function and data pointers must have equal size");

static void egl_debug_failure(const char *stage, EGLint error) {
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-surface-egl: stage=%s egl_error=0x%x\n",
                stage, (unsigned int)error);
}

struct advc_shared_egl_display {
    pthread_mutex_t lock;
    EGLDisplay display;
    unsigned int references;
};

static struct advc_shared_egl_display shared_display = {
    PTHREAD_MUTEX_INITIALIZER, EGL_NO_DISPLAY, 0,
};

static EGLDisplay acquire_display(void) {
    EGLDisplay display = EGL_NO_DISPLAY;
    if (pthread_mutex_lock(&shared_display.lock) != 0) return EGL_NO_DISPLAY;
    if (shared_display.references == 0) {
        shared_display.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (shared_display.display == EGL_NO_DISPLAY ||
            !eglInitialize(shared_display.display, NULL, NULL)) {
            shared_display.display = EGL_NO_DISPLAY;
            goto done;
        }
    }
    ++shared_display.references;
    display = shared_display.display;
done:
    (void)pthread_mutex_unlock(&shared_display.lock);
    return display;
}

static void release_display(EGLDisplay display) {
    if (display == EGL_NO_DISPLAY ||
        pthread_mutex_lock(&shared_display.lock) != 0)
        return;
    if (shared_display.references != 0 && shared_display.display == display) {
        --shared_display.references;
        if (shared_display.references == 0) {
            (void)eglTerminate(shared_display.display);
            shared_display.display = EGL_NO_DISPLAY;
        }
    }
    (void)pthread_mutex_unlock(&shared_display.lock);
}

struct advc_egl_import_cache_entry {
    EGLImageKHR image;
    GLuint texture;
    struct advc_dmabuf_descriptor descriptor;
    struct stat fd_identities[ADVC_MAX_DMABUF_OBJECTS];
    uint64_t last_use_serial;
    uint64_t completion_serial;
    int completion_fence_fd;
    int occupied;
};

struct advc_egl_ahb_cache_entry {
    AHardwareBuffer *buffer;
    EGLImageKHR image;
    GLuint texture;
    AHardwareBuffer_Desc descriptor;
    uint64_t buffer_id;
    uint64_t last_use_serial;
    uint64_t completion_serial;
    int completion_fence_fd;
    int occupied;
};

struct advc_egl_surface_producer {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time;
    uint32_t width;
    uint32_t height;
    GLuint ahb_program;
    GLint ahb_position;
    GLint ahb_texcoord;
    GLint ahb_sampler;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client_buffer;
    advc_ahardwarebuffer_get_id_fn get_ahb_id;
    struct advc_egl_import_cache_entry
        import_cache[ADVC_EGL_IMPORT_CACHE_SIZE];
    struct advc_egl_ahb_cache_entry ahb_cache[ADVC_EGL_AHB_CACHE_SIZE];
    uint64_t cache_serial;
    uint64_t submission_serial;
    int dmabuf_runtime_checked;
    int dmabuf_import_supported;
    int ahb_runtime_checked;
    int ahb_import_supported;
    int native_fence_async_supported;
};

static int discard_import_caches(
    struct advc_egl_surface_producer *producer);
static void abandon_import_caches(
    struct advc_egl_surface_producer *producer);
static int resolve_ahb_runtime(
    struct advc_egl_surface_producer *producer);
static int wait_native_fence_fd(int fd);
static int create_ahb_texture(
    struct advc_egl_surface_producer *producer, AHardwareBuffer *buffer,
    EGLImageKHR *image, GLuint *texture, AHardwareBuffer **owned_buffer);
static int get_cached_ahb(
    struct advc_egl_surface_producer *producer, AHardwareBuffer *buffer,
    const AHardwareBuffer_Desc *descriptor, uint64_t buffer_id,
    struct advc_egl_ahb_cache_entry **entry_out);

static int extension_present(const char *extensions, const char *name) {
    size_t length;
    const char *found;
    if (extensions == NULL || name == NULL || *name == '\0' || strchr(name, ' '))
        return 0;
    length = strlen(name);
    found = extensions;
    while ((found = strstr(found, name)) != NULL) {
        if ((found == extensions || found[-1] == ' ') &&
            (found[length] == '\0' || found[length] == ' '))
            return 1;
        found += length;
    }
    return 0;
}

static int dmabuf_descriptor_supported(
    const struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor) {
    const struct advc_dmabuf_plane *plane0;
    const struct advc_dmabuf_plane *plane1;
    const struct advc_dmabuf_object *object0;
    const struct advc_dmabuf_object *object1;
    uint64_t plane0_end;
    uint64_t plane1_end;
    uint64_t minimum_end;
    if (producer == NULL || descriptor == NULL ||
        advc_dmabuf_descriptor_validate(descriptor) < 0 ||
        descriptor->drm_modifier != 0 ||
        descriptor->crop_width != producer->width ||
        descriptor->crop_height != producer->height)
        return 0;

    plane0 = &descriptor->planes[0];
    object0 = &descriptor->objects[plane0->object_index];
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_ABGR8888 ||
        descriptor->drm_fourcc == ADVC_DRM_FORMAT_XBGR8888) {
        if (descriptor->plane_count != 1 ||
            (descriptor->color_matrix != ADVC_COLOR_MATRIX_UNSPECIFIED &&
             descriptor->color_matrix != ADVC_COLOR_MATRIX_RGB) ||
            (descriptor->color_range != ADVC_COLOR_RANGE_UNSPECIFIED &&
             descriptor->color_range != ADVC_COLOR_RANGE_FULL) ||
            descriptor->chroma_horizontal != ADVC_CHROMA_SITING_UNSPECIFIED ||
            descriptor->chroma_vertical != ADVC_CHROMA_SITING_UNSPECIFIED ||
            plane0->offset > INT_MAX || plane0->pitch > INT_MAX ||
            plane0->pitch < descriptor->width * UINT32_C(4))
            return 0;
        minimum_end = plane0->offset +
                      (uint64_t)plane0->pitch * (descriptor->height - 1u) +
                      (uint64_t)descriptor->width * 4u;
        return minimum_end >= plane0->offset && minimum_end <= object0->size;
    }

    /*
     * The first zero-copy VA encode route is intentionally the exact layout
     * allocated by advc_vaapi_encode_surface_allocate_linear(): one LINEAR
     * NV12 dma-buf, two planes, and explicit BT.709 limited-range metadata.
     * EGL's external-texture sampler performs the YUV-to-RGB conversion in one
     * GPU draw into MediaCodec's recordable input surface.  No CPU pixel map or
     * copy occurs here.  Compressed/QCOM modifiers remain fail-closed until an
     * importer proves their native modifier tuple.
     */
    if (descriptor->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        descriptor->object_count != 1 || descriptor->plane_count != 2 ||
        (descriptor->width & 1u) != 0 || (descriptor->height & 1u) != 0 ||
        descriptor->color_primaries != ADVC_COLOR_PRIMARIES_BT709 ||
        descriptor->color_transfer != ADVC_COLOR_TRANSFER_BT709 ||
        descriptor->color_matrix != ADVC_COLOR_MATRIX_BT709 ||
        descriptor->color_range != ADVC_COLOR_RANGE_LIMITED ||
        descriptor->chroma_horizontal != ADVC_CHROMA_SITING_MIDPOINT ||
        descriptor->chroma_vertical != ADVC_CHROMA_SITING_MIDPOINT)
        return 0;
    plane1 = &descriptor->planes[1];
    object1 = &descriptor->objects[plane1->object_index];
    if (plane0->object_index != 0 || plane1->object_index != 0 ||
        plane0->offset > INT_MAX || plane1->offset > INT_MAX ||
        plane0->pitch > INT_MAX || plane1->pitch > INT_MAX ||
        plane0->pitch < descriptor->width ||
        plane1->pitch < descriptor->width)
        return 0;
    plane0_end = plane0->offset +
                 (uint64_t)plane0->pitch * (descriptor->height - 1u) +
                 descriptor->width;
    plane1_end = plane1->offset +
                 (uint64_t)plane1->pitch * (descriptor->height / 2u - 1u) +
                 descriptor->width;
    return plane0_end >= plane0->offset && plane1_end >= plane1->offset &&
           plane0_end <= object0->size && plane1_end <= object1->size &&
           plane1->offset >= plane0_end;
}

static EGLImageKHR create_dmabuf_image(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor,
    PFNEGLCREATEIMAGEKHRPROC create_image) {
    static const EGLint fd_keys[ADVC_MAX_DMABUF_PLANES] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLint offset_keys[ADVC_MAX_DMABUF_PLANES] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLint pitch_keys[ADVC_MAX_DMABUF_PLANES] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLint modifier_lo_keys[ADVC_MAX_DMABUF_PLANES] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLint modifier_hi_keys[ADVC_MAX_DMABUF_PLANES] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };
    EGLint attributes[17 + ADVC_MAX_DMABUF_PLANES * 10];
    size_t cursor = 0;
    if (!dmabuf_descriptor_supported(producer, descriptor) ||
        create_image == NULL)
        return EGL_NO_IMAGE_KHR;
#define APPEND_ATTRIBUTE(key, value) \
    do { attributes[cursor++] = (key); attributes[cursor++] = (EGLint)(value); } while (0)
    APPEND_ATTRIBUTE(EGL_WIDTH, descriptor->width);
    APPEND_ATTRIBUTE(EGL_HEIGHT, descriptor->height);
    APPEND_ATTRIBUTE(EGL_LINUX_DRM_FOURCC_EXT, descriptor->drm_fourcc);
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        APPEND_ATTRIBUTE(EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT);
        APPEND_ATTRIBUTE(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);
        APPEND_ATTRIBUTE(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,
                         EGL_YUV_CHROMA_SITING_0_5_EXT);
        APPEND_ATTRIBUTE(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT,
                         EGL_YUV_CHROMA_SITING_0_5_EXT);
    }
    for (uint32_t i = 0; i < descriptor->plane_count; ++i) {
        const struct advc_dmabuf_plane *plane = &descriptor->planes[i];
        const struct advc_dmabuf_object *object =
            &descriptor->objects[plane->object_index];
        APPEND_ATTRIBUTE(fd_keys[i], object->fd);
        APPEND_ATTRIBUTE(offset_keys[i], plane->offset);
        APPEND_ATTRIBUTE(pitch_keys[i], plane->pitch);
        APPEND_ATTRIBUTE(modifier_lo_keys[i],
                         (uint32_t)descriptor->drm_modifier);
        APPEND_ATTRIBUTE(modifier_hi_keys[i],
                         (uint32_t)(descriptor->drm_modifier >> 32));
    }
    attributes[cursor++] = EGL_NONE;
#undef APPEND_ATTRIBUTE
    return create_image(producer->display, EGL_NO_CONTEXT,
                        EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)0, attributes);
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int ensure_ahb_program(struct advc_egl_surface_producer *producer) {
    static const char vertex_source[] =
        "attribute vec2 position; attribute vec2 texcoord;"
        "varying vec2 uv; void main(){uv=texcoord;gl_Position=vec4(position,0.0,1.0);}";
    static const char fragment_source[] =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float; varying vec2 uv; uniform samplerExternalOES image;"
        "void main(){gl_FragColor=texture2D(image,uv);}";
    GLuint vertex;
    GLuint fragment;
    GLint ok = GL_FALSE;
    if (producer->ahb_program != 0) return 0;
    vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (vertex == 0 || fragment == 0) goto fail;
    producer->ahb_program = glCreateProgram();
    if (producer->ahb_program == 0) goto fail;
    glAttachShader(producer->ahb_program, vertex);
    glAttachShader(producer->ahb_program, fragment);
    glLinkProgram(producer->ahb_program);
    glGetProgramiv(producer->ahb_program, GL_LINK_STATUS, &ok);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!ok) goto fail_program;
    producer->ahb_position = glGetAttribLocation(producer->ahb_program, "position");
    producer->ahb_texcoord = glGetAttribLocation(producer->ahb_program, "texcoord");
    producer->ahb_sampler = glGetUniformLocation(producer->ahb_program, "image");
    if (producer->ahb_position < 0 || producer->ahb_texcoord < 0 ||
        producer->ahb_sampler < 0)
        goto fail_program;
    return 0;
fail:
    if (vertex != 0) glDeleteShader(vertex);
    if (fragment != 0) glDeleteShader(fragment);
fail_program:
    if (producer->ahb_program != 0) glDeleteProgram(producer->ahb_program);
    producer->ahb_program = 0;
    return -1;
}

static void destroy_egl(struct advc_egl_surface_producer *producer) {
    if (producer->display != EGL_NO_DISPLAY) {
        int made_current =
            producer->context != EGL_NO_CONTEXT &&
            producer->surface != EGL_NO_SURFACE &&
            eglMakeCurrent(producer->display, producer->surface,
                           producer->surface, producer->context);
        if (made_current) {
            (void)discard_import_caches(producer);
            if (producer->ahb_program != 0)
                glDeleteProgram(producer->ahb_program);
        }
        (void)eglMakeCurrent(producer->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             EGL_NO_CONTEXT);
        if (producer->surface != EGL_NO_SURFACE)
            (void)eglDestroySurface(producer->display, producer->surface);
        if (producer->context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(producer->display, producer->context);
        /* Context destruction owns any texture left by a failed make-current. */
        abandon_import_caches(producer);
        release_display(producer->display);
        producer->display = EGL_NO_DISPLAY;
    }
}

int advc_egl_surface_producer_create(void *native_window, uint32_t width,
                                     uint32_t height,
                                     struct advc_egl_surface_producer **producer) {
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_NONE,
    };
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
    };
    struct advc_egl_surface_producer *created;
    EGLConfig config;
    EGLint count = 0;
    EGLint surface_width = 0;
    EGLint surface_height = 0;
    const char *failure_stage = "display_initialize";
    if (producer == NULL || native_window == NULL || width == 0 || height == 0)
        return -1;
    *producer = NULL;
    created = (struct advc_egl_surface_producer *)calloc(1, sizeof(*created));
    if (created == NULL) return -1;
    created->display = EGL_NO_DISPLAY;
    created->context = EGL_NO_CONTEXT;
    created->surface = EGL_NO_SURFACE;
    for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i)
        created->import_cache[i].completion_fence_fd = -1;
    for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i)
        created->ahb_cache[i].completion_fence_fd = -1;
    created->display = acquire_display();
    if (created->display == EGL_NO_DISPLAY) goto fail;
    failure_stage = "bind_gles_api";
    if (!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;
    failure_stage = "choose_recordable_config";
    if (!eglChooseConfig(created->display, config_attributes, &config, 1, &count) ||
        count != 1) goto fail;
    failure_stage = "create_context";
    created->context = eglCreateContext(created->display, config, EGL_NO_CONTEXT,
                                        context_attributes);
    if (created->context == EGL_NO_CONTEXT) goto fail;
    failure_stage = "create_window_surface";
    created->surface = eglCreateWindowSurface(
        created->display, config, (EGLNativeWindowType)native_window, NULL);
    if (created->surface == EGL_NO_SURFACE) goto fail;
    failure_stage = "make_current";
    if (!eglMakeCurrent(created->display, created->surface, created->surface,
                        created->context)) goto fail;
    failure_stage = "query_surface_width";
    if (!eglQuerySurface(created->display, created->surface, EGL_WIDTH,
                         &surface_width)) goto fail;
    failure_stage = "query_surface_height";
    if (!eglQuerySurface(created->display, created->surface, EGL_HEIGHT,
                         &surface_height)) goto fail;
    failure_stage = "surface_geometry";
    if (surface_width != (EGLint)width || surface_height != (EGLint)height)
        goto fail;
    failure_stage = "presentation_time_extension";
    created->presentation_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)
        eglGetProcAddress("eglPresentationTimeANDROID");
    if (created->presentation_time == NULL) goto fail;
    (void)eglSwapInterval(created->display, 0);
    created->width = width;
    created->height = height;
    *producer = created;
    return 0;

fail:
    egl_debug_failure(failure_stage, eglGetError());
    if (getenv("ADVC_DEBUG") != NULL &&
        (surface_width != 0 || surface_height != 0))
        fprintf(stderr, "advc-surface-egl: requested=%ux%u actual=%dx%d\n",
                width, height, surface_width, surface_height);
    destroy_egl(created);
    free(created);
    return -1;
}

int advc_egl_surface_producer_render(struct advc_egl_surface_producer *producer,
                                     uint64_t frame_sequence,
                                     int64_t presentation_time_ns) {
    float red;
    float green;
    float blue;
    if (producer == NULL || presentation_time_ns < 0)
        return -1;
    if (!eglMakeCurrent(producer->display, producer->surface, producer->surface,
                        producer->context)) {
        egl_debug_failure("render_make_current", eglGetError());
        return -1;
    }
    red = (float)((frame_sequence * 37u + 17u) & 255u) / 255.0f;
    green = (float)((frame_sequence * 67u + 53u) & 255u) / 255.0f;
    blue = (float)((frame_sequence * 97u + 101u) & 255u) / 255.0f;
    glViewport(0, 0, (GLsizei)producer->width, (GLsizei)producer->height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(red, green, blue, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            if (getenv("ADVC_DEBUG") != NULL)
                fprintf(stderr, "advc-surface-egl: stage=gl_clear gl_error=0x%x\n",
                        (unsigned int)gl_error);
            return -1;
        }
    }
    if (!producer->presentation_time(producer->display, producer->surface,
                                     (EGLnsecsANDROID)presentation_time_ns)) {
        egl_debug_failure("presentation_time", eglGetError());
        return -1;
    }
    if (!eglSwapBuffers(producer->display, producer->surface)) {
        egl_debug_failure("swap_buffers", eglGetError());
        return -1;
    }
    return 0;
}

int advc_egl_surface_producer_render_ahb(
    struct advc_egl_surface_producer *producer, void *hardware_buffer,
    uint64_t frame_sequence, int64_t presentation_time_ns,
    int acquire_fence_fd, int *release_fence_fd) {
    static const GLfloat positions[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    static const GLfloat texcoords[] = {0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f};
    static const EGLint acquire_template[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, -1, EGL_NONE,
    };
    static const EGLint release_attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
                                                 EGL_NO_NATIVE_FENCE_FD_ANDROID,
                                                 EGL_NONE};
    struct advc_egl_ahb_cache_entry *cache_entry = NULL;
    AHardwareBuffer *buffer = (AHardwareBuffer *)hardware_buffer;
    AHardwareBuffer *local_owned_buffer = NULL;
    AHardwareBuffer_Desc descriptor;
    EGLImageKHR local_image = EGL_NO_IMAGE_KHR;
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    EGLint acquire_attributes[3];
    GLuint local_texture = 0;
    GLuint texture = 0;
    uint64_t buffer_id = 0;
    int internal_fence_fd = -1;
    int get_id_result = -1;
    int use_async_cache = 0;
    int result = -1;
    int gpu_submitted = 0;
    (void)frame_sequence;
    if (release_fence_fd == NULL) goto done;
    *release_fence_fd = -1;
    if (producer == NULL || hardware_buffer == NULL ||
        presentation_time_ns < 0 || acquire_fence_fd < -1)
        goto done;
    if (resolve_ahb_runtime(producer) < 0) goto done;
    memset(&descriptor, 0, sizeof(descriptor));
    AHardwareBuffer_describe(buffer, &descriptor);
    if (producer->get_ahb_id != NULL)
        get_id_result = producer->get_ahb_id(buffer, &buffer_id);
    use_async_cache = advc_egl_ahb_async_cache_allowed(
        producer->native_fence_async_supported,
        producer->get_ahb_id != NULL, get_id_result);

    if (acquire_fence_fd >= 0) {
        if (producer->native_fence_async_supported) {
            memcpy(acquire_attributes, acquire_template,
                   sizeof(acquire_attributes));
            acquire_attributes[1] = acquire_fence_fd;
            acquire_fence_fd = -1; /* EGL owns the attempted native import. */
            sync = producer->create_sync(
                producer->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                acquire_attributes);
            if (sync == EGL_NO_SYNC_KHR ||
                !producer->wait_sync(producer->display, sync, 0))
                goto done;
            (void)producer->destroy_sync(producer->display, sync);
            sync = EGL_NO_SYNC_KHR;
        } else {
            if (wait_native_fence_fd(acquire_fence_fd) < 0) goto done;
            close(acquire_fence_fd);
            acquire_fence_fd = -1;
        }
    }
    if (use_async_cache) {
        if (get_cached_ahb(producer, buffer, &descriptor, buffer_id,
                           &cache_entry) < 0 || cache_entry == NULL)
            goto done;
        texture = cache_entry->texture;
    } else {
        /*
         * API < 31 or a failed system-wide ID lookup has no lifetime-safe key.
         * Never trust a recyclable wrapper pointer: retain the exact buffer,
         * import it once for this call, and quiesce before destroying it.
         */
        if (create_ahb_texture(producer, buffer, &local_image, &local_texture,
                               &local_owned_buffer) < 0)
            goto done;
        texture = local_texture;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glViewport(0, 0, (GLsizei)producer->width, (GLsizei)producer->height);
    glUseProgram(producer->ahb_program);
    glUniform1i(producer->ahb_sampler, 0);
    glVertexAttribPointer((GLuint)producer->ahb_position, 2, GL_FLOAT, GL_FALSE, 0,
                          positions);
    glVertexAttribPointer((GLuint)producer->ahb_texcoord, 2, GL_FLOAT, GL_FALSE, 0,
                          texcoords);
    glEnableVertexAttribArray((GLuint)producer->ahb_position);
    glEnableVertexAttribArray((GLuint)producer->ahb_texcoord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gpu_submitted = 1;
    if (glGetError() != GL_NO_ERROR ||
        !producer->presentation_time(producer->display, producer->surface,
                                     (EGLnsecsANDROID)presentation_time_ns) ||
        !eglSwapBuffers(producer->display, producer->surface))
        goto done;
    if (use_async_cache) {
        sync = producer->create_sync(producer->display,
                                     EGL_SYNC_NATIVE_FENCE_ANDROID,
                                     release_attributes);
        if (sync == EGL_NO_SYNC_KHR) goto done;
        glFlush();
        *release_fence_fd = producer->dup_fence(producer->display, sync);
        if (*release_fence_fd < 0) goto done;
        internal_fence_fd = fcntl(*release_fence_fd, F_DUPFD_CLOEXEC, 0);
        if (internal_fence_fd < 0) goto done;
        if (cache_entry->completion_fence_fd >= 0)
            close(cache_entry->completion_fence_fd);
        cache_entry->completion_fence_fd = internal_fence_fd;
        cache_entry->completion_serial = ++producer->submission_serial;
        internal_fence_fd = -1;
    } else {
        /* Exact synchronous fallback: no unobservable GPU read on return. */
        glFinish();
        if (glGetError() != GL_NO_ERROR) goto done;
    }
    result = 0;
done:
    if (result != 0 && gpu_submitted) glFinish();
    if (sync != EGL_NO_SYNC_KHR && producer != NULL &&
        producer->destroy_sync != NULL)
        (void)producer->destroy_sync(producer->display, sync);
    if (local_texture != 0) glDeleteTextures(1, &local_texture);
    if (local_image != EGL_NO_IMAGE_KHR && producer != NULL &&
        producer->destroy_image != NULL)
        (void)producer->destroy_image(producer->display, local_image);
    if (local_owned_buffer != NULL)
        AHardwareBuffer_release(local_owned_buffer);
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    if (internal_fence_fd >= 0) close(internal_fence_fd);
    if (result != 0 && release_fence_fd != NULL && *release_fence_fd >= 0) {
        close(*release_fence_fd);
        *release_fence_fd = -1;
    }
    return result;
}

static void resolve_native_fence_runtime(
    struct advc_egl_surface_producer *producer,
    const char *egl_extensions) {
    if (producer == NULL || producer->native_fence_async_supported ||
        !extension_present(egl_extensions, "EGL_ANDROID_native_fence_sync") ||
        !extension_present(egl_extensions, "EGL_KHR_wait_sync"))
        return;
    producer->create_sync =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    producer->destroy_sync =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    producer->wait_sync =
        (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    producer->dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
        eglGetProcAddress("eglDupNativeFenceFDANDROID");
    producer->native_fence_async_supported =
        producer->create_sync != NULL && producer->destroy_sync != NULL &&
        producer->wait_sync != NULL && producer->dup_fence != NULL;
}

static int resolve_ahb_runtime(
    struct advc_egl_surface_producer *producer) {
    const char *egl_extensions;
    const char *gl_extensions;
    void *get_id_symbol;
    if (producer == NULL ||
        !eglMakeCurrent(producer->display, producer->surface,
                        producer->surface, producer->context))
        return -1;
    if (producer->ahb_runtime_checked)
        return producer->ahb_import_supported ? 0 : -1;
    producer->ahb_runtime_checked = 1;
    egl_extensions = eglQueryString(producer->display, EGL_EXTENSIONS);
    gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extension_present(egl_extensions,
                           "EGL_ANDROID_get_native_client_buffer") ||
        !extension_present(egl_extensions, "EGL_ANDROID_image_native_buffer") ||
        !extension_present(egl_extensions, "EGL_KHR_image_base") ||
        !extension_present(gl_extensions, "GL_OES_EGL_image_external"))
        return -1;
    producer->get_client_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    producer->create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    producer->destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    producer->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (producer->get_client_buffer == NULL || producer->create_image == NULL ||
        producer->destroy_image == NULL ||
        producer->image_target_texture == NULL ||
        ensure_ahb_program(producer) < 0)
        return -1;
    producer->ahb_import_supported = 1;
    resolve_native_fence_runtime(producer, egl_extensions);

    /*
     * AHardwareBuffer_getId is public only from API 31. Resolve it lazily so
     * older Android releases retain the correct synchronous path instead of
     * gaining a hard loader dependency on a missing symbol.
     */
    get_id_symbol = dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId");
    if (get_id_symbol != NULL)
        memcpy(&producer->get_ahb_id, &get_id_symbol, sizeof(get_id_symbol));
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr,
                "advc-surface-egl: AHB cache=%s identity=%s limit=%u\n",
                producer->native_fence_async_supported &&
                        producer->get_ahb_id != NULL ?
                    "native-fence" : "synchronous",
                producer->get_ahb_id != NULL ? "system-id" : "unavailable",
                (unsigned int)ADVC_EGL_AHB_CACHE_SIZE);
    return 0;
}

static int resolve_dmabuf_runtime(
    struct advc_egl_surface_producer *producer) {
    const char *egl_extensions;
    const char *gl_extensions;
    if (producer == NULL ||
        !eglMakeCurrent(producer->display, producer->surface,
                        producer->surface, producer->context))
        return -1;
    if (producer->dmabuf_runtime_checked)
        return producer->dmabuf_import_supported ? 0 : -1;
    producer->dmabuf_runtime_checked = 1;
    egl_extensions = eglQueryString(producer->display, EGL_EXTENSIONS);
    gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    /*
     * Requiring the modifier extension even for LINEAR is deliberate: the
     * v1.5 contract never permits an implicit modifier, and registration must
     * prove that the exact explicit tuple reaches the Android importer.
     */
    if (!extension_present(egl_extensions, "EGL_EXT_image_dma_buf_import") ||
        !extension_present(egl_extensions,
                           "EGL_EXT_image_dma_buf_import_modifiers") ||
        !extension_present(egl_extensions, "EGL_KHR_image_base") ||
        !extension_present(gl_extensions, "GL_OES_EGL_image_external")) {
        if (getenv("ADVC_DEBUG") != NULL)
            fprintf(stderr,
                    "advc-surface-egl: dma-buf extensions import=%d "
                    "modifiers=%d image_base=%d external=%d\n",
                    extension_present(egl_extensions,
                                      "EGL_EXT_image_dma_buf_import"),
                    extension_present(
                        egl_extensions,
                        "EGL_EXT_image_dma_buf_import_modifiers"),
                    extension_present(egl_extensions, "EGL_KHR_image_base"),
                    extension_present(gl_extensions,
                                      "GL_OES_EGL_image_external"));
        return -1;
    }
    producer->create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    producer->destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    producer->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (producer->create_image == NULL || producer->destroy_image == NULL ||
        producer->image_target_texture == NULL ||
        ensure_ahb_program(producer) < 0)
        return -1;
    producer->dmabuf_import_supported = 1;

    resolve_native_fence_runtime(producer, egl_extensions);
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr,
                "advc-surface-egl: dma-buf cache=%s limit=%u\n",
                producer->native_fence_async_supported ? "native-fence" :
                                                         "synchronous",
                (unsigned int)ADVC_EGL_IMPORT_CACHE_SIZE);
    return 0;
}

static int descriptor_metadata_equal(
    const struct advc_dmabuf_descriptor *left,
    const struct advc_dmabuf_descriptor *right) {
    if (left == NULL || right == NULL ||
        left->buffer_id != right->buffer_id || left->width != right->width ||
        left->height != right->height ||
        left->drm_fourcc != right->drm_fourcc ||
        left->explicit_flags != right->explicit_flags ||
        left->drm_modifier != right->drm_modifier ||
        left->crop_left != right->crop_left ||
        left->crop_top != right->crop_top ||
        left->crop_width != right->crop_width ||
        left->crop_height != right->crop_height ||
        left->object_count != right->object_count ||
        left->plane_count != right->plane_count ||
        left->color_primaries != right->color_primaries ||
        left->color_transfer != right->color_transfer ||
        left->color_matrix != right->color_matrix ||
        left->color_range != right->color_range ||
        left->chroma_horizontal != right->chroma_horizontal ||
        left->chroma_vertical != right->chroma_vertical)
        return 0;
    for (uint32_t i = 0; i < left->object_count; ++i) {
        if (left->objects[i].size != right->objects[i].size) return 0;
    }
    for (uint32_t i = 0; i < left->plane_count; ++i) {
        if (left->planes[i].object_index != right->planes[i].object_index ||
            left->planes[i].offset != right->planes[i].offset ||
            left->planes[i].pitch != right->planes[i].pitch)
            return 0;
    }
    return 1;
}

static int fd_identities_equal(
    const struct stat left[ADVC_MAX_DMABUF_OBJECTS],
    const struct stat right[ADVC_MAX_DMABUF_OBJECTS], uint32_t count) {
    if (left == NULL || right == NULL || count > ADVC_MAX_DMABUF_OBJECTS)
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (left[i].st_dev != right[i].st_dev ||
            left[i].st_ino != right[i].st_ino ||
            left[i].st_rdev != right[i].st_rdev ||
            left[i].st_size != right[i].st_size)
            return 0;
    }
    return 1;
}

static void copy_descriptor_metadata(
    struct advc_dmabuf_descriptor *destination,
    const struct advc_dmabuf_descriptor *source) {
    *destination = *source;
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        destination->objects[i].fd = -1;
}

static int64_t monotonic_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    if (now.tv_sec > (time_t)(INT64_MAX / 1000)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_native_fence_fd(int fd) {
    struct pollfd item;
    int64_t deadline;
    int64_t now;
    int timeout;
    int result;
    if (fd < 0) return 0;
    now = monotonic_now_ms();
    if (now < 0 || now > INT64_MAX - ADVC_EGL_FENCE_TIMEOUT_MS) return -1;
    deadline = now + ADVC_EGL_FENCE_TIMEOUT_MS;
    memset(&item, 0, sizeof(item));
    item.fd = fd;
    item.events = POLLIN;
    for (;;) {
        now = monotonic_now_ms();
        if (now < 0 || now >= deadline) return -1;
        timeout = (int)(deadline - now);
        result = poll(&item, 1, timeout);
        if (result > 0)
            return (item.revents & POLLIN) != 0 ? 0 : -1;
        if (result == 0) return -1;
        if (errno != EINTR) return -1;
    }
}

static int ahb_descriptors_equal(const AHardwareBuffer_Desc *left,
                                 const AHardwareBuffer_Desc *right) {
    return left != NULL && right != NULL && left->width == right->width &&
           left->height == right->height && left->layers == right->layers &&
           left->format == right->format && left->usage == right->usage &&
           left->stride == right->stride;
}

static void clear_cache_entry(
    struct advc_egl_surface_producer *producer,
    struct advc_egl_import_cache_entry *entry, int have_context) {
    if (entry == NULL) return;
    if (entry->completion_fence_fd >= 0)
        close(entry->completion_fence_fd);
    if (have_context && entry->texture != 0)
        glDeleteTextures(1, &entry->texture);
    if (entry->image != EGL_NO_IMAGE_KHR && producer != NULL &&
        producer->destroy_image != NULL)
        (void)producer->destroy_image(producer->display, entry->image);
    memset(entry, 0, sizeof(*entry));
    entry->completion_fence_fd = -1;
}

static int quiesce_cache_entry(
    struct advc_egl_surface_producer *producer,
    struct advc_egl_import_cache_entry *entry) {
    if (producer == NULL || entry == NULL || !entry->occupied) return 0;
    if (entry->completion_fence_fd >= 0 &&
        wait_native_fence_fd(entry->completion_fence_fd) < 0) {
        /* A broken/missing sync_file never permits unsafe destruction. */
        glFinish();
        if (glGetError() != GL_NO_ERROR) return -1;
    }
    return 0;
}

static int destroy_cache_entry(
    struct advc_egl_surface_producer *producer,
    struct advc_egl_import_cache_entry *entry) {
    if (quiesce_cache_entry(producer, entry) < 0) return -1;
    clear_cache_entry(producer, entry, 1);
    return 0;
}

static void clear_ahb_cache_entry(
    struct advc_egl_surface_producer *producer,
    struct advc_egl_ahb_cache_entry *entry, int have_context) {
    if (entry == NULL) return;
    if (entry->completion_fence_fd >= 0)
        close(entry->completion_fence_fd);
    if (have_context && entry->texture != 0)
        glDeleteTextures(1, &entry->texture);
    if (entry->image != EGL_NO_IMAGE_KHR && producer != NULL &&
        producer->destroy_image != NULL)
        (void)producer->destroy_image(producer->display, entry->image);
    if (entry->buffer != NULL) AHardwareBuffer_release(entry->buffer);
    memset(entry, 0, sizeof(*entry));
    entry->completion_fence_fd = -1;
}

static int destroy_ahb_cache_entry(
    struct advc_egl_surface_producer *producer,
    struct advc_egl_ahb_cache_entry *entry) {
    if (producer == NULL || entry == NULL || !entry->occupied) return 0;
    if (entry->completion_fence_fd >= 0 &&
        wait_native_fence_fd(entry->completion_fence_fd) < 0) {
        glFinish();
        if (glGetError() != GL_NO_ERROR) return -1;
    }
    clear_ahb_cache_entry(producer, entry, 1);
    return 0;
}

static void abandon_import_caches(
    struct advc_egl_surface_producer *producer) {
    if (producer == NULL) return;
    for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i)
        clear_cache_entry(producer, &producer->import_cache[i], 0);
    for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i)
        clear_ahb_cache_entry(producer, &producer->ahb_cache[i], 0);
}

static int discard_import_caches(
    struct advc_egl_surface_producer *producer) {
    int latest_fence_fd = -1;
    uint64_t latest_serial = 0;
    if (producer == NULL ||
        !eglMakeCurrent(producer->display, producer->surface,
                        producer->surface, producer->context))
        return -1;
    for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i) {
        struct advc_egl_import_cache_entry *entry = &producer->import_cache[i];
        if (entry->completion_fence_fd >= 0 &&
            (latest_fence_fd < 0 ||
             entry->completion_serial > latest_serial)) {
            latest_fence_fd = entry->completion_fence_fd;
            latest_serial = entry->completion_serial;
        }
    }
    for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i) {
        struct advc_egl_ahb_cache_entry *entry = &producer->ahb_cache[i];
        if (entry->completion_fence_fd >= 0 &&
            (latest_fence_fd < 0 ||
             entry->completion_serial > latest_serial)) {
            latest_fence_fd = entry->completion_fence_fd;
            latest_serial = entry->completion_serial;
        }
    }
    /* One-context ordering means the newest fence covers every older draw. */
    if (latest_fence_fd >= 0 && wait_native_fence_fd(latest_fence_fd) < 0) {
        glFinish();
        if (glGetError() != GL_NO_ERROR) return -1;
    }
    for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i)
        clear_cache_entry(producer, &producer->import_cache[i], 1);
    for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i)
        clear_ahb_cache_entry(producer, &producer->ahb_cache[i], 1);
    return 0;
}

static int create_ahb_texture(
    struct advc_egl_surface_producer *producer, AHardwareBuffer *buffer,
    EGLImageKHR *image, GLuint *texture, AHardwareBuffer **owned_buffer) {
    static const EGLint image_attributes[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, EGL_NONE,
    };
    EGLClientBuffer client_buffer;
    if (producer == NULL || buffer == NULL || image == NULL || texture == NULL ||
        owned_buffer == NULL || producer->get_client_buffer == NULL ||
        producer->create_image == NULL || producer->destroy_image == NULL ||
        producer->image_target_texture == NULL)
        return -1;
    *image = EGL_NO_IMAGE_KHR;
    *texture = 0;
    *owned_buffer = NULL;
    AHardwareBuffer_acquire(buffer);
    *owned_buffer = buffer;
    client_buffer = producer->get_client_buffer(*owned_buffer);
    if (client_buffer == NULL) goto fail;
    *image = producer->create_image(
        producer->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        client_buffer, image_attributes);
    if (*image == EGL_NO_IMAGE_KHR) goto fail;
    glGenTextures(1, texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, *texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    producer->image_target_texture(GL_TEXTURE_EXTERNAL_OES, *image);
    if (*texture == 0 || glGetError() != GL_NO_ERROR) goto fail;
    return 0;

fail:
    if (*texture != 0) glDeleteTextures(1, texture);
    *texture = 0;
    if (*image != EGL_NO_IMAGE_KHR)
        (void)producer->destroy_image(producer->display, *image);
    *image = EGL_NO_IMAGE_KHR;
    if (*owned_buffer != NULL) AHardwareBuffer_release(*owned_buffer);
    *owned_buffer = NULL;
    return -1;
}

static int get_cached_ahb(
    struct advc_egl_surface_producer *producer, AHardwareBuffer *buffer,
    const AHardwareBuffer_Desc *descriptor, uint64_t buffer_id,
    struct advc_egl_ahb_cache_entry **entry_out) {
    AHardwareBuffer *owned_buffer = NULL;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    uint32_t target = UINT32_MAX;
    uint64_t oldest_serial = UINT64_MAX;
    if (producer == NULL || buffer == NULL || descriptor == NULL ||
        entry_out == NULL)
        return -1;
    *entry_out = NULL;
    for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i) {
        struct advc_egl_ahb_cache_entry *entry = &producer->ahb_cache[i];
        if (!entry->occupied || entry->buffer_id != buffer_id) continue;
        if (ahb_descriptors_equal(&entry->descriptor, descriptor)) {
            entry->last_use_serial = ++producer->cache_serial;
            *entry_out = entry;
            return 0;
        }
        /* A reused system ID with changed metadata must replace, never alias. */
        target = i;
        break;
    }
    if (target == UINT32_MAX) {
        for (uint32_t i = 0; i < ADVC_EGL_AHB_CACHE_SIZE; ++i) {
            struct advc_egl_ahb_cache_entry *entry = &producer->ahb_cache[i];
            if (!entry->occupied) {
                target = i;
                break;
            }
            if (entry->last_use_serial < oldest_serial) {
                target = i;
                oldest_serial = entry->last_use_serial;
            }
        }
    }
    if (target == UINT32_MAX ||
        create_ahb_texture(producer, buffer, &image, &texture,
                           &owned_buffer) < 0)
        return -1;
    if (destroy_ahb_cache_entry(producer, &producer->ahb_cache[target]) < 0) {
        glDeleteTextures(1, &texture);
        (void)producer->destroy_image(producer->display, image);
        AHardwareBuffer_release(owned_buffer);
        return -1;
    }
    producer->ahb_cache[target].buffer = owned_buffer;
    producer->ahb_cache[target].image = image;
    producer->ahb_cache[target].texture = texture;
    producer->ahb_cache[target].descriptor = *descriptor;
    producer->ahb_cache[target].buffer_id = buffer_id;
    producer->ahb_cache[target].last_use_serial = ++producer->cache_serial;
    producer->ahb_cache[target].completion_fence_fd = -1;
    producer->ahb_cache[target].occupied = 1;
    *entry_out = &producer->ahb_cache[target];
    return 0;
}

static int create_dmabuf_texture(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, EGLImageKHR *image,
    GLuint *texture) {
    if (producer == NULL || descriptor == NULL || image == NULL ||
        texture == NULL || producer->create_image == NULL ||
        producer->image_target_texture == NULL)
        return -1;
    *image = create_dmabuf_image(producer, descriptor, producer->create_image);
    *texture = 0;
    if (*image == EGL_NO_IMAGE_KHR) return -1;
    glGenTextures(1, texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, *texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    producer->image_target_texture(GL_TEXTURE_EXTERNAL_OES, *image);
    if (*texture == 0 || glGetError() != GL_NO_ERROR) {
        if (*texture != 0) glDeleteTextures(1, texture);
        *texture = 0;
        (void)producer->destroy_image(producer->display, *image);
        *image = EGL_NO_IMAGE_KHR;
        return -1;
    }
    return 0;
}

static int get_cached_dmabuf(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor,
    struct advc_egl_import_cache_entry **entry_out) {
    struct stat identities[ADVC_MAX_DMABUF_OBJECTS];
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    uint32_t target = UINT32_MAX;
    uint64_t oldest_serial = UINT64_MAX;
    if (producer == NULL || descriptor == NULL || entry_out == NULL ||
        descriptor->object_count == 0 ||
        descriptor->object_count > ADVC_MAX_DMABUF_OBJECTS)
        return -1;
    memset(identities, 0, sizeof(identities));
    for (uint32_t i = 0; i < descriptor->object_count; ++i) {
        if (fstat(descriptor->objects[i].fd, &identities[i]) < 0) return -1;
    }
    *entry_out = NULL;
    for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i) {
        struct advc_egl_import_cache_entry *entry = &producer->import_cache[i];
        if (!entry->occupied || entry->descriptor.buffer_id != descriptor->buffer_id)
            continue;
        if (descriptor_metadata_equal(&entry->descriptor, descriptor) &&
            fd_identities_equal(entry->fd_identities, identities,
                                descriptor->object_count)) {
            entry->last_use_serial = ++producer->cache_serial;
            *entry_out = entry;
            return 0;
        }
        target = i;
        break;
    }
    if (target == UINT32_MAX) {
        for (uint32_t i = 0; i < ADVC_EGL_IMPORT_CACHE_SIZE; ++i) {
            struct advc_egl_import_cache_entry *entry =
                &producer->import_cache[i];
            if (!entry->occupied) {
                target = i;
                break;
            }
            if (entry->last_use_serial < oldest_serial) {
                target = i;
                oldest_serial = entry->last_use_serial;
            }
        }
    }
    if (target == UINT32_MAX ||
        create_dmabuf_texture(producer, descriptor, &image, &texture) < 0)
        return -1;
    if (destroy_cache_entry(producer, &producer->import_cache[target]) < 0) {
        glDeleteTextures(1, &texture);
        (void)producer->destroy_image(producer->display, image);
        return -1;
    }
    producer->import_cache[target].image = image;
    producer->import_cache[target].texture = texture;
    copy_descriptor_metadata(&producer->import_cache[target].descriptor,
                             descriptor);
    memcpy(producer->import_cache[target].fd_identities, identities,
           sizeof(identities));
    producer->import_cache[target].last_use_serial = ++producer->cache_serial;
    producer->import_cache[target].completion_fence_fd = -1;
    producer->import_cache[target].occupied = 1;
    *entry_out = &producer->import_cache[target];
    return 0;
}

int advc_egl_surface_producer_validate_dmabuf(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor) {
    struct advc_egl_import_cache_entry *entry = NULL;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    if (!dmabuf_descriptor_supported(producer, descriptor) ||
        resolve_dmabuf_runtime(producer) < 0)
        return -1;
    if (producer->native_fence_async_supported)
        return get_cached_dmabuf(producer, descriptor, &entry);
    if (create_dmabuf_texture(producer, descriptor, &image, &texture) < 0) {
        egl_debug_failure("validate_dmabuf_import", eglGetError());
        return -1;
    }
    glDeleteTextures(1, &texture);
    if (!producer->destroy_image(producer->display, image)) {
        egl_debug_failure("validate_dmabuf_destroy", eglGetError());
        return -1;
    }
    return 0;
}

int advc_egl_surface_producer_render_dmabuf(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t frame_sequence,
    int64_t presentation_time_ns, int acquire_fence_fd,
    int *release_fence_fd) {
    static const GLfloat positions[] = {
        -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f,
    };
    static const EGLint acquire_template[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, -1, EGL_NONE,
    };
    static const EGLint release_attributes[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        EGL_NO_NATIVE_FENCE_FD_ANDROID,
        EGL_NONE,
    };
    struct advc_egl_import_cache_entry *cache_entry = NULL;
    EGLImageKHR local_image = EGL_NO_IMAGE_KHR;
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    EGLint acquire_attributes[3];
    GLuint local_texture = 0;
    GLuint texture;
    GLfloat texcoords[8];
    float left;
    float right;
    float top;
    float bottom;
    int internal_fence_fd = -1;
    int use_async_cache = 0;
    int result = -1;
    int gpu_submitted = 0;
    (void)frame_sequence;

    if (release_fence_fd != NULL) *release_fence_fd = -1;
    if (producer == NULL || descriptor == NULL || release_fence_fd == NULL ||
        presentation_time_ns < 0 || acquire_fence_fd < -1 ||
        !dmabuf_descriptor_supported(producer, descriptor))
        goto done;
    if (resolve_dmabuf_runtime(producer) < 0) goto done;
    use_async_cache = producer->native_fence_async_supported;

    if (acquire_fence_fd >= 0) {
        if (use_async_cache) {
            memcpy(acquire_attributes, acquire_template,
                   sizeof(acquire_attributes));
            acquire_attributes[1] = acquire_fence_fd;
            acquire_fence_fd = -1; /* EGL owns the attempted native import. */
            sync = producer->create_sync(
                producer->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                acquire_attributes);
            if (sync == EGL_NO_SYNC_KHR ||
                !producer->wait_sync(producer->display, sync, 0))
                goto done;
            (void)producer->destroy_sync(producer->display, sync);
            sync = EGL_NO_SYNC_KHR;
        } else {
            /* No native-fence EGL extension: bounded CPU wait, then quiesce. */
            if (wait_native_fence_fd(acquire_fence_fd) < 0) goto done;
            close(acquire_fence_fd);
            acquire_fence_fd = -1;
        }
    }
    if (use_async_cache) {
        if (get_cached_dmabuf(producer, descriptor, &cache_entry) < 0 ||
            cache_entry == NULL)
            goto done;
        texture = cache_entry->texture;
    } else {
        if (create_dmabuf_texture(producer, descriptor, &local_image,
                                  &local_texture) < 0)
            goto done;
        texture = local_texture;
    }

    left = (float)descriptor->crop_left / (float)descriptor->width;
    right = (float)(descriptor->crop_left + descriptor->crop_width) /
            (float)descriptor->width;
    top = (float)descriptor->crop_top / (float)descriptor->height;
    bottom = (float)(descriptor->crop_top + descriptor->crop_height) /
             (float)descriptor->height;
    /* EGL images use the same vertical flip as the proven AHB route. */
    texcoords[0] = left;  texcoords[1] = bottom;
    texcoords[2] = right; texcoords[3] = bottom;
    texcoords[4] = left;  texcoords[5] = top;
    texcoords[6] = right; texcoords[7] = top;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glViewport(0, 0, (GLsizei)producer->width, (GLsizei)producer->height);
    glUseProgram(producer->ahb_program);
    glUniform1i(producer->ahb_sampler, 0);
    glVertexAttribPointer((GLuint)producer->ahb_position, 2, GL_FLOAT, GL_FALSE,
                          0, positions);
    glVertexAttribPointer((GLuint)producer->ahb_texcoord, 2, GL_FLOAT, GL_FALSE,
                          0, texcoords);
    glEnableVertexAttribArray((GLuint)producer->ahb_position);
    glEnableVertexAttribArray((GLuint)producer->ahb_texcoord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gpu_submitted = 1;
    if (glGetError() != GL_NO_ERROR ||
        !producer->presentation_time(producer->display, producer->surface,
                                     (EGLnsecsANDROID)presentation_time_ns) ||
        !eglSwapBuffers(producer->display, producer->surface))
        goto done;

    if (use_async_cache) {
        sync = producer->create_sync(producer->display,
                                     EGL_SYNC_NATIVE_FENCE_ANDROID,
                                     release_attributes);
        if (sync == EGL_NO_SYNC_KHR) goto done;
        glFlush();
        *release_fence_fd = producer->dup_fence(producer->display, sync);
        if (*release_fence_fd < 0) goto done;
        internal_fence_fd =
            fcntl(*release_fence_fd, F_DUPFD_CLOEXEC, 0);
        if (internal_fence_fd < 0) goto done;
        /*
         * The newest fence covers every older draw in this one EGL context.
         * Retaining one owned duplicate per registered buffer therefore
         * bounds teardown state without serializing each frame.
         */
        if (cache_entry->completion_fence_fd >= 0)
            close(cache_entry->completion_fence_fd);
        cache_entry->completion_fence_fd = internal_fence_fd;
        cache_entry->completion_serial = ++producer->submission_serial;
        internal_fence_fd = -1;
    } else {
        /* Exact fail-safe fallback: no observable GPU read remains on return. */
        glFinish();
        if (glGetError() != GL_NO_ERROR) goto done;
    }
    result = 0;

done:
    if (result != 0 && gpu_submitted) glFinish();
    if (sync != EGL_NO_SYNC_KHR && producer != NULL &&
        producer->destroy_sync != NULL)
        (void)producer->destroy_sync(producer->display, sync);
    if (local_texture != 0) glDeleteTextures(1, &local_texture);
    if (local_image != EGL_NO_IMAGE_KHR && producer != NULL &&
        producer->destroy_image != NULL)
        (void)producer->destroy_image(producer->display, local_image);
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    if (internal_fence_fd >= 0) close(internal_fence_fd);
    if (result != 0 && release_fence_fd != NULL && *release_fence_fd >= 0) {
        close(*release_fence_fd);
        *release_fence_fd = -1;
    }
    return result;
}

int advc_egl_surface_producer_discard_import_caches(
    struct advc_egl_surface_producer *producer) {
    return discard_import_caches(producer);
}

void advc_egl_surface_producer_destroy(struct advc_egl_surface_producer *producer) {
    if (producer == NULL) return;
    destroy_egl(producer);
    free(producer);
}
