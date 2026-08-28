#include "advc_vaapi_preflight.h"

#include "advc/client.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef ADVC_VAAPI_PREFLIGHT_CONNECT_TIMEOUT_MS
#define ADVC_VAAPI_PREFLIGHT_CONNECT_TIMEOUT_MS 2000u
#endif

static int set_bounded_io(int fd) {
    const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                              sizeof(timeout)) == 0
               ? 0
               : -1;
}

int main(int argc, char **argv) {
    struct advc_capability_set caps;
    struct advc_vaapi_preflight_result result;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    int fd;
    int saved_errno;

    if (argc != 4) {
        fprintf(stderr, "usage: %s SOCKET PROFILE VALIDATION_TOKEN\n",
                argv[0]);
        return 2;
    }
    fd = advc_client_connect_bounded(
        argv[1], ADVC_VAAPI_PREFLIGHT_CONNECT_TIMEOUT_MS);
    if (fd < 0 || set_bounded_io(fd) < 0 ||
        advc_client_hello(fd, UINT64_MAX, &features, &max_payload) < 0 ||
        advc_client_query_capabilities(fd, &caps) < 0) {
        saved_errno = errno == 0 ? EIO : errno;
        if (fd >= 0) close(fd);
        fprintf(stderr, "decode-preflight: transport: %s\n",
                strerror(saved_errno));
        return 1;
    }
    close(fd);
    if (advc_vaapi_preflight_evaluate(&caps, features, argv[2], argv[3],
                                      &result) < 0) {
        saved_errno = errno;
        fprintf(stderr,
                "decode-preflight: profile=%s unavailable: %s\n",
                argv[2], strerror(saved_errno));
        return saved_errno == ENOTSUP ? 3 : 2;
    }
    printf("profile=%s codec=%s max=%ux%u token=exact\n", argv[2],
           result.codec_name, result.max_width, result.max_height);
    (void)max_payload;
    return 0;
}
