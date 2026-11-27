// Scenes (snapshots of the sound, blended with one control) and the dice (random settings) of
// RysumProcessor
#include "PluginProcessor.h"
#include <cstring>
#include <map>

using namespace rysum;

// ---------------------------------------------------------------------------
// scenes
// ---------------------------------------------------------------------------
bool RysumProcessor::takesPartInScenes(const juce::String &id) const
{
    // the player's setup and the output stay as they are; switching the tuning would cut the notes
    static const juce::StringArray never{
        "sceneMorph",    "output",  "master",   "binOrder", "binYaw", "binDfEq",  "trAngle",
        "trAmount",      "trBoost", "trSideEq", "irFuma",   "irN3D",  "irOrder",  "temperament",
        "mtsContinuous", "midiCC",  "mpe",      "mpeBend",  "inpOn",  "spkOrder", "spkDistComp",
        "simKbCh"}; // (the room keyboard's channel: a scene must not silence the keys)
    return !id.startsWith("dsf") && !never.contains(id);
}

void RysumProcessor::storeScene(int slot)
{
    if (slot < 0 || slot >= kNumScenes)
        return;
    auto &sc = scenes[(size_t)slot];
    sc.clear();
    for (int i = 0; i < (int)prmList.size(); ++i)
        if (takesPartInScenes(prmList[(size_t)i]->getParameterID()))
            sc.push_back({i, prmList[(size_t)i]->getValue()});
    ++sceneVersion;
    sceneVersionApplied = sceneVersion; // storing does not move anything
}

void RysumProcessor::clearScene(int slot)
{
    if (slot < 0 || slot >= kNumScenes)
        return;
    scenes[(size_t)slot].clear();
    ++sceneVersion;
    sceneVersionApplied = sceneVersion;
}

void RysumProcessor::goToScene(int slot)
{
    if (!isSceneStored(slot))
        return;
    if (auto *m = apvts.getParameter("sceneMorph"))
    {
        m->beginChangeGesture();
        m->setValueNotifyingHost(m->convertTo0to1((float)slot));
        m->endChangeGesture();
    }
    morphApplied = -1.0f; // apply even if the control was there already (the sound may have been
                          // changed since)
}

void RysumProcessor::applySceneMorph()
{
    auto *m = apvts.getParameter("sceneMorph");
    if (m == nullptr)
        return;
    // the position as modulated right now (an LFO can wander through the scenes)
    float x = m->convertFrom0to1(m->getValue());
    {
        const int idx = getParamIndex("sceneMorph");
        const float mn = getModulatedNorm(idx);
        if (!std::isnan(mn))
            x = m->convertFrom0to1(mn);
    }
    // only a moving control blends (between two moves the sound can be edited freely and stored)
    if (std::abs(x - morphApplied) < 1.0e-4f && sceneVersionApplied == sceneVersion)
        return;
    morphApplied = x;
    sceneVersionApplied = sceneVersion;

    // the stored scenes on either side of the position (empty slots are skipped)
    int left = -1, right = -1;
    for (int s = 0; s < kNumScenes; ++s)
    {
        if (!isSceneStored(s))
            continue;
        if ((float)s <= x + 1.0e-4f)
            left = s;
        if ((float)s >= x - 1.0e-4f && right < 0)
            right = s;
    }
    if (left < 0)
        left = right;
    if (right < 0)
        right = left;
    if (left < 0)
        return;
    const float t =
        right == left ? 0.0f : juce::jlimit(0.0f, 1.0f, (x - (float)left) / (float)(right - left));
    const auto &A = scenes[(size_t)left];
    const auto &B = scenes[(size_t)right];
    std::map<int, float> bv;
    for (auto &[i, v] : B)
        bv[i] = v;
    for (auto &[i, va] : A)
    {
        auto *prm = prmList[(size_t)i];
        const auto it = bv.find(i);
        const float vb = it != bv.end() ? it->second : va;
        // switches and choices change half way, everything else glides
        const bool discrete = prm->isDiscrete() || prm->isBoolean();
        const float v = discrete ? (t < 0.5f ? va : vb) : va + t * (vb - va);
        if (std::abs(prm->getValue() - v) > 1.0e-5f)
            prm->setValueNotifyingHost(v);
    }
}

juce::ValueTree RysumProcessor::scenesState() const
{
    juce::ValueTree t("SCENES");
    for (int s = 0; s < kNumScenes; ++s)
    {
        if (!isSceneStored(s))
            continue;
        juce::ValueTree sc("SCENE");
        sc.setProperty("slot", s, nullptr);
        juce::StringArray kv;
        for (auto &[i, v] : scenes[(size_t)s])
            kv.add(prmList[(size_t)i]->getParameterID() + "=" + juce::String(v, 6));
        sc.setProperty("values", kv.joinIntoString(";"), nullptr);
        t.appendChild(sc, nullptr);
    }
    return t;
}

