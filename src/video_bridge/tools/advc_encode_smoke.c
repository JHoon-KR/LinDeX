#define _GNU_SOURCE

#include "advc/client.h"
#include "advc_bitstream.h"
#include "advc_media_time.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include "ahb_transport.h"
#include <android/hardware_buffer.h>
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002u
#endif

#define DEFAULT_DEADLINE_MS UINT32_C(20000)
#define MIN_DEADLINE_MS UINT32_C(1000)
#define MAX_DEADLINE_MS UINT32_C(60000)
#define MAX_SMOKE_FRAMES UINT32_C(120)
#define MAX_SMOKE_OUTPUTS UINT32_C(2048)
#define FNV1A64_OFFSET UINT64_C(14695981039346656037)
#define FNV1A64_PRIME UINT64_C(1099511628211)

/* Optional isolated round-trip fixture output; disabled by default. */
static int bitstream_output_fd = -1;

struct encode_result {
    const char *stage;
    int status;
    int error_number;
    uint32_t detail;
    uint32_t frames_queued;
    uint32_t outputs;
    uint32_t nonempty_outputs;
    uint32_t config_packets;
    uint32_t key_packets;
    uint32_t eos_packets;
    uint32_t annex_b_packets;
    uint32_t length_prefixed_packets;
    uint32_t config_record_packets;
    uint32_t nal_units;
    uint32_t parameter_sets;
    uint32_t parameter_set_mask;
    uint32_t vcl_units;
    uint32_t vcl_packets;
    uint32_t key_vcl_units;
    uint32_t pts_nonmonotonic;
    uint64_t bytes;
    uint64_t first_vcl_pts_ns;
    uint64_t last_vcl_pts_ns;
    uint64_t fnv1a64;
    int have_vcl_pts;
    int eos;
};

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) return UINT64_MAX;
    return (uint64_t)value.tv_sec * UINT64_C(1000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static int apply_remaining_timeout(int fd, uint64_t deadline_ms) {
    struct timeval timeout;
    uint64_t now = monotonic_ms();
    uint64_t remaining;
    if (now == UINT64_MAX || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    remaining = deadline_ms - now;
    timeout.tv_sec = (time_t)(remaining / 1000u);
    timeout.tv_usec = (suseconds_t)((remaining % 1000u) * 1000u);
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) timeout.tv_usec = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        return -1;
    return 0;
}

static int sleep_until_retry(uint64_t deadline_ms) {
    uint64_t now = monotonic_ms();
    int delay;
    if (now == UINT64_MAX || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    delay = (int)(deadline_ms - now > 5 ? 5 : deadline_ms - now);
    while (poll(NULL, 0, delay) < 0) {
        if (errno != EINTR) return -1;
        now = monotonic_ms();
        if (now == UINT64_MAX || now >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        delay = (int)(deadline_ms - now > 5 ? 5 : deadline_ms - now);
    }
    return 0;
}

static int parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                     uint32_t *value) {
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_env_u32(const char *name, uint32_t default_value,
                         uint32_t minimum, uint32_t maximum, uint32_t *value) {
    const char *text = getenv(name);
    if (text == NULL) {
        *value = default_value;
        return 0;
    }
    return parse_u32(text, minimum, maximum, value);
}

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value != NULL && strcmp(value, "1") == 0;
}

static int connect_argument(const char *argument, uint64_t deadline_ms) {
    struct sockaddr_un address;
    char *end;
    long inherited;
    int fd;
    if (strncmp(argument, "fd:", 3) != 0) {
        int error;
        socklen_t error_size = sizeof(error);
        if (strlen(argument) >= sizeof(address.sun_path)) {
            errno = EINVAL;
            return -1;
        }
        fd = socket(AF_UNIX,
                    SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, argument, sizeof(address.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            if (errno != EINPROGRESS && errno != EAGAIN) goto fail;
            for (;;) {
                struct pollfd pollfd;
                uint64_t now = monotonic_ms();
                int remaining;
                int poll_status;
                if (now == UINT64_MAX || now >= deadline_ms) {
                    errno = ETIMEDOUT;
                    goto fail;
                }
                remaining = (int)(deadline_ms - now);
                memset(&pollfd, 0, sizeof(pollfd));
                pollfd.fd = fd;
                pollfd.events = POLLOUT;
                poll_status = poll(&pollfd, 1, remaining);
                if (poll_status > 0) break;
                if (poll_status == 0) {
                    errno = ETIMEDOUT;
                    goto fail;
                }
                if (errno != EINTR) goto fail;
            }
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0)
                goto fail;
            if (error != 0) {
                errno = error;
                goto fail;
            }
        }
        {
            int flags = fcntl(fd, F_GETFL);
            if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
                goto fail;
        }
        return fd;
fail:
        {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    errno = 0;
    inherited = strtol(argument + 3, &end, 10);
    if (errno != 0 || end == argument + 3 || *end != '\0' || inherited < 0 ||
        inherited > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    fd = fcntl((int)inherited, F_DUPFD_CLOEXEC, 3);
    if (fd >= 0) {
        int type;
        socklen_t type_size = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) < 0 ||
            type != SOCK_SEQPACKET) {
            int saved = errno != 0 ? errno : EPROTOTYPE;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    return fd;
}

static int write_all(int fd, const uint8_t *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t written;
        do {
            written = write(fd, data + done, size - done);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) return -1;
        done += (size_t)written;
    }
    return 0;
}

static void fill_frame(uint8_t *data, uint32_t width, uint32_t height,
                       uint32_t color_format, uint32_t frame_index) {
    size_t pixels = (size_t)width * height;
    size_t chroma = pixels / 4u;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            data[(size_t)y * width + x] =
                (uint8_t)((x * 3u + y * 5u + frame_index * 11u) & 0xffu);
        }
    }
    if (color_format == ADVC_COLOR_FORMAT_YUV420_PLANAR) {
        uint8_t *u = data + pixels;
        uint8_t *v = u + chroma;
        for (size_t i = 0; i < chroma; ++i) {
            u[i] = (uint8_t)(80u + ((i + frame_index * 3u) & 31u));
            v[i] = (uint8_t)(160u + ((i + frame_index * 5u) & 31u));
        }
    } else {
        uint8_t *uv = data + pixels;
        for (size_t i = 0; i < chroma; ++i) {
            uv[i * 2u] = (uint8_t)(80u + ((i + frame_index * 3u) & 31u));
            uv[i * 2u + 1u] =
                (uint8_t)(160u + ((i + frame_index * 5u) & 31u));
        }
    }
}

static int create_frame_memfd(const struct advc_client_session_config *config,
                              uint32_t frame_index, size_t frame_size) {
    uint8_t *frame;
    int fd;
    fd = (int)syscall(SYS_memfd_create, "advc-encode-frame",
                      MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    frame = (uint8_t *)malloc(frame_size);
    if (frame == NULL) {
        close(fd);
        return -1;
    }
    fill_frame(frame, config->width, config->height, config->color_format,
               frame_index);
    if (write_all(fd, frame, frame_size) < 0 ||
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        int saved = errno;
        free(frame);
        close(fd);
        errno = saved;
        return -1;
    }
    free(frame);
    return fd;
}

static int read_all_at(int fd, uint8_t *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t received;
        do {
            received = pread(fd, data + done, size - done, (off_t)done);
        } while (received < 0 && errno == EINTR);
        if (received <= 0) return -1;
        done += (size_t)received;
    }
    return 0;
}

static int call_failed(struct encode_result *result, const char *stage, int status,
                       uint32_t detail) {
    result->stage = stage;
    result->status = status;
    result->detail = detail;
    result->error_number = status < 0 ? errno : 0;
    return -1;
}

static void hash_bytes(struct encode_result *result, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        result->fnv1a64 ^= data[i];
        result->fnv1a64 *= FNV1A64_PRIME;
    }
}

/* Returns 0 for no output, 1 for output, 2 for EOS, and -1 on failure. */
static int consume_one_output(int socket_fd, uint32_t session_id,
                              const struct advc_client_session_config *config,
                              struct encode_result *result, uint64_t deadline_ms) {
    struct advc_client_output output;
    struct advc_bitstream_stats stats;
    uint8_t *data = NULL;
    uint64_t buffer_id;
    uint32_t detail = 0;
    const char *validation_stage = NULL;
    int validation_errno = 0;
    int status;

    if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
        return call_failed(result, "dequeue", -1, 0);
    status = advc_client_dequeue_output(socket_fd, session_id, &output, &detail);
    if (status == ADVC_STATUS_WOULD_BLOCK) return 0;
    if (status != ADVC_STATUS_OK)
        return call_failed(result, "dequeue", status, detail);

    buffer_id = output.buffer_id;
    if (result->outputs >= MAX_SMOKE_OUTPUTS) {
        validation_stage = "output_limit";
        validation_errno = EOVERFLOW;
    }
    ++result->outputs;
    if ((output.flags & ADVC_FLAG_CODEC_CONFIG) != 0) ++result->config_packets;
    if ((output.flags & ADVC_FLAG_KEY_FRAME) != 0) ++result->key_packets;
    if ((output.flags & ADVC_FLAG_END_OF_STREAM) != 0) {
        ++result->eos_packets;
        result->eos = 1;
    }
    if (validation_stage == NULL &&
        (output.width != config->width || output.height != config->height ||
        output.android_format != 0 || output.stride != 0 ||
        output.slice_height != 0 || output.crop_left != 0 || output.crop_top != 0 ||
         output.crop_right != 0 || output.crop_bottom != 0)) {
        validation_stage = "validate_metadata";
        validation_errno = EPROTO;
    } else if (validation_stage == NULL && output.size == 0) {
        if ((output.flags & ADVC_FLAG_END_OF_STREAM) == 0) {
            validation_stage = "validate_empty_output";
            validation_errno = EPROTO;
        }
    } else if (validation_stage == NULL) {
        data = (uint8_t *)malloc((size_t)output.size);
        if (data == NULL) {
            validation_stage = "allocate_output";
            validation_errno = errno;
        } else if (read_all_at(output.data_fd, data, (size_t)output.size) < 0) {
            validation_stage = "read_output";
            validation_errno = errno;
        } else if (advc_bitstream_inspect(config->mime, data, (size_t)output.size,
                                          &stats) < 0) {
            validation_stage = "validate_bitstream";
            validation_errno = errno;
        } else if (bitstream_output_fd >= 0 &&
                   write_all(bitstream_output_fd, data,
                             (size_t)output.size) < 0) {
            validation_stage = "write_bitstream_output";
            validation_errno = errno;
        } else {
            if (UINT64_MAX - result->bytes < output.size ||
                UINT32_MAX - result->nal_units < stats.nal_units ||
                UINT32_MAX - result->parameter_sets < stats.parameter_sets ||
                UINT32_MAX - result->vcl_units < stats.vcl_units ||
                UINT32_MAX - result->key_vcl_units < stats.key_vcl_units) {
                validation_stage = "metadata_overflow";
                validation_errno = EOVERFLOW;
                goto validation_complete;
            }
            ++result->nonempty_outputs;
            result->bytes += output.size;
            result->nal_units += stats.nal_units;
            result->parameter_sets += stats.parameter_sets;
            result->parameter_set_mask |= stats.parameter_set_mask;
            result->vcl_units += stats.vcl_units;
            result->key_vcl_units += stats.key_vcl_units;
            if (stats.format == ADVC_BITSTREAM_ANNEX_B)
                ++result->annex_b_packets;
            else if (stats.format == ADVC_BITSTREAM_LENGTH_PREFIXED)
                ++result->length_prefixed_packets;
            else if (stats.format == ADVC_BITSTREAM_CODEC_CONFIG_RECORD)
                ++result->config_record_packets;
            hash_bytes(result, data, (size_t)output.size);
            if (stats.vcl_units > 0) {
                ++result->vcl_packets;
                if (!result->have_vcl_pts) {
                    result->first_vcl_pts_ns = output.pts_ns;
                    result->have_vcl_pts = 1;
                } else if (output.pts_ns < result->last_vcl_pts_ns) {
                    ++result->pts_nonmonotonic;
                }
                result->last_vcl_pts_ns = output.pts_ns;
            }
        }
    }
validation_complete:
    free(data);
    advc_client_output_close(&output);

    if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
        return call_failed(result, "release_output", -1, 0);
    status = advc_client_release_output(socket_fd, session_id, buffer_id, &detail);
    if (status != ADVC_STATUS_OK)
        return call_failed(result, "release_output", status, detail);
    if (validation_stage != NULL) {
        errno = validation_errno;
        return call_failed(result, validation_stage, -1, 0);
    }
    return result->eos ? 2 : 1;
}

#if defined(__ANDROID__)
static int wait_and_close_fence(int *fence_fd, uint64_t deadline_ms) {
    struct pollfd poll_fd;
    int status;
    if (fence_fd == NULL || *fence_fd < 0) return 0;
    memset(&poll_fd, 0, sizeof(poll_fd));
    poll_fd.fd = *fence_fd;
    poll_fd.events = POLLIN;
    for (;;) {
        uint64_t now = monotonic_ms();
        int remaining;
        if (now == UINT64_MAX || now >= deadline_ms) {
            errno = ETIMEDOUT;
            status = -1;
            break;
        }
        remaining = (int)(deadline_ms - now);
        status = poll(&poll_fd, 1, remaining);
        if (status > 0) {
            if ((poll_fd.revents & (POLLIN | POLLHUP)) != 0) {
                status = 0;
                break;
            }
            errno = EIO;
            status = -1;
            break;
        }
        if (status == 0) {
            errno = ETIMEDOUT;
            status = -1;
            break;
        }
        if (errno != EINTR) break;
    }
    close(*fence_fd);
    *fence_fd = -1;
    return status;
}

/* CPU write is deliberately test-only; broker import/render remains CPU-copy-free. */
static int fill_test_ahb(AHardwareBuffer *buffer, uint32_t width, uint32_t height,
                         uint32_t frame_index, int *acquire_fence_fd) {
    AHardwareBuffer_Desc desc;
    void *pixels = NULL;
    if (buffer == NULL || acquire_fence_fd == NULL) {
        errno = EINVAL;
        return -1;
    }
    *acquire_fence_fd = -1;
    memset(&desc, 0, sizeof(desc));
    AHardwareBuffer_describe(buffer, &desc);
    if (desc.width != width || desc.height != height || desc.layers != 1 ||
        desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        desc.stride < width) {
        errno = EPROTO;
        return -1;
    }
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1,
                             NULL, &pixels) != 0 || pixels == NULL) {
        errno = EIO;
        return -1;
    }
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)pixels + (size_t)y * desc.stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t r = (x * 3u + frame_index * 17u) & 0xffu;
            uint32_t g = (y * 5u + frame_index * 29u) & 0xffu;
            uint32_t b = (x + y + frame_index * 11u) & 0xffu;
            row[x] = r | (g << 8) | (b << 16) | UINT32_C(0xff000000);
        }
    }
    if (AHardwareBuffer_unlock(buffer, acquire_fence_fd) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int submit_ahb_with_drain(
    int socket_fd, uint32_t session_id,
    const struct advc_client_session_config *config,
    const struct advc_client_ahb_input *input, struct encode_result *result,
    uint64_t deadline_ms, int *release_fence_fd) {
    for (;;) {
        uint32_t detail = 0;
        int drained;
        int status;
        if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
            return call_failed(result, "submit_ahb", -1, 0);
        status = advc_client_submit_ahb(socket_fd, session_id, input,
                                        advc_send_ahardwarebuffer_callback, NULL,
                                        release_fence_fd, &detail);
        if (status == ADVC_STATUS_OK) return 0;
        if (status != ADVC_STATUS_WOULD_BLOCK)
            return call_failed(result, "submit_ahb", status, detail);
        drained = consume_one_output(socket_fd, session_id, config, result,
                                     deadline_ms);
        if (drained < 0) return -1;
        if (drained == 2) {
            errno = EPROTO;
            return call_failed(result, "premature_eos", -1, 0);
        }
        if (drained == 0 && sleep_until_retry(deadline_ms) < 0)
            return call_failed(result, "submit_ahb", -1, 0);
    }
}
#endif

static int queue_with_drain(int socket_fd, uint32_t session_id,
                            const struct advc_client_session_config *config,
                            const struct advc_client_input *input,
                            struct encode_result *result, uint64_t deadline_ms,
                            const char *stage) {
    for (;;) {
        uint32_t detail = 0;
        int drained;
        int status;
        if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
            return call_failed(result, stage, -1, 0);
        status = advc_client_queue_input(socket_fd, session_id, input, &detail);
        if (status == ADVC_STATUS_OK) return 0;
        if (status != ADVC_STATUS_WOULD_BLOCK)
            return call_failed(result, stage, status, detail);
        drained = consume_one_output(socket_fd, session_id, config, result, deadline_ms);
        if (drained < 0) return -1;
        if (drained == 2) {
            errno = EPROTO;
            return call_failed(result, "premature_eos", -1, 0);
        }
        if (drained == 0 && sleep_until_retry(deadline_ms) < 0)
            return call_failed(result, stage, -1, 0);
    }
}

static const struct advc_codec_capability *find_encoder(
    const struct advc_capability_set *caps, const char *mime) {
    for (uint32_t i = 0; i < caps->count; ++i) {
        if (caps->codecs[i].direction == ADVC_DIRECTION_ENCODE &&
            strcmp(caps->codecs[i].mime, mime) == 0)
            return &caps->codecs[i];
    }
    return NULL;
}

static const char *acceleration_name(uint8_t acceleration) {
    switch (acceleration) {
    case ADVC_ACCELERATION_HARDWARE:
        return "hardware";
    case ADVC_ACCELERATION_SOFTWARE:
        return "software";
    case ADVC_ACCELERATION_VENDOR_SOFTWARE:
        return "vendor-software";
    default:
        return "unknown";
    }
}

static void print_json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p >= 0x20 && *p < 0x7f) {
            putchar(*p);
        } else {
            printf("\\u%04x", (unsigned)*p);
        }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    struct advc_client_session_config config;
    struct advc_capability_set caps;
    const struct advc_codec_capability *encoder = NULL;
    struct encode_result result;
    uint64_t features = 0;
    uint64_t deadline;
    size_t frame_size = 0;
    uint32_t frames = 0;
    uint32_t timeout_ms = DEFAULT_DEADLINE_MS;
    uint32_t max_payload = 0;
    uint32_t session_id = 0;
    uint32_t detail = 0;
    uint32_t next_buffer_id = 1;
    const char *expected_component = getenv("ADVC_EXPECT_CODEC_NAME");
    const char *component = "unavailable";
    const char *acceleration = "unknown";
    int socket_fd = -1;
    int status;
    int rc = EXIT_FAILURE;
    int local_surface = 0;
    int android_ahb_surface = 0;
    const char *bitstream_output = getenv("ADVC_SMOKE_BITSTREAM_OUTPUT");
