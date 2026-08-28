#include "android_ahb_socket_probe.h"

#if !defined(__ANDROID__)
#error "android_ahb_socket_probe.c is Android-only"
#endif

#include "advc/broker.h"
#include "advc/client.h"
#include "advc/session_engine.h"
#include "ahb_transport.h"
#include "android_codec_backend.h"
#include "surface_encode_probe.h"

#include <android/hardware_buffer.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct socket_probe_server {
    int fd;
    int ok;
};

static void *serve_probe(void *opaque) {
    struct socket_probe_server *server = (struct socket_probe_server *)opaque;
    struct advc_session_engine *engine =
        advc_session_engine_create(advc_android_codec_backend_ops(), NULL);
    struct advc_broker_provider provider;
    unsigned int handled = 0;
    if (engine == NULL) return NULL;
    memset(&provider, 0, sizeof(provider));
    provider.userdata = engine;
    provider.feature_bits = ADVC_FEATURE_MEMFD | ADVC_FEATURE_ENCODE |
                            ADVC_FEATURE_ANDROID_AHB_SURFACE;
    provider.handle_codec_request = advc_session_engine_handle;
    provider.after_reply = advc_session_engine_after_reply;
    while (advc_broker_handle_once(server->fd, &provider) == 0) ++handled;
    server->ok = handled >= 5;
    advc_session_engine_destroy(engine);
    close(server->fd);
    server->fd = -1;
    return NULL;
}

static int wait_fence(int fd) {
    struct pollfd item;
    int status;
    if (fd < 0) return 1;
    memset(&item, 0, sizeof(item));
    item.fd = fd;
    item.events = POLLIN;
    do {
        status = poll(&item, 1, 5000);
    } while (status < 0 && errno == EINTR);
    return status > 0 && (item.revents & (POLLIN | POLLHUP)) != 0;
}

static int run_socket_probe(void) {
    struct socket_probe_server server;
    struct advc_client_session_config config;
    struct advc_client_ahb_input input;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer *buffer = NULL;
    pthread_t thread;
    struct timeval timeout = {10, 0};
    uint64_t features = 0;
    uint32_t max_payload = 0;
    uint32_t session_id = 0;
    uint32_t detail = 0;
    void *pixels = NULL;
    int sockets[2] = {-1, -1};
    int acquire_fence = -1;
    int release_fence = -1;
    int thread_started = 0;
    int session_created = 0;
    int ok = 0;

    if (!advc_probe_android_ahb_surface() ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0)
        goto done;
    for (size_t i = 0; i < 2; ++i) {
        if (setsockopt(sockets[i], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) < 0 ||
            setsockopt(sockets[i], SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) < 0)
            goto done;
    }
    server.fd = sockets[1];
    server.ok = 0;
    if (pthread_create(&thread, NULL, serve_probe, &server) != 0) goto done;
    thread_started = 1;
    sockets[1] = -1;

    if (advc_client_hello(sockets[0], ADVC_FEATURE_MEMFD | ADVC_FEATURE_ENCODE |
                          ADVC_FEATURE_ANDROID_AHB_SURFACE,
                          &features, &max_payload) < 0 ||
        (features & ADVC_FEATURE_ANDROID_AHB_SURFACE) == 0 ||
        max_payload < ADVC_QUEUE_AHB_SIZE)
        goto done;
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 320;
    config.height = 240;
    config.bitrate = 500000;
    config.framerate_milli = 30000;
    config.transport = ADVC_TRANSPORT_ANDROID_AHB_SURFACE;
    if (advc_client_create_session(sockets[0], &config, &session_id, &detail) !=
        ADVC_STATUS_OK)
        goto done;
    session_created = 1;

    memset(&desc, 0, sizeof(desc));
    desc.width = config.width;
    desc.height = config.height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0 || buffer == NULL ||
        AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1,
                             NULL, &pixels) != 0 || pixels == NULL)
        goto done;
    AHardwareBuffer_describe(buffer, &desc);
    for (uint32_t y = 0; y < desc.height; ++y) {
        uint32_t *row = (uint32_t *)pixels + (size_t)y * desc.stride;
        for (uint32_t x = 0; x < desc.width; ++x)
            row[x] = UINT32_C(0xff3366cc);
    }
    pixels = NULL;
    if (AHardwareBuffer_unlock(buffer, &acquire_fence) != 0) goto done;
    memset(&input, 0, sizeof(input));
    input.native_buffer = buffer;
    input.width = desc.width;
    input.height = desc.height;
    input.format = desc.format;
    input.layers = desc.layers;
    input.usage = desc.usage;
    input.acquire_fence_fd = acquire_fence;
    if (advc_client_submit_ahb(sockets[0], session_id, &input,
                               advc_send_ahardwarebuffer_callback, NULL,
                               &release_fence, &detail) != ADVC_STATUS_OK ||
        !wait_fence(release_fence))
        goto done;
    if (advc_client_close_session(sockets[0], session_id, &detail) !=
        ADVC_STATUS_OK)
        goto done;
    session_created = 0;
    ok = 1;
done:
    if (pixels != NULL && buffer != NULL) (void)AHardwareBuffer_unlock(buffer, NULL);
    if (release_fence >= 0) close(release_fence);
    if (acquire_fence >= 0) close(acquire_fence);
    if (buffer != NULL) AHardwareBuffer_release(buffer);
    if (sockets[0] >= 0) {
        if (session_created)
            (void)advc_client_close_session(sockets[0], session_id, &detail);
        close(sockets[0]);
    }
    if (sockets[1] >= 0) close(sockets[1]);
    if (thread_started) {
        (void)pthread_join(thread, NULL);
        if (!server.ok) ok = 0;
    }
    return ok;
}

int advc_probe_android_ahb_socket(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int cached = -1;
    int result;
    if (pthread_mutex_lock(&lock) != 0) return 0;
    if (cached < 0) cached = run_socket_probe();
    result = cached;
    (void)pthread_mutex_unlock(&lock);
    return result;
}
