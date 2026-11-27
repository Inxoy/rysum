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
    if (id == "choreoGo" || id == "choreoPattern")
        return true; // (a lane sets the figure off: bounces dance it the same)
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
    c->cycle = autoSet.cycle && autoSet.cycleEnd > autoSet.cycleStart + 0.2;
    c->cycleStart = autoSet.cycleStart;
    c->cycleEnd = autoSet.cycleEnd;
    // solo: while any lane is solo, only the solo lanes act (the tempo lane always does)
    const bool anySolo = std::any_of(autoSet.lanes.begin(), autoSet.lanes.end(),
                                     [](const AutoLane &l) { return l.on && l.solo; });
    auto acts = [anySolo](const AutoLane &l) { return l.on && (!anySolo || l.solo); };
    for (const auto &l : autoSet.lanes)
        if (l.on && l.paramId == kAutoTempoId && !l.pts.empty())
        {
            c->hasTempo = true;
            c->tempoLane = l;
            break;
        }
    // per-pipe and path lanes: copied first, then the engine's view points into the copies
    c->spatial.reserve((size_t)kMaxAutoLanes);
    for (const auto &l : autoSet.lanes)
        if (acts(l) && (autoPipeTarget(l.paramId) >= 0 || autoPathStop(l.paramId) >= 0) &&
            !l.pts.empty() && (int)c->spatial.size() < kMaxAutoLanes)
            c->spatial.push_back(l);
    // lanes "mit jedem Anschlag neu" on a stop's control with a per-pipe target: per pipe, in the engine
    auto noteMapped = [this](const AutoLane &l, int &tgt, int &stop, bool &lg, float &sc)
    {
        return l.perNote && !l.pts.empty() && getParamIndex(l.paramId) >= 0 && autoNoteMapping(l.paramId, tgt, stop, lg, sc);
    };
    for (const auto &l : autoSet.lanes)
    {
        int tg, st; bool lg; float sc;
        if (acts(l) && noteMapped(l, tg, st, lg, sc) && (int)c->spatial.size() < kMaxAutoLanes)
            c->spatial.push_back(l);
    }
    for (const auto &l : c->spatial)
    {
        int tg, st; bool lg; float sc;
        if (const int t = autoPipeTarget(l.paramId); t >= 0)
            c->view.pipe[(size_t)c->view.numPipe++] = {&l, t};
        else if (const int s = autoPathStop(l.paramId); s >= 0 && c->view.path[(size_t)s] == nullptr)
            c->view.path[(size_t)s] = &l;
        else if (noteMapped(l, tg, st, lg, sc) && c->view.numNote < kMaxAutoLanes)
        {
            auto &nl = c->view.note[(size_t)c->view.numNote++];
            nl.lane = &l;
            nl.target = tg;
            nl.stop = st;
            nl.logRatio = lg;
            nl.scale = sc;
            nl.param = getParamIndex(l.paramId);
            nl.range = prmList[(size_t)nl.param]->getNormalisableRange();
        }
    }
    for (int li = 0; li < (int)autoSet.lanes.size(); ++li)
    {
        const auto &l = autoSet.lanes[(size_t)li];
        const int p = getParamIndex(l.paramId);
        if (p < 0 || !isAutoTarget(p) || (anySolo && !l.solo))
            continue;
        AutoCompiled::Lane cl;
        cl.param = p;
        cl.steps = autoSteps(p);
        cl.index = li;
        cl.lane = l;
        {
            int tg, st; bool lg; float sc;
            cl.perPipe = noteMapped(l, tg, st, lg, sc);
        }
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
    autoSinceKey.store(p.noteClock - p.lastKeyClock, std::memory_order_relaxed);
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
        if (L.perPipe)
            continue; // (per note, per pipe: the engine plays it - the control keeps its own value)
        // ("mit jedem Anschlag neu": one value for the whole instrument - from the last key pressed)
        const float v = autoValue(L.lane, L.lane.perNote ? p.noteClock - p.lastKeyClock : p.autoBeats, L.steps);
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

// ---------------------------------------------------------------------------
// pieces: the Verlauf as a file of its own (Documents/Rysum/Stuecke/<name>.rysumstueck)
// ---------------------------------------------------------------------------
namespace
{
juce::ValueTree pathTree(int stop, const RysumProcessor::DrawnPath &dp)
{
    juce::ValueTree p("PATH");
    p.setProperty("stop", stop, nullptr);
    juce::StringArray pts;
    for (auto &q : dp.points)
        pts.add(juce::String(q.az, 2) + " " + juce::String(q.el, 2) + " " + juce::String(q.t, 3));
    p.setProperty("points", pts.joinIntoString(","), nullptr);
    p.setProperty("pingPong", dp.pingPong, nullptr);
    p.setProperty("timing", dp.drawnTiming, nullptr);
    return p;
}
} // namespace

juce::File RysumProcessor::getPieceFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Rysum")
        .getChildFile(juce::String::fromUTF8("St\xc3\xbc"
                                             "cke"));
}