#if defined(__ANDROID__)
    AHardwareBuffer *test_ahb = NULL;
    int ahb_acquire_fence = -1;
    int ahb_release_fence = -1;
#endif

    memset(&config, 0, sizeof(config));
    memset(&caps, 0, sizeof(caps));
    memset(&result, 0, sizeof(result));
    result.stage = "arguments";
    result.status = -1;
    result.error_number = EINVAL;
    result.fnv1a64 = FNV1A64_OFFSET;

    if (bitstream_output != NULL && bitstream_output[0] != '\0') {
        bitstream_output_fd = open(bitstream_output,
                                   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                       O_NOFOLLOW,
                                   0600);
        if (bitstream_output_fd < 0) {
            result.stage = "open_bitstream_output";
            result.error_number = errno;
            goto done;
        }
    }

    if (argc != 7 ||
        (strcmp(argv[2], "video/avc") != 0 && strcmp(argv[2], "video/hevc") != 0) ||
        parse_u32(argv[3], 16, 8192, &config.width) < 0 ||
        parse_u32(argv[4], 16, 8192, &config.height) < 0 ||
        (config.width & 1u) != 0 || (config.height & 1u) != 0 ||
        parse_u32(argv[6], 1, MAX_SMOKE_FRAMES, &frames) < 0)
        goto done;
    if (strcmp(argv[5], "i420") == 0)
        config.color_format = ADVC_COLOR_FORMAT_YUV420_PLANAR;
    else if (strcmp(argv[5], "nv12") == 0)
        config.color_format = ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR;
    else if (strcmp(argv[5], "surface") == 0) {
        config.color_format = 0;
        config.transport = ADVC_TRANSPORT_BROKER_EGL_SURFACE;
        local_surface = 1;
    }
    else if (strcmp(argv[5], "ahb") == 0) {
        config.color_format = 0;
        config.transport = ADVC_TRANSPORT_ANDROID_AHB_SURFACE;
        android_ahb_surface = 1;
    }
    else
        goto done;
    if (parse_env_u32("ADVC_SMOKE_TIMEOUT_MS", DEFAULT_DEADLINE_MS,
                      MIN_DEADLINE_MS, MAX_DEADLINE_MS, &timeout_ms) < 0 ||
        parse_env_u32("ADVC_SMOKE_BITRATE", UINT32_C(2000000), 1,
                      ADVC_MAX_ENCODE_BITRATE, &config.bitrate) < 0 ||
        parse_env_u32("ADVC_SMOKE_FRAMERATE_MILLI", UINT32_C(30000), 1000,
                      240000, &config.framerate_milli) < 0 ||
        (expected_component != NULL &&
         (expected_component[0] == '\0' ||
          strnlen(expected_component, ADVC_MAX_CODEC_NAME) >= ADVC_MAX_CODEC_NAME)))
        goto done;
    config.mime = argv[2];
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = strcmp(config.mime, "video/avc") == 0
                                ? ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE
                                : ADVC_ENCODE_PROFILE_HEVC_MAIN;
    if (!local_surface && !android_ahb_surface &&
        advc_client_encode_frame_size(&config, &frame_size) < 0) {
        result.stage = "frame_contract";
        result.error_number = errno;
        goto done;
    }
