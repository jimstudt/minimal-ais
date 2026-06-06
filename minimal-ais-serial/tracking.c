#include "tracking.h"

#include <ctype.h>
#include <string.h>

void tracking_init(struct mmsi_tracker *tracker)
{
    size_t i;

    for (i = 0; i < TRACKING_MAX_MMSI; i++) {
        tracker->entries[i].mmsi = -1;
        tracker->entries[i].last_sent = 0;
    }
    tracker->next = 0;
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

static int payload_from_line(const char *line, char *payload, size_t payloadsz, int *fill)
{
    const char *p = line;
    const char *field_start;
    const char *star;
    size_t field = 0;
    size_t len;

    star = strchr(line, '*');
    if (star == NULL) {
        return -1;
    }

    field_start = line;
    while (p <= star) {
        if (*p == ',' || p == star) {
            if (field == 5) {
                len = (size_t)(p - field_start);
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

static int payload_bit(const char *payload, size_t bit)
{
    int v = ais_char_value((unsigned char)payload[bit / 6]);

    if (v < 0) {
        return -1;
    }
    return (v >> (5 - (bit % 6))) & 1;
}

static int payload_uint(const char *payload, size_t start, size_t nbits)
{
    size_t i;
    int value = 0;

    for (i = 0; i < nbits; i++) {
        int bit = payload_bit(payload, start + i);
        if (bit < 0) {
            return -1;
        }
        value = (value << 1) | bit;
    }

    return value;
}

int ais_mmsi_from_line(const char *line)
{
    char payload[512] = "";
    int fill = 0;
    size_t payload_bits;
    int msg_type;

    if (payload_from_line(line, payload, sizeof(payload), &fill) < 0) {
        return -1;
    }

    if (fill < 0 || fill > 5) {
        return -1;
    }

    payload_bits = strlen(payload) * 6u;
    if (payload_bits < (size_t)fill || payload_bits - (size_t)fill < 38u) {
        return -1;
    }

    msg_type = payload_uint(payload, 0, 6);
    if (msg_type <= 0) {
        return -1;
    }

    return payload_uint(payload, 8, 30);
}

int tracking_should_forward(struct mmsi_tracker *tracker, int mmsi,
                            int interval, time_t now)
{
    size_t i;
    size_t empty = TRACKING_MAX_MMSI;

    if (interval <= 0 || mmsi < 0) {
        return 1;
    }

    for (i = 0; i < TRACKING_MAX_MMSI; i++) {
        if (tracker->entries[i].mmsi == mmsi) {
            if (now - tracker->entries[i].last_sent < interval) {
                return 0;
            }
            tracker->entries[i].last_sent = now;
            return 1;
        }
        if (empty == TRACKING_MAX_MMSI && tracker->entries[i].mmsi < 0) {
            empty = i;
        }
    }

    if (empty == TRACKING_MAX_MMSI) {
        empty = tracker->next;
        tracker->next = (tracker->next + 1) % TRACKING_MAX_MMSI;
    }

    tracker->entries[empty].mmsi = mmsi;
    tracker->entries[empty].last_sent = now;
    return 1;
}
