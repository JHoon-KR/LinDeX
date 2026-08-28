#include "advc/client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/run/android-drm/advc-broker.sock";
    struct advc_capability_set caps;
    uint64_t features;
    uint32_t max_payload;
    char json[32768];
    int fd = advc_client_connect(path);
    if (fd < 0) {
        fprintf(stderr, "advc-ctl: connect %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (advc_client_hello(fd, ADVC_FEATURE_MEMFD | ADVC_FEATURE_DECODE,
                          &features, &max_payload) < 0 ||
        advc_client_query_capabilities(fd, &caps) < 0) {
        fprintf(stderr, "advc-ctl: broker query failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    caps.transport_features &= features;
    if (advc_capabilities_write_json(&caps, json, sizeof(json)) < 0) {
        fprintf(stderr, "advc-ctl: capability JSON overflow\n");
        return 1;
    }
    puts(json);
    (void)max_payload;
    return 0;
}
