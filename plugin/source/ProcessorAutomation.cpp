// Verlauf (automation over time) of RysumProcessor: handing the lanes to the audio thread,
// applying them every slice, and recording a hand's movement into an armed lane
#include "PluginProcessor.h"

using namespace rysum;

bool RysumProcessor::isAutoTarget(int i) const
{
    if (i < 0 || i >= (int)prmList.size())
        return false;
    const auto id = prmList[(size_t)i]->getParameterID();
    if (isFloatParam[(size_t)i])
        return true;
    // switches and choices that make sense in a piece
    for (int s = 0; s < kNumStops; ++s)
        if (id == stopParamId(s) || id == motionParamId(s) || id == jumpHardParamId(s))
            return true;
    // (only switches the engine reads in every slice: others - Saal, Lucier, tuning, the IR - are
    // read once per block or off the audio thread, so a lane would not reach them)
    static const juce::StringArray sw{"freeze", "tremOn", "windModel"};
    return sw.contains(id);
}

int RysumProcessor::autoSteps(int i) const
{
    if (i < 0 || i >= (int)prmList.size() || isFloatParam[(size_t)i])
        return 0;
    return juce::jmax(2, prmList[(size_t)i]->getNumSteps());
}

void RysumProcessor::autoChanged()
{
    ++autoVersion;
    autoDirty = true;
}

void RysumProcessor::autoPublish()
{
    autoDirty = false;
    auto c = std::make_shared<AutoCompiled>();
    c->on = autoSet.on;
    c->clock = autoSet.clock;
    c->tempo = autoSet.tempo;
    c->startOnKey = autoSet.startOnKey;
    // per-pipe and path lanes: copied first, then the engine's view points into the copies
    c->spatial.reserve((size_t)kMaxAutoLanes);
    for (const auto &l : autoSet.lanes)
        if (l.on && (autoPipeTarget(l.paramId) >= 0 || autoPathStop(l.paramId) >= 0) &&
            !l.pts.empty() && (int)c->spatial.size() < kMaxAutoLanes)
            c->spatial.push_back(l);
    for (const auto &l : c->spatial)
    {
        if (const int t = autoPipeTarget(l.paramId); t >= 0)
            c->view.pipe[(size_t)c->view.numPipe++] = {&l, t};
        else if (const int s = autoPathStop(l.paramId); s >= 0 && c->view.path[(size_t)s] == nullptr)
            c->view.path[(size_t)s] = &l;
    }
    for (int li = 0; li < (int)autoSet.lanes.size(); ++li)
    {
        const auto &l = autoSet.lanes[(size_t)li];
        const int p = getParamIndex(l.paramId);
        if (p < 0 || !isAutoTarget(p))
            continue;
        AutoCompiled::Lane cl;
        cl.param = p;
        cl.steps = autoSteps(p);
        cl.index = li;
        cl.lane = l;
        c->lanes.push_back(std::move(cl));
    }
    // the audio thread may still read one of the earlier ones: they stay alive here a while
    autoKeep.push_back(c);
    while (autoKeep.size() > 32)
        autoKeep.pop_front();
    std::atomic_store(&autoPending, std::shared_ptr<const AutoCompiled>(c));
}

// audio thread, every slice: the curves into modVals (before the routings add their part)
void RysumProcessor::applyAutomation(const EngineParams &p)
{
    autoHits.clear();
    const auto *ac = autoActive.get();
    if (ac == nullptr || !ac->on)
        return;
    const bool rec = autoRec.load(std::memory_order_relaxed) && p.autoBeatRate > 0.0;
    for (const auto &L : ac->lanes)
    {
        if (!L.lane.on || L.param < 0)
            continue;
        const auto t = (size_t)L.param;
        if (autoTouched[t].load(std::memory_order_relaxed) != 0)
        {
            // a hand holds the control: the lane gives way; armed and recording: it listens
            if (rec && L.lane.armed)
            {
                int s1, n1, s2, n2;
                autoFifo.prepareToWrite(1, s1, n1, s2, n2);
                if (n1 > 0)
                {
                    autoFifoData[(size_t)s1] = {L.index, autoLocalTime(L.lane, p.autoBeats),
                                                prmList[t]->convertTo0to1(modVals[t])};
                    autoFifo.finishedWrite(1);
                }
            }
            continue;
        }
        if (autoFlag[t])
            continue; // two lanes on one parameter: the first one counts
        const float v = autoValue(L.lane, p.autoBeats, L.steps);
        if (std::isnan(v))
            continue;
        modVals[t] = prmList[t]->convertFrom0to1(v);
        autoNorm[t].store(v, std::memory_order_relaxed);
        autoFlag[t] = 1;
        autoHits.push_back((int)t);
    }
}

