#include "advc/client.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    struct advc_capability_set caps;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    uint32_t i;
    int fd;
    if (argc != 2) {
        fprintf(stderr, "usage: %s socket\n", argv[0]);
        return 2;
    }
    fd = advc_client_connect(argv[1]);
    if (fd < 0) {
        fprintf(stderr, "connect: %s\n", strerror(errno));
        return 1;
    }
    if (advc_client_hello(fd, UINT64_MAX, &features, &max_payload) < 0) {
        fprintf(stderr, "hello: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (advc_client_query_capabilities(fd, &caps) < 0) {
        fprintf(stderr, "capabilities: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    printf("features=%" PRIu64 " max_payload=%u caps_transport=%" PRIu64
           " count=%u\n",
           features, max_payload, caps.transport_features, caps.count);
    for (i = 0; i < caps.count; ++i) {
        printf("%s direction=%u acceleration=%u max=%ux%u\n",
               caps.codecs[i].mime, caps.codecs[i].direction,
               caps.codecs[i].acceleration, caps.codecs[i].max_width,
               caps.codecs[i].max_height);
    }
    return 0;
}
