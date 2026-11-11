#pragma once

#include "Common.h"
#include <array>
#include <cmath>
#include <vector>

namespace rysum
{
inline const double kScalaDefaultReferenceHz = 440.0 * std::exp2(-9.0 / 12.0);

/** Scala tuning files: a scale (.scl) and optionally a keyboard mapping (.kbm), as specified at
    https://www.huygens-fokker.org/scala/scl_format.html and .../help.htm#mappings

    Without a .kbm the default mapping is used: linear (every MIDI note one scale degree), degree 0
    on note 60 (C4), at the 12edo C frequency corresponding to A4 = 440 Hz.
    A .kbm supplies its own reference MIDI note and absolute frequency. */
struct ScalaTuning
{
    juce::String description;
    std::vector<double>
        cents; // degrees 1..N in cents; the last one is the period (usually the octave)
    // keyboard mapping
    bool hasKbm = false;
    int mapSize = 0, firstNote = 0, lastNote = 127, middleNote = 60, refNote = 60, octaveDegree = 0;
    double refFreq = kScalaDefaultReferenceHz;
    std::vector<int> mapping; // per position in the mapping: scale degree, -1 = unmapped ("x")

    int size() const noexcept
    {
        return (int)cents.size();
    }

    /** cents of scale degree d (any integer; repeats with the period) */
    double degreeCents(int d) const noexcept
    {
        const int n = size();
        if (n == 0)
            return 0.0;
        const int q = (int)std::floor((double)d / (double)n);
        const int r = d - q * n;
        return q * cents[(size_t)n - 1] + (r == 0 ? 0.0 : cents[(size_t)r - 1]);
    }

    /** scale degree of a MIDI note, false = unmapped */
    bool noteDegree(int note, int &degree) const noexcept
    {
        if (note < firstNote || note > lastNote)
            return false;
        if (!hasKbm || mapSize <= 0)
        {
            degree = note - middleNote;
            return true;
        }
        const int off = note - middleNote;
        const int q = (int)std::floor((double)off / (double)mapSize);
        const int r = off - q * mapSize;
        const int m = r < (int)mapping.size() ? mapping[(size_t)r] : -1;
        if (m < 0)
            return false;
        degree = q * (octaveDegree > 0 ? octaveDegree : size()) + m;
        return true;
    }

    /** frequency of every MIDI note (0 = unmapped) */
    std::array<double, 128> frequencies() const
    {
        std::array<double, 128> f{};
        int refDeg = 0;
        if (!noteDegree(refNote, refDeg))
            refDeg = refNote - middleNote; // (an unmapped reference: as if linear)
        const double refCents = degreeCents(refDeg);
        for (int n = 0; n < 128; ++n)
        {
            int d = 0;
            if (noteDegree(n, d))
                f[(size_t)n] = refFreq * std::pow(2.0, (degreeCents(d) - refCents) / 1200.0);
        }
        return f;
    }

    // ------------------------------------------------------------------
    static juce::StringArray contentLines(const juce::String &text)
    {
        juce::StringArray lines, out;
        lines.addLines(text);
        for (auto &l : lines)
            if (!l.trimStart().startsWithChar('!'))
                out.add(l);
        return out;
    }

    /** parses a .scl file into 'out' (keeps out's mapping) */
    static bool parseScl(const juce::String &text, ScalaTuning &out, juce::String &error)
    {
        const auto lines = contentLines(text);
        if (lines.size() < 2)
        {
            error = "keine Scala-Datei (zu kurz)";
            return false;
        }
        const auto desc = lines[0].trim();
        const int count = lines[1].trim().upToFirstOccurrenceOf(" ", false, false).getIntValue();
        if (count <= 0 || count > 1024)
        {
            error = u8("Anzahl der Stufen fehlt oder ist ung\xc3\xbcltig");
            return false;
        }
        std::vector<double> c;
        for (int i = 2; i < lines.size() && (int)c.size() < count; ++i)
        {
            const auto t = lines[i].trim();
            if (t.isEmpty())
                continue;
            const auto tok = t.upToFirstOccurrenceOf(" ", false, false)
                                 .upToFirstOccurrenceOf("\t", false, false);
            double v = 0.0;
            if (tok.containsChar('.'))
                v = tok.getDoubleValue();
            else
            {
                const double num = tok.upToFirstOccurrenceOf("/", false, false).getDoubleValue();
                const double den =
                    tok.containsChar('/')
                        ? tok.fromFirstOccurrenceOf("/", false, false).getDoubleValue()
                        : 1.0;
                if (num <= 0.0 || den <= 0.0)
                {
                    error = u8("ung\xc3\xbcltige Stufe: ") + tok;
                    return false;
                }
                v = 1200.0 * std::log2(num / den);
            }
            c.push_back(v);
        }
        if ((int)c.size() != count)
        {
            error = "zu wenige Stufen (" + juce::String((int)c.size()) + " von " +
                    juce::String(count) + ")";
            return false;
        }
        if (!(c.back() > 0.0))
        {
            error = u8("die letzte Stufe (Periode) muss gr\xc3\xb6\xc3\x9f"
                       "er als 1/1 sein");
            return false;
        }
        out.description = desc;
        out.cents = std::move(c);
        error.clear();
        return true;
    }

    /** parses a .kbm file into 'out' (keeps the scale) */
    static bool parseKbm(const juce::String &text, ScalaTuning &out, juce::String &error)
    {
        juce::StringArray v;
        for (auto &l : contentLines(text))
        {
            const auto t = l.trim();
            if (t.isNotEmpty())
                v.add(t.upToFirstOccurrenceOf(" ", false, false));
        }
        if (v.size() < 7)
        {
            error = "keine Tastenzuordnung (.kbm): zu wenige Werte";
            return false;
        }
        ScalaTuning k = out;
        k.mapSize = v[0].getIntValue();
        k.firstNote = juce::jlimit(0, 127, v[1].getIntValue());
        k.lastNote = juce::jlimit(0, 127, v[2].getIntValue());
        k.middleNote = juce::jlimit(0, 127, v[3].getIntValue());
        k.refNote = juce::jlimit(0, 127, v[4].getIntValue());
        k.refFreq = v[5].getDoubleValue();
        k.octaveDegree = v[6].getIntValue();
        if (k.mapSize < 0 || k.mapSize > 1024 || !(k.refFreq > 0.0))
        {
            error = u8("Tastenzuordnung: ung\xc3\xbcltige Werte");
            return false;
        }
        k.mapping.clear();
        for (int i = 0; i < k.mapSize; ++i)
        {
            const auto t = 7 + i < v.size() ? v[7 + i] : juce::String("x");
            k.mapping.push_back(t.startsWithIgnoreCase("x") ? -1 : t.getIntValue());
        }
        k.hasKbm = true;
        out = std::move(k);
        error.clear();
        return true;
    }

    void clearKbm() noexcept
    {
        hasKbm = false;
        mapSize = 0;
        firstNote = 0;
        lastNote = 127;
        middleNote = 60;
        refNote = 60;
        octaveDegree = 0;
        refFreq = kScalaDefaultReferenceHz;
        mapping.clear();
    }
};
} // namespace rysum
