#define _GNU_SOURCE
#define ADVC_GATEWAY_NO_MAIN 1
#define ADVC_GATEWAY_IO_TIMEOUT_MS 100u
#define ADVC_GATEWAY_CONNECT_TIMEOUT_MS 100u
#define ADVC_GATEWAY_SOCKET_PREFIX "/tmp/"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../tools/advc_repack_gateway.c"

enum mock_mode {
    MOCK_NORMAL = 0,
    MOCK_DISCONNECT = 1,
    MOCK_STALL = 2,
};

struct mock_server {
    int listener_fd;
    enum mock_mode mode;
    unsigned int expected_rounds;
    unsigned int hellos;
    unsigned int sessions;
    unsigned int eos_inputs;
    unsigned int flushes;
    unsigned int closes;
    int saw_disconnect;
    int failed;
};

struct worker_handle {
    int client_fd;
    pthread_t thread;
};

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
            ++count;
    closedir(directory);
    return count;
}

static int make_listener_with_backlog(char *path, size_t capacity,
                                      unsigned int sequence, int backlog) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(snprintf(path, capacity, "/tmp/lindex-gateway-upstream-%ld-%u.sock",
                    (long)getpid(), sequence) > 0);
    unlink(path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, backlog) == 0);
    return fd;
}

static int make_listener(char *path, size_t capacity, unsigned int sequence) {
    return make_listener_with_backlog(path, capacity, sequence, 4);
}

static void send_mock_status(int fd, const struct advc_message *request,
                             uint32_t session_id) {
    uint8_t payload[ADVC_STATUS_SIZE] = {0};
    struct advc_message reply;
    memset(&reply, 0, sizeof(reply));
    reply.header = request->header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.session_id = session_id;
    reply.header.payload_size = sizeof(payload);
    advc_put_u32(payload + ADVC_STATUS_CODE_OFFSET, ADVC_STATUS_OK);
    reply.payload = payload;
    assert(advc_send_message(fd, &reply) == 0);
}

static void *run_mock_server(void *opaque) {
    struct mock_server *server = opaque;
    uint8_t payload[ADVC_MAX_PAYLOAD];
    uint32_t current_session = 0;
    int fd = accept4(server->listener_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
        server->failed = 1;
        return NULL;
    }
    for (;;) {
        struct advc_message request;
        prepare_message(&request, payload);
        if (advc_receive_message(fd, &request) < 0) {
            if (errno == ECONNRESET) server->saw_disconnect = 1;
            break;
        }
        if (server->mode == MOCK_STALL) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 400000000};
            advc_close_message_fds(&request);
            nanosleep(&delay, NULL);
            break;
        }
        switch (request.header.opcode) {
        case ADVC_OP_HELLO: {
            uint8_t hello[ADVC_HELLO_SIZE] = {0};
            struct advc_message reply;
            if (request.header.version_minor != ADVC_GATEWAY_UPSTREAM_MINOR)
                server->failed = 1;
            ++server->hellos;
            advc_put_u64(hello + ADVC_HELLO_FEATURES_OFFSET,
                         ADVC_FEATURE_DECODE | ADVC_FEATURE_DECODE_PRIME |
                             ADVC_FEATURE_DMABUF |
                             ADVC_FEATURE_NATIVE_FENCE);
            advc_put_u32(hello + ADVC_HELLO_MAX_PAYLOAD_OFFSET,
                         ADVC_MAX_PAYLOAD);
            memset(&reply, 0, sizeof(reply));
            reply.header = request.header;
            reply.header.message_type = ADVC_MSG_REPLY;
            reply.header.payload_size = sizeof(hello);
            reply.payload = hello;
            assert(advc_send_message(fd, &reply) == 0);
            break;
        }
        case ADVC_OP_CREATE_SESSION:
            current_session = ++server->sessions;
            send_mock_status(fd, &request, current_session);
            break;
        case ADVC_OP_QUEUE_INPUT:
            if (request.header.session_id != current_session ||
                request.header.payload_size != ADVC_QUEUE_INPUT_SIZE ||
                advc_get_u32(request.payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) !=
                    ADVC_FLAG_END_OF_STREAM)
                server->failed = 1;
            ++server->eos_inputs;
            send_mock_status(fd, &request, current_session);
            break;
        case ADVC_OP_FLUSH:
            ++server->flushes;
            send_mock_status(fd, &request, current_session);
            break;
        case ADVC_OP_CLOSE_SESSION:
            ++server->closes;
            send_mock_status(fd, &request, current_session);
            current_session = 0;
            break;
        default:
            server->failed = 1;
            break;
        }
        advc_close_message_fds(&request);
        if (server->failed) break;
    }
    close(fd);
    return NULL;
}