// message thread, 40 Hz: what the hands recorded goes into the lanes; changes go to the audio
void RysumProcessor::autoTick()
{
    const auto now = juce::Time::getMillisecondCounter();
    // recording: a control that is touched and has no lane yet gets one (armed, starting where the
    // control stood) - so recording is: press Aufnahme, start the clock, move what should move
    if (autoRec.load())
    {
        if (autoLaneMade.size() != prmList.size())
            autoLaneMade.assign(prmList.size(), 0);
        bool added = false;
        for (int i = 0; i < (int)prmList.size(); ++i)
        {
            if (autoTouched[(size_t)i].load(std::memory_order_relaxed) == 0)
            {
                autoLaneMade[(size_t)i] = 0;
                continue;
            }
            if (autoLaneMade[(size_t)i] != 0)
                continue;
            autoLaneMade[(size_t)i] = 1;
            if (!isAutoTarget(i) || (int)autoSet.lanes.size() >= kMaxAutoLanes)
                continue;
            const auto id = prmList[(size_t)i]->getParameterID();
            if (std::any_of(autoSet.lanes.begin(), autoSet.lanes.end(),
                            [&](const AutoLane &l) { return l.paramId == id; }))
                continue;
            AutoLane l;
            l.paramId = id;
            l.armed = true;
            AutoPoint a;
            a.t = 0.0;
            a.v = prmList[(size_t)i]->getValue();
            if (autoSteps(i) >= 2)
                a.shape = asStep;
            l.pts.push_back(a);
            autoSet.lanes.push_back(std::move(l));
            added = true;
        }
        if (added)
            autoChanged();
    }
    std::array<bool, kMaxAutoLanes> changed{};
    const int ready = autoFifo.getNumReady();
    if (ready > 0)
    {
        int s1, n1, s2, n2;
        autoFifo.prepareToRead(ready, s1, n1, s2, n2);
        auto take = [&](const AutoRecSample &s)
        {
            if (s.lane < 0 || s.lane >= (int)autoSet.lanes.size() || s.lane >= kMaxAutoLanes)
                return;
            auto &ps = autoPass[(size_t)s.lane];
            // a new pass: the first sample, or the time jumped (loop, seek)
            // (a lane read backwards runs backwards in its own time)
            const double dir = autoSet.lanes[(size_t)s.lane].reverse ? -1.0 : 1.0;
            const double step = (s.t - ps.tLast) * dir;
            if (!ps.active || step < -1.0e-6 || step > 4.0)
            {
                ps.active = true;
                ps.t0 = s.t;
                ps.raw.clear();
                ps.before = autoSet.lanes[(size_t)s.lane].pts;
            }
            AutoPoint pt;
            pt.t = s.t;
            pt.v = s.v;
            if (ps.raw.empty() || std::abs(s.t - ps.raw.back().t) >= 1.0 / 64.0)
                ps.raw.push_back(pt);
            else
                ps.raw.back().v = s.v; // (closer than 1/64 beat: the newest value)
            ps.tLast = s.t;
            ps.lastMs = now;
            changed[(size_t)s.lane] = true;
        };
        for (int i = 0; i < n1; ++i)
            take(autoFifoData[(size_t)(s1 + i)]);
        for (int i = 0; i < n2; ++i)
            take(autoFifoData[(size_t)(s2 + i)]);
        autoFifo.finishedRead(n1 + n2);
    }
    bool any = false;
    for (int li = 0; li < kMaxAutoLanes && li < (int)autoSet.lanes.size(); ++li)
    {
        auto &ps = autoPass[(size_t)li];
        if (changed[(size_t)li])
        {
            auto &lane = autoSet.lanes[(size_t)li];
            lane.pts = ps.before;
            auto raw = ps.raw;
            std::stable_sort(raw.begin(), raw.end(), [](const AutoPoint &a, const AutoPoint &b) { return a.t < b.t; });
            autoReplaceRange(lane, ps.t0, ps.tLast, autoThin(raw, 0.004f, 0.25));
            any = true;
        }
        else if (ps.active && now - ps.lastMs > 300)
            ps.active = false; // the hand let go
    }
    if (any)
        autoChanged();
    if (autoDirty)
        autoPublish();
}