#if !defined(__ANDROID__)
    if (android_ahb_surface) {
        result.stage = "ahb_platform";
        result.error_number = ENOTSUP;
        goto done;
    }
#endif
    deadline = monotonic_ms();
    if (deadline == UINT64_MAX || deadline > UINT64_MAX - timeout_ms) {
        result.stage = "clock";
        result.error_number = EOVERFLOW;
        goto done;
    }
    deadline += timeout_ms;
    socket_fd = connect_argument(argv[1], deadline);
    if (socket_fd < 0) {
        result.stage = "connect";
        result.error_number = errno;
        goto done;
    }

#define PREPARE_CALL(name)                                                        \
    do {                                                                          \
        if (apply_remaining_timeout(socket_fd, deadline) < 0) {                   \
            call_failed(&result, (name), -1, 0);                                  \
            goto cleanup;                                                         \
        }                                                                         \
    } while (0)

    PREPARE_CALL("hello");
    if (advc_client_hello(socket_fd,
                          ADVC_FEATURE_MEMFD | ADVC_FEATURE_ENCODE |
                              (local_surface ? ADVC_FEATURE_BROKER_EGL_SURFACE : 0) |
                              (android_ahb_surface ?
                                   ADVC_FEATURE_ANDROID_AHB_SURFACE : 0),
                          &features, &max_payload) < 0) {
        call_failed(&result, "hello", -1, 0);
        goto cleanup;
    }
    if ((features & (ADVC_FEATURE_MEMFD | ADVC_FEATURE_ENCODE |
                     (local_surface ? ADVC_FEATURE_BROKER_EGL_SURFACE : 0) |
                     (android_ahb_surface ?
                          ADVC_FEATURE_ANDROID_AHB_SURFACE : 0))) !=
            (ADVC_FEATURE_MEMFD | ADVC_FEATURE_ENCODE |
             (local_surface ? ADVC_FEATURE_BROKER_EGL_SURFACE : 0) |
             (android_ahb_surface ? ADVC_FEATURE_ANDROID_AHB_SURFACE : 0)) ||
        max_payload < ADVC_QUEUE_INPUT_SIZE) {
        errno = EPROTO;
        call_failed(&result, "hello_features", -1, 0);
        goto cleanup;
    }
    PREPARE_CALL("capabilities");
    if (advc_client_query_capabilities(socket_fd, &caps) < 0) {
        call_failed(&result, "capabilities", -1, 0);
        goto cleanup;
    }
    encoder = find_encoder(&caps, config.mime);
    if (encoder == NULL) {
        result.stage = "encoder_identity";
        result.status = ADVC_STATUS_UNSUPPORTED;
        result.error_number = 0;
        goto cleanup;
    }
    component = encoder->codec_name;
    acceleration = acceleration_name(encoder->acceleration);
    if ((expected_component != NULL && strcmp(expected_component, component) != 0) ||
        (env_enabled("ADVC_SMOKE_REQUIRE_HARDWARE") &&
         encoder->acceleration != ADVC_ACCELERATION_HARDWARE)) {
        errno = EPROTO;
        call_failed(&result, "encoder_identity", -1, 0);
        goto cleanup;
    }
    PREPARE_CALL("create");
    status = advc_client_create_session(socket_fd, &config, &session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        call_failed(&result, "create", status, detail);
        goto cleanup;
    }

