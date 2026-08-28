// SPDX-License-Identifier: MIT
// Read-only current-master DRM property and format-modifier probe.

#define ACTIVE_HANDOFF_MAIN drm_feature_probe_embedded_handoff_main
#include "../launcher/active_drm_handoff.c"
#undef ACTIVE_HANDOFF_MAIN

#define FP_DRM_MODE_TYPE_PREFERRED (1U << 3)
#define FP_DRM_CAP_ADDFB2_MODIFIERS 0x10ULL

struct fp_drm_get_cap { uint64_t capability, value; };
struct fp_drm_mode_get_blob { uint32_t blob_id, length; uint64_t data; };
struct fp_modifier_blob {
    uint32_t version, flags, count_formats, formats_offset;
    uint32_t count_modifiers, modifiers_offset;
};
struct fp_format_modifier {
    uint64_t formats;
    uint32_t offset, pad;
    uint64_t modifier;
};

#define FP_IOCTL_GET_CAP _IOWR(DRM_IOCTL_BASE, 0x0c, struct fp_drm_get_cap)
#define FP_IOCTL_MODE_GETPROPBLOB \
    _IOWR(DRM_IOCTL_BASE, 0xAC, struct fp_drm_mode_get_blob)
#define FP_IOCTL_MODE_GETFB2 \
    _IOWR(DRM_IOCTL_BASE, 0xCE, struct ah_drm_mode_fb_cmd2)

static int fp_get_property_value(int fd, uint32_t object_id,
                                 uint32_t object_type, const char *wanted,
                                 uint32_t *property_id, uint64_t *value)
{
    struct ah_drm_mode_obj_get_properties first, second;
    uint32_t *ids = NULL;
    uint64_t *values = NULL;
    int found = 0;

    memset(&first, 0, sizeof(first));
    first.obj_id = object_id;
    first.obj_type = object_type;
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &first) != 0 ||
        first.count_props > MAX_OBJECTS)
        return 0;
    ids = bounded_calloc(first.count_props, sizeof(*ids));
    values = bounded_calloc(first.count_props, sizeof(*values));
    if ((first.count_props && !ids) || (first.count_props && !values))
        goto done;
    second = first;
    second.props_ptr = ptr64(ids);
    second.prop_values_ptr = ptr64(values);
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &second) != 0 ||
        second.count_props > first.count_props)
        goto done;
    for (uint32_t index = 0; index < second.count_props; ++index) {
        struct ah_drm_mode_get_property property;
        memset(&property, 0, sizeof(property));
        property.prop_id = ids[index];
        if (ioctl(fd, AH_IOCTL_MODE_GETPROPERTY, &property) == 0 &&
            strncmp(property.name, wanted, AH_DRM_PROP_NAME_LEN) == 0) {
            if (property_id)
                *property_id = ids[index];
            if (value)
                *value = values[index];
            found = 1;
            break;
        }
    }
done:
    free(ids);
    free(values);
    return found;
}

static void fp_print_property(int fd, uint32_t object_id,
                              uint32_t object_type, const char *name)
{
    uint32_t property_id = 0;
    uint64_t value = 0;
    int present = fp_get_property_value(fd, object_id, object_type, name,
                                        &property_id, &value);
    printf("    %-24s present=%s prop=%u value=%llu\n", name,
           present ? "YES" : "no", property_id,
           (unsigned long long)value);
}

static int fp_read_binary(const char *path, unsigned char **bytes_out,
                          size_t *length_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    unsigned char *bytes;
    size_t length = 0;
    ssize_t count;

    if (fd < 0)
        return -1;
    bytes = malloc(65536);
    if (!bytes) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    while (length < 65536 &&
           (count = read(fd, bytes + length, 65536 - length)) > 0)
        length += (size_t)count;
    if (count < 0 || length == 0) {
        int saved_errno = count < 0 ? errno : ENODATA;
        free(bytes);
        close(fd);
        errno = saved_errno;
        return -1;
    }
    close(fd);
    *bytes_out = bytes;
    *length_out = length;
    return 0;
}

