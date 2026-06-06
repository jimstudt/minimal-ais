#define _POSIX_C_SOURCE 200112L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_TRACKS 4096
#define LINE_MAX_LEN 1024
#define PAYLOAD_MAX 512
#define DEFAULT_MAX_AGE (12 * 60 * 60)

struct track {
    int used;
    int mmsi;
    char sender_type[32];
    char name[64];
    double lat;
    double lon;
    double speed;
    int heading;
    int have_location;
    int have_speed;
    int have_heading;
    time_t updated;
    time_t last_seen;
};

struct scoreboard {
    struct track tracks[MAX_TRACKS];
    size_t next;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s -l address:port -o file.json [-a duration] [-v]\n"
            "\n"
            "  -l address:port  UDP address and port to listen on\n"
            "  -o file.json     JSON output file\n"
            "  -a duration      maximum entry age, default 12h\n"
            "  -v               print accepted AIS lines\n",
            prog);
}

static int parse_duration(const char *s, int *seconds)
{
    char *end;
    long value;
    long scale = 1;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || value < 0) {
        return -1;
    }

    if (*end == 's') {
        scale = 1;
        end++;
    } else if (*end == 'm') {
        scale = 60;
        end++;
    } else if (*end == 'h') {
        scale = 60 * 60;
        end++;
    } else if (*end == 'd') {
        scale = 24 * 60 * 60;
        end++;
    }

    if (*end != '\0' || value > 2147483647L / scale) {
        return -1;
    }

    *seconds = (int)(value * scale);
    return 0;
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

    expected = (hex_value((unsigned char)star[1]) << 4) |
               hex_value((unsigned char)star[2]);
    return sum == (unsigned char)expected;
}

static int payload_from_line(const char *line, char *payload, size_t payloadsz, int *fill)
{
    const char *p = line;
    const char *field_start = line;
    const char *star = strchr(line, '*');
    size_t field = 0;

    if (star == NULL) {
        return -1;
    }

    while (p <= star) {
        if (*p == ',' || p == star) {
            if (field == 5) {
                size_t len = (size_t)(p - field_start);
                if (len == 0 || len >= payloadsz) {
                    return -1;
                }
                memcpy(payload, field_start, len);
                payload[len] = '\0';
            } else if (field == 6) {
                if (p != field_start + 1 || !isdigit((unsigned char)*field_start)) {
                    return -1;
                }
                *fill = *field_start - '0';
                return payload[0] == '\0' ? -1 : 0;
            }
            field++;
            field_start = p + 1;
        }
        p++;
    }

    return -1;
}

static int ais_char_value(int c)
{
    int v = c - 48;

    if (v < 0 || v > 87) {
        return -1;
    }
    if (v > 40) {
        v -= 8;
    }
    if (v < 0 || v > 63) {
        return -1;
    }
    return v;
}

static int bit_at(const char *payload, size_t bit)
{
    int v = ais_char_value((unsigned char)payload[bit / 6]);

    if (v < 0) {
        return -1;
    }
    return (v >> (5 - (bit % 6))) & 1;
}

static unsigned int get_uint(const char *payload, size_t start, size_t nbits)
{
    size_t i;
    unsigned int value = 0;

    for (i = 0; i < nbits; i++) {
        int bit = bit_at(payload, start + i);
        if (bit < 0) {
            return 0;
        }
        value = (value << 1) | (unsigned int)bit;
    }

    return value;
}

static int get_int(const char *payload, size_t start, size_t nbits)
{
    unsigned int value = get_uint(payload, start, nbits);
    unsigned int sign = 1u << (nbits - 1);

    if ((value & sign) == 0) {
        return (int)value;
    }
    return (int)(value - (1u << nbits));
}

static char ais_text_char(unsigned int v)
{
    if (v == 0) {
        return ' ';
    }
    if (v < 32) {
        return (char)(v + 64);
    }
    return (char)v;
}

static void get_text(const char *payload, size_t start, size_t nchars,
                     char *out, size_t outsz)
{
    size_t i;
    size_t len;

    if (outsz == 0) {
        return;
    }

    for (i = 0; i < nchars && i + 1 < outsz; i++) {
        out[i] = ais_text_char(get_uint(payload, start + i * 6, 6));
    }
    out[i] = '\0';

    len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '@')) {
        out[--len] = '\0';
    }
}

