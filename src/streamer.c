/* Read-only, persistent Quest Pro Touch controller sampler.
 * Wire format: 32-byte QPR2 frame: magic, sequence, monotonic ns, <HHfHHf>.
 * Linux/aarch64 target; root is required to read trackingservice memory.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LEFT_NAME "/dev/ashmem/TS_CONTROLLER_LEFT (deleted)"
#define RIGHT_NAME "/dev/ashmem/TS_CONTROLLER_RIGHT (deleted)"
#define FIELD_OFFSET 0x1f0

struct __attribute__((packed)) frame {
    char magic[4];
    uint32_t sequence;
    uint64_t monotonic_ns;
    unsigned char values[16];
};
_Static_assert(sizeof(struct frame) == 32, "QPR2 frame must be 32 bytes");

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
    const uint64_t period_ns = 1000000000ULL / (unsigned)hz;
    uint64_t next = now_ns();
    struct frame sample = {.magic = {'Q','P','R','2'}};
    for (unsigned i = 0; !max_frames || i < max_frames; ++i) {
        sleep_until(next);
        sample.monotonic_ns = now_ns();
        if (pread(mem_fd, sample.values, 8, (off_t)left) != 8 ||
            pread(mem_fd, sample.values + 8, 8, (off_t)right) != 8) break;
        sample.sequence = i;
        if (send_all(socket_fd, &sample, sizeof(sample)) != 0) break;
        next += period_ns;
        uint64_t now = now_ns();
        if (next <= now) next = now + period_ns;
    }
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
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 4)) {
        close(server);
        return 4;
    }
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
