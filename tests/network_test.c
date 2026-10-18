/* Run independently of trackingservice; exercises production socket helpers. */
#undef NDEBUG  // zig cc -O2 defines NDEBUG, which would compile every assert out
#define main streamer_main
#include "../src/streamer.c"
#undef main
#include <assert.h>
#include <sys/mman.h>

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

static struct reply exchange(int fd, uint8_t opcode, uint8_t side, uint32_t request_id) {
    struct command cmd = {.magic = {'Q','P','C','1'}, .opcode = opcode, .side = side,
                          .request_id = request_id};
    struct reply answer;
    assert(send_all(fd, &cmd, sizeof(cmd)) == 0);
    assert(read_all(fd, &answer, sizeof(answer)) == 0);
    assert(memcmp(answer.magic, "QPA1", 4) == 0 && answer.request_id == request_id);
    return answer;
}

static void test_frame_format_command(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct client_context client = {.fd = sockets[0], .send_lock = PTHREAD_MUTEX_INITIALIZER};
    atomic_init(&client.frame_format, 3);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, command_loop, &client) == 0);
    assert(exchange(sockets[1], 2, 4, 11).status == 0);
    assert(atomic_load(&client.frame_format) == 4);
    assert(exchange(sockets[1], 2, 5, 12).status == 1);
    assert(atomic_load(&client.frame_format) == 4);
    assert(exchange(sockets[1], 9, 0, 13).status == 1);  // unknown opcode keeps the connection
    assert(exchange(sockets[1], 2, 3, 14).status == 0);
    assert(atomic_load(&client.frame_format) == 3);
    struct command bad = {.magic = {'Q','P','C','1'}, .opcode = 2, .side = 4, .amplitude = 1,
                          .request_id = 15};
    struct reply answer;
    assert(send_all(sockets[1], &bad, sizeof(bad)) == 0);
    assert(read_all(sockets[1], &answer, sizeof(answer)) == 0 && answer.status == 1);
    assert(atomic_load(&client.frame_format) == 3);
    close(sockets[1]);
    assert(pthread_join(reader, NULL) == 0);
    pthread_mutex_destroy(&client.send_lock);
    close(sockets[0]);
}

static void put(unsigned char *page, size_t offset, const void *value, size_t size) {
    memcpy(page + offset, value, size);
}

static void test_seqlock_read(void) {
    unsigned char page[4096] = {0};
    uint32_t touch_seq = 5, trigger_seq = 2;
    uint16_t x = 200, y = 40;
    float force = 0.5f, size = 3.25f, stylus = 0.75f, proximity = 0.125f, slide = 0.625f, junk = 9.0f;
    put(page, TOUCH_SEQ_OFFSET, &touch_seq, 4);
    put(page, TOUCH_RECORD_OFFSET + 16, &x, 2); put(page, TOUCH_RECORD_OFFSET + 18, &y, 2);
    put(page, TOUCH_RECORD_OFFSET + 20, &force, 4); put(page, TOUCH_RECORD_OFFSET + 24, &size, 4);
    page[TOUCH_RECORD_OFFSET + 28] = 1;
    put(page, TOUCH_RECORD_OFFSET + 4, &junk, 4);
    put(page, TRIGGER_SEQ_OFFSET, &trigger_seq, 4);
    put(page, TRIGGER_RECORD_OFFSET, &proximity, 4); put(page, TRIGGER_RECORD_OFFSET + 4, &slide, 4);
    put(page, TRIGGER_RECORD_OFFSET + TRIGGER_RECORD_BYTES, &junk, 4);
    put(page, STYLUS_OFFSET, &stylus, 4);
    int fd = memfd_create("qptp-test", 0);  // Android has no /tmp for tmpfile()
    assert(fd >= 0 && write(fd, page, sizeof(page)) == (ssize_t)sizeof(page));
    struct side_sample side = {0};
    assert(read_side(fd, 0, &side) == 0);
    assert(side.x == 200 && side.y == 40 && side.force == 0.5f && side.size == 3.25f);
    assert(side.state == 1 && side.stylus == 0.75f);
    assert(side.proximity == 0.125f && side.slide == 0.625f);
    assert(read_side(fd, 4096, &side) == -1);
    close(fd);
}

static void test_frame_layout(void) {
    struct side_sample sides[2] = {
        {.x = 0x0102, .y = 0x0304, .force = 1.0f, .size = 2.0f, .stylus = 3.0f,
         .proximity = 4.0f, .slide = 5.0f, .state = 1},
        {.x = 0x0506, .y = 0x0708, .force = 6.0f, .size = 7.0f, .stylus = 8.0f,
         .proximity = 9.0f, .slide = 10.0f, .state = 0x81}};
    unsigned char out[QPR4_BYTES];
    float value;
    memset(out, 0xee, sizeof(out));
    assert(build_frame(out, 4, 0x11223344, 0x0102030405060708ULL, sides) == 72);
    assert(memcmp(out, "QPR4", 4) == 0 && out[4] == 0x44 && out[7] == 0x11);
    assert(out[8] == 0x08 && out[15] == 0x01);
    for (int i = 0; i < 2; ++i) {
        const unsigned char *b = out + 16 + 28 * i;
        assert(b[0] == (i ? 0x06 : 0x02) && b[1] == (i ? 0x05 : 0x01));
        assert(b[2] == (i ? 0x08 : 0x04) && b[3] == (i ? 0x07 : 0x03));
        for (int field = 0; field < 5; ++field) {
            memcpy(&value, b + 4 + 4 * field, 4);
            assert(value == (float)(1 + field + 5 * i));
        }
        assert(b[24] == (i ? 0x81 : 1) && !b[25] && !b[26] && !b[27]);
    }
    assert(build_frame(out, 3, 7, 9, sides) == 56);
    assert(memcmp(out, "QPR3", 4) == 0 && out[4] == 7 && out[8] == 9);
    const float qpr3[2][4] = {{1, 3, 4, 5}, {6, 8, 9, 10}};
    for (int i = 0; i < 2; ++i) {
        const unsigned char *b = out + 16 + 20 * i;
        assert(b[0] == (i ? 0x06 : 0x02) && b[3] == (i ? 0x07 : 0x03));
        for (int field = 0; field < 4; ++field) {
            memcpy(&value, b + 4 + 4 * field, 4);
            assert(value == qpr3[i][field]);
        }
    }
}

int main(void) {
    test_frame_layout();
    test_seqlock_read();
    test_frame_format_command();
    test_stalled_send();
    test_shutdown_and_reconnect();
    puts("protocol and network tests passed");
    return 0;
}
