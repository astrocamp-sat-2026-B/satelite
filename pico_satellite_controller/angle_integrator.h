#ifndef ANGLE_INTEGRATOR_H
#define ANGLE_INTEGRATOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Streaming gyro integrator for discrete, noisy samples.
 *
 * A three-sample panel is integrated with the non-uniform Simpson rule when
 * the two sample intervals are well conditioned.  If acquisition jitter or a
 * dropped sample makes the quadratic weights noise-sensitive, the integrator
 * automatically falls back to two trapezoids.  Accumulation uses double and
 * Kahan compensation to avoid long-duration round-off drift.
 */
typedef struct {
    uint64_t time_us[3];
    double rate_dps[3];
    size_t sample_count;
    double finalized_deg;
    double compensation_deg;
    uint32_t simpson_panels;
    uint32_t trapezoid_panels;
    uint32_t discontinuities;
} angle_integrator_t;

void angle_integrator_init(angle_integrator_t *integrator);
void angle_integrator_reset(angle_integrator_t *integrator);

/* Start a new panel without integrating across an acquisition gap. */
void angle_integrator_discontinuity(angle_integrator_t *integrator,
                                    uint64_t time_us, double rate_dps);

/* Adds one timestamped sample and returns the angle at that timestamp. */
double angle_integrator_update(angle_integrator_t *integrator,
                               uint64_t time_us, double rate_dps);
double angle_integrator_value(const angle_integrator_t *integrator);

#endif