juce::Array<juce::File> RysumProcessor::getPieceFiles()
{
    auto files = getPieceFolder().findChildFiles(juce::File::findFiles, false,
                                                 juce::String("*") + kPieceExtension);
    files.sort();
    return files;
}

juce::ValueTree RysumProcessor::pieceTree() const
{
    juce::ValueTree t("RYSUMSTUECK");
    t.setProperty("version", 1, nullptr);
    t.setProperty("name", pieceName, nullptr);
    t.appendChild(autoToTree(autoSet), nullptr);
    // the drawn paths its timetables ride on (without them a timetable has nowhere to go)
    for (const auto &l : autoSet.lanes)
        if (const int s = autoPathStop(l.paramId); s >= 0)
            t.appendChild(pathTree(s, drawnPaths[(size_t)s]), nullptr);
    return t;
}

bool RysumProcessor::savePiece(const juce::File &f, juce::String &error)
{
    const auto file = f.hasFileExtension(kPieceExtension) ? f : f.withFileExtension(kPieceExtension);
    if (!file.getParentDirectory().createDirectory())
    {
        error = juce::String::fromUTF8("Ordner l\xc3\xa4sst sich nicht anlegen: ") +
                file.getParentDirectory().getFullPathName();
        return false;
    }
    const auto keepName = pieceName;
    pieceName = file.getFileNameWithoutExtension();
    auto xml = pieceTree().createXml();
    if (xml == nullptr || !xml->writeTo(file))
    {
        pieceName = keepName;
        error = juce::String::fromUTF8("Konnte nicht schreiben: ") + file.getFullPathName();
        return false;
    }
    pieceFile = file;
    pieceSavedVersion = autoVersion.load();
    return true;
}

void RysumProcessor::applyPieceTree(const juce::ValueTree &t, const juce::String &name, const juce::File &file)
{
    autoSet = autoFromTree(t.getChildWithName("VERLAUF"));
    for (const auto &p : t)
    {
        if (!p.hasType("PATH"))
            continue;
        const int s = (int)p.getProperty("stop", -1);
        if (s < 0 || s >= kNumStops)
            continue;
        std::vector<DrawnPath::Pt> pts;
        juce::StringArray items;
        items.addTokens(p["points"].toString(), ",", {});
        for (auto &it : items)
        {
            juce::StringArray v;
            v.addTokens(it, " ", {});
            if (v.size() >= 3)
                pts.push_back({v[0].getFloatValue(), v[1].getFloatValue(), v[2].getFloatValue()});
        }
        if (pts.size() < 2)
            continue;
        setDrawnPath(s, std::move(pts));
        setPathOptions(s, (bool)p.getProperty("pingPong", false), (bool)p.getProperty("timing", false));
        // (a timetable moves the stop along its drawn path: the stop's motion is "Gezeichnet")
        if (auto *m = apvts.getParameter(motionParamId(s)))
            m->setValueNotifyingHost(m->convertTo0to1((float)Motion::Drawn));
    }
    pieceName = name;
    pieceFile = file;
    setAutoRecord(false);
    autoPlay(false);
    autoSeek(0.0);
    autoChanged();
    autoPublish();
    pieceSavedVersion = autoVersion.load();
}

bool RysumProcessor::loadPiece(const juce::File &f, juce::String &error)
{
    auto xml = juce::XmlDocument::parse(f);
    if (xml == nullptr || !xml->hasTagName("RYSUMSTUECK"))
    {
        error = juce::String::fromUTF8("Kein Rysum-St\xc3\xbc"
                                       "ck: ") +
                f.getFileName();
        return false;
    }
    applyPieceTree(juce::ValueTree::fromXml(*xml), f.getFileNameWithoutExtension(), f);
    return true;
}

void RysumProcessor::newPiece()
{
    juce::ValueTree t("RYSUMSTUECK");
    AutoSet s;
    s.clock = autoSet.clock;
    s.tempo = autoSet.tempo;
    t.appendChild(autoToTree(s), nullptr);
    applyPieceTree(t, {}, {});
}

