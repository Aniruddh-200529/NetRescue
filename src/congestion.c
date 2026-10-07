#include "congestion.h"
#include <math.h>
#include <string.h>

void nr_congestion_init(NrCongestionController *c, double rate) {
    if (c == NULL) return;
    memset(c, 0, sizeof(*c));
    c->maximum_rate = isfinite(rate) && rate > 0.0 ? fmin(rate, 10000.0) : 0.0;
    c->minimum_rate = c->maximum_rate > 0.0 ? fmin(0.1, c->maximum_rate) : 0.0;
    c->rate = c->maximum_rate;
}

double nr_congestion_observe(NrCongestionController *c, double dt, double utilization, bool overflow) {
    if (c == NULL || !isfinite(dt) || dt <= 0.0 || !isfinite(utilization)) return c == NULL ? 0.0 : c->rate;
    if (overflow || utilization >= 0.8) {
        if (c->rate > c->minimum_rate) {
            c->rate = fmax(c->minimum_rate, c->rate * 0.5);
            ++c->decreases;
        }
        c->recovery_elapsed = 0.0;
    } else if (utilization <= 0.25 && c->rate < c->maximum_rate) {
        c->recovery_elapsed += dt;
        while (c->recovery_elapsed >= 1.0 && c->rate < c->maximum_rate) {
            c->recovery_elapsed -= 1.0;
            c->rate = fmin(c->maximum_rate, c->rate + fmax(0.1, c->maximum_rate * 0.1));
            ++c->increases;
        }
    } else {
        c->recovery_elapsed = 0.0;
    }
    return c->rate;
}
