#include "angle_integrator.h"

#include <math.h>
#include <string.h>

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

/* Integrate the Lagrange polynomial through n timestamped samples. */
static double lagrange_integral(const uint64_t *time_us, const double *rate,
                                size_t n) {
    double t[4];
    const double span = (double)(time_us[n - 1] - time_us[0]) * 1.0e-6;
    if (!(span > 0.0)) return 0.0;
    for (size_t i = 0; i < n; ++i) {
        t[i] = (double)(time_us[i] - time_us[0]) * 1.0e-6;
    }

    /* Polynomial multiplication, followed by its exact antiderivative. */
    double result = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double coefficients[4] = {1.0, 0.0, 0.0, 0.0};
        size_t degree = 0;
        double denominator = 1.0;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            for (size_t k = degree + 1; k > 0; --k) {
                coefficients[k] = coefficients[k - 1] -
                                  t[j] * coefficients[k];
            }
            coefficients[0] *= -t[j];
            ++degree;
            denominator *= t[i] - t[j];
        }
        if (fabs(denominator) < 1.0e-15) return 0.0;

        double basis_integral = 0.0;
        double span_power = span;
        for (size_t k = 0; k < n; ++k) {
            basis_integral += coefficients[k] * span_power / (double)(k + 1);
            span_power *= span;
        }
        result += rate[i] * basis_integral / denominator;
    }
    return result;
}

static double interpolate(const uint64_t *time_us, const double *rate,
                          size_t n, double query_s) {
    double value = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double ti = (double)(time_us[i] - time_us[0]) * 1.0e-6;
        double basis = 1.0;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const double tj = (double)(time_us[j] - time_us[0]) * 1.0e-6;
            const double denominator = ti - tj;
            if (fabs(denominator) < 1.0e-15) return rate[i];
            basis *= (query_s - tj) / denominator;
        }
        value += rate[i] * basis;
    }
    return value;
}

static double gauss_legendre_panel(const angle_integrator_t *integrator) {
    static const double nodes[3] = {
        -0.77459666924148337704, 0.0, 0.77459666924148337704};
    static const double weights[3] = {5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
    const double span =
        (double)(integrator->time_us[3] - integrator->time_us[0]) * 1.0e-6;
    double sum = 0.0;
    for (size_t i = 0; i < 3; ++i) {
        const double t = 0.5 * span * (nodes[i] + 1.0);
        sum += weights[i] * interpolate(integrator->time_us,
                                        integrator->rate_dps, 4, t);
    }
    return 0.5 * span * sum;
}

static double clenshaw_curtis_panel(const angle_integrator_t *integrator) {
    /* Five Chebyshev-Lobatto nodes and unweighted Clenshaw-Curtis weights. */
    static const double nodes[5] = {
        -1.0, -0.70710678118654752440, 0.0,
        0.70710678118654752440, 1.0};
    static const double weights[5] = {
        1.0 / 15.0, 8.0 / 15.0, 4.0 / 5.0,
        8.0 / 15.0, 1.0 / 15.0};
    const double span =
        (double)(integrator->time_us[3] - integrator->time_us[0]) * 1.0e-6;
    double sum = 0.0;
    for (size_t i = 0; i < 5; ++i) {
        const double t = 0.5 * span * (nodes[i] + 1.0);
        sum += weights[i] * interpolate(integrator->time_us,
                                        integrator->rate_dps, 4, t);
    }
    return 0.5 * span * sum;
}

static size_t panel_size(angle_integration_method_t method) {
    if (method == ANGLE_INTEGRATION_SIMPSON) return 3;
    if (method == ANGLE_INTEGRATION_GAUSS ||
        method == ANGLE_INTEGRATION_CHEBYSHEV) return 4;
    return 2;
}

void angle_integrator_init(angle_integrator_t *integrator,
                           angle_integration_method_t method) {
    memset(integrator, 0, sizeof(*integrator));
    integrator->method = method;
}

void angle_integrator_reset(angle_integrator_t *integrator) {
    const angle_integration_method_t method = integrator->method;
    angle_integrator_init(integrator, method);
}

double angle_integrator_value(const angle_integrator_t *integrator) {
    double provisional = 0.0;
    for (size_t i = 1; i < integrator->sample_count; ++i) {
        provisional += trapezoid(integrator->time_us[i - 1],
                                 integrator->rate_dps[i - 1],
                                 integrator->time_us[i],
                                 integrator->rate_dps[i]);
    }
    return integrator->finalized_deg + provisional;
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

    const size_t required = panel_size(integrator->method);
    if (integrator->sample_count < required) {
        return angle_integrator_value(integrator);
    }

    double area;
    if (integrator->method == ANGLE_INTEGRATION_GAUSS) {
        area = gauss_legendre_panel(integrator);
    } else if (integrator->method == ANGLE_INTEGRATION_CHEBYSHEV) {
        area = clenshaw_curtis_panel(integrator);
    } else if (integrator->method == ANGLE_INTEGRATION_SIMPSON) {
        /* Exact quadratic integration also handles small scheduling jitter. */
        area = lagrange_integral(integrator->time_us,
                                 integrator->rate_dps, 3);
    } else {
        area = trapezoid(integrator->time_us[0], integrator->rate_dps[0],
                         integrator->time_us[1], integrator->rate_dps[1]);
    }
    kahan_add(integrator, area);

    integrator->time_us[0] = integrator->time_us[required - 1];
    integrator->rate_dps[0] = integrator->rate_dps[required - 1];
    integrator->sample_count = 1;
    return integrator->finalized_deg;
}
