#define _GNU_SOURCE
#include "advc/broker.h"
#include "advc/session_engine.h"
#include "android_codec_backend.h"
#include "binder_pool.h"
#include "android_ahb_socket_probe.h"
#include "surface_encode_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int query_capabilities(void *unused, struct advc_capability_set *out,
                              char *error, size_t error_size) {
    (void)unused;
    return advc_probe_android_capabilities(out, error, error_size);
}

static int create_listener(const char *path) {
    struct sockaddr_un address;
    mode_t previous_umask;
    int bind_result;
    int fd;
    if (strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    unlink(path);
    /*
     * KernelSU's SELinux domain can create a socket in the shared chroot
     * runtime directory but may not be allowed to chmod that socket inode
     * afterwards.  Create it with the final mode atomically instead of
     * relying on a post-bind chmod.
     */
    previous_umask = umask(0117);
    bind_result = bind(fd, (struct sockaddr *)&address, sizeof(address));
    {
        int saved = errno;
        umask(previous_umask);
        errno = saved;
    }
    if (bind_result < 0) {
        int saved = errno;
        fprintf(stderr, "advc-broker: bind %s failed: %s\n", path,
                strerror(saved));
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    if (listen(fd, 16) < 0) {
        int saved = errno;
        fprintf(stderr, "advc-broker: listen %s failed: %s\n", path,
                strerror(saved));
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

static int peer_is_root(int fd) {
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           credentials.uid == 0;
}

static void handle_client(int client, int verify_peer) {
    struct advc_session_engine *engine;
    struct advc_broker_provider provider;

    if (verify_peer && !peer_is_root(client)) {
        fprintf(stderr, "advc-broker: rejected non-root peer\n");
        return;
    }
    engine = advc_session_engine_create(advc_android_codec_backend_ops(), NULL);
    if (engine == NULL) {
        fprintf(stderr, "advc-broker: cannot allocate session engine\n");
        return;
    }
    provider.userdata = engine;
    provider.feature_bits = ADVC_FEATURE_MEMFD | ADVC_FEATURE_DECODE |
                            ADVC_FEATURE_ENCODE | ADVC_FEATURE_AHARDWAREBUFFER |
                            ADVC_FEATURE_NATIVE_FENCE;
    if (advc_probe_broker_egl_surface())
        provider.feature_bits |= ADVC_FEATURE_BROKER_EGL_SURFACE;
    if (advc_probe_android_ahb_socket())
        provider.feature_bits |= ADVC_FEATURE_ANDROID_AHB_SURFACE;
    if (advc_probe_android_dmabuf_surface_backend(
            ADVC_DMABUF_SURFACE_VULKAN))
        provider.feature_bits |= ADVC_FEATURE_DMABUF_VULKAN |
                                 ADVC_FEATURE_DMABUF;
    if (advc_probe_android_dmabuf_surface_backend(ADVC_DMABUF_SURFACE_EGL))
        provider.feature_bits |= ADVC_FEATURE_DMABUF_EGL |
                                 ADVC_FEATURE_DMABUF;
    /*
     * DECODE_PRIME is a release gate, not a device-presence guess.  Export it
     * only when the launcher supplies the exact token representing the passed
     * QTI decode, modifier-preserving PRIME transfer, one-GPU-repack, LINEAR
     * destination, explicit-fence, EOS and teardown matrix.
     */
    provider.feature_bits &= ~ADVC_FEATURE_DECODE_PRIME;
    {
        const char *validation = getenv("ADVC_DECODE_PRIME_VALIDATION");
        if (validation != NULL &&
            strcmp(validation,
                   "validated-qcom-prime-repack-linear-fence-eos-v1") == 0)
            provider.feature_bits |= ADVC_FEATURE_DECODE_PRIME |
                                     ADVC_FEATURE_DECODE_QCOM_MODIFIER;
    }
    provider.feature_bits &= ~ADVC_FEATURE_ENCODE_QCOM_MODIFIER;
    {
        const char *validation =
            getenv("ADVC_ENCODE_QCOM_IMPORT_VALIDATION");
        if ((provider.feature_bits & ADVC_FEATURE_DMABUF_VULKAN) != 0 &&
            validation != NULL &&
            strcmp(validation,
                   "validated-turnip-qcom-nv12-surface-v1") == 0)
            provider.feature_bits |= ADVC_FEATURE_ENCODE_QCOM_MODIFIER;
    }
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-broker: dmabuf %s\n",
                advc_android_dmabuf_surface_status());
    provider.query_capabilities = query_capabilities;
    provider.handle_codec_request = advc_session_engine_handle;
    provider.after_reply = advc_session_engine_after_reply;
    while (advc_broker_handle_once(client, &provider) == 0) {}
    advc_session_engine_destroy(engine);
}

struct client_job {
    int fd;
};

static void *client_worker(void *opaque) {
    struct client_job *job = opaque;
    int client = job->fd;
    free(job);
    handle_client(client, 1);
    close(client);
    return NULL;
}

static int start_client_worker(int client) {
    struct client_job *job;
    pthread_attr_t attributes;
    pthread_t thread;
    int status;

    job = malloc(sizeof(*job));
    if (job == NULL) return ENOMEM;
    job->fd = client;
    status = pthread_attr_init(&attributes);
    if (status != 0) {
        free(job);
        return status;
    }
    status = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (status == 0)
        status = pthread_create(&thread, &attributes, client_worker, job);
    pthread_attr_destroy(&attributes);
    if (status != 0) free(job);
    return status;
}

static int parse_connected_fd(const char *text) {
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT_MAX ||
        fcntl((int)value, F_GETFD) < 0)
        return -1;
    return (int)value;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/advc-broker.sock";
    int listener;
    signal(SIGPIPE, SIG_IGN);
    (void)advc_start_binder_thread_pool("advc-broker");
    if (argc == 3 && strcmp(argv[1], "--connected-fd") == 0) {
        int client = parse_connected_fd(argv[2]);
        if (client < 0) {
            fprintf(stderr, "advc-broker: invalid connected fd\n");
            return 2;
        }
        /* The descriptor was explicitly inherited from the privileged runner. */
        handle_client(client, 0);
        close(client);
        return 0;
    }
    listener = create_listener(path);
    if (listener < 0) {
        fprintf(stderr, "advc-broker: cannot listen on %s: %s\n", path, strerror(errno));
        return 1;
    }
    for (;;) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        int worker_status;
        if (client < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "advc-broker: accept failed: %s\n", strerror(errno));
            break;
        }
        worker_status = start_client_worker(client);
        if (worker_status != 0) {
            fprintf(stderr, "advc-broker: client worker failed: %s\n",
                    strerror(worker_status));
            close(client);
        }
    }
    close(listener);
    unlink(path);
    return 1;
}
