#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

static int move_to_fd3(int fd) {
    if (fd != 3) {
        if (dup2(fd, 3) < 0) return -1;
        close(fd);
    }
    return fcntl(3, F_SETFD, 0);
}

static int child_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 125;
}

int main(int argc, char **argv) {
    int pair[2];
    int client_status;
    int broker_status;
    pid_t broker;
    pid_t client;
    int encode_mode = argc == 9 && strcmp(argv[1], "--encode") == 0;
    const char *broker_path;
    const char *client_path;

    if (argc != 4 && !encode_mode) {
        fprintf(stderr,
                "usage: %s BROKER CLIENT ANNEX_B_AVC\n"
                "       %s --encode BROKER CLIENT MIME WIDTH HEIGHT FORMAT FRAMES\n",
                argv[0], argv[0]);
        return 2;
    }
    broker_path = argv[encode_mode ? 2 : 1];
    client_path = argv[encode_mode ? 3 : 2];
    fprintf(stderr, "advc-pair: uid=%u euid=%u\n", (unsigned)getuid(),
            (unsigned)geteuid());
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) {
        perror("socketpair");
        return 1;
    }
    broker = fork();
    if (broker == 0) {
        close(pair[1]);
        if (move_to_fd3(pair[0]) < 0) _exit(126);
        execl(broker_path, broker_path, "--connected-fd", "3", (char *)NULL);
        _exit(127);
    }
    if (broker < 0) {
        perror("fork broker");
        close(pair[0]);
        close(pair[1]);
        return 1;
    }
    client = fork();
    if (client == 0) {
        close(pair[0]);
        if (move_to_fd3(pair[1]) < 0) _exit(126);
        if (encode_mode) {
            execl(client_path, client_path, "fd:3", argv[4], argv[5], argv[6],
                  argv[7], argv[8], (char *)NULL);
        } else {
            execl(client_path, client_path, "fd:3", "video/avc", "320", "240",
                  argv[3], (char *)NULL);
        }
        _exit(127);
    }
    close(pair[0]);
    close(pair[1]);
    if (client < 0) {
        perror("fork client");
        kill(broker, SIGTERM);
        waitpid(broker, &broker_status, 0);
        return 1;
    }
    if (waitpid(client, &client_status, 0) < 0) {
        perror("wait client");
        client_status = 1 << 8;
    }
    kill(broker, SIGTERM);
    if (waitpid(broker, &broker_status, 0) < 0 && errno != ECHILD)
        perror("wait broker");
    return child_status(client_status);
}