static void fp_dump_edid(const char *status_path)
{
    char edid_path[PATH_MAX];
    unsigned char *edid = NULL;
    size_t length = 0;
    int valid_checksums = 1, audio = 0, hdr_static = 0, colorimetry = 0;
    unsigned int audio_sads = 0, hdr_eotf = 0, hdr_metadata = 0;
    int min_v_hz = 0, max_v_hz = 0;
    char *slash;

    snprintf(edid_path, sizeof(edid_path), "%s", status_path);
    slash = strrchr(edid_path, '/');
    if (!slash)
        return;
    snprintf(slash + 1, sizeof(edid_path) - (size_t)(slash + 1 - edid_path),
             "edid");
    if (fp_read_binary(edid_path, &edid, &length) != 0) {
        printf("  EDID: unavailable path=%s errno=%d (%s)\n", edid_path,
               errno, strerror(errno));
        return;
    }
    if (length < 128 || length % 128 != 0 ||
        memcmp(edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8) != 0)
        valid_checksums = 0;
    for (size_t block = 0; block + 128 <= length; block += 128) {
        unsigned int sum = 0;
        for (size_t i = 0; i < 128; i++)
            sum += edid[block + i];
        if ((sum & 0xffU) != 0)
            valid_checksums = 0;
    }
    if (length >= 128) {
        for (size_t offset = 54; offset + 18 <= 126; offset += 18) {
            const unsigned char *descriptor = edid + offset;
            if (descriptor[0] == 0 && descriptor[1] == 0 &&
                descriptor[2] == 0 && descriptor[3] == 0xfd) {
                min_v_hz = descriptor[5];
                max_v_hz = descriptor[6];
            }
        }
    }
    for (size_t block = 128; block + 128 <= length; block += 128) {
        const unsigned char *extension = edid + block;
        if (extension[0] != 0x02)
            continue;
        size_t end = extension[2] >= 4 ? extension[2] : 4;
        if (end > 127)
            end = 127;
        for (size_t pos = 4; pos < end;) {
            unsigned int tag = extension[pos] >> 5;
            unsigned int payload_length = extension[pos] & 0x1f;
            if (pos + 1U + payload_length > end)
                break;
            if (tag == 1 && payload_length >= 3) {
                audio = 1;
                audio_sads += payload_length / 3U;
            }
            if (tag == 7 && payload_length >= 1) {
                unsigned int extended_tag = extension[pos + 1];
                if (extended_tag == 0x06) {
                    hdr_static = 1;
                    if (payload_length >= 2)
                        hdr_eotf |= extension[pos + 2];
                    if (payload_length >= 3)
                        hdr_metadata |= extension[pos + 3];
                }
                if (extended_tag == 0x05)
                    colorimetry = 1;
            }
            pos += 1U + payload_length;
        }
    }
    printf("  EDID: bytes=%zu blocks=%zu checksum=%s CTA_audio=%s SADs=%u "
           "HDR_block=%s HDR_EOTF=0x%02x HDR_metadata=0x%02x "
           "colorimetry=%s range=%d-%dHz\n",
           length, length / 128, valid_checksums ? "VALID" : "INVALID",
           audio ? "YES" : "no", audio_sads,
           hdr_static ? "YES" : "no", hdr_eotf, hdr_metadata,
           colorimetry ? "YES" : "no", min_v_hz, max_v_hz);
    free(edid);
}

static void fp_dump_alsa_eld(void)
{
    DIR *directory = opendir("/proc/asound");
    struct dirent *entry;
    int found = 0;
    puts("\nALSA ELD:");
    if (!directory) {
        puts("  /proc/asound unavailable");
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char card_path[PATH_MAX];
        DIR *card;
        struct dirent *eld_entry;
        if (strncmp(entry->d_name, "card", 4) != 0)
            continue;
        if (snprintf(card_path, sizeof(card_path), "/proc/asound/%s",
                     entry->d_name) >= (int)sizeof(card_path))
            continue;
        card = opendir(card_path);
        if (!card)
            continue;
        while ((eld_entry = readdir(card)) != NULL) {
            char eld_path[PATH_MAX], line[256];
            FILE *file;
            if (strncmp(eld_entry->d_name, "eld", 3) != 0)
                continue;
            if (snprintf(eld_path, sizeof(eld_path), "%s/%s",
                         card_path, eld_entry->d_name) >=
                (int)sizeof(eld_path))
                continue;
            file = fopen(eld_path, "r");
            if (!file)
                continue;
            printf("  %s:\n", eld_path);
            while (fgets(line, sizeof(line), file)) {
                if (strstr(line, "monitor_present") ||
                    strstr(line, "eld_valid") || strstr(line, "monitor_name") ||
                    strstr(line, "sad_count") || strstr(line, "connection_type"))
                    printf("    %s", line);
            }
            fclose(file);
            found = 1;
        }
        closedir(card);
    }
    closedir(directory);
    if (!found)
        puts("  no ELD nodes (audio codec/routing may be absent or disconnected)");
}

