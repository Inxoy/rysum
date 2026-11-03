#pragma once

#include "FastFFT.h"
#include <algorithm>
#include <vector>

namespace rysum
{
/** One stage of a uniformly partitioned overlap-save convolution (block B, FFT 2B) whose work is spread
    evenly over time instead of being done all at once when a block is complete. This is what keeps long
    reverbs from causing CPU spikes: before, every 512 / 8192 samples a single audio block had to do the
    whole convolution.

    Several inputs, several outputs; every output is a sum of "terms" (input k convolved with a response).
    Partition q of a term's response is its segment [(q + lag) B, (q + lag + 1) B), counted from the moment
    the input sample arrives - the output has no latency of its own:

      lag 1: the output of a block is finished at its start (the newest partition q = 0 has to wait for the
             block that just completed; all older partitions are computed during the block before, spread out).
      lag 2: everything is computed during the block before (the response must be silent for its first 2B).

    Response spectra come from RealFFT::forwardScaled of the zero-padded segment (2B floats per partition).
    Terms take effect at the start of the next block (never in the middle of a sum). All memory is allocated
    in prepare(); push() is real-time safe. */
class PartitionedConvolution
{
public:
    struct Term
    {
        int input = 0;
        const float* spectra = nullptr;     // parts * 2B floats: partitions first .. first + parts - 1
        int first = 0, parts = 0;
        bool negate = false;
    };

    void prepare (int blockSize, int lagBlocks, int numInputs, int fdlDepth, int numOutputs, int maxTermsPerOutput)
    {
        B = blockSize; N = 2 * B; lag = lagBlocks;
        K = numInputs; D = std::max (1, fdlDepth); O = numOutputs; T = std::max (1, maxTermsPerOutput);
        fft = std::make_unique<RealFFT> (N);
        inWin.assign ((size_t) K * (size_t) N);
        fdl.assign ((size_t) K * (size_t) D * (size_t) N);
        acc.assign ((size_t) O * (size_t) N);
        tmp.assign ((size_t) N);
        outCur.assign ((size_t) O * (size_t) B);
        outNext.assign (lag == 2 ? (size_t) O * (size_t) B : 0);
        terms.assign ((size_t) O * (size_t) T, Term {});
        numTerms.assign ((size_t) O, 0);
        pending.assign ((size_t) O * (size_t) T, Term {});
        numPending.assign ((size_t) O, 0);
        hasPending.assign ((size_t) O, 0);
        anyPending = false;
        sinceApply.assign ((size_t) O, 1 << 20);
        reset();
    }

    void reset() noexcept
    {
        inWin.clear(); fdl.clear(); acc.clear(); outCur.clear(); outNext.clear();
        pos = 0; fdlPos = 0;
        startBlock();
    }

    /** audio thread: replaces the terms of an output (from the next block on) */
    void setTerms (int o, const Term* t, int count) noexcept
    {
        count = std::min (count, T);
        for (int i = 0; i < count; ++i) pending[(size_t) (o * T + i)] = t[i];
        numPending[(size_t) o] = count;
        hasPending[(size_t) o] = 1;
        anyPending = true;
    }
    void clearTerms (int o) noexcept { setTerms (o, nullptr, 0); }

    /** a change of this output's terms has not taken effect yet (the old response is still in use) */
    bool termsPending (int o) const noexcept { return hasPending[(size_t) o] != 0; }
    /** blocks completed with the current terms of an output (>= 1: its output() is made with them) */
    int blocksWithTerms (int o) const noexcept { return hasPending[(size_t) o] ? 0 : sinceApply[(size_t) o]; }

    int blockSize() const noexcept { return B; }
    int position() const noexcept { return pos; }
    int toBoundary() const noexcept { return B - pos; }
    int numOutputs() const noexcept { return O; }
    /** the current block's output (index it with position()) */
    const float* output (int o) const noexcept { return outCur.data() + (size_t) o * (size_t) B; }

    /** n <= toBoundary() samples of every input (a null pointer = silence); does the work that is due */
    void push (const float* const* in, int n) noexcept
    {
        for (int k = 0; k < K; ++k)
        {
            float* w = inWin.data() + (size_t) k * (size_t) N + (size_t) (B + pos);
            if (in != nullptr && in[k] != nullptr) std::copy (in[k], in[k] + n, w);
            else std::fill (w, w + n, 0.0f);
        }
        pos += n;
        // spread: by the end of the block all of this block's work is done
        const long long target = ((long long) unitsTotal * pos + B - 1) / B;
        work ((int) (target - unitsDone));
        if (pos >= B) boundary();
    }

private:
    static constexpr int kIfftUnits = 4;          // an inverse FFT costs about as much as this many partition products

    const float* slot (int k, int s) const noexcept
    {
        return fdl.data() + ((size_t) k * (size_t) D + (size_t) ((fdlPos + s) % D)) * (size_t) N;
    }