#if defined(__ANDROID__)
    if (android_ahb_surface) {
        AHardwareBuffer_Desc desc;
        memset(&desc, 0, sizeof(desc));
        desc.width = config.width;
        desc.height = config.height;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
        if (AHardwareBuffer_allocate(&desc, &test_ahb) != 0 || test_ahb == NULL) {
            errno = ENOTSUP;
            call_failed(&result, "allocate_ahb", -1, 0);
            goto cleanup;
        }
    }
#endif

    for (uint32_t frame = 0; frame < frames; ++frame) {
        struct advc_client_input input;
        int frame_fd = (local_surface || android_ahb_surface) ? -1 :
                       create_frame_memfd(&config, frame, frame_size);
        if (!local_surface && !android_ahb_surface && frame_fd < 0) {
            call_failed(&result, "generate_frame", -1, 0);
            goto cleanup;
        }
        memset(&input, 0, sizeof(input));
        input.data_fd = frame_fd;
        input.size = (local_surface || android_ahb_surface) ? 0 : frame_size;
        input.buffer_id = next_buffer_id++;
        input.pts_ns = (uint64_t)frame * UINT64_C(1000000000000) /
                       config.framerate_milli;
        if (android_ahb_surface) {
#if defined(__ANDROID__)
            struct advc_client_ahb_input ahb_input;
            uint32_t vcl_packets_before = result.vcl_packets;
            AHardwareBuffer_Desc desc;
            if (wait_and_close_fence(&ahb_release_fence, deadline) < 0 ||
                fill_test_ahb(test_ahb, config.width, config.height, frame,
                              &ahb_acquire_fence) < 0) {
                call_failed(&result, "prepare_ahb", -1, 0);
                goto cleanup;
            }
            memset(&desc, 0, sizeof(desc));
            AHardwareBuffer_describe(test_ahb, &desc);
            memset(&ahb_input, 0, sizeof(ahb_input));
            ahb_input.native_buffer = test_ahb;
            ahb_input.pts_ns = input.pts_ns;
            ahb_input.width = desc.width;
            ahb_input.height = desc.height;
            ahb_input.format = desc.format;
            ahb_input.layers = desc.layers;
            ahb_input.usage = desc.usage;
            ahb_input.acquire_fence_fd = ahb_acquire_fence;
            status = submit_ahb_with_drain(socket_fd, session_id, &config,
                                           &ahb_input, &result, deadline,
                                           &ahb_release_fence);
            if (ahb_acquire_fence >= 0) close(ahb_acquire_fence);
            ahb_acquire_fence = -1;
            if (status == 0 && wait_and_close_fence(&ahb_release_fence, deadline) < 0)
                status = call_failed(&result, "release_fence", -1, 0);
            while (status == 0 && result.vcl_packets == vcl_packets_before) {
                int consumed = consume_one_output(socket_fd, session_id, &config,
                                                  &result, deadline);
                if (consumed < 0) status = -1;
                else if (consumed == 2) {
                    errno = EPROTO;
                    status = call_failed(&result, "premature_eos", -1, 0);
                } else if (consumed == 0 && sleep_until_retry(deadline) < 0) {
                    status = call_failed(&result, "drain_ahb_frame", -1, 0);
                }
            }
#else
            status = -1;
#endif
        } else {
            status = queue_with_drain(socket_fd, session_id, &config, &input,
                                      &result, deadline, "queue_frame");
        }
        if (frame_fd >= 0) close(frame_fd);
        if (status < 0) goto cleanup;
        if (!android_ahb_surface &&
            env_enabled("ADVC_SMOKE_POLL_AFTER_EACH")) {
            uint32_t before = result.vcl_packets;
            unsigned int polls;
            for (polls = 0; polls < 8; ++polls) {
                int consumed = consume_one_output(
                    socket_fd, session_id, &config, &result, deadline);
                if (consumed < 0) goto cleanup;
                if (consumed == 0) break;
                if (consumed == 2) {
                    errno = EPROTO;
                    call_failed(&result, "premature_eos", -1, 0);
                    goto cleanup;
                }
            }
            fprintf(stderr,
                    "advc-smoke-poll: frame=%" PRIu32
                    " submitted=%" PRIu32 " new_vcl=%" PRIu32
                    " total_vcl=%" PRIu32 " polls=%u\n",
                    frame, frame + 1u, result.vcl_packets - before,
                    result.vcl_packets, polls);
        }
        ++result.frames_queued;
    }
    {
        struct advc_client_input input;
        memset(&input, 0, sizeof(input));
        input.data_fd = -1;
        input.buffer_id = next_buffer_id;
        input.pts_ns = (uint64_t)frames * UINT64_C(1000000000000) /
                       config.framerate_milli;
        input.flags = ADVC_FLAG_END_OF_STREAM;
        if (queue_with_drain(socket_fd, session_id, &config, &input, &result,
                             deadline, "queue_eos") < 0)
            goto cleanup;
    }
    while (!result.eos) {
        int consumed = consume_one_output(socket_fd, session_id, &config, &result,
                                          deadline);
        if (consumed < 0) goto cleanup;
        if (consumed == 0 && sleep_until_retry(deadline) < 0) {
            call_failed(&result, "deadline", -1, 0);
            goto cleanup;
        }
    }
    if (result.nonempty_outputs == 0 || result.vcl_packets != frames ||
        result.pts_nonmonotonic != 0 || !result.have_vcl_pts ||
        result.first_vcl_pts_ns != 0 ||
        result.last_vcl_pts_ns !=
            advc_media_codec_roundtrip_pts_ns(
                (uint64_t)(frames - 1u) * UINT64_C(1000000000000) /
                config.framerate_milli) ||
        (result.parameter_set_mask &
         (strcmp(config.mime, "video/avc") == 0 ? 3u : 7u)) !=
            (strcmp(config.mime, "video/avc") == 0 ? 3u : 7u) ||
        result.vcl_units == 0 || result.key_vcl_units == 0) {
        errno = EPROTO;
        call_failed(&result, "incomplete_bitstream", -1, 0);
        goto cleanup;
    }
    PREPARE_CALL("close");
    status = advc_client_close_session(socket_fd, session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        call_failed(&result, "close", status, detail);
        goto cleanup;
    }
    session_id = 0;
    result.stage = "complete";
    result.status = ADVC_STATUS_OK;
    result.error_number = 0;
    result.detail = 0;
    rc = EXIT_SUCCESS;

