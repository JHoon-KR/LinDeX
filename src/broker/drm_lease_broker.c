// SPDX-License-Identifier: MIT
// Root-only stock-Android DRM lease broker. Sends only a lease fd via SCM_RIGHTS.

#define ACTIVE_HANDOFF_MAIN broker_embedded_handoff_main
#include "../launcher/active_drm_handoff.c"
#undef ACTIVE_HANDOFF_MAIN

#include <stddef.h>
#include <linux/capability.h>
#include <sys/socket.h>
#include <sys/un.h>

#define BROKER_DRM_MODE_CONNECTOR_DISPLAYPORT 10U

struct broker_reply {
    uint32_t version;
    uint32_t lessee_id;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t object_count;
    uint32_t objects[MAX_LEASE_OBJECTS];
};

static int read_security_context(char *context, size_t size)
{
    FILE *file = fopen("/proc/self/attr/current", "r");
    size_t length;
    if (!file || !fgets(context, (int)size, file)) {
        if (file)
            fclose(file);
        return -1;
    }
    fclose(file);
    length = strlen(context);
    while (length && (context[length - 1] == '\n' || context[length - 1] == '\0'))
        context[--length] = '\0';
    return length ? 0 : -1;
}

static int broker_has_root_equivalent_identity(void)
{
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];
    uint64_t effective;
    char context[128];
    if (geteuid() == 0)
        return 1;
    if (read_security_context(context, sizeof(context)) == 0 &&
        strcmp(context, "u:r:ksu:s0") == 0)
        return 1;
    memset(&header, 0, sizeof(header));
    memset(data, 0, sizeof(data));
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    if (syscall(SYS_capget, &header, data) != 0)
        return 0;
    effective = (uint64_t)data[0].effective |
                ((uint64_t)data[1].effective << 32);
    return (effective & (1ULL << CAP_SYS_PTRACE)) &&
           (effective & (1ULL << CAP_SYS_ADMIN));
}

static void broker_usage(const char *program)
{
    printf("usage: %s [--socket PATH] [--allow-active] [--preflight-only] (--auto | CONNECTOR CRTC [PLANE...])\n",
           program);
    puts("  --auto requires exactly one connected physical DP with a current mode/encoder/CRTC");
    puts("  root clients only; the selected or hinted plane is revalidated and may be replaced");
    puts("  --allow-active permits an already-attached Android scanout plane");
    puts("  --preflight-only performs no CREATE_LEASE and is safe for shell-domain diagnostics");
}

static int broker_mode_is_sane(const struct drm_mode_modeinfo *mode)
{
    return mode->clock && mode->hdisplay && mode->vdisplay &&
           mode->htotal >= mode->hdisplay && mode->vtotal >= mode->vdisplay;
}

/* Seed the same connector/CRTC/plane object list accepted by the explicit
 * command line.  The existing atomic plane selector below remains the final
 * authority and may replace this conservative plane hint. */