    /** the units of the current block: all partition products that are already possible, (lag 2) the inverse FFTs */
    void startBlock() noexcept
    {
        if (anyPending)
        {
            for (int o = 0; o < O; ++o)
                if (hasPending[(size_t) o])
                {
                    for (int i = 0; i < numPending[(size_t) o]; ++i) terms[(size_t) (o * T + i)] = pending[(size_t) (o * T + i)];
                    numTerms[(size_t) o] = numPending[(size_t) o];
                    hasPending[(size_t) o] = 0;
                    sinceApply[(size_t) o] = 0;
                }
            anyPending = false;
        }
        unitsTotal = 0;
        for (int o = 0; o < O; ++o)
        {
            int u = 0;
            for (int i = 0; i < numTerms[(size_t) o]; ++i)
            {
                const auto& t = terms[(size_t) (o * T + i)];
                u += (lag == 1 && t.first == 0) ? std::max (0, t.parts - 1) : t.parts;
            }
            if (lag == 2 && numTerms[(size_t) o] > 0) u += kIfftUnits;
            unitsTotal += u;
        }
        unitsDone = 0;
        curO = 0; curT = 0; curQ = 0;
    }

    /** does up to 'count' units in the order output -> term -> partition (lag 2: then the output's inverse FFT) */
    void work (int count) noexcept
    {
        while (count > 0 && curO < O)
        {
            const int o = curO;
            float* a = acc.data() + (size_t) o * (size_t) N;
            if (curT < numTerms[(size_t) o])
            {
                const auto& t = terms[(size_t) (o * T + curT)];
                int q = std::max (curQ, (lag == 1 && t.first == 0) ? 1 : 0);
                const int qEnd = t.parts;                     // index within the term
                for (; q < qEnd && count > 0; ++q, --count, ++unitsDone)
                {
                    const int part = t.first + q;               // partition number
                    const float* x = slot (t.input, part - (2 - lag));
                    const float* h = t.spectra + (size_t) q * (size_t) N;
                    if (t.negate) fft->multiplySub (x, h, a); else fft->multiplyAdd (x, h, a);
                }
                if (q >= qEnd) { ++curT; curQ = 0; } else curQ = q;
                continue;
            }
            if (lag == 2)
            {
                float* out = outNext.data() + (size_t) o * (size_t) B;
                if (numTerms[(size_t) o] == 0) std::fill (out, out + B, 0.0f);
                else
                {
                    // all products of this output done: back to the time domain (the last B samples are valid)
                    fft->inverse (a, tmp.data());
                    std::copy (tmp.data() + B, tmp.data() + N, out);
                    std::fill (a, a + N, 0.0f);
                    count -= kIfftUnits; unitsDone += kIfftUnits;
                }
            }
            ++curO; curT = 0; curQ = 0;
        }
    }

    void boundary() noexcept
    {
        work (1 << 30);                          // (nothing should be left)
        // the completed window [previous block | this block] of every input -> newest FDL slot
        fdlPos = (fdlPos + D - 1) % D;
        for (int k = 0; k < K; ++k)
        {
            float* w = inWin.data() + (size_t) k * (size_t) N;
            float* x = fdl.data() + ((size_t) k * (size_t) D + (size_t) fdlPos) * (size_t) N;
            fft->forward (w, x);
            std::copy (w + B, w + N, w);
        }
        if (lag == 1)
        {
            // the newest partition, then the output of the block that starts now
            for (int o = 0; o < O; ++o)
            {
                float* a = acc.data() + (size_t) o * (size_t) N;
                float* out = outCur.data() + (size_t) o * (size_t) B;
                if (numTerms[(size_t) o] == 0) { std::fill (out, out + B, 0.0f); continue; }
                for (int i = 0; i < numTerms[(size_t) o]; ++i)
                {
                    const auto& t = terms[(size_t) (o * T + i)];
                    if (t.first != 0 || t.parts <= 0) continue;
                    if (t.negate) fft->multiplySub (slot (t.input, 0), t.spectra, a); else fft->multiplyAdd (slot (t.input, 0), t.spectra, a);
                }
                fft->inverse (a, tmp.data());
                std::copy (tmp.data() + B, tmp.data() + N, out);
                std::fill (a, a + N, 0.0f);
            }
        }
        else std::swap (outCur, outNext);
        for (auto& s : sinceApply) s = std::min (s + 1, 1 << 20);
        pos = 0;
        startBlock();
    }

    int B = 512, N = 1024, lag = 1, K = 1, D = 1, O = 1, T = 1;
    std::unique_ptr<RealFFT> fft;
    AlignedFloats inWin, fdl, acc, tmp, outCur, outNext;
    std::vector<Term> terms, pending;
    std::vector<int> numTerms, numPending, sinceApply;
    std::vector<char> hasPending;
    bool anyPending = false;
    int pos = 0, fdlPos = 0;
    int unitsTotal = 0, unitsDone = 0, curO = 0, curT = 0, curQ = 0;
};
} // namespace rysum
