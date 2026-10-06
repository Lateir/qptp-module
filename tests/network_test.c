/* Run independently of trackingservice; exercises production socket helpers. */
#define main streamer_main
#include "../src/streamer.c"
#undef main
#include <assert.h>

static void test_stalled_send(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int small = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    unsigned char *data = calloc(1, 1024 * 1024);
    assert(data);
    uint64_t started = now_ns();
    assert(send_all(sockets[0], data, 1024 * 1024) == -1);
    assert(errno == ETIMEDOUT);
    uint64_t elapsed = now_ns() - started;
    assert(elapsed >= 1900000000ULL && elapsed < 3500000000ULL);
    free(data); close(sockets[0]); close(sockets[1]);
}

static void *blocked_reader(void *arg) {
    struct command cmd;
    assert(read_all(*(int *)arg, &cmd, sizeof(cmd)) == -1);
    return NULL;
}

static void test_shutdown_and_reconnect(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 4) == 0);
    socklen_t size = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &size) == 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
        int client = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(client, (struct sockaddr *)&addr, size) == 0);
        int server = accept(listener, NULL, NULL);
        assert(server >= 0);
        unsigned timeout = 5000;
        assert(setsockopt(server, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout)) == 0);
        assert(send_all(server, "QPR3", 4) == 0);
        char header[4]; assert(read_all(client, header, sizeof(header)) == 0);
        assert(memcmp(header, "QPR3", 4) == 0);
        pthread_t reader;
        assert(pthread_create(&reader, NULL, blocked_reader, &server) == 0);
        assert(shutdown(server, SHUT_RDWR) == 0);
        assert(pthread_join(reader, NULL) == 0);
        close(server); close(client);
    }
    close(listener);
}

int main(void) {
    test_stalled_send();
    test_shutdown_and_reconnect();
    puts("network tests passed");
    return 0;
}
