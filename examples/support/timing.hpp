#pragma once

// Demonstrator timing only. No warm-up, clearing, allocation or reporting is hidden
// here: everything inside `work` is measured. `after_each` runs after the clock
// stops, so the caller can check/consume each result without timing the check.
#include <chrono>
#include <limits>
#include <stdexcept>

namespace demo {

template<class Work>
double measure_once(Work&& work)
{
    const auto start = std::chrono::steady_clock::now();
    work();
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

template<class Work, class Check>
double measure_best(int repetitions, Work&& work, Check&& after_each)
{
    if (repetitions < 1) {
        throw std::invalid_argument("measure_best: repetitions must be positive");
    }

    double best = std::numeric_limits<double>::infinity();
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        const double elapsed = measure_once(work);
        after_each();
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return best;
}

template<class Work>
double measure_best(int repetitions, Work&& work)
{
    return measure_best(repetitions, work, [] {});
}

} // namespace demo