static int payload_ready(const char *payload, int fill, size_t needed_bits)
{
    size_t bits;

    if (fill < 0 || fill > 5) {
        return 0;
    }
    bits = strlen(payload) * 6u;
    return bits >= (size_t)fill && bits - (size_t)fill >= needed_bits;
}

static void scoreboard_init(struct scoreboard *board)
{
    memset(board, 0, sizeof(*board));
}

static struct track *scoreboard_get(struct scoreboard *board, int mmsi)
{
    size_t i;
    size_t empty = MAX_TRACKS;

    for (i = 0; i < MAX_TRACKS; i++) {
        if (board->tracks[i].used && board->tracks[i].mmsi == mmsi) {
            return &board->tracks[i];
        }
        if (!board->tracks[i].used && empty == MAX_TRACKS) {
            empty = i;
        }
    }

    if (empty == MAX_TRACKS) {
        empty = board->next;
        board->next = (board->next + 1) % MAX_TRACKS;
    }

    memset(&board->tracks[empty], 0, sizeof(board->tracks[empty]));
    board->tracks[empty].used = 1;
    board->tracks[empty].mmsi = mmsi;
    board->tracks[empty].heading = -1;
    return &board->tracks[empty];
}

static int set_string(char *dst, size_t dstsz, const char *src)
{
    if (strncmp(dst, src, dstsz) == 0) {
        return 0;
    }
    snprintf(dst, dstsz, "%s", src);
    return 1;
}

static int set_location(struct track *track, int raw_lon, int raw_lat)
{
    double lon;
    double lat;

    if (raw_lon == 0x6791AC0 || raw_lat == 0x3412140) {
        return 0;
    }

    lon = raw_lon / 600000.0;
    lat = raw_lat / 600000.0;
    if (track->have_location && track->lon == lon && track->lat == lat) {
        return 0;
    }

    track->lon = lon;
    track->lat = lat;
    track->have_location = 1;
    return 1;
}

static int set_speed(struct track *track, unsigned int raw_speed)
{
    double speed;

    if (raw_speed == 1023) {
        return 0;
    }

    speed = raw_speed / 10.0;
    if (track->have_speed && track->speed == speed) {
        return 0;
    }

    track->speed = speed;
    track->have_speed = 1;
    return 1;
}

static int set_heading(struct track *track, unsigned int heading)
{
    if (heading == 511) {
        return 0;
    }
    if (track->have_heading && track->heading == (int)heading) {
        return 0;
    }
    track->heading = (int)heading;
    track->have_heading = 1;
    return 1;
}

static int process_payload(struct scoreboard *board, const char *payload, int fill)
{
    int type;
    int mmsi;
    int changed = 0;
    struct track *track;
    char name[64];
    time_t now;

    if (!payload_ready(payload, fill, 38)) {
        return 0;
    }

    type = (int)get_uint(payload, 0, 6);
    if (type != 1 && type != 2 && type != 3 && type != 4 &&
        type != 18 && type != 21 && type != 24) {
        return 0;
    }

    mmsi = (int)get_uint(payload, 8, 30);
    track = scoreboard_get(board, mmsi);
    now = time(NULL);
    track->last_seen = now;

    if (type == 1 || type == 2 || type == 3) {
        if (!payload_ready(payload, fill, 137)) {
            return 0;
        }
        changed |= set_string(track->sender_type, sizeof(track->sender_type), "class-a");
        changed |= set_speed(track, get_uint(payload, 50, 10));
        changed |= set_location(track, get_int(payload, 61, 28), get_int(payload, 89, 27));
        changed |= set_heading(track, get_uint(payload, 128, 9));
    } else if (type == 4) {
        if (!payload_ready(payload, fill, 135)) {
            return 0;
        }
        changed |= set_string(track->sender_type, sizeof(track->sender_type), "base-station");
        changed |= set_location(track, get_int(payload, 79, 28), get_int(payload, 108, 27));
    } else if (type == 18) {
        if (!payload_ready(payload, fill, 133)) {
            return 0;
        }
        changed |= set_string(track->sender_type, sizeof(track->sender_type), "class-b");
        changed |= set_speed(track, get_uint(payload, 46, 10));
        changed |= set_location(track, get_int(payload, 57, 28), get_int(payload, 85, 27));
        changed |= set_heading(track, get_uint(payload, 124, 9));
    } else if (type == 21) {
        if (!payload_ready(payload, fill, 219)) {
            return 0;
        }
        changed |= set_string(track->sender_type, sizeof(track->sender_type), "aton");
        get_text(payload, 43, 20, name, sizeof(name));
        if (name[0] != '\0') {
            changed |= set_string(track->name, sizeof(track->name), name);
        }
        changed |= set_location(track, get_int(payload, 164, 28), get_int(payload, 192, 27));
    } else if (type == 24) {
        if (!payload_ready(payload, fill, 40)) {
            return 0;
        }
        changed |= set_string(track->sender_type, sizeof(track->sender_type), "class-b");
        if (get_uint(payload, 38, 2) == 0 && payload_ready(payload, fill, 160)) {
            get_text(payload, 40, 20, name, sizeof(name));
            if (name[0] != '\0') {
                changed |= set_string(track->name, sizeof(track->name), name);
            }
        }
    }

    if (changed) {
        track->updated = now;
    }
    return changed;
}