static int broker_auto_select_objects(int fd, const struct resources *resources,
                                      struct handoff_options *options,
                                      uint32_t *connector_id,
                                      uint32_t *crtc_id)
{
    uint32_t candidate_connector = 0, candidate_crtc = 0;
    uint32_t candidate_count = 0, plane_hint = 0;
    uint32_t crtc_mask;
    int current_crtc_index;

    if (options->object_count != 0) {
        puts("REFUSE AUTO: explicit object IDs cannot be combined with --auto");
        return -1;
    }

    puts("\n== Automatic physical DP path selection ==");
    for (uint32_t index = 0; index < resources->res.count_connectors; ++index) {
        struct drm_mode_get_connector connector;
        struct drm_mode_get_encoder encoder;
        struct drm_mode_crtc crtc;
        uint32_t *encoder_ids = NULL;
        int connector_crtc_index;

        if (get_connector(fd, resources->connectors[index],
                          &connector, &encoder_ids) != 0) {
            printf("REFUSE AUTO: cannot inspect visible connector %u\n",
                   resources->connectors[index]);
            return -1;
        }
        free(encoder_ids);
        if (connector.connector_type !=
                BROKER_DRM_MODE_CONNECTOR_DISPLAYPORT ||
            connector.connection != DRM_MODE_CONNECTED)
            continue;

        printf("connected DP candidate: connector=%u modes=%u encoder=%u\n",
               connector.connector_id, connector.count_modes,
               connector.encoder_id);
        if (!connector.count_modes || !connector.encoder_id ||
            index_of(resources->encoders, resources->res.count_encoders,
                     connector.encoder_id) < 0) {
            puts("REFUSE AUTO: connected DP lacks modes or a visible current encoder");
            return -1;
        }

        memset(&encoder, 0, sizeof(encoder));
        encoder.encoder_id = connector.encoder_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0 ||
            !encoder.crtc_id) {
            puts("REFUSE AUTO: connected DP current encoder has no readable current CRTC");
            return -1;
        }
        connector_crtc_index = index_of(resources->crtcs,
                                        resources->res.count_crtcs,
                                        encoder.crtc_id);
        if (connector_crtc_index < 0 || connector_crtc_index >= 32 ||
            !(encoder.possible_crtcs &
              (1U << (unsigned int)connector_crtc_index))) {
            puts("REFUSE AUTO: connected DP current CRTC is not valid for its encoder");
            return -1;
        }

        memset(&crtc, 0, sizeof(crtc));
        crtc.crtc_id = encoder.crtc_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0 ||
            !crtc.mode_valid || !broker_mode_is_sane(&crtc.mode)) {
            puts("REFUSE AUTO: connected DP current CRTC has no valid active mode");
            return -1;
        }

        candidate_connector = connector.connector_id;
        candidate_crtc = encoder.crtc_id;
        if (++candidate_count > 1) {
            puts("REFUSE AUTO: more than one connected physical DP path is eligible");
            return -1;
        }
        printf("eligible DP path: connector=%u encoder=%u CRTC=%u mode=%.32s %ux%u@%u\n",
               connector.connector_id, connector.encoder_id, encoder.crtc_id,
               crtc.mode.name, crtc.mode.hdisplay, crtc.mode.vdisplay,
               crtc.mode.vrefresh);
    }

    if (candidate_count != 1) {
        puts("REFUSE AUTO: exactly one connected physical DP path is required");
        return -1;
    }

    current_crtc_index = index_of(resources->crtcs,
                                  resources->res.count_crtcs,
                                  candidate_crtc);
    crtc_mask = 1U << (unsigned int)current_crtc_index;
    for (uint32_t index = 0; index < resources->count_planes; ++index) {
        struct drm_mode_get_plane plane;
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = resources->planes[index];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) == 0 &&
            !plane.crtc_id && !plane.fb_id &&
            (plane.possible_crtcs & crtc_mask) &&
            ah_plane_supports_xr24(fd, plane.plane_id)) {
            plane_hint = plane.plane_id;
            break;
        }
    }
    if (!plane_hint) {
        puts("REFUSE AUTO: no idle XR24-compatible plane can target the selected CRTC");
        return -1;
    }

    options->objects[0] = candidate_connector;
    options->objects[1] = candidate_crtc;
    options->objects[2] = plane_hint;
    options->object_count = 3;
    *connector_id = candidate_connector;
    *crtc_id = candidate_crtc;
    printf("AUTO PATH HINT: connector=%u CRTC=%u idle_XR24_plane=%u\n",
           candidate_connector, candidate_crtc, plane_hint);
    return 0;
}

static int send_lease_fd(int socket_fd, int lease_fd,
                         const struct broker_reply *reply)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = (void *)reply, .iov_len = sizeof(*reply) };
    struct msghdr message;
    struct cmsghdr *cmsg;
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &lease_fd, sizeof(lease_fd));
    return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == sizeof(*reply) ? 0 : -1;
}

