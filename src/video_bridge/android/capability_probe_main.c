#include "advc/capabilities.h"
#include "binder_pool.h"

#include <stdio.h>

int main(void) {
    struct advc_capability_set caps;
    char error[256];
    char json[32768];
    (void)advc_start_binder_thread_pool("advc-capability-probe");
    if (advc_probe_android_capabilities(&caps, error, sizeof(error)) < 0) {
        fprintf(stderr, "advc-capability-probe: %s\n", error);
        return 1;
    }
    if (advc_capabilities_write_json(&caps, json, sizeof(json)) < 0) {
        fprintf(stderr, "advc-capability-probe: JSON output overflow\n");
        return 1;
    }
    puts(json);
    return 0;
}
