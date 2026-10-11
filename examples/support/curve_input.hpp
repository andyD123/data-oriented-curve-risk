#pragma once

// Read the two curve sections of the recorded market curves. Later sections of
// that historical file contain instruments; these demonstrators do not read them.
// The benchmark curve is log-linear on the exported discount factors.
#include "ladder/stencil.hpp"
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace demo {

struct Curve {
    ladder::Stencils waves;
    std::vector<double> log_discounts;

    double discount(double time) const
    {
        const int bucket = waves.bucket(time);
        const double fraction = (time - waves.B[bucket - 1]) / waves.len(bucket);
        return std::exp(log_discounts[bucket - 1]
            + fraction * (log_discounts[bucket] - log_discounts[bucket - 1]));
    }
};

struct CurvePair {
    Curve discount;
    Curve projection;

    int wave_count() const
    {
        return discount.waves.K() + projection.waves.K();
    }
};

inline Curve read_curve(std::FILE* input)
{
    char name[16];
    int nodes = 0;
    if (std::fscanf(input, "%15s %d", name, &nodes) != 2
        || nodes < 2 || nodes > 100000) {
        throw std::runtime_error("invalid curve header");
    }

    Curve curve;
    curve.waves.B.resize(nodes);
    curve.log_discounts.resize(nodes);
    for (int node = 0; node < nodes; ++node) {
        double time = 0.0;
        double discount = 0.0;
        if (std::fscanf(input, "%lf %lf", &time, &discount) != 2
            || !std::isfinite(time) || !std::isfinite(discount) || discount <= 0.0) {
            throw std::runtime_error("invalid curve time/discount factor");
        }
        curve.waves.B[node] = time;
        curve.log_discounts[node] = std::log(discount);
    }
    curve.waves.validate();
    return curve;
}

struct CloseFile {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};

inline CurvePair read_curves(const char* path)
{
    std::unique_ptr<std::FILE, CloseFile> input(std::fopen(path, "r"));
    if (!input) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    int count = 0;
    if (std::fscanf(input.get(), "%d", &count) != 1 || count != 2) {
        throw std::runtime_error(std::string("cannot read two curves from ") + path);
    }
    CurvePair curves;
    curves.discount = read_curve(input.get());
    curves.projection = read_curve(input.get());
    return curves;
}

} // namespace demo
