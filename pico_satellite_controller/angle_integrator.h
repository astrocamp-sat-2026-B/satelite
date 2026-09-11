#ifndef ANGLE_INTEGRATOR_H
#define ANGLE_INTEGRATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Quadrature used to integrate angular-rate samples.  Simpson is the default
 * for flight use.  The Gauss-Legendre and Clenshaw-Curtis (Chebyshev-node)
 * modes interpolate a short, timestamped panel before applying their nodes;
 * this makes them usable with uniformly sampled hardware.
 */
typedef enum {
    ANGLE_INTEGRATION_TRAPEZOID = 0,
    ANGLE_INTEGRATION_SIMPSON,
    ANGLE_INTEGRATION_CHEBYSHEV,
    ANGLE_INTEGRATION_GAUSS,
} angle_integration_method_t;

typedef struct {
    angle_integration_method_t method;
    uint64_t time_us[4];
    double rate_dps[4];
    size_t sample_count;
    double finalized_deg;
    double compensation_deg;
} angle_integrator_t;

void angle_integrator_init(angle_integrator_t *integrator,
                           angle_integration_method_t method);
void angle_integrator_reset(angle_integrator_t *integrator);

/* Adds one timestamped sample and returns the angle at that timestamp. */
double angle_integrator_update(angle_integrator_t *integrator,
                               uint64_t time_us, double rate_dps);
double angle_integrator_value(const angle_integrator_t *integrator);

#endif