static void fp_dump_blob_text(int fd, uint64_t blob_id)
{
    struct fp_drm_mode_get_blob request;
    unsigned char *bytes;
    uint32_t limit;

    memset(&request, 0, sizeof(request));
    request.blob_id = (uint32_t)blob_id;
    if (ioctl(fd, FP_IOCTL_MODE_GETPROPBLOB, &request) != 0 ||
        request.length == 0 || request.length > (1U << 20))
        return;
    bytes = calloc(1, request.length);
    if (!bytes)
        return;
    request.data = ptr64(bytes);
    if (ioctl(fd, FP_IOCTL_MODE_GETPROPBLOB, &request) != 0) {
        free(bytes);
        return;
    }
    limit = request.length > 8192U ? 8192U : request.length;
    printf("      blob-text(%u bytes): ", request.length);
    for (uint32_t index = 0; index < limit; ++index) {
        unsigned char value = bytes[index];
        if (value == '\n' || value == '\r' || value == '\t' ||
            (value >= 32 && value <= 126))
            putchar(value);
        else if (value == 0)
            putchar(' ');
        else
            putchar('.');
    }
    if (limit < request.length)
        printf(" ...[truncated]");
    putchar('\n');
    free(bytes);
}

static void fp_dump_object_properties(int fd, uint32_t object_id,
                                      uint32_t object_type)
{
    struct ah_drm_mode_obj_get_properties first, second;
    uint32_t *ids = NULL;
    uint64_t *values = NULL;

    memset(&first, 0, sizeof(first));
    first.obj_id = object_id;
    first.obj_type = object_type;
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &first) != 0 ||
        first.count_props > MAX_OBJECTS)
        return;
    ids = bounded_calloc(first.count_props, sizeof(*ids));
    values = bounded_calloc(first.count_props, sizeof(*values));
    if ((first.count_props && !ids) || (first.count_props && !values))
        goto done;
    second = first;
    second.props_ptr = ptr64(ids);
    second.prop_values_ptr = ptr64(values);
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &second) != 0 ||
        second.count_props > first.count_props)
        goto done;
    puts("  properties:");
    for (uint32_t index = 0; index < second.count_props; ++index) {
        struct ah_drm_mode_get_property property;
        memset(&property, 0, sizeof(property));
        property.prop_id = ids[index];
        if (ioctl(fd, AH_IOCTL_MODE_GETPROPERTY, &property) == 0) {
            printf("    id=%u name=%s flags=0x%x value=0x%016llx (%llu)\n",
                   ids[index], property.name, property.flags,
                   (unsigned long long)values[index],
                   (unsigned long long)values[index]);
            if ((property.flags & 0x10U) && values[index] != 0 &&
                strcmp(property.name, "capabilities") == 0)
                fp_dump_blob_text(fd, values[index]);
        }
    }
done:
    free(ids);
    free(values);
}

static const char *fp_plane_type(uint64_t type)
{
    switch (type) {
    case 0: return "overlay";
    case 1: return "primary";
    case 2: return "cursor";
    default: return "unknown";
    }
}

static const char *fp_modifier_vendor(uint64_t modifier)
{
    switch ((unsigned int)(modifier >> 56)) {
    case 0: return modifier == 0 ? "LINEAR" : "NONE/vendor0";
    case 1: return "INTEL";
    case 2: return "AMD";
    case 3: return "NVIDIA";
    case 4: return "SAMSUNG";
    case 5: return "QCOM/UBWC-candidate";
    case 6: return "VIVANTE";
    case 7: return "BROADCOM";
    case 8: return "ARM";
    default: return "other";
    }
}

