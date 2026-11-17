#pragma once

#include "Modulation.h"
#include <cmath>
#include <algorithm>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <limits>
#include <vector>

namespace rysum
{
// ---------------------------------------------------------------------------
// Verlauf: automation over time. Every lane drives one plugin parameter along a curve of points.
// Time is counted in beats (quarter notes) everywhere: on the host's song position, or on the
// plugin's own clock (its own tempo, 60 = one beat per second).
// A lane takes the parameter over (like DAW automation); the modulation routings add on top.
// ---------------------------------------------------------------------------
constexpr int kMaxAutoLanes = 16;

/** what a segment (from its point to the next) does */
enum AutoShape
{
    asCurve = 0, // straight or bent line to the next point
    asStep,      // holds the value until the next point
    asWave,      // the line plus a sine wave (rate: cycles per beat, depth: share of the range)
    asJitter,    // the line plus smooth random wandering (rate: changes per beat)
    asStairs,    // the line in stairs (rate: steps per beat)
    kNumAutoShapes
};

inline juce::StringArray autoShapeNames()
{
    return {"Kurve", "Stufe", "Welle", "Zittern", "Treppe"};
}

struct AutoPoint
{
    double t = 0.0;     // beats, lane time
    float v = 0.0f;     // normalised value 0..1
    float bend = 0.0f;  // -1..1: the curve sags (<0) or bulges (>0) towards the next point
    int shape = asCurve;
    float rate = 4.0f;  // waves / changes / steps per beat
    float depth = 0.2f; // share of the range (wave, jitter)
};

struct AutoLane
{
    juce::String paramId;
    bool on = true;
    bool armed = false;    // record: moving the control while the clock runs writes points
    double length = 16.0;  // beats: the loop (and reverse) region [0, length)
    bool loop = false;     // the lane repeats its region
    float speed = 1.0f;    // time stretch: 2 = twice as fast, 0.5 = half
    bool reverse = false;  // the region read backwards
    double offset = 0.0;   // beats: where the lane starts on the clock
    std::vector<AutoPoint> pts; // sorted by t
    // a lane on a per-pipe target ("pipe:<t>"): the gesture spreads through the room from an
    // origin - every pipe hears it when the front reaches it (distance / speed later)
    bool spread = false;
    float ox = 0.0f, oy = 4.0f, oz = 0.0f; // origin, metres: x = right, y = front, z = up
    float waveSpeed = 4.0f;                // metres per beat
    bool inward = false;                   // the front runs from far away towards the origin
    int height = 0; // the editor: the lane's height in pixels, 0 = automatic (taller when chosen)
};

/** the whole automation of a project (message thread) */
struct AutoSet
{
    bool on = true;
    int clock = 0;         // 0 = host song position (own clock without a host), 1 = own clock
    double tempo = 60.0;   // own clock: beats per minute
    bool startOnKey = false; // own clock starts with the first key
    double viewStart = 0.0, viewLen = 32.0; // the editor's window (beats)
    double grid = 1.0;     // beats per grid step, 0 = off
    std::vector<AutoLane> lanes;
};

// ---------------------------------------------------------------------------
// evaluation: a pure function of the time (seeking, host loops and bounces give the same result)
// ---------------------------------------------------------------------------

/** clock time (beats) -> the lane's own time: offset, stretch, loop, reverse */
inline double autoLocalTime(const AutoLane &l, double beats) noexcept
{
    double tau = (beats - l.offset) * (double)l.speed;
    const double len = juce::jmax(1.0e-6, l.length);
    if (l.loop)
    {
        tau = std::fmod(tau, len);
        if (tau < 0.0)
            tau += len;
    }
    if (l.reverse)
        tau = len - juce::jlimit(0.0, len, tau);
    return tau;
}

/** smooth value noise -1..1 (cosine between hashed steps) */
inline float autoNoise(double x, uint32_t seed) noexcept
{
    const double fl = std::floor(x);
    const auto i = (uint32_t)(int64_t)fl;
    const float a = hash01(i * 2654435761u + seed) * 2.0f - 1.0f,
                b = hash01((i + 1u) * 2654435761u + seed) * 2.0f - 1.0f;
    const float f = (float)(x - fl);
    const float s = 0.5f - 0.5f * std::cos(f * juce::MathConstants<float>::pi);
    return a + (b - a) * s;
}

/** the bent progress 0..1 of a curve segment */
inline float autoBend(float u, float bend) noexcept
{
    if (std::abs(bend) < 1.0e-4f)
        return u;
    const float e = std::pow(2.0f, -bend * 3.0f); // bend +1: x^(1/8) (fast first) .. -1: x^8
    return std::pow(juce::jlimit(0.0f, 1.0f, u), e);
}

/** value of a segment from point a to point b at lane time tau (a.t <= tau) */
inline float autoSegment(const AutoPoint &a, const AutoPoint *b, double tau, int segIndex) noexcept
{
    if (b == nullptr)
        return a.v;
    const double span = juce::jmax(1.0e-9, b->t - a.t);
    float u = (float)juce::jlimit(0.0, 1.0, (tau - a.t) / span);
    const double local = tau - a.t;
    switch (a.shape)
    {
    case asStep:
        return a.v;
    case asStairs:
    {
        const float n = juce::jmax(1.0f, std::round(a.rate * (float)span));
        u = std::floor(u * n) / juce::jmax(1.0f, n - 1.0f);
        u = juce::jmin(1.0f, u);
        return a.v + (b->v - a.v) * autoBend(u, a.bend);
    }
    case asWave:
    {
        const float line = a.v + (b->v - a.v) * autoBend(u, a.bend);
        return line + a.depth * 0.5f *
                          std::sin((float)(juce::MathConstants<double>::twoPi * a.rate * local));
    }
    case asJitter:
    {
        const float line = a.v + (b->v - a.v) * autoBend(u, a.bend);
        return line + a.depth * 0.5f * autoNoise(local * a.rate, (uint32_t)segIndex * 977u + 13u);
    }
    default:
        return a.v + (b->v - a.v) * autoBend(u, a.bend);
    }
}

/** index of the last point at or before tau, -1 = before the first */
inline int autoFindSegment(const std::vector<AutoPoint> &pts, double tau) noexcept
{
    int lo = 0, hi = (int)pts.size() - 1, found = -1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (pts[(size_t)mid].t <= tau)
        {
            found = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return found;
}

/** the lane's value 0..1 at its own time tau; NaN if it has no points */
inline float autoValueAtLocal(const AutoLane &l, double tau) noexcept
{
    if (l.pts.empty())
        return std::numeric_limits<float>::quiet_NaN();
    const int i = autoFindSegment(l.pts, tau);
    if (i < 0)
        return l.pts.front().v;
    const auto *next = i + 1 < (int)l.pts.size() ? &l.pts[(size_t)i + 1] : nullptr;
    return juce::jlimit(0.0f, 1.0f, autoSegment(l.pts[(size_t)i], next, tau, i));
}

/** quantise to a parameter with steps (switches, choices); steps < 2 = continuous */
inline float autoQuantise(float v, int steps) noexcept
{
    if (steps < 2)
        return v;
    const float n = (float)(steps - 1);
    return std::round(v * n) / n;
}

/** the lane's value at the clock time (beats) */
inline float autoValue(const AutoLane &l, double beats, int steps = 0) noexcept
{
    const float v = autoValueAtLocal(l, autoLocalTime(l, beats));
    return std::isnan(v) ? v : autoQuantise(v, steps);
}

// ---------------------------------------------------------------------------
// lanes that are not parameters: per-pipe targets ("pipe:<t>", spreading through the room) and
// timed paths ("path:<stop>", the position along a stop's drawn path, 0..1)
// ---------------------------------------------------------------------------
inline juce::String autoPathLaneId(int stop)
{
    return "path:" + juce::String(stop);
}
/** the per-pipe target of a lane, -1 = not a per-pipe lane */
inline int autoPipeTarget(const juce::String &id)
{
    if (!id.startsWith("pipe:"))
        return -1;
    const int t = id.fromFirstOccurrenceOf(":", false, false).getIntValue();
    return t >= 0 && t < kNumPipeTargets ? t : -1;
}
/** the stop of a timed path lane, -1 = not a path lane */
inline int autoPathStop(const juce::String &id)
{
    if (!id.startsWith("path:"))
        return -1;
    const int s = id.fromFirstOccurrenceOf(":", false, false).getIntValue();
    return s >= 0 && s < kNumStops ? s : -1;
}
/** a per-pipe lane's value 0..1 as the offset it gives its target (0.5 = none, at full range
    like a routing at 100 %) */
inline float autoPipeOffset(float v) noexcept
{
    return 2.0f * v - 1.0f;
}

/** where a direction + distance is in metres (x = right, y = front, z = up), as the engine
    counts it */
inline void autoPosToXyz(float azDeg, float elDeg, float dist, float &x, float &y, float &z) noexcept
{
    const float a = juce::degreesToRadians(azDeg), e = juce::degreesToRadians(elDeg);
    x = -std::sin(a) * std::cos(e) * dist;
    y = std::cos(a) * std::cos(e) * dist;
    z = std::sin(e) * dist;
}

/** the radius an inward front starts from (metres) */
constexpr float kAutoInwardRadius = 25.0f;

/** the clock time (beats) a pipe at x, y, z hears of a spreading lane at clock time 'beats' */
inline double autoPipeTime(const AutoLane &l, double beats, float x, float y, float z) noexcept
{
    if (!l.spread)
        return beats;
    const float dx = x - l.ox, dy = y - l.oy, dz = z - l.oz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float run = l.inward ? juce::jmax(0.0f, kAutoInwardRadius - d) : d;
    return beats - (double)(run / juce::jmax(0.05f, l.waveSpeed));
}

/** what the engine reads of the Verlauf (built on the message thread, immutable while the audio
    thread holds it) */
struct AutoEngineView
{
    struct PipeLane
    {
        const AutoLane *lane = nullptr;
        int target = 0;
    };
    std::array<PipeLane, kMaxAutoLanes> pipe{};
    int numPipe = 0;
    std::array<const AutoLane *, kNumStops> path{}; // timed path per stop, nullptr = none

    /** adds the per-pipe lanes to a pipe's modulation offsets; true if any acted */
    bool addPipe(float *pm, double beats, float azDeg, float elDeg, float dist) const noexcept
    {
        if (numPipe <= 0)
            return false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        autoPosToXyz(azDeg, elDeg, dist, x, y, z);
        bool any = false;
        for (int i = 0; i < numPipe; ++i)
        {
            const auto &pl = pipe[(size_t)i];
            const float v = autoValue(*pl.lane, autoPipeTime(*pl.lane, beats, x, y, z));
            if (std::isnan(v))
                continue;
            pm[pl.target] += autoPipeOffset(v);
            any = true;
        }
        return any;
    }
};

// ---------------------------------------------------------------------------
// editing helpers (message thread)
// ---------------------------------------------------------------------------
inline void autoSort(AutoLane &l)
{
    std::stable_sort(l.pts.begin(), l.pts.end(),
                     [](const AutoPoint &a, const AutoPoint &b) { return a.t < b.t; });
}

/** Ramer-Douglas-Peucker: keeps the points that matter for a drawn / recorded line.
    tolV = value tolerance (0..1), timeScale = how many value units one beat counts as */
inline std::vector<AutoPoint> autoThin(const std::vector<AutoPoint> &in, float tolV,
                                       double timeScale)
{
    if (in.size() <= 2)
        return in;
    std::vector<char> keep(in.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<int, int>> stack{{0, (int)in.size() - 1}};
    while (!stack.empty())
    {
        auto [a, b] = stack.back();
        stack.pop_back();
        const double ax = in[(size_t)a].t * timeScale, ay = in[(size_t)a].v;
        const double bx = in[(size_t)b].t * timeScale, by = in[(size_t)b].v;
        const double dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
        double worst = -1.0;
        int wi = -1;
        for (int i = a + 1; i < b; ++i)
        {
            const double px = in[(size_t)i].t * timeScale - ax, py = in[(size_t)i].v - ay;
            const double d = len > 1.0e-12 ? std::abs(px * dy - py * dx) / len
                                            : std::sqrt(px * px + py * py);
            if (d > worst)
            {
                worst = d;
                wi = i;
            }
        }
        if (wi >= 0 && worst > tolV)
        {
            keep[(size_t)wi] = 1;
            stack.push_back({a, wi});
            stack.push_back({wi, b});
        }
    }
    std::vector<AutoPoint> out;
    for (size_t i = 0; i < in.size(); ++i)
        if (keep[i])
            out.push_back(in[i]);
    return out;
}

/** replaces the points of a lane in [t0, t1] by others (drawn or recorded); the value just
    outside the range stays where it was */
inline void autoReplaceRange(AutoLane &l, double t0, double t1, const std::vector<AutoPoint> &with)
{
    if (t1 < t0)
        std::swap(t0, t1);
    l.pts.erase(std::remove_if(l.pts.begin(), l.pts.end(),
                               [&](const AutoPoint &p) { return p.t >= t0 && p.t <= t1; }),
                l.pts.end());
    for (auto &p : with)
        l.pts.push_back(p);
    autoSort(l);
}

// ---------------------------------------------------------------------------
// saving (project state)
// ---------------------------------------------------------------------------
inline juce::ValueTree autoToTree(const AutoSet &s)
{
    juce::ValueTree t("VERLAUF");
    t.setProperty("on", s.on, nullptr);
    t.setProperty("clock", s.clock, nullptr);
    t.setProperty("tempo", s.tempo, nullptr);
    t.setProperty("startOnKey", s.startOnKey, nullptr);
    t.setProperty("viewStart", s.viewStart, nullptr);
    t.setProperty("viewLen", s.viewLen, nullptr);
    t.setProperty("grid", s.grid, nullptr);
    for (auto &l : s.lanes)
    {
        juce::ValueTree lt("LANE");
        lt.setProperty("param", l.paramId, nullptr);
        lt.setProperty("on", l.on, nullptr);
        lt.setProperty("length", l.length, nullptr);
        lt.setProperty("loop", l.loop, nullptr);
        lt.setProperty("speed", l.speed, nullptr);
        lt.setProperty("reverse", l.reverse, nullptr);
        lt.setProperty("offset", l.offset, nullptr);
        lt.setProperty("spread", l.spread, nullptr);
        lt.setProperty("ox", l.ox, nullptr);
        lt.setProperty("oy", l.oy, nullptr);
        lt.setProperty("oz", l.oz, nullptr);
        lt.setProperty("waveSpeed", l.waveSpeed, nullptr);
        lt.setProperty("inward", l.inward, nullptr);
        if (l.height > 0)
            lt.setProperty("height", l.height, nullptr);
        // points as text: t v bend shape rate depth; ...
        juce::String pts;
        for (auto &p : l.pts)
            pts << juce::String(p.t, 5) << ' ' << juce::String(p.v, 5) << ' '
                << juce::String(p.bend, 3) << ' ' << p.shape << ' ' << juce::String(p.rate, 3)
                << ' ' << juce::String(p.depth, 3) << ';';
        lt.setProperty("pts", pts, nullptr);
        t.appendChild(lt, nullptr);
    }
    return t;
}

inline AutoSet autoFromTree(const juce::ValueTree &t)
{
    AutoSet s;
    if (!t.isValid())
        return s;
    s.on = (bool)t.getProperty("on", true);
    s.clock = juce::jlimit(0, 1, (int)t.getProperty("clock", 0));
    s.tempo = juce::jlimit(10.0, 400.0, (double)t.getProperty("tempo", 60.0));
    s.startOnKey = (bool)t.getProperty("startOnKey", false);
    s.viewStart = juce::jmax(0.0, (double)t.getProperty("viewStart", 0.0));
    s.viewLen = juce::jlimit(1.0, 4096.0, (double)t.getProperty("viewLen", 32.0));
    s.grid = juce::jmax(0.0, (double)t.getProperty("grid", 1.0));
    for (const auto &lt : t)
    {
        if (!lt.hasType("LANE") || (int)s.lanes.size() >= kMaxAutoLanes)
            continue;
        AutoLane l;
        l.paramId = lt.getProperty("param").toString();
        l.on = (bool)lt.getProperty("on", true);
        l.length = juce::jlimit(0.25, 4096.0, (double)lt.getProperty("length", 16.0));
        l.loop = (bool)lt.getProperty("loop", false);
        l.speed = juce::jlimit(0.0625f, 16.0f, (float)(double)lt.getProperty("speed", 1.0));
        l.reverse = (bool)lt.getProperty("reverse", false);
        l.offset = (double)lt.getProperty("offset", 0.0);
        l.spread = (bool)lt.getProperty("spread", false);
        l.ox = (float)(double)lt.getProperty("ox", 0.0);
        l.oy = (float)(double)lt.getProperty("oy", 4.0);
        l.oz = (float)(double)lt.getProperty("oz", 0.0);
        l.waveSpeed = juce::jlimit(0.1f, 400.0f, (float)(double)lt.getProperty("waveSpeed", 4.0));
        l.inward = (bool)lt.getProperty("inward", false);
        l.height = juce::jlimit(0, 400, (int)lt.getProperty("height", 0));
        juce::StringArray items;
        items.addTokens(lt.getProperty("pts").toString(), ";", "");
        for (auto &it : items)
        {
            juce::StringArray f;
            f.addTokens(it, " ", "");
            f.removeEmptyStrings();
            if (f.size() < 2)
                continue;
            AutoPoint p;
            p.t = f[0].getDoubleValue();
            p.v = juce::jlimit(0.0f, 1.0f, f[1].getFloatValue());
            if (f.size() >= 6)
            {
                p.bend = juce::jlimit(-1.0f, 1.0f, f[2].getFloatValue());
                p.shape = juce::jlimit(0, kNumAutoShapes - 1, f[3].getIntValue());
                p.rate = juce::jlimit(0.01f, 64.0f, f[4].getFloatValue());
                p.depth = juce::jlimit(0.0f, 1.0f, f[5].getFloatValue());
            }
            l.pts.push_back(p);
        }
        autoSort(l);
        if (l.paramId.isNotEmpty())
            s.lanes.push_back(std::move(l));
    }
    return s;
}
} // namespace rysum
