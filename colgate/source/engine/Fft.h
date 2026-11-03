// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A small split-format radix-4 FFT. Keeping real and imaginary parts in
// separate arrays lets the compiler vectorise the butterflies on both x86_64
// and arm64, and it makes the two-channels-in-one-transform trick used by the
// analyser and the equaliser read naturally.
#pragma once

#include <cmath>
#include <utility>
#include <vector>

namespace colgate
{

class SplitFft
{
public:
    void prepare (int size)
    {
        n = size;
        logN = 0;
        while ((1 << logN) < n)
            ++logN;

        bitReversed.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < logN; ++b)
                if (i & (1 << b))
                    r |= 1 << (logN - 1 - b);
            bitReversed[(size_t) i] = r;
        }

        // Radix-4 stages over radix-2 ordered input. One contiguous twiddle run
        // per stage, so the inner loop is a plain array walk the auto-vectoriser
        // can handle. An odd power of two starts with one trivial radix-2 stage.
        startsWithRadix2 = (logN % 2) != 0;
        stages.clear();
        twiddles.clear();

        const double twoPi = 6.283185307179586476925;
        for (int q = startsWithRadix2 ? 2 : 1; q * 4 <= n; q *= 4)
        {
            Stage stage;
            stage.quarter = q;
            stage.offset = (int) twiddles.size();
            twiddles.resize (twiddles.size() + (size_t) (6 * q));
            float* w = twiddles.data() + stage.offset;
            for (int j = 0; j < q; ++j)
            {
                const double angle = -twoPi * j / (double) (4 * q);
                w[j] = (float) std::cos (angle);
                w[q + j] = (float) std::sin (angle);
                w[2 * q + j] = (float) std::cos (2.0 * angle);
                w[3 * q + j] = (float) std::sin (2.0 * angle);
                w[4 * q + j] = (float) std::cos (3.0 * angle);
                w[5 * q + j] = (float) std::sin (3.0 * angle);
            }
            stages.push_back (stage);
        }
    }

    int size() const noexcept { return n; }

    // In-place forward transform. For an inverse, call forward (im, re) and
    // divide the result by size().
    void forward (float* re, float* im) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int r = bitReversed[(size_t) i];
            if (r > i)
            {
                std::swap (re[i], re[r]);
                std::swap (im[i], im[r]);
            }
        }

        if (startsWithRadix2)
        {
            for (int i = 0; i < n; i += 2)
            {
                const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
                re[i] = ar + br;
                im[i] = ai + bi;
                re[i + 1] = ar - br;
                im[i + 1] = ai - bi;
            }
        }

        for (const auto& stage : stages)
        {
            const int q = stage.quarter;
            const float* w = twiddles.data() + stage.offset;
            const float* w1r = w;
            const float* w1i = w + q;
            const float* w2r = w + 2 * q;
            const float* w2i = w + 3 * q;
            const float* w3r = w + 4 * q;
            const float* w3i = w + 5 * q;

            for (int start = 0; start < n; start += 4 * q)
            {
                float* ar = re + start;
                float* ai = im + start;
                float* br = ar + q;
                float* bi = ai + q;
                float* cr = br + q;
                float* ci = bi + q;
                float* dr = cr + q;
                float* di = ci + q;

                for (int j = 0; j < q; ++j)
                {
                    const float tbr = br[j] * w2r[j] - bi[j] * w2i[j];
                    const float tbi = br[j] * w2i[j] + bi[j] * w2r[j];
                    const float tcr = cr[j] * w1r[j] - ci[j] * w1i[j];
                    const float tci = cr[j] * w1i[j] + ci[j] * w1r[j];
                    const float tdr = dr[j] * w3r[j] - di[j] * w3i[j];
                    const float tdi = dr[j] * w3i[j] + di[j] * w3r[j];

                    const float s0r = ar[j] + tbr;
                    const float s0i = ai[j] + tbi;
                    const float s1r = ar[j] - tbr;
                    const float s1i = ai[j] - tbi;
                    const float s2r = tcr + tdr;
                    const float s2i = tci + tdi;
                    const float s3r = tcr - tdr;
                    const float s3i = tci - tdi;

                    ar[j] = s0r + s2r;
                    ai[j] = s0i + s2i;
                    cr[j] = s0r - s2r;
                    ci[j] = s0i - s2i;
                    br[j] = s1r + s3i;
                    bi[j] = s1i - s3r;
                    dr[j] = s1r - s3i;
                    di[j] = s1i + s3r;
                }
            }
        }
    }

private:
    struct Stage
    {
        int quarter = 1;
        int offset = 0;
    };

    int n = 0;
    int logN = 0;
    bool startsWithRadix2 = false;
    std::vector<int> bitReversed;
    std::vector<Stage> stages;
    std::vector<float> twiddles;
};

} // namespace colgate