static void fp_dump_in_formats(int fd, uint32_t plane_id, uint64_t blob_id)
{
    struct fp_drm_mode_get_blob request;
    unsigned char *bytes = NULL;
    struct fp_modifier_blob *header;
    uint32_t *formats;
    struct fp_format_modifier *modifiers;

    if (!blob_id) {
        printf("  IN_FORMATS: absent/zero (implicit modifiers only)\n");
        return;
    }
    memset(&request, 0, sizeof(request));
    request.blob_id = (uint32_t)blob_id;
    if (ioctl(fd, FP_IOCTL_MODE_GETPROPBLOB, &request) != 0 ||
        request.length < sizeof(*header) || request.length > (1U << 20)) {
        printf("  IN_FORMATS blob=%llu read failed errno=%d (%s)\n",
               (unsigned long long)blob_id, errno, strerror(errno));
        return;
    }
    bytes = calloc(1, request.length);
    if (!bytes)
        return;
    request.data = ptr64(bytes);
    if (ioctl(fd, FP_IOCTL_MODE_GETPROPBLOB, &request) != 0)
        goto done;
    header = (struct fp_modifier_blob *)bytes;
    if ((uint64_t)header->formats_offset +
            (uint64_t)header->count_formats * sizeof(uint32_t) > request.length ||
        (uint64_t)header->modifiers_offset +
            (uint64_t)header->count_modifiers * sizeof(*modifiers) > request.length) {
        printf("  IN_FORMATS blob malformed\n");
        goto done;
    }
    formats = (uint32_t *)(bytes + header->formats_offset);
    modifiers = (struct fp_format_modifier *)(bytes + header->modifiers_offset);
    printf("  IN_FORMATS: formats=%u modifiers=%u\n",
           header->count_formats, header->count_modifiers);
    for (uint32_t index = 0; index < header->count_modifiers; ++index) {
        int xr24 = 0;
        for (uint32_t bit = 0; bit < 64; ++bit) {
            uint32_t format_index = modifiers[index].offset + bit;
            if ((modifiers[index].formats & (1ULL << bit)) &&
                format_index < header->count_formats &&
                formats[format_index] == AH_DRM_FORMAT_XRGB8888)
                xr24 = 1;
        }
        printf("    modifier=0x%016llx vendor=%s XR24=%s\n",
               (unsigned long long)modifiers[index].modifier,
               fp_modifier_vendor(modifiers[index].modifier),
               xr24 ? "YES" : "no");
    }
done:
    free(bytes);
    (void)plane_id;
}

static void fp_dump_framebuffer(int fd, uint32_t fb_id)
{
    struct ah_drm_mode_fb_cmd2 framebuffer;
    if (!fb_id)
        return;
    memset(&framebuffer, 0, sizeof(framebuffer));
    framebuffer.fb_id = fb_id;
    if (ioctl(fd, FP_IOCTL_MODE_GETFB2, &framebuffer) != 0) {
        printf("  GETFB2 fb=%u failed errno=%d (%s)\n",
               fb_id, errno, strerror(errno));
        return;
    }
    printf("  GETFB2 fb=%u %ux%u format=0x%08x flags=0x%x "
           "modifier=0x%016llx pitch=%u offset=%u\n",
           framebuffer.fb_id, framebuffer.width, framebuffer.height,
           framebuffer.pixel_format, framebuffer.flags,
           (unsigned long long)framebuffer.modifiers[0],
           framebuffer.pitches[0], framebuffer.offsets[0]);
}