static int process_line(struct scoreboard *board, const char *line)
{
    char payload[PAYLOAD_MAX] = "";
    int fill = 0;

    if (!valid_ais_line(line)) {
        return 0;
    }
    if (payload_from_line(line, payload, sizeof(payload), &fill) < 0) {
        return 0;
    }
    return process_payload(board, payload, fill);
}

static int scoreboard_prune(struct scoreboard *board, int max_age, time_t now)
{
    size_t i;
    int removed = 0;

    if (max_age <= 0) {
        return 0;
    }

    for (i = 0; i < MAX_TRACKS; i++) {
        if (board->tracks[i].used && now - board->tracks[i].last_seen > max_age) {
            memset(&board->tracks[i], 0, sizeof(board->tracks[i]));
            removed = 1;
        }
    }

    return removed;
}

static void json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\') {
            fputc('\\', fp);
            fputc(c, fp);
        } else if (c < 32) {
            fprintf(fp, "\\u%04x", c);
        } else {
            fputc(c, fp);
        }
    }
    fputc('"', fp);
}

static int compare_track_mmsi(const void *a, const void *b)
{
    const struct track *ta = *(const struct track *const *)a;
    const struct track *tb = *(const struct track *const *)b;

    if (ta->mmsi < tb->mmsi) {
        return -1;
    }
    if (ta->mmsi > tb->mmsi) {
        return 1;
    }
    return 0;
}