void RysumProcessor::applyScenesState(const juce::ValueTree &t)
{
    for (auto &sc : scenes)
        sc.clear();
    if (t.isValid())
        for (auto sc : t)
        {
            const int s = (int)sc.getProperty("slot", -1);
            if (s < 0 || s >= kNumScenes)
                continue;
            juce::StringArray kv;
            kv.addTokens(sc["values"].toString(), ";", {});
            for (auto &e : kv)
            {
                const int idx = getParamIndex(e.upToFirstOccurrenceOf("=", false, false));
                if (idx >= 0 && takesPartInScenes(prmList[(size_t)idx]->getParameterID()))
                    scenes[(size_t)s].push_back(
                        {idx,
                         juce::jlimit(0.0f, 1.0f,
                                      e.fromFirstOccurrenceOf("=", false, false).getFloatValue())});
            }
        }
    ++sceneVersion;
    // a loaded project / patch sounds as saved: nothing is blended until the control moves
    sceneVersionApplied = sceneVersion;
    if (auto *m = apvts.getParameter("sceneMorph"))
        morphApplied = m->convertFrom0to1(m->getValue());
}

// ---------------------------------------------------------------------------
// dice
// ---------------------------------------------------------------------------
juce::StringArray RysumProcessor::diceGroupNames()
{
    juce::StringArray a;
    for (auto *n :
         {"Register", "Raum (Bewegung, Entfernung)", "Saal", "Klang (Tremulant, Freeze, Wind)",
          "Traktur und Verfall", "Effekte", "Modulation (LFOs, H\xc3\xbcllkurven, Mengen)",
          "Lucier", "Eingang (Ort, Bewegung)"})
        a.add(juce::String::fromUTF8(n));
    return a;
}

int RysumProcessor::diceGroupOf(const juce::String &id) const
{
    auto stopParam = [&](const char *prefix)
    {
        return id.startsWith(prefix) &&
               id.substring((int)std::strlen(prefix)).containsOnly("0123456789");
    };
    if (stopParam("stop") || stopParam("vol") || stopParam("swind"))
        return dgRegister;
    for (auto *p :
         {"motion",    "rate",         "depth",      "ellipse",    "ellDir",    "ellTilt",
          "ellForm",   "pathSpread",   "jumpHard",   "cloudMix",   "cloudDens", "cloudSpread",
          "cloudSize", "cloudScatter", "pipeAtt",    "pipeSharp",  "pipeIso",   "pipeRep",
          "pipePuff",  "pipeHiFreq",   "pipeHiGain", "pipeHiShape", "pipeWidth", "pipeDirPat",
          "pipeFace",  "pipeSpin"})
        if (stopParam(p))
            return dgRaum;
    if (id == "doppler" || id.startsWith("dop") || id == "air" || id == "airInvert" || id == "distAmount" ||
        id == "irDry" || id == "irWet" || id == "irEnvelop" || id == "irDirect")
        return dgRaum;
    // (not the room keyboard's channel - on a channel the organ plays on, its keys fall silent -
    // nor where the doors lead)
    if (id == "simKbCh" || id.endsWith("DoorTo") || id.endsWith("DoorWall"))
        return -1;
    // (the 3D Saal's look makes no sound: the dice leave it)
    if (id.startsWith("look"))
        return -1;
    // (nor whether a Saal is open to the sky)
    if (id.endsWith("Open") || id.endsWith("OpenAmt"))
        return -1;
    if (id.startsWith("choreo"))
        return -1;
    if (id.startsWith("sim") && id != "simOrder")
        return dgSaal;
    if (id == "detune" || id.startsWith("trem") || id == "freezeSmear" || id == "freezeRelease" ||
        id == "windModel")
        return dgKlang;
    if (id == "click" || id == "clickBlend" || id == "releaseClick" || id == "stopPull" ||
        id == "wind" || id.startsWith("deg") || id.startsWith("spc") || id.startsWith("clickRes"))
        return dgSpieltisch;
    if (id.startsWith("fx"))
        return dgEffekte;
    if (id.startsWith("lfo") || id.startsWith("env") ||
        (id.startsWith("mod") && id.endsWith("Amt")))
        return dgModulation;
    if (id.startsWith("luc") && id != "lucSource")
        return dgLucier;
    for (auto *p : {"inpAz", "inpEl", "inpDist", "inpWidth", "inpSpin", "inpFollow", "inpSend"})
        if (id == p)
            return dgEingang;
    return -1; // never: master, output, tuning, freeze, MIDI, scenes ...
}

