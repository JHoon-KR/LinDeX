#define _GNU_SOURCE

#include "advc/client.h"
#include "advc/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct nal_span {
    size_t offset;
    size_t size;
    uint8_t type;
};

static size_t prefix_size(const uint8_t *data, size_t size, size_t offset) {
    if (offset + 4u <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1)
        return 4;
    if (offset + 3u <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1)
        return 3;
    return 0;
}

static int split_hevc(const uint8_t *data, size_t size,
                      size_t *config_size, size_t *frame_offset) {
    struct nal_span nals[64];
    size_t count = 0;
    size_t cursor = 0;
    size_t i;
    int have_vps = 0;
    int have_sps = 0;
    int have_pps = 0;
    while (cursor < size) {
        size_t prefix = prefix_size(data, size, cursor);
        size_t next;
        if (prefix == 0) {
            ++cursor;
            continue;
        }
        if (cursor + prefix + 2u > size || count >= 64u) {
            errno = EPROTO;
            return -1;
        }
        next = cursor + prefix + 2u;
        while (next < size && prefix_size(data, size, next) == 0) ++next;
        nals[count].offset = cursor;
        nals[count].size = next - cursor;
        nals[count].type = (uint8_t)((data[cursor + prefix] >> 1) & 0x3fu);
        ++count;
        cursor = next;
    }
    if (count == 0) {
        errno = EPROTO;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        have_vps |= nals[i].type == 32;
        have_sps |= nals[i].type == 33;
        have_pps |= nals[i].type == 34;
        if (nals[i].type <= 31) {
            if (!have_vps || !have_sps || !have_pps || i == 0) {
                errno = EPROTO;
                return -1;
            }
            /* Include prefix SEI between PPS and the first VCL in the AU. */
            while (i > 0 && nals[i - 1].type >= 39 &&
                   nals[i - 1].type <= 40)
                --i;
            *config_size = nals[i].offset;
            *frame_offset = nals[i].offset;
            return 0;
        }
    }
    errno = EPROTO;
    return -1;
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
    struct stat st;
    uint8_t *bytes;
    size_t done = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (uint64_t)st.st_size > ADVC_MAX_INPUT_BYTES) {
        if (fd >= 0) close(fd);
        errno = EINVAL;
        return -1;
    }
    bytes = malloc((size_t)st.st_size);
    if (bytes == NULL) {
        close(fd);
        return -1;
    }
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, bytes + done, (size_t)st.st_size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            int saved = got == 0 ? EIO : errno;
            close(fd);
            free(bytes);
            errno = saved;
            return -1;
        }
        done += (size_t)got;
    }
    close(fd);
    *data = bytes;
    *size = done;
    return 0;
}

static int queue_inline(int fd, uint32_t session, const uint8_t *data,
                        size_t size, uint64_t id, uint64_t pts,
                        uint32_t flags, uint32_t *detail) {
    struct advc_client_input input;
    memset(&input, 0, sizeof(input));
    input.data = data;
    input.size = size;
    input.data_fd = -1;
    input.buffer_id = id;
    input.pts_ns = pts;
    input.flags = flags;
    return advc_client_queue_input(fd, session, &input, detail);
}

int main(int argc, char **argv) {
    struct advc_client_session_config config;
    struct timeval timeout = {10, 0};
    uint8_t *stream = NULL;
    size_t stream_size = 0;
    size_t config_size = 0;
    size_t frame_offset = 0;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    uint32_t detail = 0;
    uint32_t session = 0;
    unsigned int frames = 0;
    int eos = 0;
    int fd = -1;
    int status;
    unsigned int attempt;
    if (argc != 5 || read_file(argv[2], &stream, &stream_size) < 0 ||
        split_hevc(stream, stream_size, &config_size, &frame_offset) < 0) {
        fprintf(stderr, "hevc probe: input error errno=%d\n", errno);
        goto fail;
    }
    fd = advc_client_connect(argv[1]);
    if (fd < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        advc_client_hello(fd,
                          ADVC_FEATURE_MEMFD | ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_AHARDWAREBUFFER |
                              ADVC_FEATURE_NATIVE_FENCE,
                          &features, &max_payload) < 0) {
        fprintf(stderr, "hevc probe: connect/hello error errno=%d\n", errno);
        goto fail;
    }
    memset(&config, 0, sizeof(config));
    config.mime = "video/hevc";
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = (uint32_t)strtoul(argv[3], NULL, 10);
    config.height = (uint32_t)strtoul(argv[4], NULL, 10);
    config.framerate_milli = 1000;
    config.transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    status = advc_client_create_session(fd, &config, &session, &detail);
    if (status != ADVC_STATUS_OK) {
        fprintf(stderr, "hevc probe: create status=%d detail=%u\n", status,
                detail);
        goto fail;
    }
    status = queue_inline(fd, session, stream, config_size, 1, 0,
                          ADVC_FLAG_CODEC_CONFIG, &detail);
    if (status != ADVC_STATUS_OK) {
        fprintf(stderr, "hevc probe: config status=%d detail=%u\n", status,
                detail);
        goto fail;
    }
    status = queue_inline(fd, session, stream + frame_offset,
                          stream_size - frame_offset, 2, 0,
                          ADVC_FLAG_KEY_FRAME, &detail);
    if (status != ADVC_STATUS_OK) {
        fprintf(stderr, "hevc probe: frame status=%d detail=%u\n", status,
                detail);
        goto fail;
    }
    status = queue_inline(fd, session, NULL, 0, 3, UINT64_C(1000000000),
                          ADVC_FLAG_END_OF_STREAM, &detail);
    if (status != ADVC_STATUS_OK) {
        fprintf(stderr, "hevc probe: eos status=%d detail=%u\n", status,
                detail);
        goto fail;
    }
    for (attempt = 0; attempt < 5000u && !eos; ++attempt) {
        struct advc_client_output output;
        struct timespec pause = {0, 1000000};
        uint64_t id;
        status = advc_client_dequeue_output(fd, session, &output, &detail);
        if (status == ADVC_STATUS_WOULD_BLOCK) {
            nanosleep(&pause, NULL);
            continue;
        }
        if (status != ADVC_STATUS_OK) {
            fprintf(stderr, "hevc probe: dequeue status=%d detail=%u\n",
                    status, detail);
            goto fail;
        }
        id = output.buffer_id;
        if ((output.flags & ADVC_FLAG_END_OF_STREAM) != 0) eos = 1;
        if ((output.size > 0 ||
             output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) &&
            output.width == config.width && output.height == config.height)
            ++frames;
        advc_client_output_close(&output);
        status = advc_client_release_output(fd, session, id, &detail);
        if (status != ADVC_STATUS_OK) {
            fprintf(stderr, "hevc probe: release status=%d detail=%u\n",
                    status, detail);
            goto fail;
        }
    }
    if (frames == 0 || !eos) {
        fprintf(stderr, "hevc probe: incomplete frames=%u eos=%d\n", frames,
                eos);
        goto fail;
    }
    status = advc_client_close_session(fd, session, &detail);
    session = 0;
    if (status != ADVC_STATUS_OK) goto fail;
    close(fd);
    free(stream);
    printf("{\"ok\":true,\"codec\":\"video/hevc\","
           "\"componentClass\":\"hardware\",\"frames\":%u,"
           "\"eos\":true,\"configBytes\":%zu,\"frameBytes\":%zu}\n",
           frames, config_size, stream_size - frame_offset);
    return 0;

fail:
    if (session != 0)
        (void)advc_client_close_session(fd, session, &detail);
    if (fd >= 0) close(fd);
    free(stream);
    return 1;
}
