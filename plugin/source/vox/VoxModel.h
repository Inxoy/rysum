// Vox humana (NeuralRysum v2) - model file loader and small layers.
// Header-only, C++17 standard library only. Loading allocates: not on the audio thread.
//
// File layout (little endian), written by neuralrysum/export_v2.py:
//   "NRV20001"  8 bytes | header length uint32 | ASCII header lines
//       cfg <key> <ints...>      tensor <name> <offset> <ndim> <dims...>
//   | float32 data, offsets count floats
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "VoxSimd.h"

namespace vox
{
struct Tensor
{
    std::vector<int> shape;
    std::vector<float> data;
    size_t numel() const noexcept { return data.size(); }
};

class ModelFile
{
public:
    bool loadFromMemory(const void *bytes, size_t size, std::string &err)
    {
        try
        {
            parse(static_cast<const char *>(bytes), size);
            return true;
        }
        catch (const std::exception &e)
        {
            err = e.what();
            cfg.clear();
            tensors.clear();
            return false;
        }
    }

    bool load(const std::string &path, std::string &err)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            err = "cannot open " + path;
            return false;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return loadFromMemory(bytes.data(), bytes.size(), err);
    }

    bool has(const std::string &name) const { return tensors.count(name) != 0; }

    const Tensor &t(const std::string &name) const
    {
        auto it = tensors.find(name);
        if (it == tensors.end())
            throw std::runtime_error("vox model has no tensor '" + name + "'");
        return it->second;
    }

    const std::vector<int> &list(const std::string &key) const
    {
        auto it = cfg.find(key);
        if (it == cfg.end() || it->second.empty())
            throw std::runtime_error("vox model has no config entry '" + key + "'");
        return it->second;
    }

    int num(const std::string &key) const { return list(key)[0]; }

    std::map<std::string, std::vector<int>> cfg;
    std::map<std::string, Tensor> tensors;

private:
    void parse(const char *p, size_t size)
    {
        if (size < 12 || std::memcmp(p, "NRV20001", 8) != 0)
            throw std::runtime_error("not a Vox humana model file");
        uint32_t hlen = 0;
        std::memcpy(&hlen, p + 8, 4);
        if (12 + static_cast<size_t>(hlen) > size)
            throw std::runtime_error("truncated header");
        std::istringstream header(std::string(p + 12, hlen));
        const char *data = p + 12 + hlen;
        const size_t nFloats = (size - 12 - hlen) / 4;
        std::string line;
        while (std::getline(header, line))
        {
            std::istringstream ls(line);
            std::string kind, name;
            ls >> kind >> name;
            if (kind == "cfg")
            {
                int v;
                while (ls >> v)
                    cfg[name].push_back(v);
            }
            else if (kind == "tensor")
            {
                size_t offset = 0, count = 1;
                int ndim = 0;
                ls >> offset >> ndim;
                Tensor t;
                for (int i = 0; i < ndim; ++i)
                {
                    int d = 0;
                    ls >> d;
                    if (d <= 0)
                        throw std::runtime_error("bad shape for " + name);
                    t.shape.push_back(d);
                    count *= static_cast<size_t>(d);
                }
                if (ls.fail() || offset + count > nFloats)
                    throw std::runtime_error("truncated data for " + name);
                t.data.resize(count);
                std::memcpy(t.data.data(), data + offset * 4, count * 4);
                tensors[name] = std::move(t);
            }
        }
    }
};

constexpr float kLeak = 0.2f;

inline void leaky(const float *src, float *dst, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] > 0.0f ? src[i] : kLeak * src[i];
}

/** dense layer y = W x + b, weights stored transposed ([in][outP]) so the inner loop runs over the
    outputs; outP = out rounded up to 8 (zero padded) */
struct Linear
{
    int in = 0, out = 0, outP = 0;
    std::vector<float> wt, b;

    void init(const Tensor &W, const Tensor &B)
    {
        if (W.shape.size() != 2)
            throw std::runtime_error("linear weight must be 2-D");
        out = W.shape[0];
        in = W.shape[1];
        outP = (out + 7) / 8 * 8;
        wt.assign(static_cast<size_t>(in) * outP, 0.0f);
        for (int o = 0; o < out; ++o)
            for (int i = 0; i < in; ++i)
                wt[static_cast<size_t>(i) * outP + o] = W.data[static_cast<size_t>(o) * in + i];
        b.assign((size_t)outP, 0.0f);
        std::copy(B.data.begin(), B.data.end(), b.begin());
    }

    /** nx inputs at once: x rows of `in` floats, y rows of `outP` floats */
    void applyBatch(const float *x, int nx, float *y) const noexcept
    {
#if VOX_SSE
        if (haveAvx2())
        {
            matmulAvx2(x, nx, in, wt.data(), b.data(), outP, y);
            return;
        }
#endif
        for (int v = 0; v < nx; ++v)
            std::memcpy(y + (size_t)v * outP, b.data(), (size_t)outP * sizeof(float));
        for (int i = 0; i < in; ++i)
        {
            const float *wc = wt.data() + static_cast<size_t>(i) * outP;
            for (int v = 0; v < nx; ++v)
            {
                const float s = x[(size_t)v * in + i];
                float *yv = y + (size_t)v * outP;
                for (int o = 0; o < outP; ++o)
                    yv[o] += s * wc[o];
            }
        }
    }

