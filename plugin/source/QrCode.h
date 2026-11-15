#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rysum
{
/** A QR code (ISO/IEC 18004) for a short text such as an address: byte mode, error correction
    level M, versions 1 to 6 (at most 106 bytes), the mask chosen by the penalty rules. Enough for
    the audience's phones to find the plugin; anything longer gives an empty code. */
struct QrCode
{
    int size = 0;              // modules per side (0: the text did not fit)
    std::vector<uint8_t> dark; // size * size, row by row

    bool at(int x, int y) const noexcept
    {
        return dark[(size_t)(y * size + x)] != 0;
    }

    static QrCode encode(const std::string &text)
    {
        QrCode q;
        // level M: all codewords, error correction codewords per block, blocks (equal in 1-6)
        static const int total[7]{0, 26, 44, 70, 100, 134, 172};
        static const int eccPer[7]{0, 10, 16, 26, 18, 24, 16};
        static const int blocks[7]{0, 1, 1, 1, 2, 2, 4};
        int ver = 0;
        for (int v = 1; v <= 6 && ver == 0; ++v)
            if (4 + 8 + 8 * (int)text.size() <= 8 * (total[v] - eccPer[v] * blocks[v]))
                ver = v;
        if (ver == 0)
            return q;
        const int dataCw = total[ver] - eccPer[ver] * blocks[ver];

        // the bits: mode (byte), count, the bytes, terminator, padding
        std::vector<uint8_t> bits;
        auto put = [&bits](unsigned val, int n)
        {
            for (int i = n - 1; i >= 0; --i)
                bits.push_back((uint8_t)((val >> i) & 1u));
        };
        put(4, 4);
        put((unsigned)text.size(), 8);
        for (unsigned char c : text)
            put(c, 8);
        const int cap = 8 * dataCw;
        put(0, std::min(4, cap - (int)bits.size()));
        while (bits.size() % 8 != 0)
            bits.push_back(0);
        for (unsigned pad = 0xEC; (int)bits.size() < cap; pad ^= 0xEC ^ 0x11)
            put(pad, 8);
        std::vector<uint8_t> data((size_t)dataCw, 0);
        for (size_t i = 0; i < bits.size(); ++i)
            data[i >> 3] = (uint8_t)(data[i >> 3] | (bits[i] << (7 - (i & 7))));

        // Reed-Solomon per block, then interleaved
        const int ecc = eccPer[ver], nb = blocks[ver], per = dataCw / nb;
        const auto div = rsDivisor(ecc);
        std::vector<std::vector<uint8_t>> eccs;
        for (int b = 0; b < nb; ++b)
            eccs.push_back(rsRemainder(data.data() + b * per, per, div));
        std::vector<uint8_t> cw;
        for (int i = 0; i < per; ++i)
            for (int b = 0; b < nb; ++b)
                cw.push_back(data[(size_t)(b * per + i)]);
        for (int i = 0; i < ecc; ++i)
            for (int b = 0; b < nb; ++b)
                cw.push_back(eccs[(size_t)b][(size_t)i]);

        q.size = 17 + 4 * ver;
        const int n = q.size;
        q.dark.assign((size_t)(n * n), 0);
        std::vector<uint8_t> isFn((size_t)(n * n), 0);
        auto setFn = [&](int x, int y, bool d)
        {
            q.dark[(size_t)(y * n + x)] = d ? 1 : 0;
            isFn[(size_t)(y * n + x)] = 1;
        };
        // timing, finders (with their light border), the alignment pattern
        for (int i = 0; i < n; ++i)
        {
            setFn(6, i, i % 2 == 0);
            setFn(i, 6, i % 2 == 0);
        }
        const int fx[3]{3, n - 4, 3}, fy[3]{3, 3, n - 4};
        for (int f = 0; f < 3; ++f)
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const int x = fx[f] + dx, y = fy[f] + dy;
                    if (x < 0 || y < 0 || x >= n || y >= n)
                        continue;
                    const int d = std::max(std::abs(dx), std::abs(dy));
                    setFn(x, y, d != 2 && d != 4);
                }
        if (ver >= 2)
        {
            const int p = 4 * ver + 10;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    setFn(p + dx, p + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
        auto drawFormat = [&](int mask)
        {
            const int d = (0 << 3) | mask; // level M = 00
            int rem = d;
            for (int i = 0; i < 10; ++i)
                rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            const int fb = ((d << 10) | rem) ^ 0x5412;
            auto bit = [fb](int i) { return ((fb >> i) & 1) != 0; };
            for (int i = 0; i <= 5; ++i)
                setFn(8, i, bit(i));
            setFn(8, 7, bit(6));
            setFn(8, 8, bit(7));
            setFn(7, 8, bit(8));
            for (int i = 9; i < 15; ++i)
                setFn(14 - i, 8, bit(i));
            for (int i = 0; i < 8; ++i)
                setFn(n - 1 - i, 8, bit(i));
            for (int i = 8; i < 15; ++i)
                setFn(8, n - 15 + i, bit(i));
            setFn(8, n - 8, true);
        };
        drawFormat(0); // (reserves the format areas)

        // the codewords, in the zigzag from the bottom right
        size_t bi = 0;
        for (int right = n - 1; right >= 1; right -= 2)
        {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < n; ++vert)
                for (int j = 0; j < 2; ++j)
                {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? n - 1 - vert : vert;
                    if (isFn[(size_t)(y * n + x)] || bi >= cw.size() * 8)
                        continue;
                    q.dark[(size_t)(y * n + x)] = (uint8_t)((cw[bi >> 3] >> (7 - (bi & 7))) & 1);
                    ++bi;
                }
        }

        auto applyMask = [&](int mask)
        {
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    if (isFn[(size_t)(y * n + x)])
                        continue;
                    bool inv = false;
                    switch (mask)
                    {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                    }
                    if (inv)
                        q.dark[(size_t)(y * n + x)] ^= 1;
                }
        };
        int best = 0;
        long bestPen = -1;
        for (int m = 0; m < 8; ++m)
        {
            applyMask(m);
            drawFormat(m);
            const long pen = q.penalty();
            if (bestPen < 0 || pen < bestPen)
            {
                bestPen = pen;
                best = m;
            }
            applyMask(m); // (undone)
        }
        applyMask(best);
        drawFormat(best);
        return q;
    }

private:
    static uint8_t gfMul(uint8_t x, uint8_t y) noexcept
    {
        int z = 0;
        for (int i = 7; i >= 0; --i)
        {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return (uint8_t)z;
    }
    static std::vector<uint8_t> rsDivisor(int degree)
    {
        std::vector<uint8_t> r((size_t)degree, 0);
        r[(size_t)degree - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i)
        {
            for (int j = 0; j < degree; ++j)
            {
                r[(size_t)j] = gfMul(r[(size_t)j], root);
                if (j + 1 < degree)
                    r[(size_t)j] ^= r[(size_t)j + 1];
            }
            root = gfMul(root, 0x02);
        }
        return r;
    }
    static std::vector<uint8_t> rsRemainder(const uint8_t *data, int len,
                                            const std::vector<uint8_t> &div)
    {
        std::vector<uint8_t> r(div.size(), 0);
        for (int i = 0; i < len; ++i)
        {
            const uint8_t factor = (uint8_t)(data[i] ^ r[0]);
            r.erase(r.begin());
            r.push_back(0);
            for (size_t k = 0; k < r.size(); ++k)
                r[k] ^= gfMul(div[k], factor);
        }
        return r;
    }
    /** rules 1, 2 and 4 of the standard (runs, 2x2 blocks, balance); rule 3 is left out (any
     * mask is valid, this only picks a good one) */
    long penalty() const
    {
        const int n = size;
        long p = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int a = 0; a < n; ++a)
            {
                int run = 0;
                bool last = false;
                for (int b = 0; b < n; ++b)
                {
                    const bool d = pass == 0 ? at(b, a) : at(a, b);
                    if (b > 0 && d == last)
                        ++run;
                    else
                    {
                        if (run >= 5)
                            p += 3 + (run - 5);
                        run = 1;
                        last = d;
                    }
                }
                if (run >= 5)
                    p += 3 + (run - 5);
            }
        for (int y = 0; y + 1 < n; ++y)
            for (int x = 0; x + 1 < n; ++x)
            {
                const bool d = at(x, y);
                if (d == at(x + 1, y) && d == at(x, y + 1) && d == at(x + 1, y + 1))
                    p += 3;
            }
        long darkN = 0;
        for (auto d : dark)
            darkN += d;
        const long tot = (long)n * n;
        const long k = (std::labs(darkN * 20 - tot * 10) + tot - 1) / tot - 1;
        p += k * 10;
        return p;
    }
};
} // namespace rysum
