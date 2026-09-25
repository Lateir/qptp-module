/* Quest Pro Touch sampler and haptics bridge.
 * Wire format: 32-byte QPR2 samples, 16-byte QPC1 commands/QPA1 replies.
 * Linux/aarch64 target; root is required to read trackingservice memory.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LEFT_NAME "/dev/ashmem/TS_CONTROLLER_LEFT (deleted)"
#define RIGHT_NAME "/dev/ashmem/TS_CONTROLLER_RIGHT (deleted)"
#define FIELD_OFFSET 0x1f0
#define DISCOVERY_PORT 27183

struct __attribute__((packed)) frame {
    char magic[4];
    uint32_t sequence;
    uint64_t monotonic_ns;
    unsigned char values[16];
};
_Static_assert(sizeof(struct frame) == 32, "QPR2 frame must be 32 bytes");

struct __attribute__((packed)) command {
    char magic[4];
    uint8_t opcode, side, zone, amplitude;
    uint16_t duration_ms;
    uint32_t request_id;
    uint16_t reserved;
};
struct __attribute__((packed)) reply {
    char magic[4];
    uint32_t request_id;
    int32_t status;
    uint32_t reserved;
};
_Static_assert(sizeof(struct command) == 16, "QPC1 command size");
_Static_assert(sizeof(struct reply) == 16, "QPA1 reply size");

struct client_context { int fd; pthread_mutex_t send_lock; };
static int send_all(int socket_fd, const void *buf, size_t length);
static uint64_t now_ns(void);
static void sleep_until(uint64_t ns);

struct controller_status {
    int connected, battery, charging, tracked;
    char id[32], model[64], serial[64], level[32];
};
struct status_context {
    struct client_context *client;
    atomic_bool stop;
};

static void clean_copy(char *dest, size_t capacity, const char *begin, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length && count + 1 < capacity; ++i) {
        unsigned char c = (unsigned char)begin[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '(' || c == ')') dest[count++] = (char)c;
    }
    dest[count] = 0;
}

static void copy_field(char *dest, size_t capacity, const char *line,
                       const char *marker, const char *end_marker) {
    const char *start = strstr(line, marker);
    if (!start) return;
    start += strlen(marker);
    const char *end = end_marker ? strstr(start, end_marker) : NULL;
    if (!end) end = start + strcspn(start, " \t\r\n");
    clean_copy(dest, capacity, start, (size_t)(end - start));
}

typedef void (*line_handler)(char *, void *);
static int run_lines(const char *program, const char *arg, line_handler handler, void *context) {
    int pipes[2];
    if (pipe(pipes)) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (child == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        execl(program, program, arg, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    FILE *out = fdopen(pipes[0], "r");
    if (!out) { close(pipes[0]); return -1; }
    char line[2048];
    while (fgets(line, sizeof(line), out)) handler(line, context);
    fclose(out);
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void device_line(char *line, void *context) {
    struct controller_status *items = context;
    char *start = strstr(line, "[Device ");
    if (!start) return;
    int side = strstr(line, "handedness=Left") ? 0 :
               strstr(line, "handedness=Right") ? 1 : -1;
    if (side < 0) return;
    struct controller_status *item = &items[side];
    start += 8;
    size_t id_length = strspn(start, "0123456789abcdefABCDEF");
    if (!id_length || start[id_length] != ' ') return;
    clean_copy(item->id, sizeof(item->id), start, id_length);
    copy_field(item->serial, sizeof(item->serial), line, "serial=", " model=");
    copy_field(item->model, sizeof(item->model), line, "model=", " version=");
    char *battery = strstr(line, "battery=");
    if (battery) {
        int value = atoi(battery + 8);
        if (value >= 0 && value <= 100) item->battery = value;
    }
    item->connected = 1;
}

struct tracking_context { struct controller_status *items; int section, side, seen; };
static void tracking_line(char *line, void *context) {
    struct tracking_context *ctx = context;
    if (strstr(line, "ControllerTrackerHost Status")) {
        ctx->section = 1; ctx->side = -1; ctx->seen = 1; return;
    }
    if (!ctx->section) return;
    if (strstr(line, "Tracking capability glue")) { ctx->section = 0; return; }
    if (strstr(line, "Left --")) ctx->side = 0;
    else if (strstr(line, "Right --")) ctx->side = 1;
    char *level = strstr(line, "Tracking Level: ");
    if (level && ctx->side >= 0 && ctx->items[ctx->side].connected) {
        struct controller_status *item = &ctx->items[ctx->side];
        copy_field(item->level, sizeof(item->level), level, "Tracking Level: ", " (");
        int tracked, valid;
        char *flags = strstr(level, "(PosTracked=");
        if (flags && sscanf(flags, "(PosTracked=%d, PosValid=%d", &tracked, &valid) == 2)
            item->tracked = tracked == 1 && valid == 1;
    }
}

struct remote_context { struct controller_status *items; int side; };
static void remote_line(char *line, void *context) {
    struct remote_context *ctx = context;
    char *paired = strstr(line, "Paired device: ");
    if (paired) {
        char id[32];
        paired += strlen("Paired device: ");
        clean_copy(id, sizeof(id), paired, strspn(paired, "0123456789abcdefABCDEF"));
        ctx->side = -1;
        for (int i = 0; i < 2; ++i)
            if (ctx->items[i].connected && strcmp(ctx->items[i].id, id) == 0) ctx->side = i;
    }
    char *charging = strstr(line, "Charging: ");
    if (charging && ctx->side >= 0) {
        charging += strlen("Charging: ");
        if (strncmp(charging, "true", 4) == 0) ctx->items[ctx->side].charging = 1;
        else if (strncmp(charging, "false", 5) == 0) ctx->items[ctx->side].charging = 0;
    }
}

static void append_controller(char *json, size_t capacity, size_t *used,
                              const char *name, const struct controller_status *item) {
    char battery[8], charging[8];
    if (item->battery < 0) strcpy(battery, "null");
    else snprintf(battery, sizeof(battery), "%d", item->battery);
    if (item->charging < 0) strcpy(charging, "null");
    else strcpy(charging, item->charging ? "true" : "false");
#define APPEND(...) do { if (*used < capacity) { int count = snprintf(json + *used, capacity - *used, __VA_ARGS__); if (count > 0) *used += (size_t)count; } } while (0)
    APPEND("\"%s\":{\"connected\":%s,\"battery_percent\":%s,\"charging\":%s,\"tracked\":%s",
           name, item->connected ? "true" : "false", battery, charging,
           item->tracked ? "true" : "false");
    if (item->connected) {
        APPEND(",\"device_id\":\"%s\",\"controller_type\":\"%s\"", item->id, item->model);
        if (item->serial[0]) APPEND(",\"serial\":\"%s\"", item->serial);
        if (item->level[0]) APPEND(",\"tracking_level\":\"%s\"", item->level);
    }
    APPEND("}");
#undef APPEND
}

static int make_status_json(char *json, size_t capacity) {
    struct controller_status items[2] = {{.battery = -1, .charging = -1},
                                         {.battery = -1, .charging = -1}};
    if (run_lines("/system_ext/bin/trackinginterface_cli", "ls", device_line, items)) return -1;
    struct tracking_context tracking = {.items = items, .side = -1};
    if (run_lines("/system/bin/dumpsys", "tracking", tracking_line, &tracking) ||
        !tracking.seen) return -1;
    struct remote_context remote = {.items = items, .side = -1};
    if (run_lines("/system/bin/dumpsys", "OVRRemoteService", remote_line, &remote)) return -1;
    size_t used = 0;
    int count = snprintf(json, capacity, "{");
    if (count < 0) return -1;
    used = (size_t)count;
    append_controller(json, capacity, &used, "left", &items[0]);
    if (used < capacity) json[used++] = ',';
    append_controller(json, capacity, &used, "right", &items[1]);
    if (used + 2 > capacity) return -1;
    json[used++] = '}'; json[used] = 0;
    return (int)used;
}

static void *status_loop(void *arg) {
    struct status_context *status = arg;
    while (!atomic_load(&status->stop)) {
        uint64_t started = now_ns();
        char json[1024];
        int length = make_status_json(json, sizeof(json));
        if (length > 0) {
            unsigned char header[8] = {'Q','P','S','1',
                                       (unsigned char)length, (unsigned char)(length >> 8),
                                       (unsigned char)(length >> 16), (unsigned char)(length >> 24)};
            pthread_mutex_lock(&status->client->send_lock);
            int failed = send_all(status->client->fd, header, sizeof(header)) ||
                         send_all(status->client->fd, json, (size_t)length);
            pthread_mutex_unlock(&status->client->send_lock);
            if (failed) break;
        }
        uint64_t next = started + 1000000000ULL;
        if (next > now_ns()) sleep_until(next);
    }
    return NULL;
}

static void *discovery_loop(void *arg) {
    uint16_t tcp_port = *(uint16_t *)arg;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons(DISCOVERY_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) { close(fd); return NULL; }
    for (;;) {
        unsigned char query[64];
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        ssize_t n = recvfrom(fd, query, sizeof(query), 0, (struct sockaddr *)&peer, &peer_size);
        if (n == 4 && memcmp(query, "QPD1", 4) == 0) {
            unsigned char answer[8] = {'Q','P','O','1',
                                       (unsigned char)tcp_port, (unsigned char)(tcp_port >> 8),
                                       1, 0};
            sendto(fd, answer, sizeof(answer), 0, (struct sockaddr *)&peer, peer_size);
        }
    }
    close(fd);
    return NULL;
}

static int read_all(int fd, void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = recv(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static int cli_run(const char *id, const char *a, const char *b, const char *c) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execl("/system_ext/bin/trackinginterface_cli", "trackinginterface_cli",
              "setControllerHapticsMultiSimple", id, a, b, c, (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int controller_id(int side, char *id, size_t capacity) {
    int pipes[2];
    if (pipe(pipes)) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (child == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        execl("/system_ext/bin/trackinginterface_cli", "trackinginterface_cli", "ls", (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    char output[16384];
    size_t used = 0;
    ssize_t n = 0;
    while (used < sizeof(output)-1 && (n = read(pipes[0], output+used, sizeof(output)-1-used)) > 0)
        used += (size_t)n;
    close(pipes[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) || n < 0) return -1;
    output[used] = 0;
    const char *hand = side == 0 ? "handedness=Left" : "handedness=Right";
    int matches = 0;
    for (char *line = strtok(output, "\n"); line; line = strtok(NULL, "\n")) {
        char *start = strstr(line, "[Device ");
        if (!start || !strstr(line, hand)) continue;
        start += 8;
        size_t length = strspn(start, "0123456789abcdefABCDEF");
        if (!length || length >= capacity || start[length] != ' ') continue;
        memcpy(id, start, length); id[length] = 0;
        ++matches;
    }
    return matches == 1 ? 0 : -1;
}

static int haptic(const struct command *cmd) {
    if (cmd->opcode != 1 || cmd->side > 1 || cmd->zone > 2 ||
        cmd->reserved || cmd->duration_ms > 5000 ||
        (cmd->amplitude && !cmd->duration_ms) ||
        (!cmd->amplitude && cmd->duration_ms)) return 1;
    char id[32], value[4];
    if (controller_id(cmd->side, id, sizeof(id))) return 2;
    snprintf(value, sizeof(value), "%u", cmd->amplitude);
    const char *args[3] = {"-1", "-1", "-1"};
    args[cmd->zone] = value;
    if (cli_run(id, args[0], args[1], args[2])) return 3;
    if (cmd->duration_ms) {
        struct timespec pause = {cmd->duration_ms / 1000,
                                 (cmd->duration_ms % 1000) * 1000000L};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
        args[cmd->zone] = "0";
        if (cli_run(id, args[0], args[1], args[2])) return 4;
    }
    return 0;
}

static void *command_loop(void *arg) {
    struct client_context *client = arg;
    struct command cmd;
    while (!read_all(client->fd, &cmd, sizeof(cmd))) {
        struct reply answer = {.magic = {'Q','P','A','1'}, .request_id = cmd.request_id};
        answer.status = memcmp(cmd.magic, "QPC1", 4) ? 1 : haptic(&cmd);
        pthread_mutex_lock(&client->send_lock);
        int result = send_all(client->fd, &answer, sizeof(answer));
        pthread_mutex_unlock(&client->send_lock);
        if (result) break;
    }
    shutdown(client->fd, SHUT_RD);
    return NULL;
}

static int find_pid(void) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(dir)) != NULL) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*entry->d_name == 0 || *end != 0 || pid <= 0) continue;
        char path[96], name[128];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        size_t count = fread(name, 1, sizeof(name)-1, file);
        name[count] = 0;
        const char *base = strrchr(name, '/');
        if (count && strcmp(base ? base + 1 : name, "trackingservice") == 0)
            found = (int)pid;
        fclose(file);
        if (found > 0) break;
    }
    closedir(dir);
    return found;
}

static int find_addresses(int pid, uint64_t *left, uint64_t *right) {
    char path[64], line[1024], mapped[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    *left = *right = 0;
    while (fgets(line, sizeof(line), file)) {
        unsigned long long start, end, offset, inode;
        char perms[8], dev[32];
        mapped[0] = 0;
        int fields = sscanf(line, "%llx-%llx %7s %llx %31s %llu %511[^\n]",
                            &start, &end, perms, &offset, dev, &inode, mapped);
        if (fields != 7 || perms[0] != 'r' || end - start < FIELD_OFFSET + 8) continue;
        if (strcmp(mapped, LEFT_NAME) == 0) *left = (uint64_t)start + FIELD_OFFSET;
        if (strcmp(mapped, RIGHT_NAME) == 0) *right = (uint64_t)start + FIELD_OFFSET;
    }
    fclose(file);
    return (*left && *right) ? 0 : -1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void sleep_until(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

static int send_all(int socket_fd, const void *buf, size_t length) {
    const unsigned char *bytes = buf;
    while (length) {
        ssize_t n = send(socket_fd, bytes, length, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static void serve_client(int socket_fd, int hz, unsigned max_frames) {
    int pid = find_pid();
    uint64_t left, right;
    if (pid < 0 || find_addresses(pid, &left, &right) != 0) return;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int mem_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (mem_fd < 0) return;
    int one = 1;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct client_context client = {.fd = socket_fd, .send_lock = PTHREAD_MUTEX_INITIALIZER};
    pthread_t reader;
    if (pthread_create(&reader, NULL, command_loop, &client)) { close(mem_fd); return; }
    struct status_context status = {.client = &client};
    atomic_init(&status.stop, false);
    pthread_t status_thread;
    if (pthread_create(&status_thread, NULL, status_loop, &status)) {
        shutdown(socket_fd, SHUT_RD);
        pthread_join(reader, NULL);
        pthread_mutex_destroy(&client.send_lock);
        close(mem_fd);
        return;
    }
    const uint64_t period_ns = 1000000000ULL / (unsigned)hz;
    uint64_t next = now_ns();
    struct frame sample = {.magic = {'Q','P','R','2'}};
    unsigned char previous[16];
    int have_previous = 0;
    uint64_t last_sent = 0;
    unsigned sequence = 0;
    for (unsigned i = 0; !max_frames || i < max_frames; ++i) {
        sleep_until(next);
        sample.monotonic_ns = now_ns();
        if (pread(mem_fd, sample.values, 8, (off_t)left) != 8 ||
            pread(mem_fd, sample.values + 8, 8, (off_t)right) != 8) break;
        if (!have_previous || memcmp(sample.values, previous, sizeof(previous)) != 0 ||
            sample.monotonic_ns - last_sent >= 195000000ULL) {
            sample.sequence = sequence++;
            pthread_mutex_lock(&client.send_lock);
            int sent = send_all(socket_fd, &sample, sizeof(sample));
            pthread_mutex_unlock(&client.send_lock);
            if (sent != 0) break;
            memcpy(previous, sample.values, sizeof(previous));
            have_previous = 1;
            last_sent = sample.monotonic_ns;
        }
        next += period_ns;
        uint64_t now = now_ns();
        if (next <= now) next = now + period_ns;
    }
    atomic_store(&status.stop, true);
    shutdown(socket_fd, SHUT_RD);
    pthread_join(reader, NULL);
    pthread_join(status_thread, NULL);
    pthread_mutex_destroy(&client.send_lock);
    close(mem_fd);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 27182;
    int hz = argc > 2 ? atoi(argv[2]) : 100;
    unsigned max_frames = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 0;
    if (port < 1024 || port > 65535 || hz < 1 || hz > 500) return 2;
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return 3;
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((uint16_t)port),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 4)) {
        close(server);
        return 4;
    }
    uint16_t tcp_port = (uint16_t)port;
    pthread_t discovery;
    if (pthread_create(&discovery, NULL, discovery_loop, &tcp_port) == 0)
        pthread_detach(discovery);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve_client(client, hz, max_frames);
        close(client);
    }
    close(server);
    return 0;
}
