#ifndef TRACKING_H
#define TRACKING_H

#include <stddef.h>
#include <time.h>

#define TRACKING_MAX_MMSI 4096

struct mmsi_entry {
    int mmsi;
    time_t last_sent;
};

struct mmsi_tracker {
    struct mmsi_entry entries[TRACKING_MAX_MMSI];
    size_t next;
};

void tracking_init(struct mmsi_tracker *tracker);
int ais_mmsi_from_line(const char *line);
int tracking_should_forward(struct mmsi_tracker *tracker, int mmsi,
                            int interval, time_t now);

#endif