int main(void)
{
    struct stat card0;
    struct resources resources;
    struct ah_drm_set_client_cap client_cap;
    struct fp_drm_get_cap cap;
    int master_pid = -1, pidfd = -1, master_fd = -1;
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START read-only DRM modifier/fence/plane feature probe");
    memset(&resources, 0, sizeof(resources));
    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode)) {
        printf("card0 unavailable errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    if (auto_find_master(&card0, &master_pid, &pidfd, &master_fd) == 0) {
        printf("current master pid=%d local_fd=%d\n", master_pid, master_fd);
    } else {
        puts("current-master duplication unavailable; falling back to a non-master read-only property fd");
        master_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (master_fd < 0) {
            printf("card0 open failed errno=%d (%s)\n", errno, strerror(errno));
            goto done;
        }
        printf("non-master property fd=%d (no state-changing ioctl will be used)\n",
               master_fd);
    }
    memset(&cap, 0, sizeof(cap));
    cap.capability = FP_DRM_CAP_ADDFB2_MODIFIERS;
    if (ioctl(master_fd, FP_IOCTL_GET_CAP, &cap) == 0)
        printf("DRM_CAP_ADDFB2_MODIFIERS=%llu\n",
               (unsigned long long)cap.value);
    client_cap.capability = AH_DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    client_cap.value = 1;
    if (ioctl(master_fd, AH_IOCTL_SET_CLIENT_CAP, &client_cap) != 0)
        goto done;
    client_cap.capability = AH_DRM_CLIENT_CAP_ATOMIC;
    if (ioctl(master_fd, AH_IOCTL_SET_CLIENT_CAP, &client_cap) != 0)
        goto done;
    if (load_resources(master_fd, &resources) != 0)
        goto done;

    printf("resources: connectors=%u crtcs=%u planes=%u\n",
           resources.res.count_connectors, resources.res.count_crtcs,
           resources.count_planes);
    for (uint32_t index = 0; index < resources.res.count_crtcs; ++index) {
        uint32_t out_prop = 0, retire_prop = 0;
        uint64_t ignored = 0;
        uint32_t crtc_id = resources.crtcs[index];
        int out = fp_get_property_value(master_fd, crtc_id,
            AH_DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", &out_prop, &ignored);
        int retire = fp_get_property_value(master_fd, crtc_id,
            AH_DRM_MODE_OBJECT_CRTC, "RETIRE_FENCE", &retire_prop, &ignored);
        printf("CRTC %u: OUT_FENCE_PTR=%s(prop=%u) RETIRE_FENCE=%s(prop=%u)\n",
               crtc_id, out ? "YES" : "no", out_prop,
               retire ? "YES" : "no", retire_prop);
        fp_print_property(master_fd, crtc_id, AH_DRM_MODE_OBJECT_CRTC,
                          "VRR_ENABLED");
    }
    for (uint32_t index = 0; index < resources.res.count_connectors; ++index) {
        uint32_t retire_prop = 0;
        uint64_t ignored = 0;
        uint32_t connector_id = resources.connectors[index];
        int retire = fp_get_property_value(master_fd, connector_id,
            AH_DRM_MODE_OBJECT_CONNECTOR, "RETIRE_FENCE", &retire_prop,
            &ignored);
        printf("connector %u: RETIRE_FENCE=%s(prop=%u)\n", connector_id,
               retire ? "YES" : "no", retire_prop);
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "audio");
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "HDR_OUTPUT_METADATA");
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "Colorspace");
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "max bpc");
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "vrr_capable");
        fp_print_property(master_fd, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR,
                          "link-status");
        char status_path[PATH_MAX];
        if (resolve_connector_status_path(master_fd, connector_id,
                                          status_path, sizeof(status_path)) == 0)
            fp_dump_edid(status_path);
    }
    for (uint32_t index = 0; index < resources.count_planes; ++index) {
        struct drm_mode_get_plane plane;
        uint64_t type = UINT64_MAX, blob = 0, ignored = 0;
        uint32_t type_prop = 0, formats_prop = 0, fence_prop = 0;
        uint32_t plane_id = resources.planes[index];
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = plane_id;
        if (ioctl(master_fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0)
            continue;
        (void)fp_get_property_value(master_fd, plane_id,
            AH_DRM_MODE_OBJECT_PLANE, "type", &type_prop, &type);
        (void)fp_get_property_value(master_fd, plane_id,
            AH_DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &formats_prop, &blob);
        int in_fence = fp_get_property_value(master_fd, plane_id,
            AH_DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", &fence_prop, &ignored);
        printf("plane %u: type=%s(%llu) crtc=%u fb=%u possible=0x%x "
               "IN_FENCE_FD=%s(prop=%u)\n", plane_id,
               fp_plane_type(type), (unsigned long long)type,
               plane.crtc_id, plane.fb_id, plane.possible_crtcs,
               in_fence ? "YES" : "no", fence_prop);
        fp_dump_framebuffer(master_fd, plane.fb_id);
        fp_dump_in_formats(master_fd, plane_id, blob);
        if (getenv("DRM_FEATURE_VERBOSE"))
            fp_dump_object_properties(master_fd, plane_id,
                                      AH_DRM_MODE_OBJECT_PLANE);
    }
    fp_dump_alsa_eld();
    result = 0;
done:
    free_resources(&resources);
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    printf("DONE exit=%d\n", result);
    return result;
}
