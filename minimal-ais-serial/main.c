#define _POSIX_C_SOURCE 200112L

#include "tracking.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#define MAX_DESTS 32
#define LINE_MAX_LEN 1024

struct dest {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char label[256];
    int interval;
    struct mmsi_tracker tracker;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s -s serial [-b baud] [-v] -d host:port[:Ns] [-d host:port[:Ns] ...]\n"
            "\n"
            "  -s serial     serial device, for example /dev/ttyUSB0\n"
            "  -b baud       serial baud rate, default 38400\n"
            "  -d host:port  UDP destination; add :Ns to rate limit each MMSI\n"
            "  -v            print valid AIS lines as they are read\n",
            prog);
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
#ifdef B57600
    case 57600:
        return B57600;
#endif
#ifdef B115200
    case 115200:
        return B115200;
#endif
    default:
        return 0;
    }
}

static void set_raw_mode(struct termios *tio)
{
    tio->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio->c_oflag &= ~OPOST;
    tio->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG);
#ifdef IEXTEN
    tio->c_lflag &= ~IEXTEN;
#endif
    tio->c_cflag &= ~(CSIZE | PARENB);
    tio->c_cflag |= CS8;
}

static int open_serial(const char *path, int baud)
{
    int fd;
    struct termios tio;
    speed_t speed = baud_to_speed(baud);

    if (speed == 0) {
        fprintf(stderr, "unsupported baud rate: %d\n", baud);
        return -1;
    }

    fd = open(path, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    set_raw_mode(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) {
        perror("cfset*speed");
        close(fd);
        return -1;
    }

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

static int parse_dest_rate(const char *spec, char *addr_spec, size_t addrsz,
                           int *interval)
{
    size_t len = strlen(spec);
    size_t i;

    *interval = 0;

    if (len >= 3 && spec[len - 1] == 's') {
        i = len - 2;
        while (i > 0 && isdigit((unsigned char)spec[i])) {
            i--;
        }
        if (spec[i] == ':' && i + 1 < len - 1) {
            long seconds = strtol(spec + i + 1, NULL, 10);
            if (seconds < 0 || seconds > 2147483647L || i >= addrsz) {
                return -1;
            }
            memcpy(addr_spec, spec, i);
            addr_spec[i] = '\0';
            *interval = (int)seconds;
            return addr_spec[0] == '\0' ? -1 : 0;
        }
    }

    if (len >= addrsz) {
        return -1;
    }
    memcpy(addr_spec, spec, len + 1);
    return 0;
}

static int parse_dest_spec(const char *spec, char *host, size_t hostsz,
                           char *port, size_t portsz, int *interval)
{
    char addr_spec[512];
    const char *colon;
    size_t hostlen;

    if (parse_dest_rate(spec, addr_spec, sizeof(addr_spec), interval) < 0) {
        return -1;
    }

    if (addr_spec[0] == '[') {
        const char *end = strchr(addr_spec, ']');
        if (end == NULL || end[1] != ':') {
            return -1;
        }
        hostlen = (size_t)(end - addr_spec - 1);
        if (hostlen == 0 || hostlen >= hostsz) {
            return -1;
        }
        memcpy(host, addr_spec + 1, hostlen);
        host[hostlen] = '\0';
        snprintf(port, portsz, "%s", end + 2);
        return port[0] == '\0' ? -1 : 0;
    }

    colon = strrchr(addr_spec, ':');
    if (colon == NULL || colon == addr_spec || colon[1] == '\0') {
        return -1;
    }

    hostlen = (size_t)(colon - addr_spec);
    if (hostlen >= hostsz) {
        return -1;
    }
    memcpy(host, addr_spec, hostlen);
    host[hostlen] = '\0';
    snprintf(port, portsz, "%s", colon + 1);
    return 0;
}

static int add_dest(const char *spec, struct dest *dests, size_t *ndests)
{
    char host[256];
    char port[32];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int gai;
    int interval;

    if (*ndests >= MAX_DESTS) {
        fprintf(stderr, "too many destinations; max is %d\n", MAX_DESTS);
        return -1;
    }

    if (parse_dest_spec(spec, host, sizeof(host), port, sizeof(port), &interval) < 0) {
        fprintf(stderr, "invalid destination '%s'; use host:port[:Ns] or [ipv6]:port[:Ns]\n", spec);
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "%s: %s\n", spec, gai_strerror(gai));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (ai->ai_family == AF_INET) {
            int on = 1;
            if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
                fprintf(stderr, "%s: SO_BROADCAST: %s\n", spec, strerror(errno));
                close(fd);
                continue;
            }
        }

        dests[*ndests].fd = fd;
        memcpy(&dests[*ndests].addr, ai->ai_addr, ai->ai_addrlen);
        dests[*ndests].addrlen = (socklen_t)ai->ai_addrlen;
        snprintf(dests[*ndests].label, sizeof(dests[*ndests].label), "%s", spec);
        dests[*ndests].interval = interval;
        tracking_init(&dests[*ndests].tracker);
        (*ndests)++;
        freeaddrinfo(res);
        return 0;
    }

    freeaddrinfo(res);
    fprintf(stderr, "%s: could not create UDP socket\n", spec);
    return -1;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int valid_ais_line(const char *line)
{
    const char *star;
    const char *p;
    unsigned char sum = 0;
    int hi;
    int lo;
    int expected;

    if (line[0] != '!') {
        return 0;
    }

    star = strchr(line, '*');
    if (star == NULL || star == line + 1) {
        return 0;
    }
    if (!isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) {
        return 0;
    }
    if (star[3] != '\0') {
        return 0;
    }

    for (p = line + 1; p < star; p++) {
        sum ^= (unsigned char)*p;
    }

    hi = hex_value((unsigned char)star[1]);
    lo = hex_value((unsigned char)star[2]);
    expected = (hi << 4) | lo;
    return sum == (unsigned char)expected;
}

static ssize_t read_line(int fd, char *buf, size_t bufsz)
{
    size_t len = 0;

    while (len + 1 < bufsz) {
        char ch;
        ssize_t n = read(fd, &ch, 1);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            if (len == 0) {
                return 0;
            }
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            buf[len++] = ch;
        }
    }

    buf[len] = '\0';

    if (len + 1 == bufsz) {
        char ch;
        while (read(fd, &ch, 1) == 1 && ch != '\n') {
        }
    }

    return (ssize_t)len;
}

