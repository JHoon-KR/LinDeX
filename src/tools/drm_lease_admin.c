// SPDX-License-Identifier: MIT
// Minimal DRM lease inventory and explicit revoke helper.

#define ACTIVE_HANDOFF_MAIN drm_lease_admin_embedded_handoff_main
#include "../launcher/active_drm_handoff.c"
#undef ACTIVE_HANDOFF_MAIN

int main(int argc, char **argv)
{
    struct stat card0;
    int master_pid = -1, pidfd = -1, master_fd = -1;
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START DRM lease inventory/admin");
    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode)) {
        puts("/dev/dri/card0 unavailable");
        return 1;
    }

    if (argc == 3 && strcmp(argv[1], "--revoke") == 0) {
        char *end = NULL;
        unsigned long id = strtoul(argv[2], &end, 10);
        if (!argv[2][0] || !end || *end || id == 0 || id > UINT32_MAX) {
            puts("invalid lessee id");
            return 2;
        }
        result = ah_revoke_lessee((uint32_t)id) == 0 ? 0 : 1;
        goto done;
    }
    if (argc != 1) {
        printf("usage: %s [--revoke LESSEE_ID]\n", argv[0]);
        result = 2;
        goto done;
    }

    if (auto_find_master(&card0, &master_pid, &pidfd, &master_fd) != 0)
        goto done;
    printf("current master pid=%d local_fd=%d\n", master_pid, master_fd);
    if (list_lessees(master_fd, "current") != 0)
        goto done;

    result = 0;

done:
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    printf("DONE exit=%d\n", result);
    return result;
}