void RysumProcessor::dice(bool wild)
{
    diceUndo = makeState(true);
    auto &rng = diceRng;
    auto setNorm = [](juce::RangedAudioParameter *p, float v)
    {
        v = juce::jlimit(0.0f, 1.0f, v);
        p->beginChangeGesture();
        p->setValueNotifyingHost(v);
        p->endChangeGesture();
    };
    auto setReal = [&](juce::RangedAudioParameter *p, float v)
    {
        setNorm(p, p->convertTo0to1(juce::jlimit(p->getNormalisableRange().start,
                                                 p->getNormalisableRange().end, v)));
    };
    auto gauss = [&]
    {
        return (rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 1.5f) * 1.41f;
    }; // ~ unit variance
    int stopsOn = 0;
    for (auto *prm : prmList)
    {
        const auto id = prm->getParameterID();
        const int g = diceGroupOf(id);
        if (g < 0 || !diceGroups[(size_t)g])
            continue;
        const bool discrete = prm->isDiscrete() || prm->isBoolean();
        const int steps = juce::jmax(2, prm->getNumSteps());
        const float now = prm->getValue();
        // routings that do nothing are left alone
        if (id.startsWith("mod") && id.endsWith("Amt"))
        {
            const int r = id.substring(3).getIntValue() - 1;
            if (r < 0 || r >= kNumRoutes || routeSrc[(size_t)r].load() < 0 ||
                routeTgt[(size_t)r].load() < 0)
                continue;
        }
        if (!wild)
        {
            // gently: small steps, a few switches
            if (discrete)
            {
                if (rng.nextFloat() < 0.12f)
                    setNorm(prm, (float)rng.nextInt(steps) / (float)(steps - 1));
            }
            else
                setNorm(prm, now + 0.07f * gauss());
            continue;
        }
        // wildly - but still playable
        const bool isStop = id.startsWith("stop") && id.substring(4).containsOnly("0123456789");
        if (isStop)
        {
            setNorm(prm, rng.nextFloat() < 0.45f ? 1.0f : 0.0f);
            continue;
        }
        if (id.startsWith("vol") && id.substring(3).containsOnly("0123456789"))
        {
            setReal(prm, -12.0f + 18.0f * rng.nextFloat());
            continue;
        }
        if (id.startsWith("swind"))
        {
            setReal(prm, 0.3f + 0.7f * rng.nextFloat());
            continue;
        }
        if (id.startsWith("rate") && id.substring(4).containsOnly("0123456789"))
        {
            setReal(prm, (rng.nextBool() ? 1.0f : -1.0f) * 1.5f * std::pow(rng.nextFloat(), 2.0f));
            continue;
        }
        if (id.startsWith("motion") && id.substring(6).containsOnly("0123456789"))
        {
            const int s = id.substring(6).getIntValue();
            const int n =
                (int)Motion::Drawn +
                (getDrawnPath(s).points.size() > 1 ? 1 : 0); // drawn only where a path was drawn
            setReal(prm, (float)rng.nextInt(n));
            continue;
        }
        if (id == "detune")
        {
            setNorm(prm, 0.35f * rng.nextFloat());
            continue;
        }
        if (id == "inpSpin")
        {
            setReal(prm, (rng.nextBool() ? 1.0f : -1.0f) * 90.0f * std::pow(rng.nextFloat(), 2.0f));
            continue;
        }
        if (id == "inpDist")
        {
            setReal(prm, 1.5f + 12.0f * rng.nextFloat());
            continue;
        }
        if (id == "inpSend")
        {
            setReal(prm, -12.0f + 12.0f * rng.nextFloat());
            continue;
        }
        if (id == "windModel" || id == "tremOn" || id == "fxPathWait" || id == "fxWindRound" ||
            id == "fxEchoSync")
        {
            setNorm(prm, rng.nextFloat() < 0.3f ? 1.0f : 0.0f);
            continue;
        }
        if (id == "simOn" || id == "lucOn")
        {
            setNorm(prm, rng.nextBool() ? 1.0f : 0.0f);
            continue;
        }
        if (id == "lucGen")
        {
            setReal(prm, 1.0f + 19.0f * rng.nextFloat());
            continue;
        }
        if (id == "lucMix")
        {
            setReal(prm, 0.3f + 0.6f * rng.nextFloat());
            continue;
        }
        if (id == "lucGrow")
        {
            setReal(prm, rng.nextFloat() < 0.7f ? 0.0f : 3.0f * rng.nextFloat());
            continue;
        }
        if (id.endsWith("Level") || id == "degAmount")
        {
            setNorm(prm, rng.nextFloat() < 0.5f ? 0.0f : 0.7f * rng.nextFloat());
            continue;
        }
        if (id.startsWith("mod") && id.endsWith("Amt"))
        {
            setNorm(prm, 0.5f + 0.5f * (rng.nextFloat() - 0.5f));
            continue;
        }
        if (discrete)
            setNorm(prm, (float)rng.nextInt(steps) / (float)(steps - 1));
        else
            setNorm(prm, rng.nextFloat());
    }
    // at least one stop pulled
    if (diceGroups[dgRegister])
    {
        for (int s = 0; s < kNumStops; ++s)
            stopsOn += apvts.getRawParameterValue(stopParamId(s))->load() > 0.5f;
        if (stopsOn == 0)
            if (auto *p = apvts.getParameter(stopParamId(rng.nextInt(kNumStops))))
                setNorm(p, 1.0f);
    }
    ++changeCounter;
}

void RysumProcessor::undoDice()
{
    if (!diceUndo.isValid())
        return;
    auto s = diceUndo;
    diceUndo = {};
    applyState(s, true);
}