cleanup:
#undef PREPARE_CALL
done:
    if (bitstream_output_fd >= 0) {
        close(bitstream_output_fd);
        bitstream_output_fd = -1;
    }
#if defined(__ANDROID__)
    if (ahb_acquire_fence >= 0) close(ahb_acquire_fence);
    if (ahb_release_fence >= 0) close(ahb_release_fence);
    if (test_ahb != NULL) AHardwareBuffer_release(test_ahb);
#endif
    if (socket_fd >= 0) close(socket_fd);
    printf("{\"ok\":%s,\"stage\":", rc == EXIT_SUCCESS ? "true" : "false");
    print_json_string(result.stage);
    printf(",\"status\":%d,\"errno\":%d,\"detail\":%" PRIu32
           ",\"mime\":", result.status, result.error_number, result.detail);
    print_json_string(config.mime != NULL ? config.mime : "");
    printf(",\"component\":");
    print_json_string(component);
    printf(",\"acceleration\":");
    print_json_string(acceleration);
    printf(",\"producer\":");
    print_json_string(local_surface ? "broker-egl-surface" :
                      (android_ahb_surface ? "android-ahb-rgba-test-producer" :
                       "client-byte-frame"));
    printf(",\"width\":%" PRIu32 ",\"height\":%" PRIu32
           ",\"color_format\":%" PRIu32 ",\"bitrate\":%" PRIu32
           ",\"framerate_milli\":%" PRIu32 ",\"deadline_ms\":%" PRIu32
           ",\"frames_requested\":%" PRIu32 ",\"frames_queued\":%" PRIu32
           ",\"outputs\":%" PRIu32 ",\"nonempty_outputs\":%" PRIu32
           ",\"bytes\":%" PRIu64 ",\"config_packets\":%" PRIu32
           ",\"key_packets\":%" PRIu32 ",\"eos_packets\":%" PRIu32
           ",\"annex_b_packets\":%" PRIu32
           ",\"length_prefixed_packets\":%" PRIu32
           ",\"config_record_packets\":%" PRIu32
           ",\"nal_units\":%" PRIu32 ",\"parameter_sets\":%" PRIu32
           ",\"parameter_set_mask\":%" PRIu32
           ",\"vcl_units\":%" PRIu32 ",\"key_vcl_units\":%" PRIu32
           ",\"vcl_packets\":%" PRIu32
           ",\"first_vcl_pts_ns\":%" PRIu64 ",\"last_vcl_pts_ns\":%" PRIu64
           ",\"pts_nonmonotonic\":%" PRIu32 ",\"fnv1a64\":\"%016" PRIx64
           "\",\"eos\":%s}\n",
           config.width, config.height, config.color_format, config.bitrate,
           config.framerate_milli, timeout_ms, frames, result.frames_queued,
           result.outputs, result.nonempty_outputs, result.bytes,
           result.config_packets, result.key_packets, result.eos_packets,
           result.annex_b_packets, result.length_prefixed_packets,
           result.config_record_packets, result.nal_units, result.parameter_sets,
           result.parameter_set_mask, result.vcl_units, result.key_vcl_units,
           result.vcl_packets,
           result.first_vcl_pts_ns,
           result.last_vcl_pts_ns, result.pts_nonmonotonic, result.fnv1a64,
           result.eos ? "true" : "false");
    return rc;
}