    void apply(const float *x, float *y) const noexcept
    {
        for (int o = 0; o < out; ++o)
            y[o] = b[(size_t)o];
        for (int i = 0; i < in; ++i)
        {
            const float v = x[i];
            if (v == 0.0f)
                continue;
            const float *wc = wt.data() + static_cast<size_t>(i) * outP;
            for (int o = 0; o < out; ++o)
                y[o] += v * wc[o];
        }
    }
};

/** causal 1-D conv, frames interleaved [time][channel]; keeps its history across calls */
struct Conv1d
{
    int cin = 0, cout = 0, k = 0, stride = 1, dil = 1, pad = 0;
    std::vector<float> w, b, hist, buf;

    void init(const Tensor &W, const Tensor &B, int stride_, int dil_, int maxFrames)
    {
        if (W.shape.size() != 3)
            throw std::runtime_error("conv weight must be 3-D");
        cout = W.shape[0];
        cin = W.shape[1];
        k = W.shape[2];
        stride = stride_;
        dil = dil_;
        pad = (k - 1) * dil + 1 - stride;
        if (pad < 0)
            throw std::runtime_error("conv kernel smaller than stride");
        w.resize(static_cast<size_t>(k) * cin * cout);
        for (int co = 0; co < cout; ++co)
            for (int ci = 0; ci < cin; ++ci)
                for (int kk = 0; kk < k; ++kk)
                    w[(static_cast<size_t>(kk) * cin + ci) * cout + co] =
                        W.data[(static_cast<size_t>(co) * cin + ci) * k + kk];
        b = B.data;
        hist.assign(static_cast<size_t>(pad) * cin, 0.0f);
        buf.assign(static_cast<size_t>(pad + maxFrames) * cin, 0.0f);
    }

    void reset() noexcept { std::fill(hist.begin(), hist.end(), 0.0f); }

    void process(const float *in, int T, float *out) noexcept
    {
        const size_t histBytes = hist.size() * sizeof(float);
        if (pad > 0)
            std::memcpy(buf.data(), hist.data(), histBytes);
        std::memcpy(buf.data() + hist.size(), in, static_cast<size_t>(T) * cin * sizeof(float));
        const int To = T / stride;
        for (int to = 0; to < To; ++to)
        {
            float *o = out + static_cast<size_t>(to) * cout;
            for (int co = 0; co < cout; ++co)
                o[co] = b[(size_t)co];
            for (int kk = 0; kk < k; ++kk)
            {
                const float *x = buf.data() + static_cast<size_t>(to * stride + kk * dil) * cin;
                const float *wk = w.data() + static_cast<size_t>(kk) * cin * cout;
                for (int ci = 0; ci < cin; ++ci)
                {
                    const float v = x[ci];
                    const float *wc = wk + static_cast<size_t>(ci) * cout;
                    for (int co = 0; co < cout; ++co)
                        o[co] += v * wc[co];
                }
            }
        }
        if (pad > 0)
            std::memcpy(hist.data(), buf.data() + static_cast<size_t>(T) * cin, histBytes);
    }
};

/** one GRU layer, PyTorch gate order (r, z, n) */
struct GRU
{
    int H = 0;
    Linear ih, hh;
    std::vector<float> h, a, c;

    void init(const ModelFile &m, const std::string &prefix)
    {
        ih.init(m.t(prefix + ".weight_ih_l0"), m.t(prefix + ".bias_ih_l0"));
        hh.init(m.t(prefix + ".weight_hh_l0"), m.t(prefix + ".bias_hh_l0"));
        H = hh.in;
        h.assign((size_t)H, 0.0f);
        a.assign((size_t)(3 * H), 0.0f);
        c.assign((size_t)(3 * H), 0.0f);
    }

    void reset() noexcept { std::fill(h.begin(), h.end(), 0.0f); }

    static float sigmoid(float v) noexcept { return 1.0f / (1.0f + std::exp(-v)); }

    void step(const float *x) noexcept
    {
        ih.apply(x, a.data());
        hh.apply(h.data(), c.data());
        for (int i = 0; i < H; ++i)
        {
            const float r = sigmoid(a[(size_t)i] + c[(size_t)i]);
            const float z = sigmoid(a[(size_t)(H + i)] + c[(size_t)(H + i)]);
            const float n = std::tanh(a[(size_t)(2 * H + i)] + r * c[(size_t)(2 * H + i)]);
            h[(size_t)i] = (1.0f - z) * n + z * h[(size_t)i];
        }
    }
};
} // namespace vox
