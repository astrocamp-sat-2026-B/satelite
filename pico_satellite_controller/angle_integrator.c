#include "angle_integrator.h"

#include <math.h>
#include <string.h>

/* Outside this ratio, generalized Simpson weights can amplify sample noise. */
#define MIN_INTERVAL_RATIO 0.80

static void kahan_add(angle_integrator_t *integrator, double increment) {
    const double corrected = increment - integrator->compensation_deg;
    const double next = integrator->finalized_deg + corrected;
    integrator->compensation_deg =
        (next - integrator->finalized_deg) - corrected;
    integrator->finalized_deg = next;
}

static double trapezoid(uint64_t t0, double y0, uint64_t t1, double y1) {
    return 0.5 * (y0 + y1) * (double)(t1 - t0) * 1.0e-6;
}

/*
 * Integral of the quadratic Lagrange interpolant through three samples.
 * Unlike the textbook h/3 expression, this remains correct for small timing
 * jitter because the actual timestamps determine every weight.
 */
static double nonuniform_simpson(const uint64_t time_us[3],
                                 const double rate[3]) {
    const double h0 = (double)(time_us[1] - time_us[0]) * 1.0e-6;
    const double h1 = (double)(time_us[2] - time_us[1]) * 1.0e-6;
    const double total = h0 + h1;
    const double w0 = total * (2.0 * h0 - h1) / (6.0 * h0);
    const double w1 = total * total * total / (6.0 * h0 * h1);
    const double w2 = total * (2.0 * h1 - h0) / (6.0 * h1);
    return w0 * rate[0] + w1 * rate[1] + w2 * rate[2];
}

void angle_integrator_init(angle_integrator_t *integrator) {
    memset(integrator, 0, sizeof(*integrator));
}

void angle_integrator_reset(angle_integrator_t *integrator) {
    angle_integrator_init(integrator);
}

void angle_integrator_discontinuity(angle_integrator_t *integrator,
                                    uint64_t time_us, double rate_dps) {
    /* Preserve the last provisional interval before breaking continuity. */
    if (integrator->sample_count == 2) {
        kahan_add(integrator,
                  trapezoid(integrator->time_us[0], integrator->rate_dps[0],
                            integrator->time_us[1], integrator->rate_dps[1]));
        ++integrator->trapezoid_panels;
    }
    integrator->time_us[0] = time_us;
    integrator->rate_dps[0] = rate_dps;
    integrator->sample_count = 1;
    ++integrator->discontinuities;
}

double angle_integrator_value(const angle_integrator_t *integrator) {
    if (integrator->sample_count < 2) return integrator->finalized_deg;
    return integrator->finalized_deg +
           trapezoid(integrator->time_us[0], integrator->rate_dps[0],
                     integrator->time_us[1], integrator->rate_dps[1]);
}

double angle_integrator_update(angle_integrator_t *integrator,
                               uint64_t time_us, double rate_dps) {
    if (!isfinite(rate_dps)) return angle_integrator_value(integrator);
    if (integrator->sample_count > 0 &&
        time_us <= integrator->time_us[integrator->sample_count - 1]) {
        return angle_integrator_value(integrator);
    }

    integrator->time_us[integrator->sample_count] = time_us;
    integrator->rate_dps[integrator->sample_count] = rate_dps;
    ++integrator->sample_count;
    if (integrator->sample_count < 3) return angle_integrator_value(integrator);

    const double h0 = (double)(integrator->time_us[1] -
                               integrator->time_us[0]);
    const double h1 = (double)(integrator->time_us[2] -
                               integrator->time_us[1]);
    const double ratio = fmin(h0, h1) / fmax(h0, h1);
    double area;
    if (h0 > 0.0 && h1 > 0.0 && ratio >= MIN_INTERVAL_RATIO) {
        area = nonuniform_simpson(integrator->time_us,
                                  integrator->rate_dps);
        ++integrator->simpson_panels;
    } else {
        area = trapezoid(integrator->time_us[0], integrator->rate_dps[0],
                         integrator->time_us[1], integrator->rate_dps[1]) +
               trapezoid(integrator->time_us[1], integrator->rate_dps[1],
                         integrator->time_us[2], integrator->rate_dps[2]);
        ++integrator->trapezoid_panels;
    }
    kahan_add(integrator, area);

    integrator->time_us[0] = integrator->time_us[2];
    integrator->rate_dps[0] = integrator->rate_dps[2];
    integrator->sample_count = 1;
    return integrator->finalized_deg;
}