static int write_json(const char *path, const struct scoreboard *board)
{
    char tmppath[512];
    const struct track *sorted[MAX_TRACKS];
    FILE *fp;
    size_t i;
    size_t ntracks = 0;
    int first = 1;

    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", path) >= (int)sizeof(tmppath)) {
        fprintf(stderr, "%s: path too long\n", path);
        return -1;
    }

    fp = fopen(tmppath, "w");
    if (fp == NULL) {
        fprintf(stderr, "%s: %s\n", tmppath, strerror(errno));
        return -1;
    }

    for (i = 0; i < MAX_TRACKS; i++) {
        if (board->tracks[i].used) {
            sorted[ntracks++] = &board->tracks[i];
        }
    }
    qsort(sorted, ntracks, sizeof(sorted[0]), compare_track_mmsi);

    fprintf(fp, "[\n");
    for (i = 0; i < ntracks; i++) {
        const struct track *t = sorted[i];

        fprintf(fp, "%s  {\"mmsi\":%d", first ? "" : ",\n", t->mmsi);
        first = 0;

        fprintf(fp, ",\"sender_type\":");
        json_string(fp, t->sender_type[0] == '\0' ? "unknown" : t->sender_type);

        fprintf(fp, ",\"name\":");
        if (t->name[0] == '\0') {
            fprintf(fp, "null");
        } else {
            json_string(fp, t->name);
        }

        fprintf(fp, ",\"location\":");
        if (t->have_location) {
            fprintf(fp, "{\"lat\":%.6f,\"lon\":%.6f}", t->lat, t->lon);
        } else {
            fprintf(fp, "null");
        }

        fprintf(fp, ",\"heading\":");
        if (t->have_heading) {
            fprintf(fp, "%d", t->heading);
        } else {
            fprintf(fp, "null");
        }

        fprintf(fp, ",\"speed\":");
        if (t->have_speed) {
            fprintf(fp, "%.1f", t->speed);
        } else {
            fprintf(fp, "null");
        }

        fprintf(fp, ",\"updated\":%ld,\"last_seen\":%ld}",
                (long)t->updated, (long)t->last_seen);
    }
    fprintf(fp, "\n]\n");

    if (fclose(fp) != 0) {
        fprintf(stderr, "%s: %s\n", tmppath, strerror(errno));
        return -1;
    }

    if (rename(tmppath, path) < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int parse_address(const char *spec, char *host, size_t hostsz,
                         char *port, size_t portsz)
{
    const char *colon;
    size_t hostlen;

    if (spec[0] == '[') {
        const char *end = strchr(spec, ']');
        if (end == NULL || end[1] != ':') {
            return -1;
        }
        hostlen = (size_t)(end - spec - 1);
        if (hostlen >= hostsz) {
            return -1;
        }
        memcpy(host, spec + 1, hostlen);
        host[hostlen] = '\0';
        snprintf(port, portsz, "%s", end + 2);
        return port[0] == '\0' ? -1 : 0;
    }

    colon = strrchr(spec, ':');
    if (colon == NULL || colon[1] == '\0') {
        return -1;
    }

    hostlen = (size_t)(colon - spec);
    if (hostlen >= hostsz) {
        return -1;
    }
    memcpy(host, spec, hostlen);
    host[hostlen] = '\0';
    snprintf(port, portsz, "%s", colon + 1);
    return 0;
}

static int open_listener(const char *spec)
{
    char host[256];
    char port[32];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int gai;
    int last_errno = 0;

    if (parse_address(spec, host, sizeof(host), port, sizeof(port)) < 0) {
        fprintf(stderr, "invalid listen address '%s'; use address:port or [ipv6]:port\n", spec);
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    gai = getaddrinfo(host[0] == '\0' ? NULL : host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "%s: %s\n", spec, gai_strerror(gai));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int on = 1;
        if (fd < 0) {
            continue;
        }

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd;
        }
        last_errno = errno;
        close(fd);
    }

    freeaddrinfo(res);
    fprintf(stderr, "%s: could not bind UDP socket: %s\n",
            spec, last_errno == 0 ? "unknown error" : strerror(last_errno));
    return -1;
}

static void handle_datagram(struct scoreboard *board, char *buf, ssize_t n,
                            int verbose, int *changed)
{
    char line[LINE_MAX_LEN];
    size_t len = 0;
    ssize_t i;

    for (i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            line[len] = '\0';
            if (len > 0) {
                if (verbose) {
                    puts(line);
                    fflush(stdout);
                }
                if (process_line(board, line)) {
                    *changed = 1;
                }
            }
            len = 0;
        } else if (ch != '\r' && len + 1 < sizeof(line)) {
            line[len++] = ch;
        }
    }

    if (len > 0) {
        line[len] = '\0';
        if (verbose) {
            puts(line);
            fflush(stdout);
        }
        if (process_line(board, line)) {
            *changed = 1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *listen_spec = NULL;
    const char *output_path = NULL;
    int max_age = DEFAULT_MAX_AGE;
    int verbose = 0;
    int opt;
    int fd;
    struct scoreboard board;
    time_t last_write = 0;

    while ((opt = getopt(argc, argv, "l:o:a:vh")) != -1) {
        switch (opt) {
        case 'l':
            listen_spec = optarg;
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'a':
            if (parse_duration(optarg, &max_age) < 0) {
                fprintf(stderr, "invalid maximum age '%s'; use seconds, Ns, Nm, Nh, or Nd\n", optarg);
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

    if (listen_spec == NULL || output_path == NULL) {
        usage(argv[0]);
        return 1;
    }

    fd = open_listener(listen_spec);
    if (fd < 0) {
        return 1;
    }

    scoreboard_init(&board);
    if (write_json(output_path, &board) < 0) {
        close(fd);
        return 1;
    }
    last_write = time(NULL);

    for (;;) {
        fd_set readfds;
        struct timeval tv;
        int ready;
        int changed = 0;
        time_t now;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (ready > 0 && FD_ISSET(fd, &readfds)) {
            char buf[4096];
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("recvfrom");
                break;
            }
            handle_datagram(&board, buf, n, verbose, &changed);
        }

        now = time(NULL);
        if (changed || now - last_write >= 60) {
            changed |= scoreboard_prune(&board, max_age, now);
            if (write_json(output_path, &board) == 0) {
                last_write = now;
            }
        }
    }

    close(fd);
    return 1;
}