static struct worker_handle start_worker(const char *upstream_path) {
    struct worker_arguments *arguments = calloc(1, sizeof(*arguments));
    struct worker_handle handle;
    int sockets[2];
    assert(arguments != NULL);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) ==
           0);
    assert(set_socket_watchdog(sockets[1], 0, 1) == 0);
    arguments->downstream_fd = sockets[1];
    strcpy(arguments->upstream_path, upstream_path);
    atomic_store_explicit(&active_clients, 1, memory_order_relaxed);
    stop_requested = 0;
    assert(pthread_create(&handle.thread, NULL, client_worker, arguments) == 0);
    handle.client_fd = sockets[0];
    return handle;
}

static void finish_worker(struct worker_handle *worker) {
    if (worker->client_fd >= 0) {
        close(worker->client_fd);
        worker->client_fd = -1;
    }
    assert(pthread_join(worker->thread, NULL) == 0);
    assert(atomic_load_explicit(&active_clients, memory_order_relaxed) == 0);
}

static struct advc_client_session_config decode_config(void) {
    struct advc_client_session_config config;
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 1920;
    config.height = 1080;
    config.framerate_milli = 60000;
    config.transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    return config;
}

static void test_v18_hello_bridges_v17_upstream(void) {
    struct mock_server server;
    struct worker_handle worker;
    pthread_t server_thread;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    memset(&server, 0, sizeof(server));
    server.mode = MOCK_NORMAL;
    server.listener_fd = make_listener(path, sizeof(path), 17);
    assert(pthread_create(&server_thread, NULL, run_mock_server, &server) == 0);
    worker = start_worker(path);
    assert(advc_client_hello(worker.client_fd, UINT64_MAX, &features,
                             &max_payload) == 0);
    assert((features & ADVC_FEATURE_ASYNC_DECODE_PRIME) != 0);
    assert(max_payload == ADVC_MAX_PAYLOAD);
    finish_worker(&worker);
    assert(pthread_join(server_thread, NULL) == 0);
    close(server.listener_fd);
    unlink(path);
    assert(!server.failed && server.hellos == 1);
}

static void test_repeated_seek_eos_teardown(void) {
    enum { ROUNDS = 128 };
    struct advc_client_session_config config = decode_config();
    struct advc_client_input eos;
    struct mock_server server;
    struct worker_handle worker;
    pthread_t server_thread;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t baseline = open_fd_count();
    memset(&server, 0, sizeof(server));
    server.mode = MOCK_NORMAL;
    server.expected_rounds = ROUNDS;
    server.listener_fd = make_listener(path, sizeof(path), 1);
    assert(pthread_create(&server_thread, NULL, run_mock_server, &server) == 0);
    worker = start_worker(path);
    memset(&eos, 0, sizeof(eos));
    eos.data_fd = -1;
    eos.flags = ADVC_FLAG_END_OF_STREAM;
    for (unsigned int i = 0; i < ROUNDS; ++i) {
        uint32_t session_id = 0;
        assert(advc_client_create_session(worker.client_fd, &config,
                                          &session_id, NULL) ==
               ADVC_STATUS_OK);
        assert(session_id != 0);
        assert(advc_client_queue_input(worker.client_fd, session_id, &eos,
                                       NULL) == ADVC_STATUS_OK);
        assert(advc_client_flush(worker.client_fd, session_id, NULL) ==
               ADVC_STATUS_OK);
        assert(advc_client_close_session(worker.client_fd, session_id, NULL) ==
               ADVC_STATUS_OK);
    }
    finish_worker(&worker);
    assert(pthread_join(server_thread, NULL) == 0);
    close(server.listener_fd);
    unlink(path);
    assert(!server.failed && server.sessions == ROUNDS &&
           server.eos_inputs == ROUNDS && server.flushes == ROUNDS &&
           server.closes == ROUNDS && server.saw_disconnect);
    assert(open_fd_count() == baseline);
}

static void test_repeated_abrupt_disconnect(void) {
    struct advc_client_session_config config = decode_config();
    size_t baseline = open_fd_count();
    for (unsigned int round = 0; round < 32; ++round) {
        struct mock_server server;
        struct worker_handle worker;
        pthread_t server_thread;
        uint32_t session_id = 0;
        char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
        memset(&server, 0, sizeof(server));
        server.mode = MOCK_DISCONNECT;
        server.listener_fd = make_listener(path, sizeof(path), 100 + round);
        assert(pthread_create(&server_thread, NULL, run_mock_server, &server) ==
               0);
        worker = start_worker(path);
        assert(advc_client_create_session(worker.client_fd, &config,
                                          &session_id, NULL) ==
               ADVC_STATUS_OK);
        finish_worker(&worker);
        assert(pthread_join(server_thread, NULL) == 0);
        close(server.listener_fd);
        unlink(path);
        assert(!server.failed && server.sessions == 1 &&
               server.saw_disconnect);
    }
    assert(open_fd_count() == baseline);
}