#ifndef DRM_LEASE_BROKER_NO_MAIN
int main(int argc, char **argv)
{
    const char *socket_path = "/data/local/tmp/debian-drm-lease.sock";
    struct handoff_options options;
    struct stat card0;
    struct resources resources;
    struct path_state state;
    struct sockaddr_un address;
    struct broker_reply reply;
    char connector_status_path[PATH_MAX];
    uint32_t connector_id = 0, crtc_id = 0, lessee_id = 0;
    int allow_active = 0, preflight_only = 0, auto_objects = 0;
    int master_pid = -1, pidfd = -1, master_fd = -1;
    int listen_fd = -1, client_fd = -1, lease_fd = -1, result = 1;
    int index;
    char broker_context[128], peer_context[128];

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START root-only Android DRM lease broker");
    memset(&options, 0, sizeof(options));
    memset(&resources, 0, sizeof(resources));
    connector_status_path[0] = '\0';
    broker_context[0] = '\0';
    (void)read_security_context(broker_context, sizeof(broker_context));
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--socket") == 0 && ++index < argc) {
            socket_path = argv[index];
        } else if (strcmp(argv[index], "--allow-active") == 0) {
            allow_active = 1;
        } else if (strcmp(argv[index], "--preflight-only") == 0) {
            preflight_only = 1;
        } else if (strcmp(argv[index], "--auto") == 0) {
            if (auto_objects || options.object_count) {
                broker_usage(argv[0]);
                goto done;
            }
            auto_objects = 1;
        } else {
            uint32_t object;
            if (auto_objects || options.object_count >= MAX_LEASE_OBJECTS ||
                parse_u32(argv[index], &object) != 0) {
                broker_usage(argv[0]);
                goto done;
            }
            options.objects[options.object_count++] = object;
        }
    }
    if ((!auto_objects && options.object_count < 3) ||
        (auto_objects && options.object_count != 0) ||
        strlen(socket_path) >= sizeof(address.sun_path)) {
        broker_usage(argv[0]);
        goto done;
    }
    if (!preflight_only && !broker_has_root_equivalent_identity()) {
        char context[128] = "unknown";
        (void)read_security_context(context, sizeof(context));
        printf("REFUSE: CREATE mode requires euid 0, CAP_SYS_PTRACE+CAP_SYS_ADMIN, or a trusted root domain (ruid=%u euid=%u context=%s)\n",
               (unsigned int)getuid(), (unsigned int)geteuid(), context);
        goto done;
    }
    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode) ||
        auto_find_master(&card0, &master_pid, &pidfd, &master_fd) != 0 ||
        load_resources(master_fd, &resources) != 0)
        goto done;
    if ((auto_objects ?
         broker_auto_select_objects(master_fd, &resources, &options,
                                    &connector_id, &crtc_id) :
         select_path_ids(&resources, &options, &connector_id, &crtc_id)) != 0 ||
        read_path_state(master_fd, &resources, connector_id, crtc_id, &state) != 0)
        goto done;
    print_path_state("broker KMS", &state);
    if (state.planes_on_crtc && !allow_active) {
        puts("REFUSE: Android currently has a plane on this CRTC; detach first or use --allow-active");
        goto done;
    }
    if (!state.connected ||
        resolve_connector_status_path(master_fd, connector_id,
                                      connector_status_path,
                                      sizeof(connector_status_path)) != 0) {
        puts("REFUSE: no physical sink with a valid EDID (stale forced-connected KMS state is ignored)");
        goto done;
    }
    puts("PHYSICAL REPROBE: clearing any stale on-digital force before EDID validation");
    if (write_connector_force(connector_status_path, "detect") != 0) {
        puts("REFUSE: connector detect reprobe failed");
        goto done;
    }
    const struct timespec reprobe_delay = {0, 750000000L};
    nanosleep(&reprobe_delay, NULL);
    if (!physical_sink_present_now(connector_status_path)) {
        puts("REFUSE: no physical sink with a valid EDID after detect reprobe");
        goto done;
    }
    if (auto_select_atomic_plane(master_fd, &resources, &options,
                                 connector_id, crtc_id) != 0 ||
        validate_disconnected_lease_path(master_fd, &resources, &options,
                                         connector_id, crtc_id) != 0)
        goto done;

    if (preflight_only) {
        puts("BROKER PREFLIGHT PASS: no CREATE_LEASE, atomic commit, or socket operation was performed");
        result = 0;
        goto done;
    }

    lease_fd = create_lease_fd(master_fd, &options, &lessee_id);
    if (lease_fd < 0)
        goto done;

    listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    unlink(socket_path);
    if (bind(listen_fd, (struct sockaddr *)&address,
             offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(listen_fd, 1) != 0)
        goto done;
    printf("BROKER READY: socket=%s master_pid=%d lessee_id=%u\n",
           socket_path, master_pid, lessee_id);
    client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0)
        goto done;
    struct ucred peer;
    memset(&peer, 0, sizeof(peer));
    socklen_t peer_length = sizeof(peer);
    socklen_t peer_context_length = sizeof(peer_context);
    memset(peer_context, 0, sizeof(peer_context));
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                   &peer, &peer_length) != 0 || peer.uid != geteuid() ||
        getsockopt(client_fd, SOL_SOCKET, SO_PEERSEC, peer_context,
                   &peer_context_length) != 0 ||
        strcmp(peer_context, broker_context) != 0) {
        printf("REFUSE CLIENT: uid=%u context=%s\n",
               (unsigned int)peer.uid, peer_context);
        goto done;
    }
    memset(&reply, 0, sizeof(reply));
    reply.version = 1;
    reply.lessee_id = lessee_id;
    reply.connector_id = connector_id;
    reply.crtc_id = crtc_id;
    reply.object_count = options.object_count;
    memcpy(reply.objects, options.objects,
           options.object_count * sizeof(options.objects[0]));
    if (send_lease_fd(client_fd, lease_fd, &reply) != 0) {
        printf("SCM_RIGHTS send failed errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    puts("LEASE HANDOFF SUCCESS: broker is closing its copy");
    result = 0;
done:
    if (lease_fd >= 0)
        close(lease_fd);
    if (client_fd >= 0)
        close(client_fd);
    if (listen_fd >= 0)
        close(listen_fd);
    if (socket_path)
        unlink(socket_path);
    free_resources(&resources);
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    printf("DONE broker exit=%d\n", result);
    return result;
}
#endif
