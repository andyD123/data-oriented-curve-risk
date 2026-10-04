#pragma once
// Stencil set for one ladder: K intervals [B[k-1], B[k]), k = 1..K, with a box profile.
// A stencil's risk is to a unit bump of the instantaneous forward inside its interval and nowhere else
// (no leakage). Pure geometry: knows nothing about the curve.
#include <algorithm>
#include <vector>

namespace ladder {

struct Stencils {
    std::vector<double> B;                       // boundaries, ascending; B[0] is the reference time
    bool open_last = false;                      // true: the last stencil extends flat beyond B[K] (Hagan 2015, eq. 2.2b);
                                                 // false: it stops at B[K] (box of length len(K))

    int K() const { return (int)B.size() - 1; }
    double len(int k) const { return B[k] - B[k-1]; }
    // interval index k in 1..K with B[k-1] <= t < B[k]; times before B[0] map to 1, at or after B[K] to K
    int bucket(double t) const {
        int k = (int)(std::upper_bound(B.begin(), B.end(), t) - B.begin());
        return std::max(1, std::min(k, K()));
    }
    // how much of stencil k lies in [0, t]: 0 before, (t - B[k-1]) inside, len(k) after
    double overlap(int k, double t) const {
        double o = std::max(t - B[k-1], 0.0);
        return (open_last && k == K()) ? o : std::min(o, len(k));
    }
    // d log D(t) / d delta_k for a box stencil
    double psi(int k, double t) const { return -overlap(k, t); }
};

} // namespace ladder