static void test_upstream_watchdog_fail_closed(void) {
    struct advc_client_session_config config = decode_config();
    struct mock_server server;
    struct worker_handle worker;
    struct timespec start;
    struct timespec finish;
    pthread_t server_thread;
    uint32_t session_id = 0;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    memset(&server, 0, sizeof(server));
    server.mode = MOCK_STALL;
    server.listener_fd = make_listener(path, sizeof(path), 999);
    assert(pthread_create(&server_thread, NULL, run_mock_server, &server) == 0);
    worker = start_worker(path);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(advc_client_create_session(worker.client_fd, &config,
                                      &session_id, NULL) < 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L <
           1000L);
    finish_worker(&worker);
    assert(pthread_join(server_thread, NULL) == 0);
    close(server.listener_fd);
    unlink(path);
}

static void test_upstream_connect_watchdog_fail_closed(void) {
    enum { MAX_FILLERS = 16 };
    struct gateway_client client;
    struct sockaddr_un address;
    struct timespec start;
    struct timespec finish;
    char path[sizeof(address.sun_path)];
    int fillers[MAX_FILLERS];
    size_t baseline = open_fd_count();
    size_t filler_count = 0;
    int listener =
        make_listener_with_backlog(path, sizeof(path), 1001, 1);

    memset(&client, 0, sizeof(client));
    client.downstream_fd = -1;
    client.upstream_fd = -1;
    client.upstream_path = path;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    while (filler_count < MAX_FILLERS) {
        int fd = socket(AF_UNIX,
                        SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            fillers[filler_count++] = fd;
            continue;
        }
        assert(errno == EAGAIN);
        close(fd);
        break;
    }
    assert(filler_count > 0 && filler_count < MAX_FILLERS);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    errno = 0;
    assert(ensure_upstream(&client) < 0);
    assert(errno == ETIMEDOUT);
    assert(client.upstream_fd == -1);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L >=
           50L);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L <
           1000L);
    while (filler_count > 0) close(fillers[--filler_count]);
    close(listener);
    unlink(path);
    assert(open_fd_count() == baseline);
}

static void test_release_watchdog_poisons_upstream(void) {
    struct gateway_client client;
    struct mock_server server;
    struct timespec start;
    struct timespec finish;
    pthread_t server_thread;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    memset(&client, 0, sizeof(client));
    memset(&server, 0, sizeof(server));
    client.downstream_fd = -1;
    client.upstream_fd = -1;
    server.mode = MOCK_STALL;
    server.listener_fd = make_listener(path, sizeof(path), 1000);
    client.upstream_path = path;
    assert(pthread_create(&server_thread, NULL, run_mock_server, &server) == 0);
    assert(ensure_upstream(&client) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(release_upstream_output(&client, 1, 1, -1) < 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L <
           1000L);
    assert(client.upstream_fd == -1);
    assert(pthread_join(server_thread, NULL) == 0);
    close(server.listener_fd);
    unlink(path);
}

static void test_stale_socket_recovery_and_live_preservation(void) {
    struct sockaddr_un address;
    char path[sizeof(address.sun_path)];
    int stale;
    int listener;
    int duplicate;
    assert(snprintf(path, sizeof(path), "/tmp/lindex-gateway-stale-%ld.sock",
                    (long)getpid()) > 0);
    unlink(path);
    stale = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(stale >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(bind(stale, (struct sockaddr *)&address, sizeof(address)) == 0);
    close(stale);
    listener = create_listener(path);
    assert(listener >= 0);
    duplicate = create_listener(path);
    assert(duplicate < 0);
    assert(access(path, F_OK) == 0);
    close(listener);
    unlink(path);
}

static void test_full_live_socket_probe_is_bounded_and_preserved(void) {
    enum { MAX_FILLERS = 16 };
    struct sockaddr_un address;
    struct timespec start;
    struct timespec finish;
    char path[sizeof(address.sun_path)];
    int fillers[MAX_FILLERS];
    size_t baseline = open_fd_count();
    size_t filler_count = 0;
    int listener =
        make_listener_with_backlog(path, sizeof(path), 1002, 1);
    int duplicate;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    while (filler_count < MAX_FILLERS) {
        int fd = socket(AF_UNIX,
                        SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            fillers[filler_count++] = fd;
            continue;
        }
        assert(errno == EAGAIN);
        close(fd);
        break;
    }
    assert(filler_count > 0 && filler_count < MAX_FILLERS);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    duplicate = create_listener(path);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert(duplicate < 0 && errno == ETIMEDOUT);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L >=
           50L);
    assert((finish.tv_sec - start.tv_sec) * 1000L +
               (finish.tv_nsec - start.tv_nsec) / 1000000L <
           1000L);
    assert(access(path, F_OK) == 0);
    while (filler_count > 0) close(fillers[--filler_count]);
    close(listener);
    unlink(path);
    assert(open_fd_count() == baseline);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_v18_hello_bridges_v17_upstream();
    test_repeated_seek_eos_teardown();
    test_repeated_abrupt_disconnect();
    test_upstream_watchdog_fail_closed();
    test_upstream_connect_watchdog_fail_closed();
    test_release_watchdog_poisons_upstream();
    test_stale_socket_recovery_and_live_preservation();
    test_full_live_socket_probe_is_bounded_and_preserved();
    return 0;
}