static void forward_line(const char *line, struct dest *dests, size_t ndests, int mmsi)
{
    char packet[LINE_MAX_LEN + 2];
    size_t len = strlen(line);
    size_t i;
    time_t now = time(NULL);

    if (len + 2 >= sizeof(packet)) {
        return;
    }

    memcpy(packet, line, len);
    packet[len++] = '\r';
    packet[len++] = '\n';

    for (i = 0; i < ndests; i++) {
        if (!tracking_should_forward(&dests[i].tracker, mmsi, dests[i].interval, now)) {
            continue;
        }
        if (sendto(dests[i].fd, packet, len, 0,
                   (const struct sockaddr *)&dests[i].addr, dests[i].addrlen) < 0) {
            fprintf(stderr, "%s: sendto: %s\n", dests[i].label, strerror(errno));
        }
    }
}

int main(int argc, char **argv)
{
    const char *serial_path = NULL;
    int baud = 38400;
    int verbose = 0;
    struct dest dests[MAX_DESTS];
    size_t ndests = 0;
    int serial_fd;
    int opt;
    size_t i;

    memset(dests, 0, sizeof(dests));

    while ((opt = getopt(argc, argv, "s:b:d:vh")) != -1) {
        switch (opt) {
        case 's':
            serial_path = optarg;
            break;
        case 'b':
            baud = atoi(optarg);
            break;
        case 'd':
            if (add_dest(optarg, dests, &ndests) < 0) {
                return 1;
            }
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (serial_path == NULL || ndests == 0) {
        usage(argv[0]);
        return 1;
    }

    serial_fd = open_serial(serial_path, baud);
    if (serial_fd < 0) {
        return 1;
    }

    for (;;) {
        char line[LINE_MAX_LEN];
        ssize_t n = read_line(serial_fd, line, sizeof(line));

        if (n < 0) {
            perror("read");
            break;
        }
        if (n == 0) {
            fprintf(stderr, "read: end of file\n");
            break;
        }
        if (!valid_ais_line(line)) {
            continue;
        }

        if (verbose) {
            puts(line);
            fflush(stdout);
        }
        forward_line(line, dests, ndests, ais_mmsi_from_line(line));
    }

    close(serial_fd);
    for (i = 0; i < ndests; i++) {
        close(dests[i].fd);
    }

    return 1;
}
