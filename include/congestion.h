#ifndef NETRESCUE_CONGESTION_H
#define NETRESCUE_CONGESTION_H

#include <stdbool.h>

typedef struct {
    double rate, minimum_rate, maximum_rate, recovery_elapsed;
    unsigned decreases, increases;
} NrCongestionController;

void nr_congestion_init(NrCongestionController *controller, double rate);
double nr_congestion_observe(NrCongestionController *controller, double delta_seconds,
                             double queue_utilization, bool queue_overflow);

#endif