// ---------------------------------------------------------------------------
// the pieces that come with Rysum (they show what the page can do; "Speichern unter" makes one
// one's own)
// ---------------------------------------------------------------------------
juce::StringArray RysumProcessor::factoryPieceNames()
{
    return {"Atem", "Welle durch den Raum", "Registerkanon", "Verfall", "Schwebung", "Fahrt"};
}

void RysumProcessor::loadFactoryPiece(int index)
{
    index = juce::jlimit(0, factoryPieceNames().size() - 1, index);
    // values are given in the parameter's own unit and stored normalised
    auto nv = [this](const juce::String &id, float real)
    {
        if (auto *p = apvts.getParameter(id))
            return juce::jlimit(0.0f, 1.0f, p->convertTo0to1(real));
        return 0.5f;
    };
    auto pt = [](double t, float v, int shape = asCurve, float bend = 0.0f, float rate = 4.0f,
                 float depth = 0.2f)
    {
        AutoPoint p;
        p.t = t;
        p.v = v;
        p.shape = shape;
        p.bend = bend;
        p.rate = rate;
        p.depth = depth;
        return p;
    };
    auto lane = [](const juce::String &id, std::vector<AutoPoint> pts, double loopLen = 0.0)
    {
        AutoLane l;
        l.paramId = id;
        l.pts = std::move(pts);
        if (loopLen > 0.0)
        {
            l.loop = true;
            l.length = loopLen;
        }
        return l;
    };
    AutoSet s;
    s.clock = 1;         // the own clock ...
    s.tempo = 60.0;
    s.startOnKey = true; // ... started by the first key
    s.grid = 1.0;
    s.viewLen = 48.0;
    juce::ValueTree t("RYSUMSTUECK");
    switch (index)
    {
    case 0:
        // "Atem": the stops breathe, each at its own pace (12, 16, 20 beats) - their pipes' light
        // swells and fades with the wind
        s.lanes.push_back(lane(windParamId(0), {pt(0, 1.0f, asCurve, 0.5f), pt(8, 0.35f, asCurve, -0.5f), pt(16, 1.0f)}, 16.0));
        s.lanes.push_back(lane(windParamId(1), {pt(0, 0.4f, asCurve, 0.5f), pt(6, 1.0f, asCurve, -0.4f), pt(12, 0.4f)}, 12.0));
        s.lanes.push_back(lane(windParamId(2), {pt(0, 0.8f), pt(10, 0.3f, asCurve, 0.3f), pt(20, 0.8f)}, 20.0));
        s.lanes.push_back(lane("wind", {pt(0, 0.08f, asJitter, 0.0f, 0.5f, 0.06f), pt(32, 0.22f, asJitter, 0.0f, 0.5f, 0.06f), pt(64, 0.08f)}, 64.0));
        break;
    case 1:
    {
        // "Welle durch den Raum": a swell of brightness runs from the front through the room every
        // 8 beats, and a wave of loudness comes back from far away, slower
        AutoLane b = lane(pipeTargetId(tgtBright), {pt(0, 0.5f), pt(1, 0.92f, asCurve, -0.6f), pt(5, 0.5f), pt(8, 0.5f)}, 8.0);
        b.spread = true;
        b.oy = 6.0f;
        b.waveSpeed = 3.0f;
        s.lanes.push_back(b);
        AutoLane v = lane(pipeTargetId(tgtLevel), {pt(0, 0.5f), pt(2, 0.62f, asWave, 0.0f, 1.0f, 0.05f), pt(10, 0.42f), pt(16, 0.5f)}, 16.0);
        v.spread = true;
        v.inward = true;
        v.waveSpeed = 1.5f;
        s.lanes.push_back(v);
        s.lanes.push_back(lane("distAmount", {pt(0, 0.6f), pt(24, 0.9f, asCurve, 0.4f), pt(48, 0.6f)}, 48.0));
        break;
    }
    case 2:
    {
        // "Registerkanon": the stops come and go in loops of different lengths (12 .. 36 beats) -
        // the registration never comes round the same way
        auto onOff = [&](int stop, double len, std::vector<std::pair<double, int>> steps)
        {
            std::vector<AutoPoint> p;
            for (auto &st : steps)
                p.push_back(pt(st.first, (float)st.second, asStep));
            s.lanes.push_back(lane(stopParamId(stop), p, len));
        };
        onOff(1, 12.0, {{0, 1}, {6, 0}});
        onOff(2, 16.0, {{0, 0}, {4, 1}, {12, 0}});
        onOff(3, 20.0, {{0, 0}, {10, 1}, {16, 0}});
        onOff(4, 28.0, {{0, 0}, {14, 1}, {22, 0}});
        onOff(6, 36.0, {{0, 0}, {24, 1}, {30, 0}});
        s.viewLen = 72.0;
        break;
    }
    case 3:
        // "Verfall": over 64 beats the organ falls apart - the sound crumbles, the pipes go out of
        // tune, the wind gets unsteady; then it stays there
        s.lanes.push_back(lane("degAmount", {pt(0, 0.0f, asCurve, -0.6f), pt(48, 0.75f, asJitter, 0.0f, 0.25f, 0.08f), pt(64, 0.8f)}));
        s.lanes.push_back(lane("detune", {pt(0, nv("detune", 0.0f), asCurve, -0.5f), pt(64, nv("detune", 28.0f))}));
        s.lanes.push_back(lane("wind", {pt(0, 0.0f), pt(32, 0.2f, asCurve, -0.3f), pt(64, 0.55f)}));
        s.lanes.push_back(lane(windParamId(0), {pt(0, 1.0f), pt(40, 0.95f, asJitter, 0.0f, 0.6f, 0.15f), pt(64, 0.6f)}));
        s.viewLen = 68.0;
        break;
    case 4:
        // "Schwebung": the tremulant comes in, its depth swells and ebbs, its speed drifts, and the
        // pipes slowly beat against each other
        s.lanes.push_back(lane("tremOn", {pt(0, 0.0f, asStep), pt(4, 1.0f, asStep)}));
        s.lanes.push_back(lane("tremDepth", {pt(0, 0.15f, asWave, 0.0f, 0.125f, 0.25f), pt(32, 0.55f, asWave, 0.0f, 0.125f, 0.25f), pt(64, 0.15f)}, 64.0));
        s.lanes.push_back(lane("tremRate", {pt(0, nv("tremRate", 3.0f), asCurve, 0.4f), pt(12, nv("tremRate", 7.0f), asCurve, -0.4f), pt(24, nv("tremRate", 3.0f))}, 24.0));
        s.lanes.push_back(lane("detune", {pt(0, nv("detune", 4.0f), asJitter, 0.0f, 0.2f, 0.15f), pt(40, nv("detune", 14.0f), asJitter, 0.0f, 0.2f, 0.15f), pt(80, nv("detune", 4.0f))}, 80.0));
        break;
    default:
    {
        // "Fahrt": the Praestant travels an arc in front of the listener on a timetable - there in
        // 6 beats, a rest, slowly on to the end, back - louder and brighter as it comes near
        DrawnPath arc;
        for (int i = 0; i <= 24; ++i)
        {
            const float u = (float)i / 24.0f;
            arc.points.push_back({-110.0f + 220.0f * u, 10.0f + 30.0f * std::sin(u * juce::MathConstants<float>::pi), u * 4.0f});
        }
        t.appendChild(pathTree(0, arc), nullptr);
        s.lanes.push_back(lane(autoPathLaneId(0), {pt(0, 0.0f, asCurve, 0.6f), pt(6, 0.5f, asStep), pt(10, 0.5f, asCurve, -0.3f), pt(20, 1.0f), pt(24, 1.0f, asCurve, 0.3f), pt(32, 0.0f)}, 32.0));
        s.lanes.push_back(lane(volParamId(0), {pt(0, nv(volParamId(0), -10.0f)), pt(10, nv(volParamId(0), 0.0f), asCurve, 0.4f), pt(20, nv(volParamId(0), -6.0f)), pt(32, nv(volParamId(0), -10.0f))}, 32.0));
        s.lanes.push_back(lane(pipeTargetId(tgtBright), {pt(0, 0.4f), pt(10, 0.7f, asCurve, 0.3f), pt(32, 0.4f)}, 32.0));
        break;
    }
    }
    t.appendChild(autoToTree(s), nullptr);
    // (a piece that comes with Rysum has no file yet: "Speichern" asks where to keep it)
    applyPieceTree(t, factoryPieceNames()[index], {});
}

// Konzert, the listener (audio thread): the player's clocks. Motion and LFO phases are pulled
// towards the player's (set outright when far off); the Verlauf runs on the player's beats.
void RysumProcessor::concertClocks(const double *motion, const double *lfo, double beats,
                                   bool running) noexcept
{
    engine.syncPhases(motion, lfo);
    const double err = beats - autoOwnBeats;
    autoOwnBeats = std::abs(err) > 0.25 ? beats : autoOwnBeats + 0.3 * err;
    autoRun.store(running);
}
