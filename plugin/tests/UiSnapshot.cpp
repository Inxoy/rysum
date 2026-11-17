#include <functional>
#include <thread>
// Renders the three views of the editor into PNG files (for checking the UI without a host).
//   RysumUiSnapshot <samples folder> <output folder>
#include "ConsoleView.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SpaceView.h"
#include "WalkView.h"
#include "VerlaufView.h"
#include "RoomModels.h"

class SnapshotApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override
    {
        return "RysumUiSnapshot";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1";
    }

    void initialise(const juce::String &cmd) override
    {
        auto args = juce::StringArray::fromTokens(cmd, true);
        if (args.size() < 2)
        {
            std::cout << "usage: RysumUiSnapshot <samples> <outdir>" << std::endl;
            quit();
            return;
        }
        samples = juce::File::getCurrentWorkingDirectory().getChildFile(args[0].unquoted());
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile(args[1].unquoted());
        outDir.createDirectory();
        proc = std::make_unique<RysumProcessor>();
        proc->loadSamples(samples);
        startTimer(100);
    }

    /** RYSUM_ONLY_WALK: only the 3D view ("Begehen"), in several rooms and from several places
        (RYSUM_WALK_OBJ = a model to load as well) */
    void walkShots()
    {
        proc->setRateAndBufferSizeDetails(48000.0, 512);
        proc->prepareToPlay(48000.0, 512);
        auto setReal = [&](const juce::String &id, float v)
        {
            if (auto *q = proc->apvts.getParameter(id))
                q->setValueNotifyingHost(q->convertTo0to1(v));
        };
        juce::AudioBuffer<float> st(
            juce::jmax(2, proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()),
            512);
        auto run = [&](int blocks, std::initializer_list<int> on)
        {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 0)
                    for (int n : on)
                        mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                st.clear();
                proc->processBlock(st, mb);
            }
        };
        setReal("simOn", 1.0f);
        juce::SharedResourcePointer<rysum::ui::Assets> assets; // (fonts: alive while an editor is)
        juce::SharedResourcePointer<rysum::ui::LookAndFeel> lnf;
        rysum::ui::WalkView wv(*proc);
        wv.setLookAndFeel(&lnf.get());
        wv.setSize(1280, 760);
        wv.setTestMode(true);
        wv.setVisible(true);
        auto shot = [&](const juce::String &name)
        {
            auto img = wv.createComponentSnapshot(wv.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile(name);
            f.deleteFile();
            juce::FileOutputStream os(f);
            png.writeImageToStream(img, os);
            std::cout << "wrote " << f.getFullPathName() << "  (" << wv.renderStats() << ")"
                      << std::endl;
        };
        struct View
        {
            float x, y, yaw, pitch;
        };
        const View views[]{{0, 0, 0, 0},     {0, 0, 0, -35},   {0, 0, 0, 40},    {3, -6, 60, -10},
                           {-4, 5, -120, 5}, {5, 8, 90, -50},  {-5, -9, 180, 30}};
        const int shapes[]{0, 1, 6, 7, 5, 2};
        const auto only = juce::SystemStats::getEnvironmentVariable("RYSUM_WALK_SHAPE", {});
        for (int sh : shapes)
        {
            if (only.isNotEmpty() && only.getIntValue() != sh)
                continue;
            setReal(RysumProcessor::roomParamId(0, "Shape"), (float)sh);
            run(20, sh == shapes[0] ? std::initializer_list<int>{48, 55, 60, 64, 67}
                                    : std::initializer_list<int>{});
            int i = 0;
            for (const auto &v : views)
            {
                wv.setPoseForTest(v.x, v.y, v.yaw);
                wv.setPitchForTest(v.pitch);
                run(4, {});
                shot("walk_s" + juce::String(sh) + "_" + juce::String(i++) + ".png");
            }
        }
        const auto obj = juce::SystemStats::getEnvironmentVariable("RYSUM_WALK_OBJ", {});
        if (obj.isNotEmpty())
        {
            setReal(RysumProcessor::roomParamId(0, "Shape"), 0.0f);
            auto objFile = obj == "test" ? outDir.getChildFile("test_kapelle.obj") : juce::File(obj);
            if (obj == "test")
            {
                objFile.replaceWithText(rysum::makeTestObj());
                proc->walkZUp = true;
            }
            wv.loadModelForTest(objFile);
            run(20, {});
            int i = 0;
            for (const auto &v : views)
            {
                wv.setPoseForTest(v.x, v.y, v.yaw);
                wv.setPitchForTest(v.pitch);
                run(4, {});
                shot("walk_obj_" + juce::String(i++) + ".png");
            }
        }
        if (std::getenv("RYSUM_WALK_NOCHECKS") == nullptr)
            walkChecks(wv, setReal, run, shot);
        std::cout << "walk: done" << std::endl;
        wv.setLookAndFeel(nullptr);
        std::cout << "walk: look reset" << std::endl;
    }

    /** the walk itself: smooth walking, a jump, materials of single surfaces, a model that stops the
        walker and hides pipes, the player pulling the pipes */
    template <typename SetReal, typename Run, typename Shot>
    void walkChecks(rysum::ui::WalkView &wv, SetReal &&setReal, Run &&run, Shot &&shot)
    {
        using namespace rysum;
        auto say = [](const juce::String &what, bool ok)
        { std::cout << what << (ok ? "  ok" : "  FAIL") << std::endl; };
        auto roomOf = [&]
        {
            RoomParams r = proc->readParams().roomAt(0);
            finishPlan(r, proc->getDrawnPlan(0));
            return r;
        };
        proc->clearWalkModel();
        setReal(RysumProcessor::roomParamId(0, "Shape"), 0.0f);
        run(10, {});
        // ---- smooth walking: the speed builds up, and dies away after the key is let go
        wv.setPoseForTest(0, 0, 0);
        const float y0 = proc->walkPose[1].load();
        for (int i = 0; i < 6; ++i)
            wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
        const float yEarly = proc->walkPose[1].load() - y0;
        for (int i = 0; i < 54; ++i)
            wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
        const float y1 = proc->walkPose[1].load() - y0;
        for (int i = 0; i < 60; ++i)
            wv.advanceForTest(1.0 / 60.0, false, false, false, false, false);
        const float y2 = proc->walkPose[1].load() - y0;
        say("walk: 0.1 s " + juce::String(yEarly, 3) + " m (at once it would be 0.16), 1 s " +
                juce::String(y1, 2) + " m, coasting on " + juce::String(y2 - y1, 2) + " m",
            yEarly > 0.01f && yEarly < 0.1f && y1 > 1.2f && y1 < 1.6f && y2 - y1 > 0.05f &&
                y2 - y1 < 0.5f);
        // ---- a jump: up, and down again
        float top = 0.0f;
        wv.advanceForTest(1.0 / 60.0, false, false, false, false, true);
        int frames = 1;
        for (; frames < 240; ++frames)
        {
            wv.advanceForTest(1.0 / 60.0, false, false, false, false, false);
            top = juce::jmax(top, wv.heightForTest());
            if (frames > 3 && wv.heightForTest() <= 0.0f)
                break;
        }
        say("jump: " + juce::String(top, 2) + " m high, back on the floor after " +
                juce::String(frames / 60.0, 2) + " s, the engine's listener at " +
                juce::String(proc->engine.viewZ.load(), 2) + " m",
            top > 0.6f && top < 1.2f && frames < 80 && wv.heightForTest() == 0.0f &&
                proc->engine.viewZ.load() == 0.0f);
        // ---- a material for one wall: point at the right wall, M -> stone ... ; the reverb follows
        wv.setPoseForTest(0, 0, -90);
        run(4, {});
        shot("walk_mat_0.png");
        const int aim = wv.aimForTest();
        wv.cycleMaterialForTest();
        const int m1 = proc->getSurfaceMaterial(0, aim);
        for (int i = 0; i < 3; ++i)
            wv.cycleMaterialForTest(); // -> glass
        const int m2 = proc->getSurfaceMaterial(0, aim);
        say("material: the cross points at surface " + juce::String(aim) + " (right wall = 3), M: " +
                juce::String(m1) + ", then " + juce::String(m2) + " (" +
                juce::String(surfMaterial(m2).name) + ")",
            aim == kSurfWall0 + 1 && m1 == 0 && m2 == 3);
        proc->setSurfaceMaterial(0, kSurfFloor, 2);        // a wooden floor
        proc->setSurfaceMaterial(0, kSurfWall0 + 2, 5);    // a mirror in front
        run(6, {});
        wv.setPoseForTest(0, 0, -50);
        wv.setPitchForTest(-8);
        run(4, {});
        shot("walk_mat_1.png");
        {
            // the decay: all hard against one wall a curtain / all walls curtains
            RoomParams r = roomOf();
            auto rt = [&](std::array<int8_t, kMaxSurf> surf)
            {
                r.plan.surf = surf;
                return traceRoom(r, 0.3f, 1500).rtLow;
            };
            auto all = FloorPlan::noSurf();
            all.fill(0);
            auto one = all;
            one[kSurfWall0 + 1] = 4;
            auto walls = all;
            for (int i = 0; i < 4; ++i)
                walls[(size_t)(kSurfWall0 + i)] = 4;
            const float tAll = rt(all), tOne = rt(one), tWalls = rt(walls);
            r.plan.surf = one;
            const float eOne = eyringRt(r, false, 0.3f);
            say("material: T30 all stone " + juce::String(tAll, 2) + " s, one wall a curtain " +
                    juce::String(tOne, 2) + " s (Eyring " + juce::String(eOne, 2) +
                    " s), all walls curtains " + juce::String(tWalls, 2) + " s",
                tOne < 0.8f * tAll && tWalls < 0.7f * tOne && eOne < tAll);
        }
        {
            // a state round trip keeps the materials
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->setSurfaceMaterial(0, -1, -1);
            const bool cleared = proc->getSurfaceMaterial(0, kSurfFloor) == -1;
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            say("material: stored with the project",
                cleared && proc->getSurfaceMaterial(0, kSurfFloor) == 2 &&
                    proc->getSurfaceMaterial(0, kSurfWall0 + 1) == 3 &&
                    proc->getSurfaceMaterial(0, kSurfWall0 + 2) == 5);
            proc->setSurfaceMaterial(0, -1, -1);
        }
        // ---- the rooms that come with the plugin: each loads, the Saal takes its size, the
        // listener stands free (not inside a wall or column); in the stair tower one climbs
        {
            const auto &rooms = builtInRooms();
            for (int i = 0; i < (int)rooms.size(); ++i)
            {
                wv.loadBuiltInRoomForTest(i);
                run(20, {});
                auto m = proc->getWalkMesh();
                const RoomParams r = roomOf();
                const int n = m != nullptr ? (int)m->tris.size() : 0;
                // standing at home, a few steps forwards: it moves (nothing holds the listener)
                wv.setPoseForTest(0, 0, 0);
                for (int k = 0; k < 30; ++k)
                    wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
                const float moved = proc->walkPose[1].load();
                say(juce::String("built-in room ") + rooms[(size_t)i].name + ": " + juce::String(n) +
                        " triangles, " + juce::String(m ? m->size.x : 0.0f, 1) + " x " +
                        juce::String(m ? m->size.y : 0.0f, 1) + " x " + juce::String(m ? m->size.z : 0.0f, 1) +
                        " m, Saal " + juce::String(r.W(), 1) + " x " + juce::String(r.L(), 1) + " x " +
                        juce::String(r.H(), 1) + ", walked " + juce::String(moved, 2) + " m",
                    m != nullptr && n > 100 && n < 20000 && std::abs(r.W() - m->size.x) < 0.2f &&
                        std::abs(r.L() - juce::jmin(100.0f, m->size.y)) < 0.2f && moved > 0.3f);
                wv.setPoseForTest(0, 0, 0);
                run(4, {});
                shot(juce::String("walk_room_") + juce::String(i) + "_a.png");
                wv.setPoseForTest(0, 0, 180);
                run(4, {});
                shot(juce::String("walk_room_") + juce::String(i) + "_b.png");
                if (juce::String(rooms[(size_t)i].name) == "Treppenturm" && m != nullptr)
                {
                    // the foot of the first flight (inner corner 1.5 / 1.5 m, the wall 0.8 m):
                    // walking up it for six seconds
                    const auto home = listenerPos(r);
                    wv.setPoseForTest(0.8f + 0.75f - home.x, 0.8f + 1.0f - home.y, 0);
                    for (int k = 0; k < 360; ++k)
                        wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
                    const float up = wv.heightForTest();
                    shot("walk_room_stairs.png");
                    say("built-in room Treppenturm: up the first flight to " + juce::String(up, 2) +
                            " m (its top 4 m)",
                        up > 3.0f);
                }
            }
            // ---- pipes flying in the crypt bounce off its columns and walls: none goes into a solid
            // (a pipe that started inside one may still be in it: only new ones count)
            {
                wv.loadBuiltInRoomForTest(2);
                setReal(RysumProcessor::stopParamId(1), 1.0f);
                setReal(RysumProcessor::motionParamId(0), (float)Motion::Flight);
                setReal(RysumProcessor::motionParamId(1), (float)Motion::Fall);
                setReal(RysumProcessor::rateParamId(0), 0.5f);
                setReal(RysumProcessor::rateParamId(1), 0.6f);
                run(20, {48, 55, 60, 64, 67});
                const auto ac = proc->getWalkAcoustics();
                auto inside = [&](int s, int k)
                {
                    Vec3f q;
                    if (ac == nullptr || !proc->engine.bodyAt(s, k, q))
                        return false;
                    return ac->insideSolid(q);
                };
                std::array<std::array<bool, kNumKeys>, 2> was{};
                for (int s = 0; s < 2; ++s)
                    for (int k = 0; k < kNumKeys; ++k)
                        was[(size_t)s][(size_t)k] = inside(s, k);
                int went = 0, startedIn = 0;
                for (auto &st : was)
                    for (bool b : st)
                        startedIn += b ? 1 : 0;
                const uint32_t knocks0 = proc->engine.impactCount.load();
                for (int round = 0; round < 60; ++round)
                {
                    run(10, {});
                    for (int s = 0; s < 2; ++s)
                        for (int k = 0; k < kNumKeys; ++k)
                        {
                            const bool now = inside(s, k);
                            if (now && !was[(size_t)s][(size_t)k])
                                ++went;
                            was[(size_t)s][(size_t)k] = now;
                        }
                }
                say("model: pipes flying and falling in the crypt for 6.4 s - " + juce::String(went) +
                        " went into a column or wall (" + juce::String(startedIn) + " started inside one), " +
                        juce::String((int)(proc->engine.impactCount.load() - knocks0)) + " knocks",
                    went == 0);
                shot("walk_room_krypta_flug.png");
                // ---- light off: the room dark, lit by the sounding pipes
                auto meanOf = [&]
                {
                    auto img = wv.createComponentSnapshot(wv.getLocalBounds().reduced(0, 120), true, 1.0f);
                    double sum = 0;
                    int n = 0;
                    for (int y = 0; y < img.getHeight(); y += 4)
                        for (int x = 0; x < img.getWidth(); x += 4)
                        {
                            sum += img.getPixelAt(x, y).getBrightness();
                            ++n;
                        }
                    return sum / juce::jmax(1, n);
                };
                setReal(RysumProcessor::motionParamId(0), 0.0f);
                setReal(RysumProcessor::motionParamId(1), 0.0f);
                setReal(RysumProcessor::stopParamId(6), 1.0f); // (the Trompete too: its funnels)
                wv.setPoseForTest(0, 0, 0);
                run(20, {48, 55, 60, 64, 67});
                shot("walk_pipes_kinds.png");
                const double lightOn = meanOf();
                wv.setDarkForTest(true);
                run(20, {48, 55, 60, 64, 67});
                const double lit = meanOf();
                shot("walk_dark_lit.png");
                run(1, {});
                {
                    juce::MidiBuffer offs;
                    for (int n2 : {48, 55, 60, 64, 67})
                        offs.addEvent(juce::MidiMessage::noteOff(1, n2), 0);
                    juce::AudioBuffer<float> sb(16, 512);
                    proc->processBlock(sb, offs);
                }
                run(150, {});
                for (int f = 0; f < 40; ++f) // (the glow of a pipe fades frame by frame)
                    meanOf();
                const double unlit = meanOf();
                shot("walk_dark_silent.png");
                say("light off: the picture " + juce::String(lightOn, 3) + " with the light on, " +
                        juce::String(lit, 3) + " dark with a chord, " + juce::String(unlit, 3) +
                        " dark and silent",
                    lit < lightOn && unlit < lit * 0.85 && unlit < 0.058); // (0.051: the ink black itself)
                wv.setDarkForTest(false);
                {
                    // (close up: only the wooden Gedackt and the Trompete with its funnels, in the bare Saal)
                    proc->clearWalkModel();
                    auto *p0 = proc->apvts.getRawParameterValue(RysumProcessor::stopParamId(0));
                    const float was0 = p0 != nullptr ? p0->load() : 1.0f;
                    setReal(RysumProcessor::stopParamId(0), 0.0f);
                    setReal(RysumProcessor::roomParamId(0, "W"), 20.0f);
                    setReal(RysumProcessor::roomParamId(0, "L"), 30.0f);
                    setReal(RysumProcessor::roomParamId(0, "H"), 12.0f);
                    run(10, {});
                    wv.setPoseForTest(0.0f, -1.5f, 0.0f);
                    wv.setPitchForTest(8.0f);
                    run(4, {});
                    shot("walk_pipes_close.png");
                    setReal(RysumProcessor::stopParamId(1), 0.0f); run(4, {}); wv.setPoseForTest(-1.0f, 1.0f, 20.0f); wv.setPitchForTest(48.0f);
                    run(4, {});
                    shot("walk_pipes_close2.png");
                    setReal(RysumProcessor::stopParamId(0), was0);
                }
                setReal(RysumProcessor::stopParamId(1), 0.0f);
                setReal(RysumProcessor::stopParamId(6), 0.0f);
            }
            proc->clearWalkModel();
            setReal(RysumProcessor::roomParamId(0, "Lx"), 0.5f);
            setReal(RysumProcessor::roomParamId(0, "Ly"), 0.4f);
            run(10, {});
        }
        // ---- a model: the chapel. Its walls stop the walker, its altar carries, its pillars hide pipes
        {
            auto objFile = outDir.getChildFile("test_kapelle.obj");
            objFile.replaceWithText(makeTestObj());
            proc->walkZUp = true;
            wv.loadModelForTest(objFile);
            // the Saal as large as the model
            setReal(RysumProcessor::roomParamId(0, "W"), 12.8f);
            setReal(RysumProcessor::roomParamId(0, "L"), 24.8f);
            setReal(RysumProcessor::roomParamId(0, "H"), 9.2f);
            setReal(RysumProcessor::roomParamId(0, "Size"), 1.0f);
            run(20, {});
            const auto ac = proc->getWalkAcoustics();
            const RoomParams r = roomOf();
            const auto home = listenerPos(r);
            say("model: " + juce::String(ac != nullptr ? (int)ac->tris.size() : 0) +
                    " triangles for the rays, the engine uses it: " +
                    juce::String((int)proc->engine.meshActive.load()),
                ac != nullptr && ac->tris.size() > 100 && proc->engine.meshActive.load());
            if (ac != nullptr)
            {
                // a pillar stands at x 3.0 .. 3.6, y 4.4 .. 5.0 (the model is moved by its corner:
                // +0.4 m): walking into it from the left stops in front of it
                wv.setPoseForTest(1.5f - home.x, 4.7f - home.y, -90);
                for (int i = 0; i < 180; ++i)
                    wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
                const float xStop = home.x + proc->walkPose[0].load();
                say("model: walking into a pillar stops at x = " + juce::String(xStop, 2) +
                        " m (the pillar begins at 3.0)",
                    xStop > 2.2f && xStop < 3.0f);
                shot("walk_obj_pillar.png");
                // the same walk a metre to the side passes
                wv.setPoseForTest(1.5f - home.x, 6.5f - home.y, -90);
                for (int i = 0; i < 180; ++i)
                    wv.advanceForTest(1.0 / 60.0, true, false, false, false, false);
                say("model: beside the pillar the way is free (x = " +
                        juce::String(home.x + proc->walkPose[0].load(), 2) + " m)",
                    home.x + proc->walkPose[0].load() > 5.0f);
                // line of sight: behind the pillar a point is hidden, the way around bends
                Pos heard;
                float loss = 0, fc = 0;
                const Vec3f L{2.0f, 4.7f, 1.6f}, S{6.0f, 4.7f, 1.6f}, S2{6.0f, 7.5f, 1.6f};
                const bool hid = meshDetour(*ac, L, S, heard, loss, fc);
                const bool hid2 = meshDetour(*ac, L, S2, heard, loss, fc);
                Pos h1;
                float l1 = 0, f1 = 0;
                meshDetour(*ac, L, S, h1, l1, f1);
                say("model: a pipe behind the pillar is hidden (" + juce::String(l1, 1) +
                        " dB, lowpass " + juce::String(f1, 0) + " Hz, heard from " +
                        juce::String(h1.az, 0) + " deg), one beside it is in sight",
                    hid && !hid2 && l1 > 0.2f && l1 < 12.0f && f1 < 15000.0f);
                // the reverb: rays through the model against the bare shoebox
                const auto bare = traceRoom(r, 0.3f, 1500), inModel = traceMesh(*ac, r, 0.3f, 1500);
                say("model: T30 traced through the model " + juce::String(inModel.rtLow, 2) +
                        " s, the bare Saal " + juce::String(bare.rtLow, 2) + " s",
                    inModel.valid && inModel.rtLow > 0.3f * bare.rtLow &&
                        inModel.rtLow < 1.5f * bare.rtLow);
                // the engine: a chord, the listener behind a pillar -> some pipes around the corner
                wv.setPoseForTest(2.0f - home.x, 4.7f - home.y, -90);
                run(60, {48, 52, 55, 60, 64, 67, 72});
                say("model: the engine hears " + juce::String(proc->engine.hiddenNow.load()) +
                        " sounding pipes around the model's corners",
                    proc->engine.hiddenNow.load() >= 0);
                wv.setPoseForTest(0, -6, 0);
                wv.setPitchForTest(25);
                run(4, {});
                shot("walk_obj_up.png");
                wv.setPoseForTest(0, 0, 150);
                wv.setPitchForTest(-5);
                run(4, {});
                shot("walk_obj_door.png");
            }
            proc->clearWalkModel();
        }
        // ---- the player and the pipes: a stop hovers in the Saal, the player pulls / pushes
        {
            setReal(RysumProcessor::motionParamId(0), (float)(int)Motion::Hover);
            setReal(RysumProcessor::depthParamId(0), 180.0f);
            setReal("lvPlayerForce", 1.0f);
            wv.setPoseForTest(0, 0, 0);
            run(30, {});
            const RoomParams r = roomOf();
            const auto home = listenerPos(r);
            auto meanDist = [&]
            {
                float sum = 0.0f;
                int n = 0;
                for (int k = 0; k < kNumKeys; ++k)
                {
                    Vec3f b;
                    if (!proc->engine.bodyAt(0, k, b))
                        continue;
                    sum += std::sqrt((b.x - home.x) * (b.x - home.x) + (b.y - home.y) * (b.y - home.y));
                    ++n;
                }
                return n > 0 ? sum / (float)n : -1.0f;
            };
            const float d0 = meanDist();
            run(200, {});
            const float dRest = meanDist();
            setReal("lvPlayerMode", 1.0f);
            run(300, {});
            const float dPull = meanDist();
            wv.setPoseForTest(0, 0, 0);
            shot("walk_player_pull.png");
            setReal("lvPlayerMode", 2.0f);
            run(300, {});
            const float dPush = meanDist();
            shot("walk_player_push.png");
            say("player: the hovering pipes rest at " + juce::String(d0, 2) + " m (" +
                    juce::String(dRest, 2) + " m after 2 s), pulled to " + juce::String(dPull, 2) +
                    " m, pushed away to " + juce::String(dPush, 2) + " m",
                d0 > 0.0f && std::abs(dRest - d0) < 0.2f && dPull < d0 - 1.0f && dPush > dPull + 2.0f);
            setReal("lvPlayerMode", 0.0f);

            // ---- the player's hands: shockwave, freeze, call home, carry and throw, knock, clap
            setReal(RysumProcessor::depthParamId(0), 60.0f);
            wv.setPoseForTest(0, 0, 0);
            wv.callHome();
            run(400, {});
            const float dA = meanDist();
            wv.shock();
            run(40, {});
            const float dB = meanDist();
            wv.setFreeze(true);
            run(4, {});
            const float dF0 = meanDist();
            run(60, {});
            const float dF1 = meanDist();
            wv.setFreeze(false);
            shot("walk_shock.png");
            wv.callHome();
            run(400, {});
            const float dC = meanDist();
            say("hands: at home " + juce::String(dA, 2) + " m, after a shockwave " + juce::String(dB, 2) +
                    " m, frozen " + juce::String(dF0, 3) + " -> " + juce::String(dF1, 3) +
                    " m, called home " + juce::String(dC, 2) + " m",
                dB > dA + 0.5f && std::abs(dF1 - dF0) < 0.005f && std::abs(dC - dA) < 0.1f);

            // look at the nearest pipe ahead, take it, turn a quarter and let go
            int kNear = -1;
            float best = 1.0e9f;
            for (int k = 0; k < kNumKeys; ++k)
            {
                Vec3f b;
                if (!proc->engine.bodyAt(0, k, b))
                    continue;
                const float d = std::sqrt((b.x - home.x) * (b.x - home.x) + (b.y - home.y) * (b.y - home.y));
                if (d > 1.5f && d < best)
                {
                    best = d;
                    kNear = k;
                }
            }
            Vec3f at;
            proc->engine.bodyAt(0, juce::jmax(0, kNear), at);
            const float yawTo = juce::radiansToDegrees(std::atan2(-(at.x - home.x), at.y - home.y));
            wv.setPoseForTest(0, 0, yawTo);
            wv.setPitchForTest(juce::radiansToDegrees(std::atan2(at.z - home.z, best)));
            run(2, {});
            shot("walk_aim.png");
            const int aS = wv.aimStopForTest(), aK = wv.aimKeyForTest();
            const uint32_t knocks0 = proc->engine.impactCount.load();
            const bool hit = wv.strikeAt(wv.getLocalBounds().getCentre().toFloat());
            run(4, {});
            const uint32_t knocks1 = proc->engine.impactCount.load();
            say("hands: the cross is on pipe " + juce::String(aS) + "/" + juce::String(aK) + " (" +
                    juce::String(best, 1) + " m away), a click knocks on it: " +
                    juce::String((int)(knocks1 - knocks0)) + " knock",
                aS == 0 && aK >= 0 && hit && knocks1 == knocks0 + 1);
            wv.setGrab(true);
            wv.setPitchForTest(0.0f);
            for (int i = 0; i < 120; ++i)
            {
                wv.setPoseForTest(0, 0, yawTo + juce::jmin(90.0f, (float)i * 1.5f));
                wv.updateActionsForTest();
                run(1, {});
            }
            Vec3f held;
            proc->engine.bodyAt(0, juce::jmax(0, aK), held);
            const float ya = juce::degreesToRadians(yawTo + 90.0f);
            const float hd = std::sqrt((held.x - home.x) * (held.x - home.x) + (held.y - home.y) * (held.y - home.y));
            const float along = (held.x - home.x) * -std::sin(ya) + (held.y - home.y) * std::cos(ya);
            shot("walk_carry.png");
            wv.setGrab(false);
            run(300, {});
            Vec3f thrown;
            proc->engine.bodyAt(0, juce::jmax(0, aK), thrown);
            run(200, {});
            Vec3f rest;
            proc->engine.bodyAt(0, juce::jmax(0, aK), rest);
            const float flew = std::sqrt((thrown.x - held.x) * (thrown.x - held.x) + (thrown.y - held.y) * (thrown.y - held.y));
            const float crept = std::sqrt((rest.x - thrown.x) * (rest.x - thrown.x) + (rest.y - thrown.y) * (rest.y - thrown.y));
            say("hands: carried " + juce::String(hd, 2) + " m ahead (" + juce::String(along, 2) +
                    " m along the view), thrown " + juce::String(flew, 2) + " m, then it stays (" +
                    juce::String(crept, 3) + " m in 2 s)",
                hd > 1.0f && hd < 3.2f && along > 0.9f * hd && flew > 1.0f && crept < 0.3f);

            // ---- arranging: choosing pipes, a formation, undo, a layout kept and brought back (also through the
            // project), the magnet, the sweep, the list of the keys
            {
                auto& en = proc->engine;
                auto snapshot = [&]
                {
                    std::array<Vec3f, kNumKeys> a{};
                    for (int k = 0; k < kNumKeys; ++k)
                        en.bodyAt(0, k, a[(size_t)k]);
                    return a;
                };
                auto apart = [&](const std::array<Vec3f, kNumKeys> &a, int keys)
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int k = 0; k < keys; ++k)
                    {
                        Vec3f b;
                        if (!en.bodyAt(0, k, b))
                            continue;
                        const auto &p = a[(size_t)k];
                        sum += std::sqrt((b.x - p.x) * (b.x - p.x) + (b.y - p.y) * (b.y - p.y));
                        ++n;
                    }
                    return n > 0 ? sum / (float)n : 99.0f;
                };
                auto targets = [&]
                {
                    std::array<Vec3f, kNumKeys> a{};
                    for (int k = 0; k < kNumKeys; ++k)
                        a[(size_t)k] = {en.goX[0][(size_t)k].load(), en.goY[0][(size_t)k].load(), en.goZ[0][(size_t)k].load()};
                    return a;
                };
                wv.setPoseForTest(0, 0, 0);
                wv.setPitchForTest(0.0f);
                run(20, {});
                const auto before = snapshot();
                for (int k = 0; k < 12; ++k)
                    wv.selectPipe(0, k, false);
                const int chosen = wv.selectedCount();
                const bool row = wv.formation(1);
                run(400, {});
                const float offRow = apart(targets(), 12);
                Vec3f lo, hi;
                en.bodyAt(0, 0, lo);
                en.bodyAt(0, 11, hi);
                shot("walk_row.png");
                say("arranging: " + juce::String(chosen) + " pipes chosen, a row: they stand " + juce::String(offRow, 2) +
                        " m from their places in it, low on the left (" + juce::String(lo.x, 1) + " < " + juce::String(hi.x, 1) + " m)",
                    chosen == 12 && row && offRow < 0.25f && lo.x < hi.x);

                // knocked over the row like strings: Alt+drag across the view
                const uint32_t i0 = en.impactCount.load();
                const auto mid = wv.getLocalBounds().getCentre().toFloat();
                int swept = 0;
                for (float x = 60.0f; x < (float)wv.getWidth() - 60.0f; x += 40.0f)
                    swept += wv.sweepTo({x, mid.y + 10.0f}, {x + 40.0f, mid.y + 10.0f}, 1.0f);
                run(10, {});
                const uint32_t i1 = en.impactCount.load();
                say("arranging: a sweep over the row knocks " + juce::String(swept) + " pipes (the row and those behind it), each once (" +
                        juce::String((int)(i1 - i0)) + " knocks)",
                    swept >= 6 && (int)(i1 - i0) == swept);
                run(150, {});

                const int depth = wv.undoDepth();
                wv.undo();
                run(400, {});
                const float offUndo = apart(before, kNumKeys);
                say("arranging: undo brings them back (" + juce::String(offUndo, 2) + " m from where they stood)",
                    depth >= 1 && offUndo < 0.25f && wv.undoDepth() == depth - 1);

                wv.clearSelection();
                const bool kept = wv.storeLayout(0);
                wv.formation(2);
                run(400, {});
                const float offCircle = apart(before, kNumKeys);
                wv.recallLayout(0);
                run(400, {});
                const float offBack = apart(before, kNumKeys);
                // (kept with the project)
                const auto keptHas = proc->walkLayouts[0].has[0];
                const auto keptAt = proc->walkLayouts[0].at[0][3];
                juce::MemoryBlock ls;
                proc->getStateInformation(ls);
                proc->walkLayouts[0] = {};
                proc->setStateInformation(ls.getData(), (int)ls.getSize());
                const auto &back = proc->walkLayouts[0];
                const bool inProject = back.has[0] == keptHas && keptHas != 0 &&
                                       std::abs(back.at[0][3].x - keptAt.x) < 0.01f && std::abs(back.at[0][3].z - keptAt.z) < 0.01f;
                say("arranging: F1 keeps the layout, a circle takes them " + juce::String(offCircle, 1) + " m away, F1 brings them back (" +
                        juce::String(offBack, 2) + " m), the layout is in the project: " + (inProject ? "yes" : "no"),
                    kept && offCircle > 0.5f && offBack < 0.25f && inProject);

                // the magnet: three metres ahead; everything comes closer
                wv.setPoseForTest(0, 0, 0);
                wv.setPitchForTest(0.0f);
                run(2, {});
                auto toMagnet = [&]
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int k = 0; k < kNumKeys; ++k)
                    {
                        Vec3f b;
                        if (!en.bodyAt(0, k, b))
                            continue;
                        const float dx = b.x - en.magnetX.load(), dy = b.y - en.magnetY.load(), dz = b.z - en.magnetZ.load();
                        sum += std::sqrt(dx * dx + dy * dy + dz * dz);
                        ++n;
                    }
                    return n > 0 ? sum / (float)n : 99.0f;
                };
                wv.setMagnet(1);
                wv.updateActionsForTest();
                const float m0 = toMagnet();
                for (int i = 0; i < 300; ++i)
                {
                    wv.updateActionsForTest();
                    run(1, {});
                }
                const float m1 = toMagnet();
                shot("walk_magnet.png");
                wv.setMagnet(0);
                say("arranging: the magnet draws the pipes from " + juce::String(m0, 2) + " m to " + juce::String(m1, 2) + " m around its point",
                    m1 < m0 - 1.0f && m1 < 2.0f);
                wv.undo();
                run(400, {});

                // ---- more hands: a pinned pipe stays, pause, slow motion, a pipe held sounding, the whirlwind
                {
                    wv.setPoseForTest(0, 0, 0);
                    run(10, {});
                    const auto a = snapshot();
                    wv.togglePin(0, 5);
                    en.playerAct(OrganEngine::Act::Shock, -1, -1, {}, 1.5f);
                    run(60, {});
                    Vec3f p5;
                    en.bodyAt(0, 5, p5);
                    const float pinMoved = std::sqrt((p5.x - a[5].x) * (p5.x - a[5].x) + (p5.y - a[5].y) * (p5.y - a[5].y));
                    const float othersMoved = apart(a, kNumKeys);
                    wv.togglePin(0, 5);
                    say("more hands: a pinned pipe stays (" + juce::String(pinMoved, 3) + " m) while a shockwave drives the others " +
                            juce::String(othersMoved, 2) + " m",
                        pinMoved < 0.01f && othersMoved > 0.3f);
                    wv.undo();
                    run(400, {});

                    // pause: a shockwave waits until it is over
                    const auto b = snapshot();
                    wv.setPause(true);
                    en.playerAct(OrganEngine::Act::Shock, -1, -1, {}, 1.5f);
                    run(60, {});
                    const float duringPause = apart(b, kNumKeys);
                    wv.setPause(false);
                    run(60, {});
                    const float afterPause = apart(b, kNumKeys);
                    say("more hands: paused, a shockwave moves nothing (" + juce::String(duringPause, 3) + " m), then they fly (" +
                            juce::String(afterPause, 2) + " m)",
                        duringPause < 0.005f && afterPause > 0.3f);
                    en.playerAct(OrganEngine::Act::Home);
                    run(400, {});

                    // slow motion: the same shockwave goes a quarter as far in the same time
                    auto flight = [&](bool slowly)
                    {
                        const auto c0 = snapshot();
                        wv.setSlow(slowly);
                        en.playerAct(OrganEngine::Act::Shock, -1, -1, {}, 1.5f);
                        run(20, {});
                        const float d = apart(c0, kNumKeys);
                        wv.setSlow(false);
                        en.playerAct(OrganEngine::Act::Home);
                        run(500, {});
                        return d;
                    };
                    const float normal = flight(false), slowly = flight(true);
                    say("more hands: in slow motion a shockwave takes them " + juce::String(slowly, 2) + " m in the time it otherwise takes them " +
                            juce::String(normal, 2) + " m",
                        slowly < 0.45f * normal && slowly > 0.0f);

                    // a pipe held sounding: a voice while held, none after
                    run(300, {});
                    const int v0 = en.activeVoices.load();
                    wv.bowStart(0, 20);
                    run(20, {});
                    const int v1 = en.activeVoices.load();
                    wv.bowStop();
                    run(500, {});
                    const int v2 = en.activeVoices.load();
                    say("more hands: a pipe held sounding: " + juce::String(v0) + " -> " + juce::String(v1) + " voices, let go: " + juce::String(v2),
                        v1 > v0 && v2 <= v0);

                    // the whirlwind: the pipes near it go round
                    shot("walk_whirl_before.png");
                    const auto w0 = snapshot();
                    wv.whirl();
                    run(150, {});
                    const float whirled = apart(w0, kNumKeys);
                    shot("walk_whirl.png");
                    say("more hands: a whirlwind takes the pipes round, " + juce::String(whirled, 2) + " m on average",
                        whirled > 0.5f);
                    run(400, {});
                    wv.undo();
                    run(400, {});
                }

                // ---- the room: larger, lanterns (kept with the project), the ears left behind
                {
                    auto *sizeP = proc->apvts.getParameter(RysumProcessor::roomParamId(0, "Size"));
                    const float s0 = sizeP->convertFrom0to1(sizeP->getValue());
                    const bool grown = wv.scaleRoom(1.5f);
                    const float s1 = sizeP->convertFrom0to1(sizeP->getValue());
                    wv.scaleRoom(1.0f / 1.5f);
                    const float s2 = sizeP->convertFrom0to1(sizeP->getValue());
                    say("room: Ctrl+wheel makes the Saal larger (" + juce::String(s0, 2) + " -> " + juce::String(s1, 2) + ") and back (" +
                            juce::String(s2, 2) + ")",
                        grown && std::abs(s1 - 1.5f * s0) < 0.01f && std::abs(s2 - s0) < 0.01f);

                    // (in the dark, with nothing sounding: what a lantern on the floor ahead lights)
                    auto brightness = [&]
                    {
                        auto img = wv.createComponentSnapshot(wv.getLocalBounds().reduced(0, 120), true, 1.0f);
                        double sum = 0;
                        int n = 0;
                        for (int y = 0; y < img.getHeight(); y += 4)
                            for (int x = 0; x < img.getWidth(); x += 4)
                            {
                                sum += img.getPixelAt(x, y).getBrightness();
                                ++n;
                            }
                        return sum / juce::jmax(1, n);
                    };
                    run(800, {});
                    wv.setPoseForTest(0, 0, 0);
                    wv.setPitchForTest(-30.0f);
                    wv.setDarkForTest(true);
                    run(4, {});
                    shot("walk_dark_nolantern.png");
                    const double darkB = brightness();
                    wv.placeLantern();
                    run(4, {});
                    shot("walk_lantern.png");
                    const double lanternB = brightness();
                    wv.setPoseForTest(0, 0, 40);
                    wv.setPitchForTest(-30.0f);
                    run(2, {});
                    shot("walk_aim_lantern.png");
                    wv.placeLantern();
                    wv.setPoseForTest(0, 0, 0);
                    run(4, {});
                    shot("walk_lanterns.png");
                    say("room: in the dark a lantern on the floor lights the room (" + juce::String(darkB, 3) + " -> " +
                            juce::String(lanternB, 3) + ")",
                        lanternB > darkB * 1.15);
                    const auto lanterns = proc->walkLanterns;
                    juce::MemoryBlock lsb;
                    proc->getStateInformation(lsb);
                    proc->walkLanterns.clear();
                    proc->setStateInformation(lsb.getData(), (int)lsb.getSize());
                    const bool keptL = proc->walkLanterns.size() == 2 && lanterns.size() == 2 &&
                                       std::abs(proc->walkLanterns[1].x - lanterns[1].x) < 0.01f;
                    wv.removeLantern();
                    const size_t afterRemove = proc->walkLanterns.size();
                    wv.clearLanterns();
                    wv.setDarkForTest(false);
                    say("room: two lanterns, kept with the project: " + juce::String(keptL ? "yes" : "no") + ", Shift+K takes one away (" +
                            juce::String((int)afterRemove) + " left), Ctrl+K all (" + juce::String((int)proc->walkLanterns.size()) + ")",
                        keptL && afterRemove == 1 && proc->walkLanterns.empty());

                    wv.setPoseForTest(0, 0, 0);
                    wv.leaveEars(true);
                    wv.setPoseForTest(3.0f, 2.0f, 90.0f);
                    const float earX = en.viewX.load(), earYaw = en.viewYaw.load();
                    shot("walk_ears.png");
                    wv.leaveEars(false);
                    const float backX = en.viewX.load();
                    say("room: the ears stay behind (heard from x " + juce::String(earX, 1) + ", yaw " + juce::String(earYaw, 0) +
                            " while you stand at 3.0), H brings them back (x " + juce::String(backX, 1) + ")",
                        std::abs(earX) < 0.01f && std::abs(earYaw) < 0.01f && std::abs(backX - 3.0f) < 0.01f);
                    wv.setPoseForTest(0, 0, 0);
                }

                wv.setHelpForTest(true);
                shot("walk_keys.png");
                wv.setHelpForTest(false);
            }

            // ---- the audience: a "phone" (this computer, 127.0.0.1) opens the page, gets a pipe,
            // knocks it, sends a shockwave, blows wind; a connection that never sends holds nothing up
            {
                const bool on = wv.setAudienceForTest(true);
                auto *au = proc->getAudience();
                const int port = au != nullptr ? au->serverForTest().portNumber() : 0;
                auto http = [&](const juce::String &req, double *ms = nullptr) -> juce::String
                {
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    juce::StreamingSocket s;
                    if (port <= 0 || !s.connect("127.0.0.1", port, 1000))
                        return {};
                    const auto raw = req + " HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n";
                    s.write(raw.toRawUTF8(), (int)std::strlen(raw.toRawUTF8()));
                    juce::MemoryBlock got;
                    while (juce::Time::getMillisecondCounterHiRes() - t0 < 3000.0)
                    {
                        if (s.waitUntilReady(true, 100) != 1)
                            continue;
                        char buf[4096];
                        const int n = s.read(buf, (int)sizeof(buf), false);
                        if (n <= 0)
                            break;
                        got.append(buf, (size_t)n);
                    }
                    if (ms != nullptr)
                        *ms = juce::Time::getMillisecondCounterHiRes() - t0;
                    return juce::String::fromUTF8((const char *)got.getData(), (int)got.getSize());
                };
                const auto page = http("GET /");
                http("POST /hi?p=testphone1");
                au->tickForTest();
                const auto hello = http("POST /hi?p=testphone1");
                const bool gotPipe = hello.contains("\"c\":\"#") && au->phoneCount() == 1 && au->hands()[0].stop == 0;
                say("audience: the page is served (" + juce::String(page.length()) + " bytes), the phone gets a pipe: " +
                        hello.fromFirstOccurrenceOf("\r\n\r\n", false, false),
                    on && page.startsWith("HTTP/1.1 200") && page.contains("Druckwelle") && gotPipe);
                {
                    // the page in the plugin's hand: its fonts and the stops' photographs, from the plugin's data
                    const auto font = http("GET /f/roman.ttf"), knob = http("GET /i/k1.jpg"), none = http("GET /i/k9.jpg");
                    say("audience: the page gets the plugin's font and the stop's photograph, and nothing else",
                        font.startsWith("HTTP/1.1 200") && font.contains("font/ttf") && knob.startsWith("HTTP/1.1 200") &&
                            knob.contains("image/jpeg") && none.startsWith("HTTP/1.1 404") && hello.contains("\"s\":0"));
                }

                const uint32_t k0 = proc->engine.impactCount.load();
                http("POST /act?p=testphone1&a=k&s=1");
                au->tickForTest();
                run(4, {});
                const uint32_t k1 = proc->engine.impactCount.load();
                http("POST /act?p=testphone1&a=k&s=1"); // (at once again: too fast, dropped)
                au->tickForTest();
                run(4, {});
                const uint32_t k2 = proc->engine.impactCount.load();
                say("audience: a tap knocks the phone's pipe (" + juce::String((int)(k1 - k0)) +
                        " knock), a second tap within 90 ms does not (" + juce::String((int)(k2 - k1)) + ")",
                    k1 == k0 + 1 && k2 == k1);

                wv.callHome();
                run(400, {});
                const float s0 = meanDist();
                http("POST /act?p=testphone1&a=s&s=1.2");
                au->tickForTest();
                run(60, {});
                const float s1 = meanDist();
                say("audience: a swipe sends a shockwave: the pipes " + juce::String(s0, 2) + " -> " + juce::String(s1, 2) + " m away",
                    s1 > s0 + 0.5f);

                wv.callHome();
                run(400, {});
                auto alongRight = [&]
                {
                    const float ya = juce::degreesToRadians(proc->walkPose[2].load());
                    float sum = 0.0f;
                    int n = 0;
                    for (int k = 0; k < kNumKeys; ++k)
                    {
                        Vec3f b;
                        if (!proc->engine.bodyAt(0, k, b))
                            continue;
                        sum += (b.x - home.x) * std::cos(ya) + (b.y - home.y) * std::sin(ya);
                        ++n;
                    }
                    return n > 0 ? sum / (float)n : 0.0f;
                };
                const float w0 = alongRight();
                for (int i = 0; i < 10; ++i)
                {
                    http("POST /act?p=testphone1&a=w&x=1&y=0");
                    au->tickForTest();
                    run(8, {});
                }
                const float w1 = alongRight();
                say("audience: dragging to the right blows the pipes to the right of the view: " + juce::String(w0, 2) +
                        " -> " + juce::String(w1, 2) + " m",
                    w1 > w0 + 0.3f);

                juce::StreamingSocket idle;
                idle.connect("127.0.0.1", port, 1000); // (opened, never sends)
                double ms = 0.0;
                const auto r = http("POST /hi?p=testphone2", &ms);
                AudienceServer second;
                const bool secondOk = second.start(port, true);
                say("audience: an idle connection holds nothing up (" + juce::String(ms, 0) + " ms), a second instance opens port " +
                        juce::String(second.portNumber()) + " instead of " + juce::String(port),
                    r.startsWith("HTTP/1.1 200") && ms < 500.0 && secondOk && second.portNumber() != port);
                second.stop();
                idle.close();

                // the QR code as a picture, for a decoder outside the test
                // (and codes of other lengths: versions 3 to 6, several blocks)
                const char *texts[]{"http://192.168.178.123:8796/",
                                    "http://192.168.178.123:8796/abcdefghijklmnopqrstuvwxyz0123",
                                    "http://192.168.178.123:8796/abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz012345"};
                for (int t = 0; t <= 3; ++t)
                {
                    const QrCode extra = t > 0 ? QrCode::encode(texts[t - 1]) : QrCode{};
                    const auto &q = t > 0 ? extra : au->qr();
                    juce::Image img(juce::Image::RGB, (q.size + 8) * 8, (q.size + 8) * 8, true);
                    {
                        // (the drawing reaches the image when the Graphics goes)
                        juce::Graphics g(img);
                        g.fillAll(juce::Colours::white);
                        g.setColour(juce::Colours::black);
                        for (int y = 0; y < q.size; ++y)
                            for (int x = 0; x < q.size; ++x)
                                if (q.at(x, y))
                                    g.fillRect((x + 4) * 8, (y + 4) * 8, 8, 8);
                    }
                    juce::PNGImageFormat png;
                    auto f = outDir.getChildFile("audience_qr" + juce::String(t) + ".png");
                    f.deleteFile();
                    juce::FileOutputStream os(f);
                    png.writeImageToStream(img, os);
                    std::cout << "audience: QR for " << (t > 0 ? juce::String(texts[t - 1]) : au->url()) << " (" << q.size
                              << " modules) in " << f.getFullPathName() << std::endl;
                }
                // (look at the phone's pipe, which has just knocked: its ring in the phone's colour)
                if (!au->hands().empty() && au->hands()[0].stop >= 0)
                {
                    Vec3f pp;
                    proc->engine.bodyAt(au->hands()[0].stop, au->hands()[0].key, pp);
                    wv.setPoseForTest(0, 0, juce::radiansToDegrees(std::atan2(-(pp.x - home.x), pp.y - home.y)));
                    wv.setPitchForTest(0.0f);
                    http("POST /act?p=testphone1&a=k&s=1");
                    au->tickForTest();
                    run(2, {});
                }
                shot("walk_audience.png");
                if (const char *serve = std::getenv("RYSUM_AUDIENCE_SERVE"))
                {
                    // (by hand: a browser on this computer plays the phone for so many seconds)
                    const uint32_t k0 = proc->engine.impactCount.load();
                    const int taps0 = au->serverForTest().knocksServed();
                    const double until = juce::Time::getMillisecondCounterHiRes() + 1000.0 * std::atof(serve);
                    std::cout << "audience: serving " << au->url() << " for " << serve << " s" << std::endl;
                    while (juce::Time::getMillisecondCounterHiRes() < until)
                    {
                        au->tickForTest();
                        run(2, {});
                        juce::Thread::sleep(15);
                    }
                    std::cout << "audience: phones " << au->phoneCount() << ", taps received "
                              << au->serverForTest().knocksServed() - taps0 << ", knocks "
                              << (int)(proc->engine.impactCount.load() - k0) << std::endl;
                    shot("walk_audience_live.png");
                }
                wv.setAudienceForTest(false);
                juce::StreamingSocket after;
                say("audience: switched off, the port is closed",
                    proc->getAudience() == nullptr && !after.connect("127.0.0.1", port, 500));
            }

            // a clap is heard (the late reverb is built in the background by the plugin, not in this test)
            {
                juce::AudioBuffer<float> buf(
                    juce::jmax(2, proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()), 512);
                auto level = [&](int blocks)
                {
                    double sum = 0.0;
                    for (int b = 0; b < blocks; ++b)
                    {
                        juce::MidiBuffer mb;
                        mb.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                        for (int n : {48, 55, 60, 64, 67})
                            mb.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                        buf.clear();
                        proc->processBlock(buf, mb);
                        sum += (double)buf.getRMSLevel(0, 0, 512) * (double)buf.getRMSLevel(0, 0, 512);
                    }
                    return 10.0 * std::log10(sum / blocks + 1.0e-12);
                };
                wv.callHome();
                level(900); // (the notes of the pictures let go, their reverb gone)
                const double quiet = level(20);
                wv.clap();
                const double loud = level(10);
                level(30);
                const double after = level(20);
                say("hands: a clap " + juce::String(loud, 1) + " dB, over after half a second (" +
                        juce::String(after, 1) + " dB; silence before " + juce::String(quiet, 1) + " dB)",
                    loud > quiet + 20.0 && loud > -50.0 && after < loud - 40.0);
            }
            setReal(RysumProcessor::motionParamId(0), 0.0f);
            // "Alles zuruecksetzen" (patch menu): a loaded church, lanterns, a pose and changed parameters are
            // gone; a new instance's state
            {
                wv.loadBuiltInRoomForTest(1);
                proc->walkLanterns.push_back({3.0f, 4.0f, 2.0f});
                proc->walkPose[0] = 2.5f;
                setReal("lvSelfPlay", 0.7f);
                setReal(RysumProcessor::stopParamId(3), 1.0f);
                const bool had = proc->getWalkMesh() != nullptr;
                wv.resetView();
                proc->resetEverything();
                auto atDefault = [&](const juce::String &id)
                {
                    auto *q = proc->apvts.getParameter(id);
                    return q != nullptr && std::abs(q->getValue() - q->getDefaultValue()) < 1.0e-4f;
                };
                const bool ok = had && proc->getWalkMesh() == nullptr && proc->walkModelFile == juce::File() &&
                                proc->walkLanterns.empty() && proc->walkPose[0].load() == 0.0f &&
                                atDefault("lvSelfPlay") && atDefault(RysumProcessor::stopParamId(3)) &&
                                proc->getPatchName().isEmpty();
                say(juce::String("reset everything: the model, lanterns, pose and parameters as in a new instance (model was ") +
                        (had ? "loaded" : "NOT loaded") + ")",
                    ok);
            }
            // AmbiX out: turning the head in the 3D view turns the scene sent out. A held chord, its
            // direction from the first-order intensity (ACN: W, Y = left, Z, X = front); the head
            // turned 90 degrees to the left moves it 90 degrees to the right
            {
                setReal("output", 1.0f);
                setReal("simOn", 0.0f);
                setReal(RysumProcessor::stopParamId(0), 1.0f);
                setReal(RysumProcessor::motionParamId(0), 0.0f);
                juce::AudioBuffer<float> ab(36, 512);
                auto direction = [&](float yaw, double &level)
                {
                    proc->engine.viewWalkOn = true;
                    proc->engine.viewYaw = yaw;
                    double wx = 0.0, wy = 0.0, ww = 0.0;
                    for (int b = 0; b < 60; ++b)
                    {
                        juce::MidiBuffer mb;
                        if (b == 0)
                            for (int m : {60, 64, 67})
                                mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                        ab.clear();
                        proc->processBlock(ab, mb);
                        if (b < 20)
                            continue;
                        const float *w = ab.getReadPointer(0), *y = ab.getReadPointer(1), *x = ab.getReadPointer(3);
                        for (int i = 0; i < ab.getNumSamples(); ++i)
                        {
                            wx += (double)w[i] * x[i];
                            wy += (double)w[i] * y[i];
                            ww += (double)w[i] * w[i];
                        }
                    }
                    for (int b = 0; b < 60; ++b)
                    {
                        juce::MidiBuffer mb;
                        if (b == 0)
                            mb.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                        ab.clear();
                        proc->processBlock(ab, mb);
                    }
                    level = ww;
                    return juce::radiansToDegrees(std::atan2(wy, wx));
                };
                double l0 = 0.0, l1 = 0.0;
                const double a0 = direction(0.0f, l0), a1 = direction(90.0f, l1);
                proc->engine.viewYaw = 0.0f;
                const double turned = rysum::wrapAz((float)(a1 - a0));
                say("AmbiX out: the scene at " + juce::String(a0, 1) + " deg, with the head turned 90 deg left at " +
                        juce::String(a1, 1) + " deg (turned " + juce::String(turned, 1) + ", expected -90)",
                    l0 > 1.0e-6 && l1 > 1.0e-6 && std::abs(turned + 90.0) < 5.0);
                setReal("output", 0.0f);
            }
        }
    }

    void timerCallback() override
    {
        if (proc->getLibrary() == nullptr && ++waited < 200)
            return;
        if (std::getenv("RYSUM_ONLY_MTS") != nullptr || std::getenv("RYSUM_ONLY_SCALA") != nullptr)
        {
            stopTimer();
            if (std::getenv("RYSUM_ONLY_SCALA") != nullptr)
                testScalaTuning();
            else
                testMidiTuning();
            quit();
            return;
        }
        if (std::getenv("RYSUM_ONLY_CONCERT") != nullptr)
        {
            stopTimer();
            testConcert();
            quit();
            return;
        }
        if (std::getenv("RYSUM_CONCERT_SERVE") != nullptr)
        {
            stopTimer();
            serveConcert();
            quit();
            return;
        }
        if (std::getenv("RYSUM_ONLY_WALK") != nullptr)
        {
            stopTimer();
            walkShots();
            std::cout << "walk: view gone" << std::endl;
            quit();
            return;
        }
        if (stage == 0)
        {
            std::cout << "status: " << proc->getLoadStatus() << std::endl;
            // a synthetic 4-channel AmbiX IR to exercise the room path
            {
                // like a host: main input and side chain stereo, 5th-order output
                juce::AudioProcessor::BusesLayout lay;
                lay.inputBuses.add(juce::AudioChannelSet::stereo());
                lay.inputBuses.add(juce::AudioChannelSet::stereo());
                lay.outputBuses.add(juce::AudioChannelSet::ambisonic(5));
                const bool okLayout = proc->setBusesLayout(lay);
                proc->setRateAndBufferSizeDetails(48000.0, 512);
                std::cout << "bus layout with inputs: "
                          << (okLayout && proc->getTotalNumInputChannels() == 4 ? "ok" : "FAIL")
                          << std::endl;
            }
            proc->prepareToPlay(48000.0, 512);
            juce::AudioBuffer<float> ir(4, 48000);
            juce::Random rnd(5);
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < 48000; ++i)
                    ir.setSample(c, i,
                                 (rnd.nextFloat() * 2 - 1) *
                                     std::exp(-5.0f * static_cast<float>(i) / 48000.0f));
            irFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("Testkirche_ambix.wav");
            irFile.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(irFile.createOutputStream());
            if (auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions()
                                                     .withSampleRate(44100.0)
                                                     .withNumChannels(4)
                                                     .withBitsPerSample(24)))
                w->writeFromAudioSampleBuffer(ir, 0, ir.getNumSamples());
            proc->loadIr(irFile);
            stage = 1;
            waited = 0;
            return;
        }
        if (!proc->getIrStatus().contains("AmbiX") && ++waited < 100)
            return;
        stopTimer();
        std::cout << "ir: " << proc->getIrStatus() << std::endl;

        // play something so held keys / sounding pipes / meters show
        for (auto *id : {"stop0", "stop1", "stop2", "stop6"})
            proc->apvts.getParameter(id)->setValueNotifyingHost(1.0f);
        proc->apvts.getParameter("motion6")->setValueNotifyingHost(
            proc->apvts.getParameter("motion6")->convertTo0to1(1.0f));
        juce::AudioBuffer<float> buf(36, 512);
        for (int b = 0; b < 40; ++b)
        {
            juce::MidiBuffer mb;
            if (b == 2)
                for (int m : {57, 64, 69})
                    mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
            if (b == 1)
            {
                proc->apvts.getParameter("swind2")->setValueNotifyingHost(0.4f);
                proc->apvts.getParameter("vol1")->setValueNotifyingHost(0.6f);
            }
            buf.clear(), proc->processBlock(buf, mb);
        }
        // the room must add sound: same phrase with and without IR, compare the energy
        {
            proc->apvts.getParameter("wind")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("click")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("releaseClick")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("output")->setValueNotifyingHost(
                proc->apvts.getParameter("output")->convertTo0to1(1.0f));
            auto phrase = [&]()
            {
                double e = 0;
                for (int b = 0; b < 200; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 10)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    if (b == 40)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    buf.clear(), proc->processBlock(buf, mb);
                    for (int c = 0; c < 4; ++c)
                        for (int i = 0; i < 512; ++i)
                            e += (double)buf.getSample(c, i) * buf.getSample(c, i);
                }
                return e;
            };
            const double withIr = phrase();
            proc->clearIr();
            {
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
            }
            const double dry = phrase();
            std::cout << "energy with IR / dry: " << withIr / dry
                      << (withIr > dry * 1.2 ? "  ok" : "  FAIL") << std::endl;
            proc->loadIr(irFile);
        }

        // typed values
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            struct T
            {
                const char *id;
                const char *txt;
                float expect;
            };
            const T tests[]{{"click", "50", 0.5f},     {"click", "35 %", 0.35f},
                            {"lfo1Rate", "1,5", 1.5f}, {"env1Attack", "2 s", 2.0f},
                            {"rate0", "-2.5", -2.5f},  {"degRate", "22", 22000.0f},
                            {"mod1Amt", "-40", -0.4f}, {"master", "-6", -6.0f}};
            bool ok = true;
            for (auto &t : tests)
            {
                const float v = rysum::ui::parseValueText(*P(t.id), t.txt);
                if (std::abs(v - t.expect) > 1e-3f * std::max(1.0f, std::abs(t.expect)))
                {
                    ok = false;
                    std::cout << "  parse " << t.id << " '" << t.txt << "' -> " << v << std::endl;
                }
            }
            // ms when the parameter shows ms
            P("env1Attack")->setValueNotifyingHost(P("env1Attack")->convertTo0to1(0.3f));
            ok = ok && std::abs(rysum::ui::parseValueText(*P("env1Attack"), "500") - 0.5f) < 1e-4f;
            P("env1Attack")->setValueNotifyingHost(P("env1Attack")->getDefaultValue());
            const float cents = rysum::ui::parseValueText(*P("pitch"), "440");
            ok = ok && std::abs(cents - 1200.0f * std::log2(440.0f / (float)rysum::kA4)) < 0.1f;
            std::cout << "typed values: " << (ok ? "ok" : "FAIL") << std::endl;
        }

        // binaural: output on "Binaural", decoder designed in the background, 2-channel host buffer
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            const float outBefore = P("output")->getValue();
            P("output")->setValueNotifyingHost(
                P("output")->convertTo0to1((float)RysumProcessor::kBinauralOutput));
            auto pump = [&](std::function<bool()> done)
            {
                for (int i = 0; i < 100 && !done(); ++i)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            };
            pump([&] { return proc->binauralReady(); });
            std::cout << "binaural: " << proc->getBinauralStatus()
                      << (proc->binauralReady() ? "  ok" : "  FAIL") << std::endl;
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(0.0f);
            const float irWetBefore = P("irWet")->getValue();
            P("irWet")->setValueNotifyingHost(0.0f);
            auto play = [&](int ch)
            {
                juce::AudioBuffer<float> st(ch, 512);
                double l = 0, r = 0, rest = 0;
                for (int b = 0; b < 260; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0)
                        for (int m : {57, 60, 64, 69})
                            mb.addEvent(juce::MidiMessage::noteOff(1, m),
                                        0); // earlier tests left keys down
                    if (b == 150)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                    if (b == 250)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                    st.clear(), proc->processBlock(st, mb);
                    if (b > 170 && b < 250)
                        for (int i = 0; i < 512; ++i)
                        {
                            l += (double)st.getSample(0, i) * st.getSample(0, i);
                            if (ch > 1)
                                r += (double)st.getSample(1, i) * st.getSample(1, i);
                            for (int c = 2; c < ch; ++c)
                                rest += (double)st.getSample(c, i) * st.getSample(c, i);
                        }
                }
                return std::array<double, 3>{l, r, rest};
            };
            const auto e2 = play(2), e36 = play(36);
            const double pipeAz =
                proc->engine.currentPosition(0, rysum::keyIndexForMidi(69), proc->readParams()).az;
            std::cout << "binaural Praestant a' (az " << pipeAz << "): L/R "
                      << 10 * std::log10(e2[0] / e2[1]) << " dB, 36-ch bus: L/R "
                      << 10 * std::log10(e36[0] / e36[1]) << " dB, other channels " << e36[2]
                      << ((e2[0] > 1e-6 &&
                           (pipeAz < 0 ? e2[1] > 1.5 * e2[0] : e2[0] > 1.5 * e2[1]) &&
                           e36[2] == 0.0)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            // an own SOFA file (the full FABIAN set)
            const juce::File sofa(
                juce::SystemStats::getEnvironmentVariable("RYSUM_TEST_SOFA", {})); // optional
            if (sofa.existsAsFile())
            {
                proc->setSofaFile(sofa);
                pump([&] { return proc->getBinauralStatus().contains("11950"); });
                std::cout << "own SOFA: " << proc->getBinauralStatus()
                          << (proc->getBinauralStatus().contains("11950") ? "  ok" : "  FAIL")
                          << std::endl;
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                std::cout << "SOFA path in state: "
                          << (proc->getXmlFromBinary(mb.getData(), (int)mb.getSize())
                                      ->toString()
                                      .contains("HATO_0_48k.sofa")
                                  ? "ok"
                                  : "FAIL")
                          << std::endl;
                P("binOrder")->setValueNotifyingHost(P("binOrder")->convertTo0to1(4.0f));
                pump([&] { return proc->getBinauralStatus().contains("5. Ordnung"); });
                std::cout << "5th order: " << proc->getBinauralStatus()
                          << (proc->getBinauralStatus().contains("5. Ordnung") ? "  ok" : "  FAIL")
                          << std::endl;
                const auto e5 = play(2);
                std::cout << "5th order own SOFA: L/R " << 10 * std::log10(e5[0] / e5[1]) << " dB"
                          << std::endl;
                proc->setSofaFile({});
                P("binOrder")->setValueNotifyingHost(P("binOrder")->getDefaultValue());
                pump(
                    [&]
                    {
                        return proc->getBinauralStatus().startsWith("FABIAN") &&
                               proc->getBinauralStatus().contains("3. Ordnung");
                    });
            }
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(1.0f);
            P("irWet")->setValueNotifyingHost(irWetBefore);
            P("output")->setValueNotifyingHost(outBefore);
        }

        // "Halten": two quick strikes keep the note down, a third strike lets it go; computer
        // keyboard notes
        {
            juce::AudioBuffer<float> st(2, 512);
            auto block = [&](std::initializer_list<juce::MidiMessage> msgs)
            {
                juce::MidiBuffer mb;
                int pos = 0;
                for (auto &m : msgs)
                    mb.addEvent(m, pos += 40);
                st.clear(), proc->processBlock(st, mb);
            };
            const int k62 = rysum::keyIndexForMidi(62);
            auto held = [&] { return (proc->engine.heldMask.load() >> k62) & 1u; };
            proc->latchOn = true;
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62),
                   juce::MidiMessage::noteOn(1, 62, (juce::uint8)90)});
            for (int i = 0; i < 5; ++i)
                block({});
            block({juce::MidiMessage::noteOff(1, 62)});
            for (int i = 0; i < 50; ++i)
                block({});
            const bool latchedOk = held() != 0 && proc->latchedCount.load() == 1;
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90)});
            const bool stillDown = held() != 0;
            block({juce::MidiMessage::noteOff(1, 62)});
            const bool released = held() == 0 && proc->latchedCount.load() == 0;
            // single strike: normal
            block({juce::MidiMessage::noteOn(1, 64, (juce::uint8)90)});
            for (int i = 0; i < 40; ++i)
                block({});
            block({juce::MidiMessage::noteOff(1, 64)});
            const bool normal =
                ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi(64)) & 1u) == 0;
            // switching it off lets everything go
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62),
                   juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62)});
            const bool again = held() != 0;
            proc->latchOn = false;
            block({});
            const bool offReleases = held() == 0;
            std::cout << "Halten: latched " << latchedOk << ", third strike keeps it until key up "
                      << stillDown << ", released " << released << ", single strike normal "
                      << normal << ", switching off releases " << (again && offReleases)
                      << ((latchedOk && stillDown && released && normal && again && offReleases)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            proc->keyboardState.noteOn(1, 60, 0.8f);
            block({});
            const bool kbOn =
                ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi(60)) & 1u) != 0;
            proc->keyboardState.noteOff(1, 60, 0.0f);
            block({});
            std::cout << "on-screen / computer keyboard notes reach the engine: "
                      << (kbOn ? "ok" : "FAIL") << std::endl;
        }

        // patches
        {
            auto files = RysumProcessor::getPatchFiles();
            std::cout << "factory patches: " << files.size()
                      << (files.size() >= 6 ? "  ok" : "  FAIL") << std::endl;
            juce::File welle;
            for (auto &f : files)
                if (f.getFileName().contains("Welle"))
                    welle = f;
            const float outBefore = proc->apvts.getRawParameterValue("output")->load();
            const bool loaded = proc->loadPatch(welle);
            std::cout << "load factory patch: "
                      << ((loaded && proc->getRouteSrc(0) == rysum::srcLfo2 &&
                           proc->getRouteTgt(0) == rysum::tgtAz &&
                           proc->apvts.getRawParameterValue("stop0")->load() > 0.5f &&
                           juce::exactlyEqual(proc->apvts.getRawParameterValue("output")->load(),
                                              outBefore))
                              ? "ok"
                              : "FAIL")
                      << std::endl;
            auto *w = proc->apvts.getParameter("wind");
            w->setValueNotifyingHost(0.33f);
            proc->savePatch("Test Patch");
            w->setValueNotifyingHost(0.9f);
            proc->setRoute(0, -1, -1);
            proc->loadPatch(RysumProcessor::getPatchFolder().getChildFile("Test Patch.rysum"));
            std::cout << "save / load patch: "
                      << ((std::abs(w->getValue() - 0.33f) < 0.01f &&
                           proc->getRouteSrc(0) == rysum::srcLfo2 &&
                           proc->getPatchName() == "Test Patch")
                              ? "ok"
                              : "FAIL")
                      << std::endl;
            w->setValueNotifyingHost(0.0f);
        }

        // every factory patch: play a chord through it (with the test room loaded), report the
        // level
        {
            std::cout << "factory warnings: "
                      << (proc->factoryWarnings.isEmpty()
                              ? juce::String("none")
                              : proc->factoryWarnings.joinIntoString("; "))
                      << std::endl;
            proc->loadIr(irFile);
            for (int w = 0; w < 100 && !proc->getIrStatus().contains("AmbiX"); ++w)
                juce::Thread::sleep(20);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            bool allOk = true;
            // RYSUM_QUICK: skip playing every factory patch (for quick looks at the pages)
            const bool quick = std::getenv("RYSUM_QUICK") != nullptr;
            // RYSUM_PATCHES_FROM=n: only the patches numbered n and up (a new set)
            const int fromNum = std::getenv("RYSUM_PATCHES_FROM") != nullptr
                                    ? juce::String(std::getenv("RYSUM_PATCHES_FROM")).getIntValue()
                                    : 0;
            bool roomsOk = true, knocksOk = true;
            for (auto &f : quick ? juce::Array<juce::File>() : RysumProcessor::getPatchFiles())
            {
                const int num = f.getFileNameWithoutExtension().getIntValue();
                if (num < fromNum)
                    continue;
                proc->loadPatch(f);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                // a patch that brings a room for the 3D view: that model loaded, the Saal its size,
                // the light as it says
                juce::String roomNote;
                if (auto xml = juce::XmlDocument::parse(f))
                    if (auto *ex = xml->getChildByName("EXTRA"))
                        if (auto *s3 = ex->getChildByName("SAAL3D"))
                        {
                            const auto want = s3->getStringAttribute("room");
                            auto m = proc->getWalkMesh();
                            const bool okRoom =
                                want.isEmpty() ||
                                (m != nullptr && proc->walkModelFile.getFileName() == want &&
                                 std::abs(proc->apvts.getRawParameterValue("simW")->load() -
                                          juce::jlimit(2.0f, 40.0f, m->size.x)) < 0.05f &&
                                 std::abs(proc->apvts.getRawParameterValue("simH")->load() -
                                          juce::jlimit(2.0f, 40.0f, m->size.z)) < 0.05f);
                            const bool okDark = !s3->hasAttribute("dark") ||
                                                s3->getBoolAttribute("dark") == proc->walkDark;
                            roomsOk = roomsOk && okRoom && okDark;
                            roomNote = ", room " + want + (proc->walkDark ? " (dark)" : "") +
                                       (okRoom && okDark ? "" : " NOT LOADED");
                        }
                const uint32_t impactsBefore = proc->engine.impactCount.load();
                const bool wantKnocks =
                    num >= 160 && proc->apvts.getRawParameterValue("lvImpLevel")->load() > -59.9f;
                float peak = 0.0f;
                double e = 0.0;
                bool finite = true;
                long cnt = 0;
                int maxGrains = 0;
                float maxErode = 0.0f;
                const bool feedInput = proc->apvts.getRawParameterValue("inpOn")->load() > 0.5f;
                rysum::XorShift noise(11);
                // the routes that are on when the patch is loaded must stay on (scenes used to
                // switch them off) (taken from the file: loading has already applied scene A)
                std::vector<int> liveRoutes;
                if (auto xml = juce::XmlDocument::parse(f))
                    for (int r = 0; r < rysum::kNumRoutes; ++r)
                        for (auto *e : xml->getChildWithTagNameIterator("PARAM"))
                            if (e->getStringAttribute("id") == proc->getModAmountId(r) &&
                                std::abs(e->getDoubleAttribute("value")) > 1.0e-4 &&
                                proc->getRouteSrc(r) >= 0)
                                liveRoutes.push_back(r);
                for (int b = 0; b < 600; ++b) // 6.4 s
                {
                    juce::MidiBuffer mb;
                    if (b == 5)
                        for (int m : {41, 53, 57, 60, 65, 69, 72})
                            mb.addEvent(
                                juce::MidiMessage::noteOn(1, m, (juce::uint8)(b % 2 ? 60 : 110)),
                                0);
                    // the hands: mod wheel up and down, pitch wheel swinging, while the chord
                    // sounds
                    if (b >= 60 && b < 420 && b % 6 == 0)
                    {
                        const double ph = (double)(b - 60) / 360.0;
                        mb.addEvent(
                            juce::MidiMessage::controllerEvent(
                                1, 1,
                                juce::jlimit(
                                    0, 127,
                                    (int)std::round(
                                        127.0 * std::sin(juce::MathConstants<double>::pi * ph)))),
                            0);
                        mb.addEvent(
                            juce::MidiMessage::pitchWheel(
                                1, juce::jlimit(
                                       0, 16383,
                                       8192 + (int)(8000.0 *
                                                    std::sin(juce::MathConstants<double>::twoPi *
                                                             2.0 * ph)))),
                            1);
                    }
                    if (b == 420)
                    {
                        mb.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 0);
                        mb.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
                    }
                    if (b % 20 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(
                            2); // the scene timer (moving through the scenes)
                    if (b == 420)
                        for (int m : {41, 53, 57, 60, 65, 69, 72})
                            mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    buf.clear();
                    if (feedInput)
                        for (int c = 0; c < 2; ++c)
                            for (int i = 0; i < 512; ++i)
                                buf.setSample(
                                    c, i,
                                    0.1f * noise.bi()); // a patch for the input: something to hear
                    proc->processBlock(buf, mb);
                    maxGrains = std::max(maxGrains, proc->engine.cloudGrainsNow.load());
                    maxErode = std::max(maxErode, proc->engine.spaceErosionNow.load());
                    for (int c = 0; c < 4; ++c)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample(c, i);
                            if (!std::isfinite(v))
                                finite = false;
                            peak = std::max(peak, std::abs(v));
                            if (c == 0)
                            {
                                e += (double)v * v;
                                ++cnt;
                            }
                        }
                }
                juce::MidiBuffer off;
                for (int m = 36; m < 82; ++m)
                    off.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                buf.clear(), proc->processBlock(buf, off);
                for (int b = 0; b < 300; ++b)
                {
                    juce::MidiBuffer none;
                    buf.clear(), proc->processBlock(buf, none);
                } // let tails / freeze die
                const float rmsDb = juce::Decibels::gainToDecibels(
                    (float)std::sqrt(e / static_cast<double>(juce::jmax(1L, cnt))));
                bool routesKept = true;
                for (int r : liveRoutes)
                    if (std::abs(
                            proc->apvts.getRawParameterValue(proc->getModAmountId(r))->load()) <
                        1.0e-4f)
                        routesKept = false;
                const uint32_t impacts = proc->engine.impactCount.load() - impactsBefore;
                if (wantKnocks && impacts == 0)
                    knocksOk = false;
                const bool ok = finite && peak > 1.0e-4f && routesKept;
                allOk = allOk && ok;
                if (feedInput && std::getenv("RYSUM_DEBUG_INPUT") != nullptr)
                {
                    const auto pp = proc->readParams();
                    std::cout << "    input: on " << pp.input.on << " level " << pp.input.level
                              << " dist " << pp.input.dist << " shown "
                              << proc->engine.inShowCount.load() << " peak "
                              << proc->engine.inPeak.load() << " out " << pp.outputMode
                              << std::endl;
                }
                if (auto *io = proc->apvts.getParameter("inpOn"))
                    io->setValueNotifyingHost(0.0f); // the next patch: without the input again
                std::cout << "  " << f.getFileNameWithoutExtension()
                          << (feedInput ? " [Eingang]" : "") << ": peak "
                          << juce::Decibels::gainToDecibels(peak) << " dB, rms W " << rmsDb << " dB"
                          << (maxGrains > 0 ? ", grains up to " + juce::String(maxGrains)
                                            : juce::String())
                          << (maxErode > 0.0f ? ", erosion up to " + juce::String(maxErode, 2)
                                              : juce::String())
                          << (routesKept ? "" : ", a route was switched off")
                          << (impacts > 0 ? ", knocks " + juce::String((int)impacts) : juce::String())
                          << (wantKnocks && impacts == 0 ? ", NO KNOCKS" : "") << roomNote
                          << (ok ? "" : "  FAIL") << std::endl;
            }
            std::cout << "all factory patches play: " << (allOk ? "ok" : "FAIL") << std::endl;
            if (!quick)
            {
                std::cout << "patches with a room load it: " << (roomsOk ? "ok" : "FAIL")
                          << std::endl;
                std::cout << "the 3D Saal's patches knock: " << (knocksOk ? "ok" : "FAIL")
                          << std::endl;
                // one's own patch saved in a built-in room keeps the room and the light
                if (rysum::builtInRoomIndex(proc->walkModelFile.getFileName()) >= 0 &&
                    proc->getWalkMesh() != nullptr)
                {
                    const auto had = proc->walkModelFile.getFileName();
                    const bool dark = proc->walkDark;
                    proc->savePatch("Test Raum");
                    const auto tf = RysumProcessor::getPatchFolder().getChildFile("Test Raum.rysum");
                    bool kept = false;
                    if (auto xml = juce::XmlDocument::parse(tf))
                        if (auto *ex = xml->getChildByName("EXTRA"))
                            if (auto *s3 = ex->getChildByName("SAAL3D"))
                                kept = s3->getStringAttribute("room") == had &&
                                       s3->getBoolAttribute("dark") == dark;
                    tf.deleteFile();
                    std::cout << "a saved patch keeps its built-in room: " << (kept ? "ok" : "FAIL")
                              << std::endl;
                }
            }

            // set 14 (playing with the hands): how much one note a' changes with velocity, mod
            // wheel and pitch wheel (W channel, first 1.5 s: level difference, and the share of the
            // highs (first difference) as brightness)
            for (auto &f : quick ? juce::Array<juce::File>() : RysumProcessor::getPatchFiles())
            {
                const int num = f.getFileNameWithoutExtension().getIntValue();
                if (num < 118 || num > 133)
                    continue;
                auto play = [&](int vel, int wheel, int bend, double &level, double &bright,
                                juce::AudioBuffer<float> *keep)
                {
                    proc->loadPatch(f);
                    juce::MidiBuffer pre;
                    pre.addEvent(juce::MidiMessage::controllerEvent(1, 1, wheel), 0);
                    pre.addEvent(juce::MidiMessage::pitchWheel(1, bend), 0);
                    buf.clear();
                    proc->processBlock(buf, pre);
                    for (int w = 0; w < 6; ++w)
                    {
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
                        juce::MidiBuffer none;
                        buf.clear();
                        proc->processBlock(buf, none);
                    }
                    double e = 0, d = 0;
                    float prev = 0.0f;
                    for (int b = 0; b < 141; ++b)
                    {
                        juce::MidiBuffer mb;
                        if (b == 0)
                            mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)vel), 0);
                        buf.clear();
                        proc->processBlock(buf, mb);
                        if (b % 20 == 0)
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample(0, i);
                            e += (double)v * v;
                            d += (double)(v - prev) * (v - prev);
                            prev = v;
                        }
                        if (keep != nullptr)
                            for (int i = 0; i < 512 && b * 512 + i < keep->getNumSamples(); ++i)
                                keep->setSample(0, b * 512 + i, buf.getSample(0, i));
                    }
                    juce::MidiBuffer off;
                    for (int m = 36; m < 82; ++m)
                        off.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    off.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 1);
                    off.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 1);
                    buf.clear();
                    proc->processBlock(buf, off);
                    for (int b = 0; b < 400; ++b)
                    {
                        juce::MidiBuffer none;
                        buf.clear();
                        proc->processBlock(buf, none);
                    }
                    level = 10.0 * std::log10(juce::jmax(1e-30, e));
                    bright = 10.0 * std::log10(juce::jmax(1e-30, d) / juce::jmax(1e-30, e));
                };
                juce::AudioBuffer<float> a(1, 141 * 512), b2(1, 141 * 512);
                auto diffDb =
                    [](const juce::AudioBuffer<float> &x, const juce::AudioBuffer<float> &y)
                {
                    double dd = 0, ee = 0;
                    for (int i = 0; i < x.getNumSamples(); ++i)
                    {
                        const double t = x.getSample(0, i) - y.getSample(0, i);
                        dd += t * t;
                        ee += (double)y.getSample(0, i) * y.getSample(0, i);
                    }
                    return 10.0 * std::log10(juce::jmax(1e-30, dd) / juce::jmax(1e-30, ee));
                };
                double l0, b0, l1, b1, lw, bw, lb, bb;
                play(25, 0, 8192, l0, b0, nullptr);
                play(120, 0, 8192, l1, b1, &a);
                play(120, 127, 8192, lw, bw, &b2);
                const double wheelDiff = diffDb(b2, a);
                play(120, 0, 0, lb, bb, &b2);
                const double bendDiff = diffDb(b2, a);
                std::cout << "  expression " << f.getFileNameWithoutExtension()
                          << ": velocity 25 -> 120: level " << juce::String(l1 - l0, 1)
                          << " dB, brightness " << juce::String(b1 - b0, 1)
                          << " dB; mod wheel 0 -> 127: level " << juce::String(lw - l1, 1)
                          << " dB, brightness " << juce::String(bw - b1, 1) << " dB, change "
                          << juce::String(wheelDiff, 1) << " dB; pitch wheel down: change "
                          << juce::String(bendDiff, 1) << " dB" << std::endl;
            }
            // back to a plain state for the rest of the test
            proc->setRoute(0, -1, -1);
        }

        // modulation: LFO 1 -> global wind (a parameter), LFO 2 -> pipe direction with spread,
        // envelope -> distance
        {
            using namespace rysum;
            auto setP = [&](const char *id, float v)
            {
                auto *a = proc->apvts.getParameter(id);
                a->setValueNotifyingHost(a->convertTo0to1(v));
            };
            const int windIdx = proc->getParamIndex("wind");
            proc->setRoute(0, srcLfo1, kGlobalTargetBase + windIdx);
            setP("mod1Amt", 0.4f);
            proc->setRoute(1, srcLfo2, tgtAz);
            setP("mod2Amt", 0.5f);
            proc->setRoute(2, srcEnv1, tgtDist);
            setP("mod3Amt", 0.3f);
            proc->setRoute(3, srcLfo3, kGlobalTargetBase + proc->getParamIndex("irWet"));
            setP("mod4Amt", -0.3f);
            setP("lfo1Rate", 2.0f);
            setP("lfo2Spread", 1.0f);
            setP("lfo2Rate", 0.7f);
            setP("lfo4Spread", 0.6f);
            setP("lfo3Shape", 4.0f);
            float lo = 2, hi = -1;
            for (int b = 0; b < 120; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 1)
                    for (int m : {45, 52, 57, 64, 69, 72})
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                buf.clear(), proc->processBlock(buf, mb);
                const float n = proc->getModulatedNorm(windIdx);
                if (!std::isnan(n))
                {
                    lo = juce::jmin(lo, n);
                    hi = juce::jmax(hi, n);
                }
            }
            for (int b = 0; b < 200 && !(proc->getModulatedNorm(windIdx) > 0.3f); ++b)
            {
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
            }
            const float base = proc->apvts.getParameter("wind")->getValue();
            std::cout << "wind modulated: base " << base << " range " << lo << " .. " << hi
                      << ((hi - lo > 0.3f && lo <= base + 1e-4f && hi > base) ? "  ok" : "  FAIL")
                      << std::endl;
            {
                auto prm = proc->readParams();
                // a silent pipe (Praestant C, not played) and a sounding one (a)
                float dSilent = 0, dSound = 0;
                for (int k : {0, 20})
                {
                    const auto a = proc->engine.currentPosition(0, k, prm),
                               b = proc->engine.displayPosition(0, k, prm);
                    (k == 0 ? dSilent : dSound) =
                        std::abs(rysum::wrapAz(b.az - a.az)) + std::abs(b.dist - a.dist);
                }
                std::cout << "raum shows modulation: silent " << dSilent << ", sounding " << dSound
                          << ((dSilent > 0.5f && dSound > 0.5f) ? "  ok" : "  FAIL") << std::endl;
                proc->setRoute(19, rysum::srcLfo4, rysum::tgtEl);
                setP("mod20Amt", 0.2f);
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
                auto prm2 = proc->readParams();
                std::cout << "slot 20: "
                          << ((prm2.mod.routes[19].src == rysum::srcLfo4 &&
                               std::abs(prm2.mod.routes[19].amount - 0.2f) < 1e-4f)
                                  ? "ok"
                                  : "FAIL")
                          << std::endl;
            }
            std::cout << "not modulated -> NaN: "
                      << (std::isnan(proc->getModulatedNorm(proc->getParamIndex("air"))) ? "ok"
                                                                                         : "FAIL")
                      << std::endl;
        }

        // Verlauf (automation): a lane takes its parameter over at the clock's time, a switch
        // steps, a held control makes its lane give way, a hand records, the project keeps it
        {
            using namespace rysum;
            juce::AudioBuffer<float> abuf(proc->getTotalNumOutputChannels(), 256);
            juce::MidiBuffer none;
            auto block = [&]
            {
                abuf.clear();
                proc->processBlock(abuf, none);
            };
            const int air = proc->getParamIndex("air"), stop = proc->getParamIndex("stop2");
            auto &S = proc->getAuto();
            S = AutoSet();
            AutoLane la;
            la.paramId = "air";
            la.pts = {{0.0, 0.2f}, {8.0, 0.8f}};
            AutoLane ls;
            ls.paramId = "stop2";
            AutoPoint s0{0.0, 0.0f}, s1{2.0, 1.0f};
            s0.shape = asStep;
            ls.pts = {s0, s1};
            S.clock = 1; // own clock
            S.lanes = {la, ls};
            proc->autoPublish();
            proc->autoPlay(false);
            proc->autoSeek(4.0);
            block();
            block();
            const float a4 = proc->getAutomatedNorm(air), m4 = proc->getModulatedNorm(air);
            const float st4 = proc->getAutomatedNorm(stop);
            proc->autoSeek(1.0);
            block();
            const float st1 = proc->getAutomatedNorm(stop);
            std::cout << "verlauf: air at beat 4 = " << a4 << " (mod " << m4 << "), stop 2 at beat 1 / 4 = "
                      << st1 << " / " << st4
                      << ((std::abs(a4 - 0.5f) < 0.01f && std::abs(m4 - 0.5f) < 0.01f && st1 == 0.0f &&
                           st4 == 1.0f)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            // the own clock runs: 60 BPM = one beat per second
            proc->autoSeek(0.0);
            proc->autoPlay(true);
            block();
            const double t0 = proc->getAutoNow();
            const int nb = (int)std::round(proc->getSampleRate() / 256.0);
            for (int i = 0; i < nb; ++i)
                block();
            const double t1 = proc->getAutoNow();
            std::cout << "verlauf clock: one second = " << (t1 - t0) << " beats"
                      << (std::abs(t1 - t0 - 1.0) < 0.02 ? "  ok" : "  FAIL") << std::endl;
            // the tempo lane drives the own clock (120 BPM: two beats a second); a repeated section
            // (A-B) keeps the clock inside it; solo: only the solo lane acts
            {
                AutoLane tl;
                tl.paramId = kAutoTempoId;
                tl.pts = {{0.0, autoTempoNorm(120.0)}};
                S.lanes.push_back(tl);
                proc->autoPublish();
                proc->autoSeek(0.0);
                block();
                const double u0 = proc->getAutoNow();
                for (int i = 0; i < nb; ++i)
                    block();
                const double tempoBeats = proc->getAutoNow() - u0;
                S.lanes.pop_back();
                S.cycle = true;
                S.cycleStart = 1.0;
                S.cycleEnd = 3.0;
                proc->autoPublish();
                proc->autoSeek(1.0);
                double lo = 1.0e9, hi = -1.0e9;
                for (int i = 0; i < nb * 5; ++i)
                {
                    block();
                    lo = std::min(lo, proc->getAutoNow());
                    hi = std::max(hi, proc->getAutoNow());
                }
                S.cycle = false;
                S.lanes[1].solo = true; // stop 2 solo: air is not driven
                proc->autoPublish();
                block();
                block();
                const bool soloOk = std::isnan(proc->getAutomatedNorm(air)) && !std::isnan(proc->getAutomatedNorm(stop));
                S.lanes[1].solo = false;
                proc->autoPublish();
                block();
                const bool tempoOk = std::abs(tempoBeats - 2.0) < 0.05, cycleOk = lo >= 1.0 && hi < 3.0 && hi > 2.8;
                std::cout << "verlauf tempo lane: one second at 120 BPM = " << tempoBeats << " beats; A-B 1..3: clock "
                          << lo << " .. " << hi << "; solo " << (int)soloOk
                          << ((tempoOk && cycleOk && soloOk) ? "  ok" : "  FAIL") << std::endl;
            }
            // variation from pass to pass of a looping lane: chance 50 % moves in about half the
            // passes, "every 3rd" in exactly passes 2, 5, 8 ..., "Streuung" changes the value per
            // pass, and the same pass is always the same
            {
                AutoLane v;
                v.paramId = "air";
                v.loop = true;
                v.length = 4.0;
                v.pts = {{0.0, 0.2f}, {4.0, 0.8f}};
                v.pts[0].chance = 0.5f;
                int moved = 0;
                for (int p = 0; p < 200; ++p)
                    moved += autoValue(v, p * 4.0 + 2.0) > 0.3f ? 1 : 0;
                v.pts[0].chance = 1.0f;
                v.pts[0].every = 3;
                bool everyOk = true;
                for (int p = 0; p < 12; ++p)
                    everyOk = everyOk && ((autoValue(v, p * 4.0 + 2.0) > 0.3f) == (p % 3 == 2));
                v.pts[0].every = 1;
                v.pts[0].vary = 0.2f;
                const float va = autoValue(v, 0.0), vb = autoValue(v, 4.0), vc = autoValue(v, 8.0);
                const bool varyOk = (std::abs(va - vb) > 1e-3f || std::abs(vb - vc) > 1e-3f) &&
                                    autoValue(v, 4.0) == vb && va >= 0.0f && va <= 0.4f;
                // saved and loaded: the variations, the markers, the section
                AutoSet vs;
                v.pts[0].chance = 0.7f;
                v.pts[0].every = 2;
                vs.lanes = {v};
                vs.markers = {{4.0, "A"}, {12.0, "Schluss"}};
                vs.cycle = true;
                vs.cycleStart = 4.0;
                vs.cycleEnd = 12.0;
                const auto back = autoFromTree(autoToTree(vs));
                const bool savedOk = back.markers.size() == 2 && back.markers[1].name == "Schluss" && back.cycle &&
                                     back.cycleEnd == 12.0 && back.lanes.size() == 1 &&
                                     std::abs(back.lanes[0].pts[0].chance - 0.7f) < 1e-3f &&
                                     back.lanes[0].pts[0].every == 2 && std::abs(back.lanes[0].pts[0].vary - 0.2f) < 1e-3f;
                std::cout << "verlauf variation: chance 50 % moved in " << moved << " of 200 passes, every 3rd "
                          << (int)everyOk << ", Streuung " << (int)varyOk << ", saved " << (int)savedOk
                          << ((moved > 70 && moved < 130 && everyOk && varyOk && savedOk) ? "  ok" : "  FAIL")
                          << std::endl;
            }
            // loop, stretch, reverse: pure functions of the time
            {
                AutoLane l = la;
                l.length = 8.0;
                l.loop = true;
                const bool loopOk = std::abs(autoValue(l, 2.0) - autoValue(l, 10.0)) < 1e-5f;
                l.speed = 2.0f;
                const bool speedOk = std::abs(autoValue(l, 2.0) - autoValue(la, 4.0)) < 1e-5f;
                l.speed = 1.0f;
                l.reverse = true;
                const bool revOk = std::abs(autoValue(l, 2.0) - autoValue(la, 6.0)) < 1e-5f;
                AutoLane w = la;
                w.pts[0].shape = asJitter;
                w.pts[0].depth = 0.5f;
                const bool jitterSame = autoValue(w, 3.3) == autoValue(w, 3.3) &&
                                        std::abs(autoValue(w, 3.3) - autoValue(la, 3.3)) > 1e-4f;
                std::cout << "verlauf loop / stretch / reverse / jitter: " << loopOk << speedOk << revOk
                          << jitterSame << ((loopOk && speedOk && revOk && jitterSame) ? "  ok" : "  FAIL")
                          << std::endl;
            }
            // a hand on the control: the lane gives way; armed + recording: it writes
            auto *airP = proc->getParamAt(air);
            S.lanes[0].armed = true;
            proc->autoPublish();
            proc->setAutoRecord(true);
            airP->beginChangeGesture();
            airP->setValueNotifyingHost(0.9f);
            for (int i = 0; i < nb / 2; ++i)
                block();
            const bool gaveWay = std::isnan(proc->getAutomatedNorm(air));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200); // (the 40 Hz timer)
            airP->endChangeGesture();
            proc->setAutoRecord(false);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            int recorded = 0;
            for (auto &p : S.lanes[0].pts)
                if (std::abs(p.v - 0.9f) < 0.01f)
                    ++recorded;
            block();
            const bool back = !std::isnan(proc->getAutomatedNorm(air));
            std::cout << "verlauf hand: gives way " << gaveWay << ", recorded points at 0.9: " << recorded
                      << ", takes over again " << back << ((gaveWay && recorded >= 1 && back) ? "  ok" : "  FAIL")
                      << std::endl;
            // spreading: a pipe 2 m from the origin hears a step at beat 2 two beats later, one 6 m
            // away six beats later; through the engine, a silent pipe's shown direction follows
            {
                AutoLane w;
                w.paramId = pipeTargetId(tgtAz);
                w.spread = true;
                w.ox = w.oy = w.oz = 0.0f;
                w.waveSpeed = 1.0f;
                AutoPoint a0{0.0, 0.5f}, a1{2.0, 0.75f};
                a0.shape = asStep;
                w.pts = {a0, a1};
                const float nearV = autoValue(w, autoPipeTime(w, 5.0, 0.0f, 2.0f, 0.0f)),
                            farV = autoValue(w, autoPipeTime(w, 5.0, 0.0f, 6.0f, 0.0f));
                w.inward = true; // from 25 m in: the far pipe first
                const float farIn = autoValue(w, autoPipeTime(w, 22.0, 0.0f, 6.0f, 0.0f)),
                            nearIn = autoValue(w, autoPipeTime(w, 22.0, 0.0f, 2.0f, 0.0f));
                w.inward = false;
                AutoEngineView view;
                view.pipe[0] = {&w, tgtAz};
                view.numPipe = 1;
                auto prm = proc->readParams();
                const auto base = proc->engine.displayPosition(0, 3, prm);
                float bx = 0, by = 0, bz = 0;
                const auto home = proc->engine.currentPosition(0, 3, prm); // (what the lanes measure from)
                autoPosToXyz(home.az, home.el, home.dist, bx, by, bz);
                const double reach = std::sqrt(bx * bx + by * by + bz * bz); // beats until it arrives
                prm.autoView = &view;
                prm.autoBeats = 2.0 + reach - 0.5;
                const float before = proc->engine.displayPosition(0, 3, prm).az;
                prm.autoBeats = 2.0 + reach + 0.5;
                const float after = proc->engine.displayPosition(0, 3, prm).az;
                const bool engineOk = std::abs(wrapAz(before - base.az)) < 0.01f &&
                                      std::abs(wrapAz(after - before - 90.0f)) < 0.5f;
                // saved and loaded with the lane
                AutoSet ss;
                w.ox = 3.5f;
                w.waveSpeed = 7.0f;
                w.inward = true;
                ss.lanes = {w};
                const auto back = autoFromTree(autoToTree(ss));
                const bool savedOk = back.lanes.size() == 1 && back.lanes[0].spread &&
                                     std::abs(back.lanes[0].ox - 3.5f) < 1e-4f &&
                                     std::abs(back.lanes[0].waveSpeed - 7.0f) < 1e-4f && back.lanes[0].inward &&
                                     std::abs(back.lanes[0].speed - 1.0f) < 1e-6f;
                const bool ok = std::abs(nearV - 0.75f) < 1e-5f && std::abs(farV - 0.5f) < 1e-5f &&
                                std::abs(farIn - 0.75f) < 1e-5f && std::abs(nearIn - 0.5f) < 1e-5f &&
                                engineOk && savedOk;
                std::cout << "verlauf spreads: near " << nearV << " far " << farV << ", inward far " << farIn
                          << " near " << nearIn << ", engine " << engineOk << " (az " << before - base.az
                          << " -> " << after - base.az << "), saved " << savedOk << (ok ? "  ok" : "  FAIL")
                          << std::endl;
            }
            // a timetable: where along its drawn path a stop is comes from its lane
            {
                const auto pathBefore = proc->getDrawnPath(0);
                auto *motion0 = proc->apvts.getParameter(RysumProcessor::motionParamId(0));
                const float motionBefore = motion0->getValue();
                std::vector<RysumProcessor::DrawnPath::Pt> pts;
                for (int i = 0; i <= 20; ++i)
                    pts.push_back({-90.0f + 9.0f * (float)i, 10.0f, 0.1f * (float)i});
                proc->setDrawnPath(0, pts);
                proc->setPathOptions(0, false, false);
                motion0->setValueNotifyingHost(motion0->convertTo0to1((float)rysum::Motion::Drawn));
                AutoLane tl;
                tl.paramId = autoPathLaneId(0);
                tl.pts = {{0.0, 0.0f}, {8.0, 1.0f}};
                const auto lanesBefore = S.lanes;
                S.lanes.push_back(tl);
                proc->autoPublish();
                proc->autoPlay(false);
                proc->autoSeek(2.0);
                block();
                block();
                const double loopPh = proc->engine.motionPhase[0].load();
                proc->setPathOptions(0, true, false);
                proc->autoSeek(6.0);
                block();
                block();
                const double pingPh = proc->engine.motionPhase[0].load();
                S.lanes = lanesBefore;
                proc->autoPublish();
                proc->setDrawnPath(0, pathBefore.points);
                proc->setPathOptions(0, pathBefore.pingPong, pathBefore.drawnTiming);
                motion0->setValueNotifyingHost(motionBefore);
                block();
                const bool ok = std::abs(loopPh - 0.25) < 0.01 && std::abs(pingPh - 0.375) < 0.01;
                std::cout << "verlauf timetable: phase at beat 2 (loop) " << loopPh << ", at beat 6 (there and back) "
                          << pingPh << (ok ? "  ok" : "  FAIL") << std::endl;
            }
            // the host's clock: its song position (also standing still), else the own clock
            {
                struct FakeHead : juce::AudioPlayHead
                {
                    juce::Optional<PositionInfo> getPosition() const override
                    {
                        PositionInfo p;
                        if (hasPpq)
                            p.setPpqPosition(ppq);
                        p.setBpm(120.0);
                        p.setIsPlaying(playing);
                        return p;
                    }
                    double ppq = 4.0;
                    bool playing = false, hasPpq = true;
                } head;
                S.lanes[0].armed = false;
                S.lanes[0].pts = la.pts; // (without what the hand recorded)
                S.clock = 0;
                proc->autoPublish();
                proc->autoPlay(false);
                proc->setPlayHead(&head);
                block();
                block();
                const float stopped = proc->getAutomatedNorm(air);
                const bool hostNow = proc->isAutoHostClock(), stillNow = !proc->isAutoMoving();
                head.playing = true;
                head.ppq = 6.0;
                block();
                const float playing = proc->getAutomatedNorm(air);
                const bool moving = proc->isAutoMoving();
                head.hasPpq = false;
                head.playing = false;
                proc->autoSeek(2.0);
                block();
                const bool fallback = !proc->isAutoHostClock() && std::abs(proc->getAutoNow() - 2.0) < 1e-6;
                proc->setPlayHead(nullptr);
                const bool ok = std::abs(stopped - 0.5f) < 0.01f && hostNow && stillNow &&
                                std::abs(playing - 0.65f) < 0.01f && moving && fallback;
                std::cout << "verlauf host clock: at ppq 4 (stopped) " << stopped << ", ppq 6 (playing) "
                          << playing << ", no position -> own clock " << fallback << (ok ? "  ok" : "  FAIL")
                          << std::endl;
                S.clock = 1;
                proc->autoPublish();
            }
            // an LFO routed to the automated parameter swings around the curve, not the hand
            {
                airP->setValueNotifyingHost(0.95f);
                proc->setRoute(19, srcLfo1, kGlobalTargetBase + air);
                proc->apvts.getParameter("mod20Amt")->setValueNotifyingHost(
                    proc->apvts.getParameter("mod20Amt")->convertTo0to1(0.2f));
                proc->autoPlay(false);
                proc->autoSeek(4.0);
                float lo = 1.0f, hi = 0.0f;
                for (int i = 0; i < nb * 3; ++i)
                {
                    block();
                    const float n = proc->getModulatedNorm(air);
                    lo = juce::jmin(lo, n);
                    hi = juce::jmax(hi, n);
                }
                proc->setRoute(19, -1, -1);
                proc->apvts.getParameter("mod20Amt")->setValueNotifyingHost(
                    proc->apvts.getParameter("mod20Amt")->convertTo0to1(0.0f));
                airP->setValueNotifyingHost(airP->getDefaultValue());
                const bool ok = hi - lo > 0.2f && lo > 0.25f && hi < 0.75f;
                std::cout << "verlauf + LFO: range " << lo << " .. " << hi << " around the curve's 0.5"
                          << (ok ? "  ok" : "  FAIL") << std::endl;
            }
            // a lane pulls the only stop while a key is held: silent before, sound after
            {
                std::array<float, kNumStops> stopsBefore{};
                for (int s = 0; s < kNumStops; ++s)
                {
                    auto *sp = proc->apvts.getParameter(RysumProcessor::stopParamId(s));
                    stopsBefore[(size_t)s] = sp->getValue();
                    sp->setValueNotifyingHost(0.0f);
                }
                S.lanes.resize(2); // air, stop2 (0 until beat 2, then 1)
                proc->autoPublish();
                proc->autoSeek(0.0);
                proc->autoPlay(true);
                double before = 0.0, after = 0.0;
                juce::MidiBuffer on;
                on.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                abuf.clear();
                proc->processBlock(abuf, on);
                for (int i = 0; i < nb * 3; ++i)
                {
                    block();
                    const double now = proc->getAutoNow();
                    const double e = abuf.getRMSLevel(0, 0, abuf.getNumSamples());
                    if (now > 1.0 && now < 1.9)
                        before = juce::jmax(before, e);
                    if (now > 2.5)
                        after = juce::jmax(after, e);
                }
                juce::MidiBuffer off;
                off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                abuf.clear();
                proc->processBlock(abuf, off);
                proc->autoPlay(false);
                for (int s = 0; s < kNumStops; ++s)
                    proc->apvts.getParameter(RysumProcessor::stopParamId(s))
                        ->setValueNotifyingHost(stopsBefore[(size_t)s]);
                std::cout << "verlauf pulls a stop: rms before " << before << ", after " << after
                          << ((after > 1e-4 && after > before * 8.0) ? "  ok" : "  FAIL") << std::endl;
            }
            // saved with the project, and loaded back
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            const auto xml = juce::AudioProcessor::getXmlFromBinary(mb.getData(), (int)mb.getSize());
            const bool saved = xml != nullptr && xml->toString().contains("VERLAUF") &&
                               xml->toString().contains("stop2");
            const auto lanesSaved = S.lanes.size();
            const auto ptsSaved = S.lanes[0].pts.size();
            S = AutoSet();
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            const bool loaded = S.lanes.size() == lanesSaved && S.lanes[1].paramId == "stop2" &&
                                S.lanes[0].pts.size() == ptsSaved && S.clock == 1;
            std::cout << "verlauf saved with the project and loaded back: " << ((saved && loaded) ? "ok" : "FAIL")
                      << std::endl;
            proc->autoPlay(false);
            proc->autoSeek(0.0);
            airP->setValueNotifyingHost(airP->getDefaultValue());
            S = AutoSet();
            proc->autoPublish();
            block();
            block();
            std::cout << "verlauf removed -> NaN: "
                      << (std::isnan(proc->getAutomatedNorm(air)) && std::isnan(proc->getModulatedNorm(air))
                              ? "ok"
                              : "FAIL")
                      << std::endl;
        }

        // MIDI learn: a knob, a range, a button, and a learned CC wins over the fixed map
        {
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
                proc->clearMidiMap(i);
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            juce::AudioBuffer<float> st(2, 512);
            auto cc = [&](int ch, int num, int val)
            {
                juce::MidiBuffer mb;
                mb.addEvent(juce::MidiMessage::controllerEvent(ch, num, val), 10);
                st.clear(), proc->processBlock(st, mb);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
            };
            const int knob = proc->learnMidiFor(proc->getParamIndex("swind0"));
            const bool waiting = proc->getLearningSlot() == knob;
            cc(2, 21, 64);
            const auto m = proc->getMidiMap(knob);
            const bool learned = m.cc == 21 && m.channel == 2 && proc->getLearningSlot() < 0;
            const bool follows = std::abs(P("swind0")->getValue() - 64.0f / 127.0f) < 0.002f;
            auto r = m;
            r.lo = 0.2f;
            r.hi = 0.8f;
            proc->setMidiMap(knob, r);
            cc(2, 21, 127);
            const float top = P("swind0")->getValue();
            cc(2, 21, 0);
            const float bottom = P("swind0")->getValue();
            cc(3, 21, 127);
            const bool otherChannel = std::abs(P("swind0")->getValue() - bottom) < 1e-4f;
            std::cout << "MIDI learn knob: waiting " << waiting << ", learned CC 21 ch 2 "
                      << learned << ", follows " << follows << ", range " << bottom << ".." << top
                      << ", other channel ignored " << otherChannel
                      << ((waiting && learned && follows && std::abs(top - 0.8f) < 0.002f &&
                           std::abs(bottom - 0.2f) < 0.002f && otherChannel)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            P("tremOn")->setValueNotifyingHost(0.0f);
            const int button = proc->learnMidiFor(proc->getParamIndex("tremOn"));
            cc(1, 80, 127); // the learning press itself does not switch
            const bool learnNoFlip =
                P("tremOn")->getValue() < 0.5f && proc->getMidiMap(button).toggle;
            cc(1, 80, 0);
            cc(1, 80, 127);
            const bool on = P("tremOn")->getValue() > 0.5f;
            cc(1, 80, 0);
            const bool stays = P("tremOn")->getValue() > 0.5f;
            cc(1, 80, 127);
            const bool off = P("tremOn")->getValue() < 0.5f;
            cc(1, 80, 0);
            std::cout << "MIDI learn button: " << learnNoFlip << on << stays << off
                      << ((learnNoFlip && on && stays && off) ? "  ok" : "  FAIL") << std::endl;

            P("wind")->setValueNotifyingHost(0.0f);
            const int over = proc->learnMidiFor(proc->getParamIndex("click"));
            cc(1, 11, 100);
            cc(1, 11, 127);
            const bool clickMoved = std::abs(P("click")->getValue() - 1.0f) < 0.002f,
                       windStill = P("wind")->getValue() < 0.001f;
            std::cout << "learned CC 11 replaces the fixed map: "
                      << ((clickMoved && windStill) ? "ok" : "FAIL") << std::endl;
            proc->clearMidiMap(over);
            P("click")->setValueNotifyingHost(P("click")->getDefaultValue());
            P("tremOn")->setValueNotifyingHost(0.0f);

            juce::MemoryBlock mbm;
            proc->getStateInformation(mbm);
            const auto xml = proc->getXmlFromBinary(mbm.getData(), (int)mbm.getSize())->toString();
            for (int i = 0; i < 8; ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(
                    100); // saved as the default for new instances
            const auto saved = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                   .getChildFile("Rysum/MIDI-Regler.xml")
                                   .loadFileAsString();
            std::cout << "MIDI assignments in the state " << xml.contains("swind0")
                      << ", saved for new instances " << saved.contains("tremOn")
                      << ((xml.contains("swind0") && saved.contains("tremOn")) ? "  ok" : "  FAIL")
                      << std::endl;
            {
                RysumProcessor fresh;
                const bool inherits =
                    fresh.getMidiMap(knob).cc == 21 &&
                    fresh.getMidiMap(button).param == fresh.getParamIndex("tremOn");
                std::cout << "a new instance starts with them: " << (inherits ? "ok" : "FAIL")
                          << std::endl;
            }
            proc->learnMidiFor(
                proc->getParamIndex("simTail")); // shows "bewegen ..." in the snapshot
        }

        // free tuning: a Scala file (kept in the state), MIDI Tuning Standard SysEx (as Hexatone
        // sends it)
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            const auto scl = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("rysum_test_19edo.scl");
            juce::String txt("! 19-EDO\n19 equal\n 19\n");
            for (int i = 1; i <= 19; ++i)
                txt << juce::String(1200.0 * i / 19.0, 4) << "\n";
            scl.replaceWithText(txt);
            const bool loaded = proc->loadTuningFile(scl);
            const int t = juce::roundToInt(proc->apvts.getRawParameterValue("temperament")->load());
            const bool free = proc->readParams().notes != nullptr;
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            const bool inState = proc->getXmlFromBinary(mb.getData(), (int)mb.getSize())
                                     ->toString()
                                     .contains("19 equal");
            std::cout << "scala file: loaded " << loaded << ", tuning " << t << ", free notes "
                      << free << ", in the state " << inState << " ("
                      << proc->getTuningStatus().upToFirstOccurrenceOf("\n", false, false) << ")"
                      << ((loaded && t == RysumProcessor::kScalaTuning && free && inState)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            // MTS: without a master, a real-time single-note tuning change SysEx retunes note 69 to
            // b-flat (466.16 Hz)
            proc->loadPatch(RysumProcessor::getPatchFolder().getChildFile(
                "01 Init.rysum")); // a clean sound (earlier tests leave all sorts)
            P("temperament")
                ->setValueNotifyingHost(
                    P("temperament")->convertTo0to1((float)RysumProcessor::kMtsTuning));
            juce::AudioBuffer<float> st(2, 512);
            const juce::uint8 sx[]{0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00,
                                   0x01, 69,   70,   0x00, 0x00, 0xF7};
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(0.0f);
            for (auto *id : {"click", "releaseClick", "wind", "irWet"})
                P(id)->setValueNotifyingHost(0.0f);
            for (int r = 0; r < rysum::kNumRoutes; ++r)
                proc->clearRoute(r); // earlier tests modulate the wind (pitch gusts)
            P("output")->setValueNotifyingHost(0.0f);
            {
                juce::MidiBuffer m;
                m.addEvent(juce::MidiMessage(sx, (int)sizeof(sx)), 0);
                st.clear(), proc->processBlock(st, m);
            }
            const auto status = proc->getTuningStatus();
            std::vector<float> rec;
            for (int b = 0; b < 140; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0)
                    m.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                if (b == 139)
                    m.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                st.clear(), proc->processBlock(st, m);
                if (b >= 60)
                    for (int i = 0; i < 512; ++i)
                        rec.push_back(st.getSample(0, i));
            }
            const double sr = proc->getSampleRate() > 0 ? proc->getSampleRate() : 48000.0;
            auto ac = [&](int lag)
            {
                double s2 = 0;
                for (size_t i = 0; i + (size_t)lag < rec.size(); ++i)
                    s2 += (double)rec[i] * rec[i + (size_t)lag];
                return s2;
            };
            int best = 0;
            double bv = -1e18;
            for (int lag = (int)(sr / 520.0); lag <= (int)(sr / 420.0); ++lag)
            {
                const double v = ac(lag);
                if (v > bv)
                {
                    bv = v;
                    best = lag;
                }
            }
            const double y0 = ac(best - 1), y1 = ac(best), y2 = ac(best + 1);
            const double hz = sr / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
            const double err = 1200.0 * std::log2(hz / 466.1638);
            std::cout << "MTS SysEx: " << status << "; note 69 sounds at " << hz << " Hz (" << err
                      << " ct from 466.16)"
                      << ((status == "MIDI Tuning (MTS-ESP) empfangen" && std::abs(err) < 6.0)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(1.0f);
            P("temperament")->setValueNotifyingHost(P("temperament")->convertTo0to1(0.0f));
        }

        // drawn trajectory (Trompete): kept in the state, the engine follows it; ellipse settings
        // for the picture
        {
            auto setP = [&](const juce::String &id, float v)
            {
                auto *a = proc->apvts.getParameter(id);
                a->setValueNotifyingHost(a->convertTo0to1(v));
            };
            std::vector<RysumProcessor::DrawnPath::Pt> pts;
            for (int i = 0; i < 120; ++i)
            {
                const float t = (float)i / 119.0f;
                pts.push_back({160.0f * std::sin(6.283f * t), 30.0f + 25.0f * std::sin(12.566f * t),
                               2.0f * t * t}); // a figure eight, drawn faster at the end
            }
            proc->setDrawnPath(6, pts);
            setP(RysumProcessor::motionParamId(6), (float)rysum::Motion::Drawn);
            setP(RysumProcessor::rateParamId(6), 0.05f);
            setP(RysumProcessor::ellipseParamId(6), 0.5f);
            setP(RysumProcessor::ellDirParamId(6), 45.0f);
            setP(RysumProcessor::ellFormParamId(6), 1.0f);
            const auto prm = proc->readParams();
            const auto pos = proc->engine.currentPosition(6, 0, prm);
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->setDrawnPath(6, {});
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            const auto &back = proc->getDrawnPath(6);
            std::cout << "drawn path: " << back.points.size()
                      << " points after the state round trip, pipe 0 at az " << pos.az << " el "
                      << pos.el
                      << ((back.points.size() == 120 &&
                           std::abs(back.points[60].az - pts[60].az) < 0.01f &&
                           proc->engine.getPath(6).n > 100)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
        }

        editor.reset(dynamic_cast<RysumEditor *>(proc->createEditor()));
        if (std::getenv("RYSUM_D2D_BENCH") != nullptr)
        {
            // a real window at full-HD size (the interface 1.5x, as in full screen): how many whole
            // repaints per second each renderer manages per page, and what leaving out single
            // drawing effects gains
            editor->setSize(1920, 1080);
            editor->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar);
            editor->setTopLeftPosition(
                juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_X", "0").getIntValue(), 0);
            editor->setVisible(true);
            auto *c = dynamic_cast<RysumContent *>(editor->getChildComponent(0));
            auto *peer = editor->getPeer();
            if (c == nullptr || peer == nullptr)
            {
                std::cout << "BENCH: no window" << std::endl;
                std::_Exit(1);
            }
            const auto engines = peer->getAvailableRenderingEngines();
            auto measure = [&](int view, int skip, const juce::String &label)
            {
                rysum::ui::dbg::skip = skip;
                c->showView(view);
                auto spin = [&](double ms)
                {
                    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
                    while (juce::Time::getMillisecondCounterHiRes() < end)
                    {
                        editor->repaint();
                        c->tick();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                    }
                };
                spin(400);
                const int p0 = c->paintsDone;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                spin(1500);
                const double fps = (c->paintsDone - p0) /
                                   ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                std::cout << "BENCH " << engines[peer->getCurrentRenderingEngine()] << "  " << label
                          << ": " << fps << " repaints/s" << std::endl;
            };
            const char *names[]{"Spieltisch", "Raum", "Saal", "Klang",  "Effekte",
                                "Modulation", "",     "",     "Begehen"};
            {
                // as in the app: audio running in real time (a held chord), repaints only from the
                // interface's own timers
                std::atomic<bool> audioRun{true};
                std::thread audio(
                    [&]
                    {
                        juce::AudioBuffer<float> b(16, 512);
                        bool first = true;
                        double next = juce::Time::getMillisecondCounterHiRes();
                        while (audioRun.load())
                        {
                            juce::MidiBuffer mb;
                            if (first)
                            {
                                for (int n : {48, 55, 60, 64, 67})
                                    mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100),
                                                0);
                                first = false;
                            }
                            b.clear();
                            proc->processBlock(b, mb);
                            next += 512.0 / 48.0;
                            const double wait = next - juce::Time::getMillisecondCounterHiRes();
                            if (wait > 0)
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds((int)(wait * 1000.0)));
                        }
                    });
                for (int e = 0; e < engines.size(); ++e)
                {
                    peer->setCurrentRenderingEngine(e);
                    for (int v : {0, 5, 8})
                    {
                        c->showView(v);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                        const int p0 = c->paintsDone;
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                        const double fps =
                            (c->paintsDone - p0) /
                            ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                        std::cout << "BENCH natural, audio running, " << engines[e] << "  "
                                  << names[v] << ": " << fps << " paints/s" << std::endl;
                    }
                }
                // the full-screen window (kiosk mode), as F11 opens it
                if (c->onToggleFullscreen)
                    c->onToggleFullscreen();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
                if (auto *fp = c->getPeer())
                {
                    const auto fe = fp->getAvailableRenderingEngines();
                    for (int e = 0; e < fe.size(); ++e)
                    {
                        fp->setCurrentRenderingEngine(e);
                        for (int v : {0, 5, 8})
                        {
                            c->showView(v);
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                            const int p0 = c->paintsDone;
                            const double t0 = juce::Time::getMillisecondCounterHiRes();
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                            const double fps =
                                (c->paintsDone - p0) /
                                ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                            std::cout << "BENCH full screen, audio running, " << fe[e] << "  "
                                      << names[v] << ": " << fps << " paints/s   (screen "
                                      << fp->getBounds().getWidth() << " x "
                                      << fp->getBounds().getHeight() << ", scale "
                                      << fp->getPlatformScaleFactor() << ")" << std::endl;
                        }
                    }
                    fp->setCurrentRenderingEngine(
                        fe.indexOf("Direct2D") >= 0 ? fe.indexOf("Direct2D") : 0);
                }
                if (c->onToggleFullscreen)
                    c->onToggleFullscreen();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
                audioRun = false;
                audio.join();
                for (int e = 0; e < engines.size(); ++e)
                {
                    peer->setCurrentRenderingEngine(e);
                    for (int v : {0, 5})
                    {
                        c->showView(v);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                        const int p0 = c->paintsDone;
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                        const double fps =
                            (c->paintsDone - p0) /
                            ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                        std::cout << "BENCH natural, no audio, " << engines[e] << "  " << names[v]
                                  << ": " << fps << " paints/s" << std::endl;
                    }
                }
            }
            for (int e = 0; e < engines.size(); ++e)
            {
                peer->setCurrentRenderingEngine(e);
                for (int v : {0, 1, 2, 3, 4, 5, 8})
                    measure(v, 0, names[v]);
            }
            peer->setCurrentRenderingEngine(
                engines.indexOf("Direct2D") >= 0 ? engines.indexOf("Direct2D") : 0);
            namespace D = rysum::ui::dbg;
            for (int v : {0, 5})
            {
                measure(v, D::skipOverlay, juce::String(names[v]) + " without the overlay");
                measure(v, D::skipCover, juce::String(names[v]) + " without photos (drawCover)");
                measure(v, D::skipGrain, juce::String(names[v]) + " without grain tiles");
                measure(v, D::skipClipPath, juce::String(names[v]) + " without round clips");
                measure(v, D::skipTranslucent, juce::String(names[v]) + " without the faint wall");
                measure(v, 31, juce::String(names[v]) + " without all of these");
            }
            std::cout.flush();
            std::_Exit(0);
        }
        editor->setSize(1280, 720);
        auto *content = editor->getChildComponent(0);
        auto shoot = [&](int view, const juce::String &name)
        {
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(view);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(
                120); // slider timers (modulation, MIDI tags)
            auto img = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile(name);
            f.deleteFile();
            juce::FileOutputStream os(f);
            png.writeImageToStream(img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        };
        shoot(0, "ui_spieltisch.png");
        {
            // the spatial degradation tab and the grain cloud tab (found by their button text)
            std::function<juce::Button *(juce::Component *, const juce::String &)> findButton =
                [&](juce::Component *c, const juce::String &t) -> juce::Button *
            {
                if (!c->isVisible())
                    return nullptr;
                if (auto *b = dynamic_cast<juce::TextButton *>(c))
                    if (b->getButtonText() == t)
                        return b;
                for (auto *ch : c->getChildren())
                    if (auto *f = findButton(ch, t))
                        return f;
                return nullptr;
            };
            auto setN = [&](const juce::String &id, float v)
            {
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            };
            setN("spcErode", 0.35f);
            setN("spcGrid", 0.4f);
            setN("spcDrops", 0.2f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(0);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            std::function<juce::Component *(juce::Component *, bool)> findView =
                [&](juce::Component *c, bool console) -> juce::Component *
            {
                if (console ? dynamic_cast<rysum::ui::ConsoleView *>(c) != nullptr
                            : dynamic_cast<rysum::ui::SpaceView *>(c) != nullptr)
                    return c;
                for (auto *ch : c->getChildren())
                    if (auto *f = findView(ch, console))
                        return f;
                return nullptr;
            };
            // the buttons of the Spieltisch must not cover one another ("pro Pfeife" / "Summe" used
            // to lie over the Glitch tab)
            if (auto *cv = findView(content, true))
            {
                bool clear = true;
                for (auto *a : cv->getChildren())
                    for (auto *b : cv->getChildren())
                        if (a != b && a->isVisible() && b->isVisible() &&
                            dynamic_cast<juce::TextButton *>(a) != nullptr &&
                            dynamic_cast<juce::TextButton *>(b) != nullptr &&
                            a->getBounds().intersects(b->getBounds()))
                            clear = false;
                std::cout << "Spieltisch buttons do not overlap: " << (clear ? "ok" : "FAIL")
                          << std::endl;
            }
            if (auto *b =
                    findView(content, true) ? findButton(findView(content, true), "Raum") : nullptr)
                b->triggerClick();
            else
                std::cout << "Raum tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            shoot(0, "ui_spieltisch_raumverfall.png");
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Glitch")
                                                  : nullptr)
                b->triggerClick();
            else
                std::cout << "Glitch tab button not found  FAIL" << std::endl;
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Resonanz")
                                                  : nullptr)
                b->triggerClick();
            else
                std::cout << "Resonanz tab button not found  FAIL" << std::endl;
            setN("degStutter", 0.4f);
            setN("clickRes", 0.6f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            shoot(0, "ui_spieltisch_glitch_resonanz.png");
            setN("degStutter", 0.0f);
            setN("clickRes", 0.0f);
            if (auto *b = findView(content, true)
                              ? findButton(findView(content, true),
                                           juce::String::fromUTF8("Ger\xc3\xa4usche"))
                              : nullptr)
                b->triggerClick();
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Klang")
                                                  : nullptr)
                b->triggerClick();
            for (auto *id : {"spcErode", "spcGrid", "spcDrops"})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->getDefaultValue());
            setN(RysumProcessor::cloudParamId("Mix", 6), 0.6f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(1);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            if (auto *b = findView(content, false) ? findButton(findView(content, false), "Wolke")
                                                   : nullptr)
                b->triggerClick();
            else
                std::cout << "Wolke tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            auto img = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            {
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_raum_wolke.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img, os);
            }
            setN(RysumProcessor::pipeSndParamId("Att", 6), 0.5f);
            setN(RysumProcessor::pipeSndParamId("HiGain", 6), 6.0f);
            if (auto *b = findView(content, false) ? findButton(findView(content, false), "Pfeife")
                                                   : nullptr)
                b->triggerClick();
            else
                std::cout << "Pfeife tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            {
                auto img2 = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_raum_pfeife.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img2, os);
            }
            setN(RysumProcessor::pipeSndParamId("Att", 6), 0.0f);
            setN(RysumProcessor::pipeSndParamId("HiGain", 6), 0.0f);
            if (auto *b = findView(content, false)
                              ? findButton(findView(content, false), "Bewegung")
                              : nullptr)
                b->triggerClick();
            setN(RysumProcessor::cloudParamId("Mix", 6), 0.0f);
        }
        for (int k = 0; k < rysum::kNumKeys; k += 3)
            proc->setPipeMuted(6, k, true); // some Trompete pipes muted
        shoot(1, "ui_raum.png");
        {
            // the arrangement "Tonnetz" and the motion "Schwebung" (their buttons, found by text)
            std::function<juce::Button *(juce::Component *, const juce::String &)> fb =
                [&](juce::Component *c, const juce::String &t) -> juce::Button *
            {
                if (!c->isVisible())
                    return nullptr;
                if (auto *b = dynamic_cast<juce::TextButton *>(c))
                    if (b->getButtonText() == t)
                        return b;
                for (auto *ch : c->getChildren())
                    if (auto *f = fb(ch, t))
                        return f;
                return nullptr;
            };
            const int before = proc->layouts[0].type;
            bool found = true;
            if (auto *b = fb(content, "Tonnetz"))
                b->triggerClick();
            else
                found = false;
            if (auto *b = fb(content, "Schwebung"))
                b->triggerClick();
            else
                found = false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            shoot(1, "ui_raum_tonnetz.png");
            std::cout << "Raum: Tonnetz and Schwebung buttons "
                      << (found ? "ok" : "not found  FAIL") << std::endl;
            (void)before;
        }
        for (int k = 0; k < rysum::kNumKeys; ++k)
            proc->setPipeMuted(6, k, false);
        {
            // the audio input: on, stereo, kreisend; some noise through the processor so the meter
            // and the places show
            auto setN = [&](const char *id, float v)
            {
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            };
            setN("inpOn", 1.0f);
            setN("inpSpin", 20.0f);
            setN("inpWidth", 90.0f);
            setN("inpAz", 30.0f);
            setN("inpDist", 3.0f);
            juce::AudioBuffer<float> io(36, 512);
            rysum::XorShift r(5);
            float meanAbs = 0.0f;
            for (int b = 0; b < 40; ++b)
            {
                io.clear();
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                        io.setSample(c, i, 0.2f * r.bi());
                juce::MidiBuffer mb;
                proc->processBlock(io, mb);
                if (b > 30)
                    meanAbs += io.getMagnitude(0, 0, 512) + io.getMagnitude(1, 0, 512);
            }
            std::cout << "input through the processor: out peak " << meanAbs / 18.0f
                      << ", shown sources " << proc->engine.inShowCount.load()
                      << ((proc->engine.inShowCount.load() == 2 && meanAbs > 0.01f) ? "  ok"
                                                                                    : "  FAIL")
                      << std::endl;
            rysum::ui::SpaceView *sv = nullptr;
            std::function<void(juce::Component *)> find = [&](juce::Component *c)
            {
                if (auto *v = dynamic_cast<rysum::ui::SpaceView *>(c))
                    sv = v;
                for (auto *ch : c->getChildren())
                    if (sv == nullptr)
                        find(ch);
            };
            find(editor.get());
            if (sv != nullptr)
                sv->selectInput();
            shoot(1, "ui_eingang.png");
            if (sv != nullptr)
                sv->selectStop(6);
            setN("inpOn", 0.0f);
            setN("inpSpin", 0.0f);
            setN("inpWidth", 60.0f);
            setN("inpAz", 0.0f);
            setN("inpDist", 4.0f);
        }
        shoot(3, "ui_klang.png");
        {
            // two loudspeakers (transaural): the output switched, the HRTF decoder and then the
            // crosstalk filters designed
            if (auto *o = proc->apvts.getParameter("output"))
                o->setValueNotifyingHost(
                    o->convertTo0to1((float)RysumProcessor::kTransauralOutput));
            for (int t = 0; t < 300 && proc->getTransauralFilters() == nullptr; ++t)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            shoot(3, "ui_klang_transaural.png");
            const auto f = proc->getTransauralFilters();
            std::cout << "transaural in the plugin: "
                      << (f != nullptr ? f->description : juce::String("not designed  FAIL"))
                      << std::endl;
            if (auto *o = proc->apvts.getParameter("output"))
                o->setValueNotifyingHost(o->getDefaultValue());
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        }
        {
            // loudspeakers: 7.1.4 at 3rd order through the processor - sound on the layout's
            // channels only, the LFE channel silent
            auto l714 = rysum::speakerPresets()[3];
            proc->setSpeakerLayout(l714, false);
            auto *o = proc->apvts.getParameter("output");
            const float outBefore = o->getValue();
            o->setValueNotifyingHost(o->convertTo0to1((float)RysumProcessor::kSpeakerOutput));
            juce::AudioBuffer<float> io(36, 512);
            double e[36]{};
            for (int b = 0; b < 60; ++b)
            {
                io.clear();
                juce::MidiBuffer mb;
                if (b == 2)
                    for (int m : {48, 55, 60, 64})
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                proc->processBlock(io, mb);
                if (b > 20)
                    for (int c = 0; c < 36; ++c)
                        e[c] += io.getRMSLevel(c, 0, 512);
            }
            int sounding = 0;
            for (int c = 0; c < 12; ++c)
                sounding += e[c] > 1.0e-4 ? 1 : 0;
            double rest = 0;
            for (int c = 12; c < 36; ++c)
                rest += e[c];
            proc->speakerTest = 0;
            io.clear();
            {
                juce::MidiBuffer none;
                proc->processBlock(io, none);
            }
            proc->speakerTest = -1;
            std::cout << "loudspeakers 7.1.4: " << sounding << " of 12 channels sound, LFE " << e[3]
                      << ", above 12: " << rest
                      << ((sounding == 11 && e[3] == 0.0 && rest == 0.0) ? "  ok" : "  FAIL")
                      << std::endl;
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->setSpeakerLayout(rysum::speakerPresets()[0], false);
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            std::cout << "loudspeaker layout in the state: "
                      << (proc->getSpeakerLayout().speakers.size() == 11 ? "ok" : "FAIL")
                      << std::endl;
            shoot(7, "ui_lautsprecher.png");
            for (int b = 0; b < 20; ++b)
            {
                io.clear();
                juce::MidiBuffer noteOffs;
                if (b == 0)
                    for (int m : {48, 55, 60, 64})
                        noteOffs.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                proc->processBlock(io, noteOffs);
            }
            o->setValueNotifyingHost(outBefore);
            proc->setSpeakerLayout(rysum::speakerPresets()[4], false);
        }
        shoot(5, "ui_mod.png");
        {
            // the Verlauf page with a few lanes: a wave, a loop with a pencil line, stepped stops
            using namespace rysum;
            auto &S = proc->getAuto();
            S = AutoSet();
            S.clock = 1;
            AutoLane a;
            a.paramId = "wind";
            a.pts = {{0.0, 0.1f}, {6.0, 0.7f}, {14.0, 0.4f}, {22.0, 0.9f}};
            a.pts[1].shape = asWave;
            a.pts[1].rate = 1.5f;
            a.pts[1].depth = 0.25f;
            a.pts[2].bend = 0.7f;
            AutoLane b;
            b.paramId = "air";
            b.loop = true;
            b.length = 6.0;
            b.pts = {{0.0, 0.3f}, {2.0, 0.85f}, {4.0, 0.5f}, {6.0, 0.3f}};
            b.pts[2].shape = asJitter;
            b.pts[2].depth = 0.3f;
            AutoLane c;
            c.paramId = "stop2";
            c.armed = true;
            for (int k = 0; k < 8; ++k)
            {
                AutoPoint p{k * 3.0, (float)(k % 2)};
                p.shape = asStep;
                c.pts.push_back(p);
            }
            AutoLane d;
            d.paramId = "doppler";
            d.speed = 0.5f;
            d.pts = {{0.0, 0.0f}, {8.0, 1.0f}};
            d.pts[0].shape = asStairs;
            d.pts[0].rate = 1.0f;
            AutoLane e; // a swell of brightness running through the room from front left
            e.paramId = pipeTargetId(tgtBright);
            e.spread = true;
            e.ox = -6.0f;
            e.oy = 8.0f;
            e.waveSpeed = 3.0f;
            e.pts = {{0.0, 0.5f}, {2.0, 0.9f}, {5.0, 0.5f}, {7.0, 0.2f}, {10.0, 0.5f}};
            e.pts[1].shape = asWave;
            e.pts[1].rate = 2.0f;
            e.pts[1].depth = 0.15f;
            AutoLane f; // the Praestant's timetable: there in 6 beats, wait, back by beat 16
            f.paramId = autoPathLaneId(0);
            f.pts = {{0.0, 0.0f}, {6.0, 0.45f}, {10.0, 0.45f}, {16.0, 1.0f}};
            f.pts[0].bend = 0.6f;
            S.lanes = {e, a, b, c, d, f};
            const auto pathBefore = proc->getDrawnPath(0);
            auto *motion0 = proc->apvts.getParameter(RysumProcessor::motionParamId(0));
            const float motionBefore = motion0->getValue();
            {
                std::vector<RysumProcessor::DrawnPath::Pt> pts;
                for (int i = 0; i <= 40; ++i)
                {
                    const float u = (float)i / 40.0f;
                    pts.push_back({-120.0f + 240.0f * u, 15.0f + 45.0f * std::sin(u * 3.14159f), u});
                }
                proc->setDrawnPath(0, pts);
                proc->setPathOptions(0, true, false);
                motion0->setValueNotifyingHost(motion0->convertTo0to1((float)rysum::Motion::Drawn));
            }
            proc->autoChanged();
            proc->autoPublish();
            proc->autoSeek(9.0);
            {
                juce::AudioBuffer<float> abuf(proc->getTotalNumOutputChannels(), 256);
                juce::MidiBuffer none;
                proc->processBlock(abuf, none);
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            shoot(9, "ui_verlauf.png");
            if (auto *rc = dynamic_cast<RysumContent *>(content))
                rc->selectRaumStop(0);
            shoot(1, "ui_raum_fahrplan.png");
            proc->setDrawnPath(0, pathBefore.points);
            proc->setPathOptions(0, pathBefore.pingPong, pathBefore.drawnTiming);
            motion0->setValueNotifyingHost(motionBefore);
            // the sliders follow their curves (their timers run only in a window on screen)
            if (auto *rc = dynamic_cast<RysumContent *>(content))
            {
                std::function<rysum::ui::ParamRow *(juce::Component &, const juce::String &)> findRow;
                findRow = [&](juce::Component &parent, const juce::String &id) -> rysum::ui::ParamRow *
                {
                    for (auto *ch : parent.getChildren())
                    {
                        if (auto *pr = dynamic_cast<rysum::ui::ParamRow *>(ch))
                            if (pr->getParamId() == id && pr->isShowing())
                                return pr;
                        if (auto *f = findRow(*ch, id))
                            return f;
                    }
                    return nullptr;
                };
                editor->addToDesktop(0);
                editor->setVisible(true);
                rc->showView(0);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
                const int windIdx = proc->getParamIndex("wind");
                auto *windP = proc->getParamAt(windIdx);
                auto *row = findRow(*rc, "wind");
                const float curve = proc->getAutomatedNorm(windIdx);
                const double follows = row != nullptr ? row->slider.getValue() : -1.0;
                const bool followOk = row != nullptr && !std::isnan(curve) &&
                                      std::abs(follows - windP->convertFrom0to1(curve)) < 1e-3 &&
                                      std::abs(curve - windP->getValue()) > 0.02f;
                S = AutoSet();
                proc->autoPublish();
                {
                    juce::AudioBuffer<float> abuf(proc->getTotalNumOutputChannels(), 256);
                    juce::MidiBuffer none;
                    proc->processBlock(abuf, none);
                    proc->processBlock(abuf, none);
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                const bool backOk = row != nullptr && std::abs(row->slider.getValue() -
                                                               windP->convertFrom0to1(windP->getValue())) < 1e-3;
                editor->removeFromDesktop();
                std::cout << "verlauf: the wind slider follows its curve " << followOk << " ("
                          << follows << "), back to the parameter after " << backOk
                          << ((followOk && backOk) ? "  ok" : "  FAIL") << std::endl;
            }
            // editing on the page with the mouse and the keys: a click sets a point, a click on the
            // line chooses its section and a drag bends it, a frame chooses points that then move
            // together, Entf deletes them, Strg+Z brings them back, a lane's lower edge sets its
            // height, the hint line names what is under the mouse, and while recording a touched
            // control gets its own lane
            {
                S = AutoSet();
                S.clock = 1;
                S.grid = 1.0;
                S.lanes = {e, a, b, c, d, f};
                proc->autoChanged();
                proc->autoPublish();
                shoot(9, "ui_verlauf.png");
                std::function<rysum::ui::VerlaufView *(juce::Component &)> findView;
                findView = [&](juce::Component &parent) -> rysum::ui::VerlaufView *
                {
                    for (auto *ch : parent.getChildren())
                    {
                        if (auto *v = dynamic_cast<rysum::ui::VerlaufView *>(ch))
                            return v;
                        if (auto *v = findView(*ch))
                            return v;
                    }
                    return nullptr;
                };
                auto *vv = findView(*content);
                rysum::ui::VerlaufTimeline *tl = nullptr;
                if (vv != nullptr)
                    for (auto *ch : vv->getChildren())
                        if (auto *t = dynamic_cast<rysum::ui::VerlaufTimeline *>(ch))
                            tl = t;
                bool editOk = false;
                juce::String why;
                if (tl != nullptr && tl->getWidth() > 400)
                {
                    using TL = rysum::ui::VerlaufTimeline;
                    const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
                    auto ev = [&](juce::Point<float> p, juce::Point<float> down, int clicks, bool dragged,
                                  juce::ModifierKeys mods)
                    {
                        const auto now = juce::Time::getCurrentTime();
                        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods,
                                                juce::MouseInputSource::defaultPressure,
                                                juce::MouseInputSource::defaultOrientation,
                                                juce::MouseInputSource::defaultRotation,
                                                juce::MouseInputSource::defaultTiltX,
                                                juce::MouseInputSource::defaultTiltY, tl, tl, now, down, now, clicks,
                                                dragged);
                    };
                    auto click = [&](juce::Point<float> p, juce::ModifierKeys mods = {})
                    {
                        tl->mouseDown(ev(p, p, 1, false, mods.withFlags(juce::ModifierKeys::leftButtonModifier)));
                        tl->mouseUp(ev(p, p, 1, false, mods));
                    };
                    auto dragTo = [&](juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
                    {
                        const auto m = mods.withFlags(juce::ModifierKeys::leftButtonModifier);
                        tl->mouseDown(ev(from, from, 1, false, m));
                        for (int i = 1; i <= 8; ++i)
                            tl->mouseDrag(ev(from + (to - from) * ((float)i / 8.0f), from, 1, true, m));
                        tl->mouseUp(ev(to, from, 1, true, mods));
                    };
                    juce::ignoreUnused(left);
                    const int li = 1; // the wind lane
                    auto curve = [&]
                    {
                        return tl->laneRect(li).withTrimmedLeft(TL::kHeaderW).reduced(0, 7);
                    };
                    auto at = [&](double beat, float v)
                    {
                        const auto c = curve();
                        return juce::Point<float>(tl->xOfBeat(beat), (float)c.getBottom() - v * (float)c.getHeight());
                    };
                    // (the lane chosen first: a click into a lane that is not chosen only chooses it)
                    click({60.0f, (float)tl->laneRect(li).getY() + 10.0f});
                    auto &W = S.lanes[(size_t)li];
                    const size_t n0 = W.pts.size();
                    // 1: a click on empty space sets a point (on the grid)
                    click(at(18.2, 0.08f));
                    const bool added = W.pts.size() == n0 + 1 &&
                                       std::any_of(W.pts.begin(), W.pts.end(),
                                                   [](const AutoPoint &p) { return std::abs(p.t - 18.0) < 1e-6; });
                    // 2: on the line: a click adds nothing, a drag bends that section
                    const float onLine = rysum::autoValue(W, 10.0);
                    click(at(10.0, onLine));
                    const bool lineNoPoint = W.pts.size() == n0 + 1;
                    W.pts[1].bend = 0.0f;
                    dragTo(at(10.0, onLine), at(10.0, onLine) + juce::Point<float>(0.0f, 40.0f));
                    const bool bent = std::abs(W.pts[1].bend) > 0.1f && W.pts.size() == n0 + 1;
                    // 3: a frame around the points at 6 and 14, then both moved by two beats
                    dragTo(at(5.3, 0.98f), at(14.7, 0.02f));
                    const bool frameNoPoint = W.pts.size() == n0 + 1;
                    const float v6 = W.pts[1].v, v14 = W.pts[2].v;
                    dragTo(at(6.0, v6), at(8.0, v6));
                    auto has = [&](double t, float v)
                    {
                        // (the lane looked up anew: undo replaces the lanes)
                        const auto &P = S.lanes[(size_t)li].pts;
                        return std::any_of(P.begin(), P.end(), [&](const AutoPoint &p)
                                           { return std::abs(p.t - t) < 1e-6 && std::abs(p.v - v) < 1e-4f; });
                    };
                    const bool movedBoth = has(8.0, v6) && has(16.0, v14) && W.pts.size() == n0 + 1;
                    if (!movedBoth)
                    {
                        std::cout << "  verlauf move: wanted 8 / " << v6 << " and 16 / " << v14 << ", have";
                        for (auto &p : W.pts)
                            std::cout << "  " << p.t << " / " << p.v;
                        std::cout << std::endl;
                    }
                    // 4: Entf deletes both, Strg+Z brings them back; Strg+C / Strg+V copies them
                    vv->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
                    const bool deleted = S.lanes[(size_t)li].pts.size() == n0 - 1;
                    vv->keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0));
                    const bool undone = S.lanes[(size_t)li].pts.size() == n0 + 1 && has(8.0, v6);
                    dragTo(at(7.3, 0.98f), at(16.7, 0.02f));
                    vv->keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0));
                    proc->autoSeek(40.0);
                    {
                        juce::AudioBuffer<float> abuf(proc->getTotalNumOutputChannels(), 64);
                        juce::MidiBuffer none;
                        proc->processBlock(abuf, none);
                    }
                    const size_t before = S.lanes[(size_t)li].pts.size();
                    vv->keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0));
                    const auto &W3 = S.lanes[(size_t)li];
                    const bool pasted = W3.pts.size() == before + 2;
                    // 5: the lane's lower edge sets its height; a double-click there makes it automatic
                    const auto r = tl->laneRect(li);
                    const int h0 = r.getHeight();
                    dragTo({60.0f, (float)r.getBottom() - 2.0f}, {60.0f, (float)r.getBottom() + 38.0f});
                    const bool resized = std::abs(S.lanes[(size_t)li].height - (h0 + 40)) <= 1;
                    // 6: the hint line follows the mouse
                    tl->mouseMove(ev(at(8.0, v6), at(8.0, v6), 1, false, {}));
                    const auto hintPoint = tl->hintText();
                    tl->mouseMove(ev({60.0f, (float)tl->laneRect(li).getCentreY() - 10.0f}, {}, 1, false, {}));
                    const auto hintHeader = tl->hintText();
                    const bool hints = hintPoint.contains("Punkt") && hintHeader.contains("Spurkopf");
                    // markers and the repeated section, in the band over the ruler
                    {
                        const float by = (float)TL::kMarkH * 0.5f;
                        const auto mp = juce::Point<float>(tl->xOfBeat(4.0), by);
                        tl->mouseDoubleClick(ev(mp, mp, 2, false, left));
                        const bool markerOk = S.markers.size() == 1 && S.markers[0].name == "A" &&
                                              std::abs(S.markers[0].beat - 4.0) < 1e-6;
                        dragTo({tl->xOfBeat(8.0), by}, {tl->xOfBeat(16.0), by});
                        const bool cycleOk = S.cycle && std::abs(S.cycleStart - 8.0) < 1e-6 &&
                                             std::abs(S.cycleEnd - 16.0) < 1e-6;
                        // a Euclidean rhythm (5 of 8, one per beat) into the visible range
                        vv->testFill(11);
                        int hits = 0;
                        for (auto &p : S.lanes[(size_t)li].pts)
                            hits += p.shape == asStep && p.v > 0.5f && p.t >= 0.0 && p.t < 8.0 ? 1 : 0;
                        std::cout << "verlauf markers: double-click sets A " << (int)markerOk
                                  << ", A-B by dragging " << (int)cycleOk << ", 5 of 8 = " << hits << " hits"
                                  << ((markerOk && cycleOk && hits == 5) ? "  ok" : "  FAIL") << std::endl;
                    }
                    shoot(9, "ui_verlauf_edit.png");
                    // 7: recording: a control without a lane gets one when touched
                    int free = -1;
                    for (int i = 0; i < proc->getNumParams() && free < 0; ++i)
                        if (proc->isAutoTarget(i) && proc->isModTarget(i))
                        {
                            const auto id = proc->getParamAt(i)->getParameterID();
                            if (std::none_of(S.lanes.begin(), S.lanes.end(),
                                             [&](const AutoLane &l) { return l.paramId == id; }))
                                free = i;
                        }
                    bool recLane = false;
                    if (free >= 0)
                    {
                        const size_t lanes0 = S.lanes.size();
                        auto *fp = proc->getParamAt(free);
                        proc->setAutoRecord(true);
                        fp->beginChangeGesture();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
                        fp->endChangeGesture();
                        proc->setAutoRecord(false);
                        recLane = S.lanes.size() == lanes0 + 1 && S.lanes.back().armed &&
                                  S.lanes.back().paramId == fp->getParameterID();
                    }
                    editOk = added && lineNoPoint && bent && frameNoPoint && movedBoth && deleted && undone &&
                             pasted && resized && hints && recLane;
                    why << "click " << (int)added << ", line " << (int)lineNoPoint << (int)bent << ", frame "
                        << (int)frameNoPoint << ", move " << (int)movedBoth << ", delete/undo " << (int)deleted
                        << (int)undone << ", paste " << (int)pasted << ", height " << (int)resized << ", hints "
                        << (int)hints << ", lane by recording " << (int)recLane;
                }
                else
                    why = "page not found";
                std::cout << "verlauf editing: " << why << (editOk ? "  ok" : "  FAIL") << std::endl;
            }
            // pieces: every one that comes with Rysum loads, each lane drives something real; a
            // piece saved to a file comes back the same; "Fahrt" brings its drawn path along
            {
                auto *m0 = proc->apvts.getParameter(RysumProcessor::motionParamId(0));
                const float m0Before = m0->getValue(); // ("Fahrt" sets the Praestant to its drawn path)
                bool factoryOk = true;
                juce::String bad;
                const auto names = RysumProcessor::factoryPieceNames();
                for (int i = 0; i < names.size(); ++i)
                {
                    proc->loadFactoryPiece(i);
                    bool ok = !S.lanes.empty() && proc->getPieceName() == names[i] && !proc->isPieceChanged();
                    for (auto &l : S.lanes)
                    {
                        const int pi = proc->getParamIndex(l.paramId);
                        const bool real = (pi >= 0 && proc->isAutoTarget(pi)) || autoPipeTarget(l.paramId) >= 0 ||
                                          autoPathStop(l.paramId) >= 0;
                        ok = ok && real && l.pts.size() >= 2;
                        if (!real)
                            bad << " " << l.paramId;
                    }
                    if (!ok)
                        bad << " [" << names[i] << "]";
                    factoryOk = factoryOk && ok;
                    if (i == 0)
                        shoot(9, "ui_verlauf_stueck.png");
                }
                const bool pathOk = proc->getDrawnPath(0).points.size() > 10;
                const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getChildFile("rysum-test-stueck")
                                     .getChildFile(juce::String("Probe") + RysumProcessor::kPieceExtension);
                juce::String err;
                const auto before = rysum::autoToTree(S).toXmlString();
                const bool saved = proc->savePiece(tmp, err) && tmp.existsAsFile() && proc->getPieceName() == "Probe";
                proc->newPiece();
                const bool emptied = S.lanes.empty() && proc->getPieceName().isEmpty();
                const bool loaded = proc->loadPiece(tmp, err) && rysum::autoToTree(S).toXmlString() == before &&
                                    !proc->isPieceChanged() && proc->getPieceFile() == tmp;
                tmp.getParentDirectory().deleteRecursively();
                std::cout << "verlauf pieces: the 6 that come with it " << (int)factoryOk << bad << ", its path "
                          << (int)pathOk << ", saved " << (int)saved << ", new = empty " << (int)emptied
                          << ", loaded back the same " << (int)loaded
                          << ((factoryOk && pathOk && saved && emptied && loaded) ? "  ok" : "  FAIL") << std::endl;
                proc->setDrawnPath(0, {});
                m0->setValueNotifyingHost(m0Before);
                proc->newPiece();
            }
            S = AutoSet();
            proc->autoChanged();
            proc->autoPublish();
            proc->autoSeek(0.0);
        }
        {
            proc->apvts.getParameter("windModel")->setValueNotifyingHost(1.0f);
            shoot(3, "ui_klang_wind2.png");
            proc->apvts.getParameter("windModel")->setValueNotifyingHost(0.0f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->openMidiMap();
            shoot(5, "ui_midi.png");
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
                proc->clearMidiMap(i);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
        }
        {
            // right-click menus are top-level windows without a parent: they must get the plugin's
            // look by default
            auto &lf = juce::LookAndFeel::getDefaultLookAndFeel();
            const bool ours = &lf == &editor->getLookAndFeel();
            std::cout << "default look = plugin look (menus, dialogs): " << (ours ? "ok" : "FAIL")
                      << std::endl;
            // paint a menu the way JUCE does, to look at it
            juce::Image img(juce::Image::ARGB, 260, 150, true);
            {
                juce::Graphics g(img);
                lf.drawPopupMenuBackground(g, 260, 150);
                const char *items[]{"Verstimmung", "Modulieren mit", "Entfernen: LFO 1 (+25 %)",
                                    "Modulationsseite \xc3\xb6"
                                    "ffnen"};
                for (int i = 0; i < 4; ++i)
                    lf.drawPopupMenuItem(g, {4, 6 + i * 34, 252, 30}, false, true, i == 2, false,
                                         i == 1, juce::String::fromUTF8(items[i]), {}, nullptr,
                                         nullptr);
            }
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile("ui_menu.png");
            f.deleteFile();
            juce::FileOutputStream os(f);
            png.writeImageToStream(img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        }
        {
            // effects page with some effects switched on (so the diagrams show something)
            for (auto [id, v] : {std::pair<const char *, float>{"fxEchoLevel", 0.5f},
                                 {"fxPartSpread", 70.0f},
                                 {"fxEchoTilt", 10.0f}})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            {
                // own sounds from a chosen folder (not <sample-library>/noise/eigene): read, kept
                // in the state, back to standard
                auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("rysum_own_sounds_test");
                dir.deleteRecursively();
                dir.getChildFile("sub").createDirectory();
                juce::AudioBuffer<float> snd(1, 9600);
                for (int i = 0; i < 9600; ++i)
                    snd.setSample(0, i,
                                  0.3f * std::exp(-static_cast<float>(i) / 900.0f) *
                                      std::sin(static_cast<float>(i) * 0.7f));
                for (auto name : {"a.wav", "sub/b.wav"})
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::OutputStream> stream(
                        dir.getChildFile(name).createOutputStream());
                    const auto options = juce::AudioFormatWriterOptions{}
                                             .withSampleRate(48000.0)
                                             .withNumChannels(1)
                                             .withBitsPerSample(24);
                    auto writer = wav.createWriterFor(stream, options);
                    if (writer == nullptr || !writer->writeFromAudioSampleBuffer(snd, 0, 9600))
                    {
                        std::cerr << "Failed to write custom sound fixture: " << name << std::endl;
                        setApplicationReturnValue(1);
                        quit();
                        return;
                    }
                }
                auto waitFor = [&](int n)
                {
                    for (int k = 0; k < 150 && proc->getNumCustomSounds() != n; ++k)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
                };
                const int before = proc->getNumCustomSounds();
                proc->setCustomFolder(dir);
                waitFor(2);
                const int got = proc->getNumCustomSounds();
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                proc->setCustomFolder({});
                waitFor(before);
                proc->setStateInformation(mb.getData(), (int)mb.getSize());
                waitFor(2);
                const bool restored =
                    proc->getCustomFolder() == dir && proc->getNumCustomSounds() == 2;
                shoot(4, "ui_effekte_ordner.png");
                proc->setCustomFolder({});
                waitFor(before);
                std::cout << "own sounds from a chosen folder: read " << got << ", from the state "
                          << restored << ", back to standard "
                          << (!proc->hasOwnCustomFolder() && proc->getNumCustomSounds() == before)
                          << ((got == 2 && restored && !proc->hasOwnCustomFolder()) ? "  ok"
                                                                                    : "  FAIL")
                          << std::endl;
                dir.deleteRecursively();
            }
            shoot(4, "ui_effekte.png");
            for (auto *id : {"fxEchoLevel", "fxPartSpread", "fxEchoTilt"})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->getDefaultValue());
            {
                // second tab: Stimmung & Belichtung
                std::function<juce::Button *(juce::Component *, const juce::String &)> fb =
                    [&](juce::Component *c, const juce::String &t) -> juce::Button *
                {
                    if (!c->isVisible())
                        return nullptr;
                    if (auto *b = dynamic_cast<juce::TextButton *>(c))
                        if (b->getButtonText() == t)
                            return b;
                    for (auto *ch : c->getChildren())
                        if (auto *f = fb(ch, t))
                            return f;
                    return nullptr;
                };
                auto setV = [&](const char *id, float v)
                {
                    if (auto *prm = proc->apvts.getParameter(id))
                        prm->setValueNotifyingHost(prm->convertTo0to1(v));
                };
                setV("fxGhostLevel", -6.0f);
                setV("fxDexMix", 0.5f);
                setV("fxExpoMix", 0.4f);
                if (auto *c = dynamic_cast<RysumContent *>(content))
                {
                    c->showView(4);
                    c->tick();
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                if (auto *b = fb(content, juce::String::fromUTF8("Stimmung & Belichtung")))
                    b->triggerClick();
                else
                    std::cout << "Stimmung & Belichtung tab button not found  FAIL" << std::endl;
                juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
                shoot(4, "ui_effekte_stimmung_belichtung.png");
                {
                    // third tab: the living organ - a Kalkant, a walking listener, some worn pipes
                    setV("lvBellows", 0.8f);
                    setV("lvBlower", 1.0f);
                    setV("lvWalkShape", 4.0f);
                    setV("lvWalkSize", 5.0f);
                    setV("lvWear", 1.0f);
                    for (int k = 0; k < rysum::kNumKeys; ++k)
                        proc->engine.wear[0][(size_t)k] = (float)((k * 7) % 11) / 11.0f;
                    {
                        // the wear belongs to the project, not to a patch
                        juce::MemoryBlock ws;
                        proc->getStateInformation(ws);
                        proc->engine.wear[0][5] = 0.0f;
                        proc->setStateInformation(ws.getData(), (int)ws.getSize());
                        std::cout << "wear in the project state: "
                                  << (std::abs(proc->engine.wear[0][5].load() - 2.0f / 11.0f) <
                                              0.001f
                                          ? "ok"
                                          : "FAIL")
                                  << std::endl;
                    }
                    juce::AudioBuffer<float> lb(36, 512);
                    // (the lowest pressure while the chord sounds: at one instant it may just have been
                    // lifted by a tread)
                    float lowest = 1.0f;
                    for (int b2 = 0; b2 < 300; ++b2)
                    {
                        juce::MidiBuffer mb2;
                        if (b2 == 0)
                            for (int m : {48, 55, 60, 64, 67})
                                mb2.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                        lb.clear(), proc->processBlock(lb, mb2);
                        lowest = juce::jmin(lowest, proc->engine.bellowsNow.load());
                    }
                    if (auto *b = fb(content, "Lebendige Orgel"))
                        b->triggerClick();
                    else
                        std::cout << "Lebendige Orgel tab button not found  FAIL" << std::endl;
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
                    shoot(4, "ui_effekte_lebendige_orgel.png");
                    // the "Summe": EQ and compressor on the output
                    {
                        using rysum::MasterBus;
                        rysum::MasterSettings ms;
                        ms.eqOn = true;
                        ms.midDb = 6.0f;
                        ms.lowDb = -6.0f;
                        const float atMid = MasterBus::responseDb(ms, 48000.0, 1000.0f),
                                    atLow = MasterBus::responseDb(ms, 48000.0, 30.0f),
                                    atHigh = MasterBus::responseDb(ms, 48000.0, 15000.0f);
                        // a 1 kHz tone at 0.25 through it: twice as loud; then the compressor on a
                        // loud tone (0 dBFS, threshold -20 dB, 4:1): about -15 dB
                        auto peakAfter = [](rysum::MasterBus &mb, const rysum::MasterSettings &st, float amp)
                        {
                            mb.prepare(48000.0);
                            std::vector<float> a(48000), b2(48000);
                            for (int i = 0; i < 48000; ++i)
                                a[(size_t)i] = b2[(size_t)i] = amp * std::sin(juce::MathConstants<float>::twoPi * 1000.0f * (float)i / 48000.0f);
                            float *ch[2]{a.data(), b2.data()};
                            for (int i = 0; i < 48000; i += 480)
                            {
                                float *c2[2]{ch[0] + i, ch[1] + i};
                                mb.process(c2, 2, 480, st);
                            }
                            float p = 0.0f;
                            for (int i = 24000; i < 48000; ++i)
                                p = juce::jmax(p, std::abs(a[(size_t)i]));
                            return p;
                        };
                        rysum::MasterBus mb;
                        const float eqPeak = peakAfter(mb, ms, 0.25f);
                        rysum::MasterSettings cs;
                        cs.compOn = true;
                        cs.threshDb = -20.0f;
                        cs.ratio = 4.0f;
                        cs.attackMs = 0.1f; // (the static curve: the detector on every crest)
                        const float compDb = 20.0f * std::log10(peakAfter(mb, cs, 1.0f));
                        const bool ok = std::abs(atMid - 6.0f) < 0.2f && std::abs(atLow + 6.0f) < 0.5f &&
                                        std::abs(atHigh) < 0.3f && std::abs(eqPeak - 0.5f) < 0.03f &&
                                        std::abs(compDb + 15.0f) < 1.0f;
                        std::cout << "Summe: EQ +6 dB at 1 kHz = " << atMid << ", -6 dB low shelf at 30 Hz = " << atLow
                                  << ", 15 kHz = " << atHigh << ", tone 0.25 -> " << eqPeak
                                  << "; compressor 0 dBFS at -20 dB, 4:1 -> " << compDb << " dB"
                                  << (ok ? "  ok" : "  FAIL") << std::endl;
                        proc->apvts.getParameter("mstEqOn")->setValueNotifyingHost(1.0f);
                        proc->apvts.getParameter("mstMidDb")->setValueNotifyingHost(
                            proc->apvts.getParameter("mstMidDb")->convertTo0to1(4.0f));
                        proc->apvts.getParameter("mstCompOn")->setValueNotifyingHost(1.0f);
                        if (auto *b = fb(content, "Summe"))
                            b->triggerClick();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
                        shoot(4, "ui_effekte_summe.png");
                        for (auto *id : {"mstEqOn", "mstMidDb", "mstCompOn"})
                        {
                            auto *pp = proc->apvts.getParameter(id);
                            pp->setValueNotifyingHost(pp->getDefaultValue());
                        }
                        if (auto *b = fb(content, "Lebendige Orgel"))
                            b->triggerClick();
                    }
                    std::cout << "living organ in the plugin: bellows pressure down to "
                              << lowest << ", listener at "
                              << proc->engine.listenerXNow.load() << " / "
                              << proc->engine.listenerYNow.load() << " m"
                              << (lowest < 0.999f ? "  ok" : "  FAIL")
                              << std::endl;
                    juce::MidiBuffer offs;
                    for (int m : {48, 55, 60, 64, 67})
                        offs.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    lb.clear(), proc->processBlock(lb, offs);
                    for (auto *id :
                         {"lvBellows", "lvBlower", "lvWalkShape", "lvWalkSize", "lvWear"})
                        if (auto *prm = proc->apvts.getParameter(id))
                            prm->setValueNotifyingHost(prm->getDefaultValue());
                    proc->resetWear();
                    for (int b2 = 0; b2 < 60; ++b2)
                    {
                        juce::MidiBuffer none;
                        lb.clear(), proc->processBlock(lb, none);
                    }
                }
                if (auto *b = fb(content, juce::String::fromUTF8("Kl\xc3\xa4nge")))
                    b->triggerClick();
                for (auto *id : {"fxGhostLevel", "fxDexMix", "fxExpoMix"})
                    if (auto *prm = proc->apvts.getParameter(id))
                        prm->setValueNotifyingHost(prm->getDefaultValue());
            }
        }
        {
            // the simulated room: on, a note played, ray tracing done in the background
            auto *P = proc->apvts.getParameter("simOn");
            P->setValueNotifyingHost(1.0f);
            juce::AudioBuffer<float> st(16, 512);
            for (int b = 0; b < 60; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 2)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
                st.clear(), proc->processBlock(st, mb);
            }
            for (int i = 0; i < 40 && !proc->getRoomStatus().contains("Strahlen"); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            for (int b = 0; b < 20; ++b)
            {
                juce::MidiBuffer mb;
                st.clear(), proc->processBlock(st, mb);
            } // the engine takes the response
            std::cout << "Saal: " << proc->getRoomStatus()
                      << (proc->getRoomStatus().contains("Strahlen") ? "  ok" : "  FAIL")
                      << std::endl;
            std::cout << "Saal: engine RT " << proc->engine.roomRtLow.load() << " s, tail in use "
                      << proc->engine.tailSerialInUse.load() << ", " << proc->engine.erActive.load()
                      << " sounds with reflections"
                      << (proc->engine.erActive.load() > 0 &&
                                  proc->engine.tailSerialInUse.load() != 0xffffffffu
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            // Lucier mode on: the response of 12 passes is built in the background, the engine
            // takes it
            auto *L = proc->apvts.getParameter("lucOn");
            if (auto *md = proc->apvts.getParameter("lucMode"))
                md->setValueNotifyingHost(1.0f); // live
            L->setValueNotifyingHost(1.0f);
            if (auto *g = proc->apvts.getParameter("lucGen"))
                g->setValueNotifyingHost(g->convertTo0to1(12.0f));
            for (int i = 0; i < 60 && proc->getLucierView().curve.empty(); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            for (int b = 0; b < 200; ++b)
            {
                juce::MidiBuffer mb;
                st.clear(), proc->processBlock(st, mb);
            }
            const auto &lv = proc->getLucierView();
            std::cout << "Lucier: " << lv.what << ", gen " << lv.generation << ", "
                      << lv.peaks.size() << " resonances, first "
                      << (lv.peaks.empty() ? 0.0f : lv.peaks[0]) << " Hz, engine level "
                      << proc->engine.lucierLevel.load()
                      << (lv.curve.size() > 10 &&
                                  proc->engine.lucierSerialInUse.load() != 0xffffffffu
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            shoot(2, "ui_saal.png");
            {
                // the piece: record 3 s of the organ, then it plays: the recording, generation 1, 2
                // - by itself, then it ends
                auto setReal = [&](const char *id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                setReal("lucGen", 2.0f);
                setReal("lucGap", 0.5f);
                setReal("lucMix", 0.5f);
                setReal("lucRecSrc", 0.0f);
                proc->lucierRecord();
                auto &pc = proc->getLucierPiece();
                const bool modeOk = proc->isLucierPieceMode();
                for (int b = 0; b < 280; ++b) // 3 s
                {
                    juce::MidiBuffer mb;
                    if (b == 10)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    if (b == 200)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                for (int i = 0; i < 20 && proc->getLucierView().curve.empty(); ++i)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
                proc->lucierStopRecording();
                const double recSec = (double)pc.getRecording().size() / pc.recordingRate();
                const bool saved = juce::File(pc.recordingFile).existsAsFile();
                // play it through: blocks of audio, the message thread computing the generations in
                // between
                int maxGen = 0;
                float peak1 = 0.0f;
                bool ended = false;
                for (int b = 0; b < 4000 && !ended; ++b)
                {
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                    if (b % 8 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                    maxGen = std::max(maxGen, pc.playingGen.load());
                    if (pc.playingGen.load() >= 1)
                        peak1 = std::max(peak1, st.getMagnitude(0, 0, 512));
                    ended = pc.state.load() == rysum::LucierPiece::stFinished;
                }
                std::cout << "Lucier piece: mode " << modeOk << ", recorded " << recSec
                          << " s, saved " << saved << ", reached generation " << maxGen
                          << ", level from gen 1 " << peak1 << ", ended " << ended
                          << ((modeOk && recSec > 1.0 && saved && maxGen == 2 && peak1 > 0.01f &&
                               ended)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                proc->lucierPlay();
                for (int b = 0; b < 300; ++b)
                {
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                    if (b % 8 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                }
                shoot(2, "ui_saal_stueck.png");
                proc->lucierStop();
                juce::File(pc.recordingFile).deleteFile();
                if (auto *md = proc->apvts.getParameter("lucMode"))
                    md->setValueNotifyingHost(1.0f);
            }
            {
                // several rooms: the Gedackt in Saal 2 (a chapel) - its own late reverb is built
                // and used
                auto setReal = [&](const juce::String &id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                setReal(RysumProcessor::stopRoomParamId(1), 1.0f);
                setReal("stop1", 1.0f);
                for (int b = 0; b < 40; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 1)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                for (int i = 0; i < 80 && proc->engine.tailSerialInUseR[1].load() == 0xffffffffu;
                     ++i)
                {
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                }
                const int used = proc->engine.roomsUsed.load();
                std::cout << "rooms: used mask " << used << ", room 2 RT "
                          << proc->engine.heard[1].rtLow.load() << " s, tail in use "
                          << proc->engine.tailSerialInUseR[1].load()
                          << (((used & 2) != 0 &&
                               proc->engine.tailSerialInUseR[1].load() != 0xffffffffu &&
                               proc->engine.heard[1].rtLow.load() > 1.0f)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                shoot(2, "ui_saal_raeume.png");
                {
                    juce::MidiBuffer mb;
                    mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                setReal(RysumProcessor::stopRoomParamId(1), 0.0f);
            }
            {
                // floor plans: L-shape, cross, drawn - the late reverb is rebuilt for each, the
                // page shows the plan
                L->setValueNotifyingHost(0.0f);
                auto setReal = [&](const juce::String &id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                auto settle = [&]
                {
                    for (int b = 0; b < 30; ++b)
                    {
                        juce::MidiBuffer mb;
                        st.clear(), proc->processBlock(st, mb);
                    }
                    for (int i = 0; i < 40; ++i)
                    {
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
                        juce::MidiBuffer mb;
                        st.clear(), proc->processBlock(st, mb);
                    }
                };
                const float rtBox = proc->engine.roomRtLow.load();
                const juce::String names[]{"l", "kreuz", "rund", "gezeichnet"};
                const int shapes[]{rysum::shapeL, rysum::shapeCross, rysum::shapeRound,
                                   rysum::shapeDrawn};
                for (int i = 0; i < 4; ++i)
                {
                    setReal("simShape", (float)shapes[i]);
                    setReal("simShapeAmt", 0.7f);
                    {
                        juce::MidiBuffer mb;
                        mb.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
                        st.clear(), proc->processBlock(st, mb);
                    }
                    settle();
                    std::cout << "Saal plan " << names[i] << ": RT "
                              << proc->engine.roomRtLow.load() << " s (shoebox " << rtBox << " s), "
                              << proc->engine.erActive.load() << " sounds with reflections, "
                              << proc->getRoomStatus() << std::endl;
                    shoot(2, "ui_saal_plan_" + names[i] + ".png");
                }
                // the drawn plan goes through the project state
                rysum::FloorPlan d;
                d.n = 5;
                const float xs[]{0.0f, 1.0f, 0.8f, 0.3f, 0.1f}, ys[]{0.1f, 0.0f, 1.0f, 0.85f, 0.6f};
                for (int k = 0; k < 5; ++k)
                {
                    d.x[(size_t)k] = xs[k];
                    d.y[(size_t)k] = ys[k];
                }
                proc->setDrawnPlan(2, d);
                juce::MemoryBlock mb2;
                proc->getStateInformation(mb2);
                proc->setDrawnPlan(2, rysum::defaultDrawnPlan());
                proc->setStateInformation(mb2.getData(), (int)mb2.getSize());
                const auto &back = proc->getDrawnPlan(2);
                std::cout << "drawn floor plan: " << back.n << " corners after the state round trip"
                          << ((back.n == 5 && std::abs(back.x[2] - 0.8f) < 1e-3f &&
                               proc->engine.getDrawnPlan(2).n == 5)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                setReal("simShape", 0.0f);
                setReal("simShapeAmt", 0.5f);
                L->setValueNotifyingHost(1.0f);
            }
            L->setValueNotifyingHost(0.0f);
            {
                juce::MidiBuffer mb;
                mb.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
                st.clear(), proc->processBlock(st, mb);
            }
            P->setValueNotifyingHost(0.0f);
        }

        if (auto *c = dynamic_cast<RysumContent *>(content))
        {
            // MIDI buttons -> buttons on screen ("MIDI-Tasten zuordnen"), and the immersive 3D view
            juce::AudioBuffer<float> st(16, 512);
            auto send = [&](const juce::MidiMessage &m)
            {
                juce::MidiBuffer mb;
                mb.addEvent(m, 0);
                st.clear(), proc->processBlock(st, mb);
            };
            auto pump = [&]
            {
                c->tick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
            };
            auto &learn = c->buttonLearn();
            // every button of every page has a path of its own that leads back to it
            int count = 0, wrong = 0;
            {
                juce::StringArray seen;
                std::function<void(juce::Component *)> each = [&](juce::Component *p)
                {
                    for (auto *ch : p->getChildren())
                    {
                        if (auto *b = dynamic_cast<juce::Button *>(ch))
                        {
                            const auto path = rysum::ui::buttonPath(*c, *b);
                            ++count;
                            if (path.isEmpty() || seen.contains(path) ||
                                rysum::ui::findButton(*c, path) != b)
                                ++wrong;
                            seen.add(path);
                        }
                        each(ch);
                    }
                };
                each(c);
            }
            std::cout << "MIDI-Tasten: " << count << " buttons, " << wrong
                      << " without a path of their own" << (count > 100 && wrong == 0 ? "  ok" : "  FAIL")
                      << std::endl;
            // a button without a parameter (the page "Klang"): a controller button clicks it
            c->showView(0);
            auto *tab = rysum::ui::findButton(*c, "@seite-3");
            bool tabOk = false;
            juce::String tabName;
            if (tab != nullptr)
            {
                c->startButtonLearn();
                const bool waits = learn.learn(*tab);
                send(juce::MidiMessage::controllerEvent(1, 30, 127));
                send(juce::MidiMessage::controllerEvent(1, 30, 0));
                pump();
                tabName = learn.mappingOf(*tab);
                const bool notYet = !tab->getToggleState(); // (assigning does not operate)
                c->stopButtonLearn();
                send(juce::MidiMessage::controllerEvent(1, 30, 127));
                send(juce::MidiMessage::controllerEvent(1, 30, 0));
                pump();
                tabOk = waits && notYet && tab->getToggleState() && tabName.startsWith("CC 30");
            }
            std::cout << "MIDI-Tasten: CC -> page button: \"" << tabName << "\""
                      << (tabOk ? "  ok" : "  FAIL") << std::endl;
            // a switch with a parameter ("Saal an"), learned from a pad that sends a note: the
            // note flips the parameter and plays no pipe
            {
                juce::Button *sw = nullptr;
                std::function<void(juce::Component *)> each = [&](juce::Component *p)
                {
                    for (auto *ch : p->getChildren())
                    {
                        if (auto *b = dynamic_cast<juce::Button *>(ch))
                            if (b->getProperties()["midiParam"].toString() == "simOn")
                                sw = b;
                        each(ch);
                    }
                };
                each(c);
                bool ok = false;
                juce::String name;
                if (sw != nullptr)
                {
                    auto *q = proc->apvts.getParameter("simOn");
                    const float before = q->getValue();
                    c->startButtonLearn();
                    learn.learn(*sw);
                    send(juce::MidiMessage::noteOn(10, 36, (juce::uint8)100));
                    send(juce::MidiMessage::noteOff(10, 36));
                    pump();
                    name = learn.mappingOf(*sw);
                    c->stopButtonLearn();
                    const bool same = std::abs(q->getValue() - before) < 0.01f;
                    const int heldBefore = proc->keyboardState.isNoteOn(10, 36) ? 1 : 0;
                    send(juce::MidiMessage::noteOn(10, 36, (juce::uint8)100));
                    const bool silent = !proc->keyboardState.isNoteOn(10, 36);
                    send(juce::MidiMessage::noteOff(10, 36));
                    pump();
                    const bool flipped = std::abs(q->getValue() - before) > 0.5f;
                    // ... and it goes through the project
                    juce::MemoryBlock state;
                    proc->getStateInformation(state);
                    learn.forget(*sw);
                    const bool gone = learn.mappingOf(*sw).isEmpty();
                    proc->setStateInformation(state.getData(), (int)state.getSize());
                    pump();
                    const bool back = learn.mappingOf(*sw) == name && tab != nullptr &&
                                      learn.mappingOf(*tab) == tabName;
                    ok = same && heldBefore == 0 && silent && flipped && gone && back &&
                         name.startsWith("Note");
                    std::cout << "MIDI-Tasten: note -> switch: \"" << name << "\"  not while assigning "
                              << same << ", silent " << silent << ", flipped " << flipped
                              << ", saved " << (gone && back) << (ok ? "  ok" : "  FAIL")
                              << std::endl;
                    learn.forget(*sw);
                    q->setValueNotifyingHost(before);
                }
                else
                    std::cout << "MIDI-Tasten: switch not found  FAIL" << std::endl;
            }
            // the page of assignments, as a picture
            c->showView(0);
            c->startButtonLearn();
            shoot(0, "ui_midi_tasten.png");
            c->stopButtonLearn();
            // the 3D view: a held button (Q Greifen) follows the MIDI button; "Immersiv" shows
            // nothing but the picture, over the whole window
            std::function<rysum::ui::WalkView *(juce::Component *)> findWalk =
                [&](juce::Component *p) -> rysum::ui::WalkView *
            {
                if (auto *w = dynamic_cast<rysum::ui::WalkView *>(p))
                    return w;
                for (auto *ch : p->getChildren())
                    if (auto *f = findWalk(ch))
                        return f;
                return nullptr;
            };
            if (auto *wv = findWalk(c))
            {
                wv->setTestMode(true);
                c->showView(8);
                auto *grab = dynamic_cast<rysum::ui::HoldButton *>(
                    rysum::ui::findButton(*c, "@begehen/Q Greifen"));
                bool holdOk = false;
                if (grab != nullptr)
                {
                    c->startButtonLearn();
                    learn.learn(*grab);
                    send(juce::MidiMessage::controllerEvent(1, 31, 127));
                    send(juce::MidiMessage::controllerEvent(1, 31, 0));
                    pump();
                    c->stopButtonLearn();
                    send(juce::MidiMessage::controllerEvent(1, 31, 127));
                    c->tick();
                    const bool down = grab->held();
                    send(juce::MidiMessage::controllerEvent(1, 31, 0));
                    c->tick();
                    holdOk = down && !grab->held();
                    learn.forget(*grab);
                }
                std::cout << "MIDI-Tasten: held button follows the MIDI button"
                          << (holdOk ? "  ok" : "  FAIL") << std::endl;
                const auto windowed = wv->getBounds();
                wv->setImmersive(true);
                int visibleChildren = 0;
                for (auto *ch : wv->getChildren())
                    visibleChildren += ch->isVisible() ? 1 : 0;
                const bool fills = wv->getBounds() == c->getLocalBounds();
                shoot(8, "ui_begehen_immersiv.png");
                const bool stays = wv->isImmersive();
                wv->setImmersive(false);
                int shownAgain = 0;
                for (auto *ch : wv->getChildren())
                    shownAgain += ch->isVisible() ? 1 : 0;
                std::cout << "Begehen immersiv: fills the window " << fills << ", controls shown "
                          << visibleChildren << ", afterwards " << shownAgain
                          << (fills && stays && visibleChildren == 0 && shownAgain > 10 &&
                                      wv->getBounds() == windowed
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
            }
            if (tab != nullptr)
                learn.forget(*tab);
            c->showView(0);
        }

        {
            // the 3D Saal's illusions through the whole plugin: Zeit als Entfernung (the memory is
            // made by the timer, the output falls back in time), Hoerraum (the late reverb is
            // built for the larger room), Fluestern (the ears' difference at the output)
            auto setReal = [&](const char *id, float v)
            {
                if (auto *q = proc->apvts.getParameter(id))
                    q->setValueNotifyingHost(q->convertTo0to1(v));
            };
            juce::AudioBuffer<float> st(16, 512);
            auto run = [&](int blocks, std::initializer_list<int> on, double *l = nullptr, double *r = nullptr)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0)
                        for (int n : on)
                            mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                    st.clear(), proc->processBlock(st, mb);
                    for (int i = 0; i < 512 && l != nullptr; ++i)
                    {
                        *l += (double)st.getSample(0, i) * st.getSample(0, i);
                        *r += (double)st.getSample(1, i) * st.getSample(1, i);
                    }
                }
            };
            auto pumpRun = [&](int times)
            {
                for (int i = 0; i < times; ++i)
                {
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
                    run(8, {});
                }
            };
            auto off = [&]
            {
                juce::MidiBuffer mb;
                for (int n : {48, 55, 60})
                    mb.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                st.clear(), proc->processBlock(st, mb);
            };
            // Zeit als Entfernung
            setReal("lvListenY", 8.0f);
            setReal("lvTimeDepth", 10.0f);
            run(8, {48, 55, 60});
            pumpRun(40); // (~3.4 s of sound, the timer in between)
            const float back = proc->timeBackNow.load();
            double pastL = 0, pastR = 0;
            run(20, {}, &pastL, &pastR); // (so far back nothing was played yet: silence)
            setReal("lvTimeDepth", 0.0f);
            pumpRun(40);
            double nowL = 0, nowR = 0;
            run(20, {}, &nowL, &nowR);
            std::cout << "Zeit als Entfernung (plugin): hearing " << back << " s back; energy there " << pastL + pastR
                      << ", back in the present " << nowL + nowR << ", then " << proc->timeBackNow.load() << " s back"
                      << (back > 3.0f && pastL + pastR < 1.0e-6 * (nowL + nowR) && nowL + nowR > 1.0e-4 &&
                                  proc->timeBackNow.load() == 0.0f
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            setReal("lvListenY", 0.0f);
            off();
            // Hoerraum: the Saal's late reverb (ray-traced) for a room four times the size
            setReal("simOn", 1.0f);
            pumpRun(60);
            const float rtSeen = proc->engine.heard[0].rtLow.load();
            setReal("lvRoomHear", 4.0f);
            pumpRun(120);
            const float rtHeard = proc->engine.heard[0].rtLow.load();
            setReal("lvRoomHear", 1.0f);
            pumpRun(60);
            std::cout << "Hoerraum (plugin): reverb time as seen " << rtSeen << " s, four times the size " << rtHeard
                      << " s, back " << proc->engine.heard[0].rtLow.load() << " s"
                      << (rtSeen > 0.2f && rtHeard > 2.0f * rtSeen &&
                                  std::abs(proc->engine.heard[0].rtLow.load() - rtSeen) < 0.1f * rtSeen
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            setReal("simOn", 0.0f);
            // Fluestern: binaural, the stop's pipes half a metre to the left, the listener walks
            // up to 15 cm - the left ear gains much more than the right
            if (proc->binauralReady())
            {
                auto *out = proc->apvts.getParameter("output");
                const float outBefore = out->getValue();
                out->setValueNotifyingHost(out->convertTo0to1((float)RysumProcessor::kBinauralOutput));
                std::array<float, rysum::kNumKeys> az{}, dist{};
                for (int k = 0; k < rysum::kNumKeys; ++k)
                {
                    az[(size_t)k] = proc->engine.baseAz[0][(size_t)k].load();
                    dist[(size_t)k] = proc->engine.baseDist[0][(size_t)k].load();
                    proc->engine.baseAz[0][(size_t)k] = 90.0f;
                    proc->engine.baseEl[0][(size_t)k] = 0.0f;
                    proc->engine.baseDist[0][(size_t)k] = 0.5f;
                }
                setReal("lvListenX", -0.35f);
                auto ears = [&](float whisper)
                {
                    setReal("lvWhisper", whisper);
                    run(40, {60});
                    double l = 0, r = 0;
                    run(60, {}, &l, &r);
                    juce::MidiBuffer mb;
                    mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    st.clear(), proc->processBlock(st, mb);
                    run(80, {});
                    return std::make_pair(10.0 * std::log10(juce::jmax(1e-30, l)), 10.0 * std::log10(juce::jmax(1e-30, r)));
                };
                const auto without = ears(0.0f), with = ears(1.0f);
                std::cout << "Fluestern (plugin, binaural): left - right without " << without.first - without.second
                          << " dB, with " << with.first - with.second << " dB; the left ear changes by "
                          << with.first - without.first << " dB"
                          << (with.first - with.second > without.first - without.second + 6.0
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                setReal("lvWhisper", 0.0f);
                setReal("lvListenX", 0.0f);
                for (int k = 0; k < rysum::kNumKeys; ++k)
                {
                    proc->engine.baseAz[0][(size_t)k] = az[(size_t)k];
                    proc->engine.baseDist[0][(size_t)k] = dist[(size_t)k];
                }
                out->setValueNotifyingHost(outBefore);
                pumpRun(10);
            }
            else
                std::cout << "Fluestern (plugin): no binaural decoder  FAIL" << std::endl;
        }

        {
            // "Begehen": the Saal in 3D. A chord sounds; walking moves the listener in the engine;
            // a model loads and goes through the project state
            auto setReal = [&](const char *id, float v)
            {
                if (auto *q = proc->apvts.getParameter(id))
                    q->setValueNotifyingHost(q->convertTo0to1(v));
            };
            setReal("simOn", 1.0f);
            juce::AudioBuffer<float> st(16, 512);
            auto run = [&](int blocks, std::initializer_list<int> on)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0)
                        for (int n : on)
                            mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
            };
            std::function<rysum::ui::WalkView *(juce::Component *)> findWalk =
                [&](juce::Component *c) -> rysum::ui::WalkView *
            {
                if (auto *w = dynamic_cast<rysum::ui::WalkView *>(c))
                    return w;
                for (auto *ch : c->getChildren())
                    if (auto *f = findWalk(ch))
                        return f;
                return nullptr;
            };
            auto *wv = findWalk(content);
            if (wv != nullptr)
                wv->setTestMode(true);
            for (int i = 0; i < 4; ++i)
                proc->walkPose[(size_t)i] = 0.0f;
            proc->clearWalkModel();
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->showView(8);
            run(30, {48, 55, 60, 64, 67});
            shoot(8, "ui_begehen.png");
            std::cout << "Begehen: view on " << proc->engine.viewWalkOn.load()
                      << (proc->engine.viewWalkOn.load() && wv != nullptr ? "  ok" : "  FAIL")
                      << std::endl;
            if (wv != nullptr)
            {
                // two seconds forwards, then turned to the left by 90 degrees: the engine's
                // listener follows
                for (int i = 0; i < 40; ++i)
                    wv->stepForTest(0.05, true, false, false, false, 0.0f);
                run(40, {});
                const float wy = proc->engine.viewY.load(), ly = proc->engine.listenerYNow.load();
                std::cout << "Begehen: walked " << wy << " m forwards, listener in the engine "
                          << ly << " m"
                          << (wy > 2.5f && std::abs(ly - wy) < 0.1f ? "  ok" : "  FAIL")
                          << std::endl;
                wv->stepForTest(0.05, false, false, false, false, 90.0f);
                std::cout << "Begehen: yaw " << proc->engine.viewYaw.load()
                          << (std::abs(proc->engine.viewYaw.load() - 90.0f) < 0.01f ? "  ok"
                                                                                    : "  FAIL")
                          << std::endl;
                shoot(8, "ui_begehen_gedreht.png");
                // a room model (OBJ, z up): the small chapel from Walk3D
                auto objFile = outDir.getChildFile("test_kapelle.obj");
                objFile.replaceWithText(rysum::makeTestObj());
                proc->walkZUp = true;
                wv->loadModelForTest(objFile);
                for (int i = 0; i < 4; ++i)
                    proc->walkPose[(size_t)i] = 0.0f;
                if (auto *c = dynamic_cast<RysumContent *>(content))
                {
                    c->showView(2);
                    c->showView(8);
                }
                auto m = proc->getWalkMesh();
                std::cout << "Begehen: model " << (m ? (int)m->tris.size() : 0) << " triangles, "
                          << (m ? m->size.x : 0.0f) << " x " << (m ? m->size.y : 0.0f) << " x "
                          << (m ? m->size.z : 0.0f) << " m"
                          << (m && m->tris.size() == 16 * 12 &&
                                      std::abs(m->size.x - 12.8f) < 0.01f &&
                                      std::abs(m->size.z - 9.2f) < 0.01f
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                shoot(8, "ui_begehen_modell.png");
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                proc->clearWalkModel();
                proc->setStateInformation(mb.getData(), (int)mb.getSize());
                std::cout << "Begehen: model after the state round trip "
                          << (proc->getWalkMesh() != nullptr ? "loaded" : "missing")
                          << (proc->getWalkMesh() != nullptr && proc->walkZUp ? "  ok" : "  FAIL")
                          << std::endl;
                {
                    // the same file read with Y up (the usual OBJ convention): its height becomes
                    // the length
                    juce::String e;
                    auto m2 = rysum::loadObjRoom(objFile, false, 1.0f, e);
                    std::cout << "Begehen: read with Y up " << (m2 ? m2->size.x : 0.0f) << " x "
                              << (m2 ? m2->size.y : 0.0f) << " x " << (m2 ? m2->size.z : 0.0f)
                              << " m"
                              << (m2 && std::abs(m2->size.x - 12.8f) < 0.01f &&
                                          std::abs(m2->size.y - 9.2f) < 0.01f &&
                                          std::abs(m2->size.z - 24.8f) < 0.01f
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    // no line end after the last face, decimals (read the same whatever the locale)
                    auto f2 = outDir.getChildFile("test_dreieck.obj");
                    f2.replaceWithText("v 0 0 0\nv 1.5 0 0\nv 0 2.25 0.5\nf 1 2 3");
                    auto m3 = rysum::loadObjRoom(f2, true, 1.0f, e);
                    std::cout << "Begehen: one triangle without a final line end: "
                              << (m3 ? (int)m3->tris.size() : 0) << ", " << (m3 ? m3->size.x : 0.0f)
                              << " x " << (m3 ? m3->size.y : 0.0f) << " x "
                              << (m3 ? m3->size.z : 0.0f)
                              << (m3 && m3->tris.size() == 1 &&
                                          std::abs(m3->size.x - 1.5f) < 1e-4f &&
                                          std::abs(m3->size.y - 2.25f) < 1e-4f
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                }
                {
                    // speed: a model at the limit (200 x 200 quads = 80,000 triangles, a wavy floor
                    // over 24 m), painted 20 times
                    juce::MemoryOutputStream os;
                    const int N = 200;
                    for (int y = 0; y <= N; ++y)
                        for (int x = 0; x <= N; ++x)
                            os << "v " << juce::String(x * 0.12f, 3) << " "
                               << juce::String(y * 0.12f, 3) << " "
                               << juce::String(0.4f * std::sin(x * 0.2f) * std::cos(y * 0.17f), 3)
                               << "\n";
                    for (int y = 0; y < N; ++y)
                        for (int x = 0; x < N; ++x)
                        {
                            const int a = y * (N + 1) + x + 1;
                            os << "f " << a << " " << a + 1 << " " << a + N + 2 << " " << a + N + 1
                               << "\n";
                        }
                    auto big = outDir.getChildFile("test_gross.obj");
                    big.replaceWithText(os.toString());
                    proc->loadWalkModel(big, true, 1.0f);
                    auto mb3 = proc->getWalkMesh();
                    juce::Image frame(juce::Image::ARGB, wv->getWidth(), wv->getHeight(), true);
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int i = 0; i < 20; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
                    std::cout << "Begehen: " << (mb3 ? (int)mb3->tris.size() : 0)
                              << " triangles: " << ms << " ms per frame (" << wv->renderWidth()
                              << " px wide)"
                              << (mb3 && mb3->tris.size() == 80000 ? "  ok" : "  FAIL")
                              << std::endl;
                }
                proc->clearWalkModel();
                {
                    // the Saal's own room with all eight stops on and a chord sounding: the frame
                    // time at full resolution, then the way the app runs it (the resolution
                    // adapting), for the Saal and for the largest model
                    std::array<float, rysum::kNumStops> stopWas{};
                    for (int s = 0; s < rysum::kNumStops; ++s)
                    {
                        stopWas[(size_t)s] =
                            proc->apvts.getRawParameterValue(RysumProcessor::stopParamId(s))
                                ->load();
                        setReal(RysumProcessor::stopParamId(s).toRawUTF8(), 1.0f);
                    }
                    run(20, {48, 55, 60, 64, 67});
                    if (auto *c = dynamic_cast<RysumContent *>(content))
                    {
                        c->showView(2);
                        c->showView(8);
                    }
                    juce::Image frame(juce::Image::ARGB, wv->getWidth(), wv->getHeight(), true);
                    double t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int i = 0; i < 20; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
                    std::cout << "Begehen: all stops: " << ms << " ms per frame (geometry "
                              << wv->geometryMs() << " ms, 3D total " << wv->frameMs() << " ms, "
                              << wv->renderStats() << ", " << wv->renderWidth() << " px wide)"
                              << (wv->renderWidth() >= wv->getWidth() ? "  ok" : "  FAIL")
                              << std::endl;
                    wv->setTestMode(false);
                    for (int i = 0; i < 100; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    std::cout << "Begehen: adapting, the Saal settles at " << wv->renderWidth()
                              << " px wide (3D " << wv->frameMs() << " ms, geometry "
                              << wv->geometryMs() << " ms)"
                              << (wv->renderWidth() >= (int)(0.4f * (float)wv->getWidth())
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    proc->loadWalkModel(outDir.getChildFile("test_gross.obj"), true, 1.0f);
                    for (int i = 0; i < 100; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    std::cout << "Begehen: adapting, 80,000 triangles settle at "
                              << wv->renderWidth() << " px wide (3D " << wv->frameMs()
                              << " ms, geometry " << wv->geometryMs() << " ms)"
                              << (wv->renderWidth() >= (int)(0.4f * (float)wv->getWidth())
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    proc->clearWalkModel();
                    wv->setTestMode(true);
                    for (int s = 0; s < rysum::kNumStops; ++s)
                        setReal(RysumProcessor::stopParamId(s).toRawUTF8(), stopWas[(size_t)s]);
                    shoot(8, "ui_begehen_alle.png");
                }
                // what the walls look like follows the absorption (the Saal page's materials)
                {
                    const bool okM =
                        rysum::wallMaterial(0.5f, 0.85f, 0.3f).kind == rysum::Material::curtain &&
                        rysum::wallMaterial(0.12f, 0.55f, 0.3f).kind == rysum::Material::wood &&
                        rysum::wallMaterial(0.02f, 0.3f, 0.3f).kind == rysum::Material::stone &&
                        rysum::wallMaterial(0.04f, 0.1f, 0.3f).kind == rysum::Material::glass &&
                        rysum::wallMaterial(0.0f, 0.0f, 0.0f).kind == rysum::Material::mirror &&
                        rysum::wallMaterial(0.5f, 0.85f, 0.3f).tone <
                            rysum::wallMaterial(0.02f, 0.3f, 0.3f).tone;
                    std::cout << "Begehen: materials from the absorption (curtain, wood, stone, "
                                 "glass, mirror; curtain darker than stone)"
                              << (okM ? "  ok" : "  FAIL") << std::endl;
                    const struct
                    {
                        float aw, af, hi;
                        const char *name;
                    } looks[]{{0.5f, 0.35f, 0.85f, "vorhang"},
                              {0.12f, 0.15f, 0.55f, "holz"},
                              {0.04f, 0.06f, 0.1f, "glas"}};
                    for (auto &lk : looks)
                    {
                        setReal("simAbs", lk.aw);
                        setReal("simAbsFloor", lk.af);
                        setReal("simHigh", lk.hi);
                        run(4, {});
                        shoot(8, juce::String("ui_begehen_") + lk.name + ".png");
                    }
                    setReal("simAbs", 0.1f);
                    setReal("simAbsFloor", 0.25f);
                    setReal("simHigh", 0.5f);
                }
                // a cross-shaped church: walked (held inside the walls, as when walking) into an
                // arm until pipes are out of sight
                {
                    setReal("simShape", (float)rysum::shapeCross);
                    setReal("simShapeAmt", 0.8f);
                    for (int i = 0; i < 4; ++i)
                        proc->walkPose[(size_t)i] = 0.0f;
                    if (auto *c = dynamic_cast<RysumContent *>(content))
                    {
                        c->showView(2);
                        c->showView(8);
                    }
                    run(10, {});
                    int bestHidden = 0;
                    float bx = 0, by = 0;
                    // (a snake over the grid: the listener walks - at most 6 m/s - from one point
                    // to the next)
                    for (int ix = 0; ix <= 14; ++ix)
                        for (int j = 0; j <= 13; ++j)
                        {
                            const float ox = -7.0f + (float)ix,
                                        oy = (ix % 2 == 0) ? -10.0f + (float)j : 3.0f - (float)j;
                            wv->setPoseForTest(ox, oy, 0.0f);
                            run(24, {});
                            const int hid = proc->engine.hiddenNow.load();
                            if (hid > bestHidden)
                            {
                                bestHidden = hid;
                                bx = proc->walkPose[0].load();
                                by = proc->walkPose[1].load();
                            }
                        }
                    // look towards the crossing (the middle of the plan)
                    const float W = proc->engine.heard[0].W.load(),
                                L = proc->engine.heard[0].L.load();
                    const float hx = proc->engine.heard[0].Lx.load() * W,
                                hy = proc->engine.heard[0].Ly.load() * L;
                    const float dx = 0.5f * W - (hx + bx), dy = 0.5f * L - (hy + by);
                    wv->setPoseForTest(bx, by, juce::radiansToDegrees(std::atan2(-dx, dy)));
                    run(400, {}); // (walks back there)
                    std::cout << "Begehen: cross-shaped plan, at " << bx << " / " << by << " m "
                              << proc->engine.hiddenNow.load()
                              << " sounding pipes heard around the corner"
                              << (proc->engine.hiddenNow.load() > 0 ? "  ok" : "  FAIL")
                              << std::endl;
                    shoot(8, "ui_begehen_kreuz.png");
                    setReal("simShape", 0.0f);
                    setReal("simShapeAmt", 0.5f);
                    for (int i = 0; i < 4; ++i)
                        proc->walkPose[(size_t)i] = 0.0f;
                }
                // room motion: the Praestant flies, the Gedackt falls, and they knock (through the whole processor)
                {
                    setReal (RysumProcessor::stopParamId (1).toRawUTF8(), 1.0f);
                    setReal (RysumProcessor::motionParamId (0).toRawUTF8(), (float) rysum::Motion::Flight);
                    setReal (RysumProcessor::motionParamId (1).toRawUTF8(), (float) rysum::Motion::Fall);
                    setReal (RysumProcessor::rateParamId (0).toRawUTF8(), 0.5f);
                    setReal (RysumProcessor::rateParamId (1).toRawUTF8(), 0.6f);
                    setReal ("lvImpLevel", 0.0f); setReal ("lvImpSrc", 1.0f);
                    const uint32_t before = proc->engine.impactCount.load();
                    run (300, { 48, 55, 60, 64, 67 });
                    const uint32_t knocks = proc->engine.impactCount.load() - before;
                    float maxPeak = 0.0f;
                    {
                        juce::AudioBuffer<float> st2 (16, 512);
                        for (int b = 0; b < 100; ++b) { juce::MidiBuffer mb; st2.clear(); proc->processBlock (st2, mb); maxPeak = juce::jmax (maxPeak, st2.getMagnitude (0, 512)); }
                    }
                    rysum::Vec3f q;
                    std::cout << "room motion: " << knocks << " knocks in 3.2 s, output peak " << juce::Decibels::gainToDecibels (maxPeak) << " dB, body shown "
                              << (proc->engine.bodyAt (1, 10, q) ? "yes" : "no") << (knocks > 5 && proc->engine.bodyAt (1, 10, q) && maxPeak < 4.0f ? "  ok" : "  FAIL") << std::endl;
                    shoot (8, "ui_begehen_flug.png");
                    shoot (2, "ui_saal_flug.png");
                    if (auto* c = dynamic_cast<RysumContent*> (content)) c->showView (4);
                    std::function<juce::TextButton* (juce::Component*)> liveTab = [&] (juce::Component* c) -> juce::TextButton*
                    {
                        if (auto* b = dynamic_cast<juce::TextButton*> (c)) if (b->getButtonText() == "Lebendige Orgel") return b;
                        for (auto* ch : c->getChildren()) if (auto* f = liveTab (ch)) return f;
                        return nullptr;
                    };
                    if (auto* b = liveTab (content)) b->triggerClick();
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (80);
                    shoot (4, "ui_effekte_aufprall.png");
                    setReal (RysumProcessor::motionParamId (0).toRawUTF8(), 0.0f); setReal (RysumProcessor::motionParamId (1).toRawUTF8(), 0.0f);
                    setReal (RysumProcessor::stopParamId (1).toRawUTF8(), 0.0f);
                    setReal ("lvImpLevel", -60.0f); setReal ("lvImpSrc", 0.0f);
                }
            }
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->showView(2);
            std::cout << "Begehen: leaving the view gives the listener back "
                      << (proc->engine.viewWalkOn.load() ? "no" : "yes")
                      << (!proc->engine.viewWalkOn.load() ? "  ok" : "  FAIL") << std::endl;
            juce::MidiBuffer off;
            for (int n : {48, 55, 60, 64, 67})
                off.addEvent(juce::MidiMessage::noteOff(1, n), 0);
            st.clear(), proc->processBlock(st, off);
            setReal("simOn", 0.0f);
        }

        {
            // scenes: A with a soft Gedackt, B louder with more wind leaks; half way glides,
            // switches change at the middle
            auto setReal = [&](const char *id, float v)
            {
                if (auto *q = proc->apvts.getParameter(id))
                    q->setValueNotifyingHost(q->convertTo0to1(v));
            };
            auto real = [&](const char *id)
            { return proc->apvts.getRawParameterValue(id)->load(); };
            setReal("vol1", -10.0f);
            setReal("fxLeakLevel", 0.0f);
            setReal("tremOn", 0.0f);
            setReal("sceneMorph", 0.0f);
            proc->storeScene(0);
            setReal("vol1", 2.0f);
            setReal("fxLeakLevel", 0.8f);
            setReal("tremOn", 1.0f);
            proc->storeScene(1);
            const float master0 = real("master");
            setReal("sceneMorph", 0.25f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            const float v25 = real("vol1"), t25 = real("tremOn");
            setReal("sceneMorph", 0.75f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            const float v75 = real("vol1"), t75 = real("tremOn"), l75 = real("fxLeakLevel");
            // stored in the state
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->clearScene(0);
            proc->clearScene(1);
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            const bool kept =
                proc->isSceneStored(0) && proc->isSceneStored(1) && !proc->isSceneStored(2);
            std::cout << "scenes: at 25 % vol " << v25 << " dB trem " << t25 << ", at 75 % vol "
                      << v75 << " dB trem " << t75 << " leaks " << l75 << ", master unchanged "
                      << juce::exactlyEqual(real("master"), master0) << ", kept in the state "
                      << kept
                      << ((std::abs(v25 - (-7.0f)) < 0.6f && t25 < 0.5f &&
                           std::abs(v75 - (-1.0f)) < 0.6f && t75 > 0.5f && kept)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            shoot(0, "ui_spieltisch.png");
            proc->clearScene(0);
            proc->clearScene(1);
            setReal("sceneMorph", 0.0f);
            setReal("vol1", 0.0f);
            setReal("fxLeakLevel", 0.0f);
            setReal("tremOn", 0.0f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

            // dice: wild changes things, keeps master/output, at least one stop; undo restores
            juce::MemoryBlock before;
            proc->getStateInformation(before);
            std::vector<float> normBefore;
            for (auto *q : proc->getParameters())
                normBefore.push_back(q->getValue());
            const float m0 = real("master");
            proc->dice(true);
            int stops = 0, changed = 0;
            for (int s2 = 0; s2 < rysum::kNumStops; ++s2)
                stops += real(RysumProcessor::stopParamId(s2).toRawUTF8()) > 0.5f;
            juce::MemoryBlock after;
            proc->getStateInformation(after);
            changed = after == before ? 0 : 1;
            proc->undoDice();
            juce::MemoryBlock undone;
            proc->getStateInformation(undone);
            float worstUndo = 0.0f;
            {
                int i = 0;
                for (auto *q : proc->getParameters())
                    worstUndo =
                        std::max(worstUndo, std::abs(q->getValue() - normBefore[(size_t)i++]));
            }
            const bool restored = worstUndo < 1.0e-4f;
            std::cout << "dice: changed " << changed << ", stops pulled " << stops
                      << ", master kept " << juce::exactlyEqual(real("master"), m0)
                      << ", undo restores (worst " << worstUndo << ")"
                      << ((changed && stops > 0 && juce::exactlyEqual(real("master"), m0) &&
                           restored)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            if (!restored)
            {
                auto xa =
                    proc->getXmlFromBinary(before.getData(), (int)before.getSize())->toString();
                auto xb =
                    proc->getXmlFromBinary(undone.getData(), (int)undone.getSize())->toString();
                juce::StringArray la, lb;
                la.addLines(xa);
                lb.addLines(xb);
                for (int i = 0, shown = 0; i < juce::jmax(la.size(), lb.size()) && shown < 6; ++i)
                    if (la[i] != lb[i])
                    {
                        std::cout << "- " << la[i].substring(0, 200) << "\n+ "
                                  << lb[i].substring(0, 200) << std::endl;
                        ++shown;
                    }
            }
            shoot(6, "ui_orgel.png");
        }

        // full screen: the interface moves into a window covering the screen, and back
        if (std::getenv("RYSUM_SKIP_FULLSCREEN") == nullptr)
        {
            auto *c = dynamic_cast<RysumContent *>(content);
            c->showView(0);
            editor->toggleFullscreen();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto *top = c->getTopLevelComponent();
            const auto screen =
                juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->totalArea;
            const bool moved = editor->isFullscreen() && top != editor.get() &&
                               top->getBounds() == screen && c->isShowing();
            if (moved)
            {
                auto img = top->createComponentSnapshot(top->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_fullscreen.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img, os);
            }
            // F11 / Esc are handled by the interface itself
            c->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            const bool back = !editor->isFullscreen() && c->getParentComponent() == editor.get();
            std::cout << "full screen: " << screen.toString() << ", moved " << moved
                      << ", back with Esc " << back << ((moved && back) ? "  ok" : "  FAIL")
                      << std::endl;
        }

        // state round trip
        juce::MemoryBlock mbk;
        proc->getStateInformation(mbk);
        proc->setStateInformation(mbk.getData(), (int)mbk.getSize());
        juce::MemoryBlock mbk2;
        proc->getStateInformation(mbk2);
        std::cout << "state bytes: " << mbk.getSize()
                  << (mbk == mbk2 ? "  round trip identical" : "  ROUND TRIP DIFFERS") << std::endl;
        // pluginval-style: wiggle each parameter, restore, compare
        {
            juce::Random rnd(1);
            for (auto *prm : proc->getParameters())
            {
                prm->setValue(rnd.nextFloat());
                proc->setStateInformation(mbk.getData(), (int)mbk.getSize());
                juce::MemoryBlock m3;
                proc->getStateInformation(m3);
                if (!(m3 == mbk))
                {
                    std::cout << "state differs after wiggling " << prm->getName(64) << std::endl;
                    mbk2 = m3;
                    break;
                }
            }
        }
        if (!(mbk == mbk2))
        {
            auto a = proc->getXmlFromBinary(mbk.getData(), (int)mbk.getSize())->toString();
            auto b = proc->getXmlFromBinary(mbk2.getData(), (int)mbk2.getSize())->toString();
            juce::StringArray la, lb;
            la.addLines(a);
            lb.addLines(b);
            for (int i = 0; i < juce::jmax(la.size(), lb.size()); ++i)
                if (la[i] != lb[i])
                    std::cout << "- " << la[i].substring(0, 300) << "\n+ "
                              << lb[i].substring(0, 300) << std::endl;
        }

        editor.reset();
        proc.reset();
        std::cout << "done" << std::endl;
        quit();
    }

    void shutdown() override
    {
        editor.reset();
        proc.reset();
        std::cout << "shutdown: done" << std::endl;
    }

private:
    void testScalaTuning()
    {
        bool passed = true;
        auto check = [&](bool ok, const char *message)
        {
            std::cout << (ok ? "  ok    " : "  FAIL  ") << message << std::endl;
            passed = passed && ok;
        };
        auto set = [&](const char *id, float value)
        {
            auto *parameter = proc->apvts.getParameter(id);
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        check(proc->getLibrary() != nullptr, "Scala test loaded the sample library");
        if (proc->getLibrary() == nullptr)
        {
            setApplicationReturnValue(1);
            return;
        }
        proc->setRateAndBufferSizeDetails(48000.0, 512);
        proc->prepareToPlay(48000.0, 512);
        set("output", 0.0f);
        set("dsfOn", 1.0f);
        set("dsfMidi", 1.0f);
        set("dsfOffset", 0.0f);
        set("dsfFootage", 2.0f);
        set("dsfMixtur", 1.0f);
        set("temperament", (float)RysumProcessor::kScalaTuning);
        set("pitch", 100.0f);
        {
            juce::AudioBuffer<float> fallbackAudio(36, 512);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            fallbackAudio.clear();
            proc->processBlock(fallbackAudio, midi);
            const auto params = proc->readParams();
            bool correct = params.notes != nullptr && params.pitchCents == 0.0 &&
                           params.correctSamplePitch;
            if (params.notes != nullptr)
                for (int n = 0; n < 128; ++n)
                    correct = correct && params.notes->mapped[(size_t)n] != 0 &&
                              std::abs(params.notes->hz[(size_t)n] -
                                       440.0 * std::exp2((n - 69) / 12.0)) < 0.001;
            check(correct, "empty Scala selection uses calibrated 12edo at A440 for every MIDI note");
            check(proc->getScalaReferenceText().contains("C4 (MIDI 60)") &&
                      proc->getTuningStatus().contains("12edo, A4 = 440 Hz") &&
                      std::abs(proc->getReferenceToneFrequency() - rysum::kScalaDefaultReferenceHz) < 0.001,
                  "empty Scala selection displays C4 and Testton follows its default reference");
        }
        juce::String scale("Reference test\n31\n");
        for (int degree = 1; degree <= 31; ++degree)
            scale << juce::String(1200.0 * degree / 31.0, 8) << "\n";
        const auto scl = outDir.getChildFile("reference.scl");
        const auto kbm = outDir.getChildFile("reference.kbm");
        check(scl.replaceWithText(scale) && proc->loadTuningFile(scl), "Scala scale loaded");
        juce::AudioBuffer<float> audio(36, 512);
        auto play = [&](int note)
        {
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::allSoundOff(1), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 1);
            audio.clear();
            proc->processBlock(audio, midi);
        };
        for (const float pitch : {-200.0f, 0.0f, 100.0f})
        {
            set("pitch", pitch);
            play(60);
            const auto params = proc->readParams();
            check(params.notes != nullptr && params.pitchCents == 0.0 &&
                      std::abs(params.notes->hz[60] - rysum::kScalaDefaultReferenceHz) < 0.001 &&
                      std::abs(proc->getReferenceToneFrequency() - rysum::kScalaDefaultReferenceHz) < 0.001,
                  "Scala degree zero and Testton use C4 at the 12edo A440 reference independently of Stimmton");
        }
        check(proc->getScalaReferenceText().contains("C4 (MIDI 60)") &&
                  proc->getScalaReferenceText().contains("261.625565") &&
                  proc->getTuningStatus().contains("Stufe 0 = C4"),
              "default Scala display identifies its MIDI reference, frequency and degree zero");
        check(kbm.replaceWithText("0\n0\n127\n55\n57\n432.1255\n0\n") && proc->loadTuningFile(kbm),
              "KBM with a distinct middle and reference note loaded");
        play(57);
        auto params = proc->readParams();
        check(params.notes != nullptr && params.pitchCents == 0.0 &&
                  std::abs(params.notes->hz[57] - 432.1255) < 0.001 &&
                  std::abs(proc->getReferenceToneFrequency() - 432.1255) < 0.001,
              "organ target and Testton follow the KBM reference frequency");
        check(proc->getScalaReferenceText().contains("A3 (MIDI 57)") &&
                  proc->getScalaReferenceText().contains("432.125500") &&
                  proc->getTuningStatus().contains("Mittelnote: MIDI 55"),
              "KBM display distinguishes reference note and middle note");
        juce::MemoryBlock saved;
        proc->getStateInformation(saved);
        proc->clearKbm();
        play(60);
        params = proc->readParams();
        check(!proc->hasKbm() && params.notes != nullptr &&
                  std::abs(params.notes->hz[60] - rysum::kScalaDefaultReferenceHz) < 0.001 &&
                  proc->getScalaReferenceText().contains("C4 (MIDI 60)"),
              "clearing KBM restores the default C4 reference and display");
        proc->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        play(57);
        params = proc->readParams();
        check(proc->hasKbm() && params.notes != nullptr &&
                  std::abs(params.notes->hz[57] - 432.1255) < 0.001 &&
                  proc->getScalaReferenceText().contains("A3 (MIDI 57)"),
              "project restoration restores the KBM reference and display");
        check(proc->apvts.getParameter("pitch")->getCurrentValueAsText().startsWith("A4 = "),
              "normal Stimmton uses scientific A4 notation");
        proc->releaseResources();
        setApplicationReturnValue(passed ? 0 : 1);
    }

    /** RYSUM_ONLY_CONCERT: a player and a listener in this process, over 127.0.0.1, in real time,
        with up to 80 ms of uneven arrival at the listener */
    void testConcert()
    {
        auto say = [](const juce::String &what, bool ok)
        { std::cout << what << (ok ? "  ok" : "  FAIL") << std::endl; };
        auto &A = *proc;
        auto B = std::make_unique<RysumProcessor>();
        B->loadSamples(samples);
        for (int i = 0; i < 400 && B->getLibrary() == nullptr; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        if (B->getLibrary() == nullptr)
        {
            say("concert: the listener's samples load", false);
            return;
        }
        constexpr int bs = 256;
        constexpr double sr = 48000.0;
        for (auto *p : {&A, B.get()})
        {
            p->setRateAndBufferSizeDetails(sr, bs);
            p->prepareToPlay(sr, bs);
        }
        const int chans = juce::jmax(2, A.getTotalNumInputChannels(), A.getTotalNumOutputChannels());
        juce::AudioBuffer<float> ba(chans, bs), bb(chans, bs);
        auto setN = [](RysumProcessor &p, const juce::String &id, float n)
        {
            if (auto *q = p.apvts.getParameter(id))
                q->setValueNotifyingHost(n);
        };
        auto getN = [](RysumProcessor &p, const juce::String &id)
        { return p.apvts.getParameter(id)->getValue(); };
        // both play in real time: the player here (with the message thread), the listener on a
        // thread of its own, as an audio device would drive it
        double wallStart = juce::Time::getMillisecondCounterHiRes();
        long long blocks = 0;
        juce::SpinLock rmsLock;
        std::vector<double> rmsB;
        struct ListenerAudio : juce::Thread
        {
            ListenerAudio(RysumProcessor &p, juce::AudioBuffer<float> &b, juce::SpinLock &l, std::vector<double> &r)
                : juce::Thread("listener audio"), proc(p), buf(b), lock(l), rms(r)
            {
            }
            void run() override
            {
                const double start = juce::Time::getMillisecondCounterHiRes();
                for (long long k = 0; !threadShouldExit(); ++k)
                {
                    juce::MidiBuffer none;
                    buf.clear();
                    proc.processBlock(buf, none);
                    {
                        const juce::SpinLock::ScopedLockType sl(lock);
                        rms.push_back(buf.getRMSLevel(0, 0, buf.getNumSamples()));
                    }
                    const double due = start + (double)(k + 1) * buf.getNumSamples() / 48000.0 * 1000.0;
                    while (juce::Time::getMillisecondCounterHiRes() < due && !threadShouldExit())
                        juce::Thread::sleep(1);
                }
            }
            RysumProcessor &proc;
            juce::AudioBuffer<float> &buf;
            juce::SpinLock &lock;
            std::vector<double> &rms;
        } listenerAudio(*B, bb, rmsLock, rmsB);
        listenerAudio.startThread(juce::Thread::Priority::highest);
        std::function<void(long long)> feedA; // (the player's audio input, block by block)
        auto run = [&](double seconds, const std::function<void(long long, juce::MidiBuffer &)> &playA = {})
        {
            const long long n = (long long)std::ceil(seconds * sr / bs);
            for (long long k = 0; k < n; ++k, ++blocks)
            {
                juce::MidiBuffer ma;
                if (playA)
                    playA(k, ma);
                ba.clear();
                if (feedA)
                    feedA(k);
                A.processBlock(ba, ma);
                const double due = wallStart + (double)(blocks + 1) * bs / sr * 1000.0;
                while (juce::Time::getMillisecondCounterHiRes() < due)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            }
        };
        auto rmsLast = [&](double seconds)
        {
            const juce::SpinLock::ScopedLockType sl(rmsLock);
            const size_t n = juce::jmin(rmsB.size(), (size_t)(seconds * sr / bs));
            double e = 0.0;
            for (size_t i = rmsB.size() - n; i < rmsB.size(); ++i)
                e = juce::jmax(e, (double)rmsB[i]);
            return e;
        };
        // the listener's own setup, which must stay theirs
        const float outBefore = getN(*B, "output");
        setN(*B, "output", B->apvts.getParameter("output")->convertTo0to1(1.0f));
        const float outOwn = getN(*B, "output");
        B->walkPose[0] = 3.25f;
        const auto folderBefore = B->getSamplesFolder();
        const float airOwn = getN(*B, "air");
        // the player's piece
        for (int s = 0; s < rysum::kNumStops; ++s)
            setN(A, RysumProcessor::stopParamId(s), s == 0 ? 1.0f : 0.0f);
        setN(A, "air", 0.3f);
        {
            auto &S = A.getAuto();
            S = rysum::AutoSet();
            S.clock = 1;
            rysum::AutoLane l;
            l.paramId = "doppler";
            l.pts = {{0.0, 0.0f}, {16.0, 1.0f}};
            S.lanes = {l};
            A.autoPublish();
        }
        setN(A, RysumProcessor::motionParamId(0),
             A.apvts.getParameter(RysumProcessor::motionParamId(0))->convertTo0to1((float)rysum::Motion::Rotation));
        setN(A, RysumProcessor::rateParamId(0), A.apvts.getParameter(RysumProcessor::rateParamId(0))->convertTo0to1(0.5f));

        const bool started = A.concert.startPerformer(8791, true);
        B->concert.setDelayMs(200.0);
        B->concert.testJitterMs = 80.0;
        B->concert.startListener("127.0.0.1", 8791);
        run(1.5);
        say("concert: the listener joins (player sees " + juce::String(A.concert.listenerCount()) + ")",
            started && B->concert.joined() && A.concert.listenerCount() == 1);
        {
            // the panel, as the player sees it
            juce::SharedResourcePointer<rysum::ui::Assets> assets;
            juce::SharedResourcePointer<rysum::ui::LookAndFeel> lnf;
            auto panel = rysum::ui::makeConcertPanel(A);
            panel->setLookAndFeel(&*lnf);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto img = panel->createComponentSnapshot(panel->getLocalBounds(), true, 2.0f);
            auto f = outDir.getChildFile("ui_konzert.png");
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat().writeImageToStream(img, os);
            panel->setLookAndFeel(nullptr);
            wallStart = juce::Time::getMillisecondCounterHiRes() - (double)blocks * bs / sr * 1000.0;
        }
        // the same sound: every parameter that is not the listener's own
        {
            setN(A, "air", 0.71f);
            run(0.8);
            int differ = 0;
            juce::String first;
            for (int i = 0; i < A.getNumParams(); ++i)
            {
                const auto id = A.getParamAt(i)->getParameterID();
                if (id == "output" || id == "master" || id.startsWith("bin") || id.startsWith("spk") ||
                    id.startsWith("dsf") || id == "audioThreads" || id == "midiCC" || id == "inpOn" ||
                    id == "trAngle" || id == "trAmount" || id == "trBoost" || id == "trSideEq")
                    continue;
                if (std::abs(A.getParamAt(i)->getValue() - B->getParamAt(i)->getValue()) > 1e-4f)
                {
                    if (differ++ == 0)
                        first = id + " " + juce::String(A.getParamAt(i)->getValue()) + " / " +
                                juce::String(B->getParamAt(i)->getValue());
                }
            }
            say("concert: the listener's controls are the player's (" + juce::String(differ) + " differ" +
                    (differ > 0 ? ", e.g. " + first : juce::String()) + "), the Verlauf came with it (" +
                    juce::String((int)B->getAuto().lanes.size()) + " lane)",
                differ == 0 && B->getAuto().lanes.size() == 1);
            say("concert: the listener keeps its own output, place and samples",
                std::abs(getN(*B, "output") - outOwn) < 1e-6f && B->walkPose[0].load() == 3.25f &&
                    B->getSamplesFolder() == folderBefore);
            // saving while listening (a host, the standalone on quit) keeps the listener's own
            juce::MemoryBlock saved;
            B->getStateInformation(saved);
            float airSaved = -1.0f;
            if (auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), (int)saved.getSize()))
                for (auto *e : xml->getChildWithTagNameIterator("PARAM"))
                    if (e->getStringAttribute("id") == "air")
                        airSaved = (float)e->getDoubleAttribute("value");
            const float airOwnPlain = B->apvts.getParameter("air")->convertFrom0to1(airOwn);
            say("concert: saving while listening keeps the listener's own setup (air " + juce::String(airSaved, 3) +
                    ", own " + juce::String(airOwnPlain, 3) + ")",
                std::abs(airSaved - airOwnPlain) < 1e-3f);
            // what makes sound of its own on the player stays off at the listener
            setN(A, "lvSelfPlay", 0.5f);
            run(0.6);
            say("concert: Selbstspiel stays off at the listener (" + juce::String(getN(*B, "lvSelfPlay"), 2) + ")",
                getN(*B, "lvSelfPlay") == 0.0f);
            setN(A, "lvSelfPlay", 0.0f);
        }
        // a built-in room on the player, the listener walks in it; a held note goes on sounding
        // while the player changes things that travel as the whole state
        {
            juce::ValueTree s3("SAAL3D");
            s3.setProperty("room", juce::String(rysum::builtInRooms()[0].file), nullptr);
            A.applyPatchRoom(s3);
            run(1.5);
            const bool roomCame = B->walkModelFile.getFileName() == A.walkModelFile.getFileName() &&
                                  A.walkModelFile.existsAsFile();
            B->walkPose[0] = 2.5f; // (the listener walks)
            const int seq0 = A.concert.stateSendsForTest();
            run(0.6, [&](long long k, juce::MidiBuffer &m)
                { if (k == 0) m.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8)100), 0); });
            A.setPipePosition(0, 5, {30.0f, 10.0f, 6.0f});
            run(1.2);
            const auto afterState = rmsLast(0.4);
            // dragging a pipe for a second: the state goes out at most twice a second
            run(1.0, [&](long long k, juce::MidiBuffer &)
                { A.setPipePosition(0, 6, {(float)k * 0.5f, 12.0f, 6.0f}); });
            run(0.8);
            const auto afterDrag = rmsLast(0.4);
            const int sends = A.concert.stateSendsForTest() - seq0;
            run(0.2, [&](long long k, juce::MidiBuffer &m)
                { if (k == 0) m.addEvent(juce::MidiMessage::noteOff(1, 62), 0); });
            say("concert: the player's built-in room comes (" + juce::String(roomCame ? "yes" : "no") +
                    "), the listener stays where it walked (" + juce::String(B->walkPose[0].load(), 2) + ")",
                roomCame && B->walkPose[0].load() == 2.5f);
            say("concert: a held note sounds on through state changes (rms " + juce::String(afterState, 4) + ", " +
                    juce::String(afterDrag, 4) + "), " + juce::String(sends) + " states sent",
                afterState > 1e-3 && afterDrag > 1e-3 && sends >= 2 && sends <= 5);
        }
        // two notes 100 ms apart arrive 100 ms apart (despite the uneven network)
        {
            const long long b0 = 3;
            run(0.6,
                [&](long long k, juce::MidiBuffer &m)
                {
                    if (k == b0)
                        m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    if (k == b0 + 18)
                        m.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 192); // +4800 samples
                    if (k == b0 + 60)
                    {
                        m.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                        m.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
                    }
                });
            run(0.3);
            const auto t = B->concert.testNoteTimes();
            const double gap = t.size() >= 2 ? t[t.size() - 1] - t[t.size() - 2] : -1.0;
            say("concert: two notes 100 ms apart are " + juce::String(gap * 1000.0, 2) + " ms apart at the listener",
                std::abs(gap - 0.1) <= (double)bs / sr + 1e-6);
            say("concert: and they sounded (rms " + juce::String(rmsLast(0.6), 4) + ")", rmsLast(0.6) > 1e-3);
        }
        // the clocks: the Verlauf and the motion follow the player's, the delay later
        {
            A.autoPlay(true);
            run(2.5);
            const double lagBeats = A.getAutoNow() - B->getAutoNow(); // expected 0.2 s at 60 BPM
            const double phA = A.engine.motionPhase[0].load(), phB = B->engine.motionPhase[0].load();
            const double lagPhase = phA - phB; // expected 0.5 Hz * 0.2 s = 0.1
            say("concert: the Verlauf clock trails by " + juce::String(lagBeats, 3) + " beats, the motion by " +
                    juce::String(lagPhase, 3) + " cycles (expected 0.2 / 0.1)",
                std::abs(lagBeats - 0.2) < 0.03 && std::abs(lagPhase - 0.1) < 0.02);
            A.autoPlay(false);
        }
        // a stop pulled, a note 2 ms later: it sounds at the listener
        {
            setN(A, RysumProcessor::stopParamId(0), 0.0f);
            run(0.5);
            const auto before = rmsLast(0.2);
            setN(A, RysumProcessor::stopParamId(2), 1.0f);
            run(0.5,
                [&](long long k, juce::MidiBuffer &m)
                {
                    if (k == 0)
                        m.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8)100), 96);
                });
            const auto after = rmsLast(0.25);
            say("concert: a stop pulled and a note 2 ms later sound (rms " + juce::String(before, 4) + " -> " +
                    juce::String(after, 4) + ")",
                after > 1e-3 && after > before * 4.0);
            run(0.2, [&](long long k, juce::MidiBuffer &m)
                { if (k == 0) m.addEvent(juce::MidiMessage::noteOff(1, 67), 0); });
        }
        // the pipes as bodies in the Saal: the player throws one and knocks on another - the
        // listener (standing somewhere else) sees them fly the same way, the delay later, and
        // hears the knock
        {
            setN(A, RysumProcessor::motionParamId(2),
                 A.apvts.getParameter(RysumProcessor::motionParamId(2))->convertTo0to1((float)rysum::Motion::Hover));
            B->engine.viewWalkOn = true;
            B->engine.viewX = 3.0f; // (the listener elsewhere: its own physics would push differently)
            run(1.0);
            rysum::Vec3f a0{}, b0{};
            const bool bodies = A.engine.bodyAt(2, 10, a0) && B->engine.bodyAt(2, 10, b0);
            const int knocks0 = B->concert.knocksHeardForTest();
            A.engine.playerAct(rysum::OrganEngine::Act::Throw, 2, 10, {5.0f, 3.0f, 2.0f});
            A.engine.playerAct(rysum::OrganEngine::Act::Strike, 2, 12, {a0.x, a0.y, a0.z}, 1.0f);
            std::vector<rysum::Vec3f> histA, histB;
            run(1.5, [&](long long, juce::MidiBuffer &)
                {
                    rysum::Vec3f a{}, b{};
                    A.engine.bodyAt(2, 10, a);
                    B->engine.bodyAt(2, 10, b);
                    histA.push_back(a);
                    histB.push_back(b);
                });
            // while it flies: the listener's pipe is where the player's was 200 ms before
            const int lag = (int)std::lround(0.2 * sr / bs);
            float worst = 0.0f;
            for (size_t i = (size_t)lag + 4; i < histB.size(); ++i)
            {
                float best = 1.0e9f;
                for (int d = -4; d <= 4; ++d)
                {
                    const auto &a = histA[i - (size_t)lag + (size_t)d < histA.size() ? i - (size_t)lag + (size_t)d : i];
                    const auto &b = histB[i];
                    best = juce::jmin(best, std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                                      (a.z - b.z) * (a.z - b.z)));
                }
                worst = juce::jmax(worst, best);
            }
            run(2.5);
            rysum::Vec3f a1{}, b1{};
            A.engine.bodyAt(2, 10, a1);
            B->engine.bodyAt(2, 10, b1);
            const float moved = std::sqrt((a1.x - a0.x) * (a1.x - a0.x) + (a1.y - a0.y) * (a1.y - a0.y));
            const float apart = std::sqrt((a1.x - b1.x) * (a1.x - b1.x) + (a1.y - b1.y) * (a1.y - b1.y) +
                                          (a1.z - b1.z) * (a1.z - b1.z));
            say("concert: a pipe thrown by the player flies the same at the listener (moved " + juce::String(moved, 2) +
                    " m, at most " + juce::String(worst, 3) + " m off in flight, " + juce::String(apart, 3) +
                    " m apart at rest)",
                bodies && moved > 1.0f && worst < 0.25f && apart < 0.05f);
            say("concert: the player's knock is heard at the listener (" +
                    juce::String(B->concert.knocksHeardForTest() - knocks0) + ")",
                B->concert.knocksHeardForTest() > knocks0);
            setN(A, RysumProcessor::motionParamId(2), 0.0f);
            B->engine.viewWalkOn = false;
            B->engine.viewX = 0.0f;
            run(0.5);
        }
        // the player's audio input: a click with a note at the same moment arrive together, a tone
        // goes on into the listener's input
        {
            setN(A, "inpOn", 1.0f);
            run(0.5);
            const bool switchCame = getN(*B, "inpOn") > 0.5f;
            B->concert.testResetLoud();
            float peakB = 0.0f;
            feedA = [&](long long k)
            {
                if (k == 10)
                    for (int c = 0; c < juce::jmin(2, A.getTotalNumInputChannels()); ++c)
                        ba.setSample(c, 100, 0.9f);
                if (k >= 40)
                    for (int c = 0; c < juce::jmin(2, A.getTotalNumInputChannels()); ++c)
                        for (int i = 0; i < bs; ++i)
                            ba.setSample(c, i, 0.3f * (float)std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * (double)(k * bs + i) / sr));
            };
            run(1.6,
                [&](long long k, juce::MidiBuffer &m)
                {
                    if (k == 10)
                        m.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8)100), 100);
                    if (k == 30)
                        m.addEvent(juce::MidiMessage::noteOff(1, 72), 0);
                    if (k > 200)
                        peakB = juce::jmax(peakB, B->engine.inPeak.load());
                });
            feedA = {};
            const auto notes = B->concert.testNoteTimes();
            const double apart = notes.empty() ? 1.0 : B->concert.testLoudAt() - notes.back();
            say("concert: the player's input comes (switch " + juce::String(switchCame ? "on" : "off") +
                    "), a click and a note played together are " + juce::String(apart * 1000.0, 2) +
                    " ms apart at the listener, a tone reaches its input (peak " + juce::String(peakB, 3) + ")",
                A.getTotalNumInputChannels() > 0 && switchCame && B->concert.testLoudAt() > 0.0 &&
                    std::abs(apart) < 0.002 && peakB > 0.2f && peakB < 0.4f);
            setN(A, "inpOn", 0.0f);
            run(0.5);
        }
        // the others in the Saal: the player sees the listener where it stands, the listener the player
        {
            B->concert.setName("Anna");
            B->walkPose[0] = 2.0f;
            B->walkPose[1] = 3.0f;
            B->walkPose[2] = 45.0f;
            A.walkPose[0] = -1.0f;
            A.walkPose[1] = 4.0f;
            A.walkPose[2] = 180.0f;
            run(0.6);
            const auto fa = A.concert.figures(), fb = B->concert.figures();
            const bool seesListener = fa.size() == 1 && fa[0].name == "Anna" && std::abs(fa[0].x - 2.0f) < 1e-4f &&
                                      std::abs(fa[0].y - 3.0f) < 1e-4f && std::abs(fa[0].yaw - 45.0f) < 1e-4f;
            const bool seesPlayer = fb.size() == 1 && fb[0].id == 0 && std::abs(fb[0].x + 1.0f) < 1e-4f &&
                                    std::abs(fb[0].y - 4.0f) < 1e-4f && std::abs(fb[0].yaw - 180.0f) < 1e-4f;
            say("concert: the player sees the listener in the Saal (" + juce::String((int)fa.size()) + " figure" +
                    (fa.empty() ? juce::String() : ", " + fa[0].name) + "), the listener sees the player (" +
                    juce::String((int)fb.size()) + ", not itself)",
                seesListener && seesPlayer);
            A.walkPose[0] = A.walkPose[1] = A.walkPose[2] = 0.0f;
        }
        // the listeners' hands: only as far as the player allows, and they act at the player
        {
            using Act = rysum::OrganEngine::Act;
            setN(A, RysumProcessor::motionParamId(2),
                 A.apvts.getParameter(RysumProcessor::motionParamId(2))->convertTo0to1((float)rysum::Motion::Hover));
            A.concert.setGuestHands(0);
            run(1.0);
            auto at = [&](RysumProcessor &p) { rysum::Vec3f v{}; p.engine.bodyAt(2, 5, v); return v; };
            auto dist = [](rysum::Vec3f a, rysum::Vec3f b)
            { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
            const auto p0 = at(A);
            const auto knocks0 = A.engine.impactCount.load();
            B->engine.playerAct(Act::Throw, 2, 5, {0.0f, 6.0f, 2.0f});
            B->engine.playerAct(Act::Strike, 2, 6, p0, 1.0f);
            run(1.0);
            const bool refused = dist(at(A), p0) < 0.05f && A.engine.impactCount.load() == knocks0;
            A.concert.setGuestHands(1);
            run(0.3);
            const int levelSeen1 = B->concert.allowedHands();
            B->engine.playerAct(Act::Throw, 2, 5, {0.0f, 6.0f, 2.0f});
            B->engine.playerAct(Act::Strike, 2, 6, p0, 1.0f);
            run(1.0);
            const bool knockOnly = dist(at(A), p0) < 0.05f && A.engine.impactCount.load() > knocks0;
            A.concert.setGuestHands(2);
            run(0.3);
            const int levelSeen2 = B->concert.allowedHands();
            // the listener takes the pipe: it follows the listener's hand at the player
            const rysum::Vec3f hand{p0.x + 1.5f, p0.y, p0.z + 0.5f};
            B->engine.grabX = hand.x;
            B->engine.grabY = hand.y;
            B->engine.grabZ = hand.z;
            B->engine.grabKey = 5;
            B->engine.grabStop = 2;
            run(0.8);
            const float carried = dist(at(A), hand), ownHand = dist(at(*B), hand);
            // and throws it
            B->engine.grabStop = -1;
            B->engine.grabKey = -1;
            B->engine.playerAct(Act::Throw, 2, 5, {0.0f, 6.0f, 2.0f});
            run(4.5);
            const float thrown = dist(at(A), hand), apart = dist(at(A), at(*B));
            say("concert: a listener's hands - nothing without leave (" + juce::String(refused ? "yes" : "no") +
                    "), knocking only with the first (" + juce::String(knockOnly ? "yes" : "no") + ", leave seen " +
                    juce::String(levelSeen1) + "/" + juce::String(levelSeen2) + "), the carried pipe follows its hand at the player (" +
                    juce::String(carried, 2) + " m off, " + juce::String(ownHand, 2) + " m in its own view), thrown " +
                    juce::String(thrown, 1) + " m, both see it at the same place (" + juce::String(apart, 2) + " m)",
                refused && knockOnly && levelSeen1 == 1 && levelSeen2 == 2 && carried < 0.3f && ownHand < 0.05f &&
                    thrown > 1.0f && apart < 0.1f);
            A.concert.setGuestHands(0);
            setN(A, RysumProcessor::motionParamId(2), 0.0f);
            run(0.5);
        }
        // nothing changes: the state is not sent again and again
        {
            run(0.5);
            const int seq0 = A.concert.stateSendsForTest();
            run(3.0);
            const int sends = A.concert.stateSendsForTest() - seq0;
            say("concert: an unchanged piece sends no state (" + juce::String(sends) + " in 3 s)", sends == 0);
        }
        // someone joins late while a chord is held: they hear it
        {
            B->concert.stop();
            run(1.2, [&](long long k, juce::MidiBuffer &m)
                {
                    if (k == 0)
                        for (int n : {60, 64, 67})
                            m.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                });
            const auto quiet = rmsLast(0.3);
            B->concert.startListener("127.0.0.1", 8791);
            run(1.5);
            const auto heard = rmsLast(0.5);
            say("concert: a late listener hears the held chord (rms " + juce::String(quiet, 4) + " -> " +
                    juce::String(heard, 4) + ")",
                B->concert.joined() && heard > 1e-3 && heard > quiet * 4.0);
        }
        // the player leaves: the listener falls silent and gets its own setup back
        {
            A.concert.stop();
            run(3.0);
            const auto silent = rmsLast(0.5);
            say("concert: the player gone, the listener falls silent (rms " + juce::String(silent, 5) + ")",
                silent < 2e-4);
            B->concert.stop();
            run(0.2);
            say("concert: the listener's own setup is back (air " + juce::String(getN(*B, "air"), 3) + ")",
                std::abs(getN(*B, "air") - airOwn) < 1e-4f && std::abs(getN(*B, "output") - outOwn) < 1e-6f);
        }
        setN(*B, "output", outBefore);
        listenerAudio.stopThread(3000);
        B->releaseResources();
        B.reset();
    }

    /** a player for the listener app (RYSUM_CONCERT_SERVE=port): waits for a listener on this
        computer, then plays a chord and throws a pipe for a few seconds, in real time */
    void serveConcert()
    {
        auto &A = *proc;
        const int port = juce::String(std::getenv("RYSUM_CONCERT_SERVE")).getIntValue();
        constexpr int bs = 256;
        constexpr double sr = 48000.0;
        A.setRateAndBufferSizeDetails(sr, bs);
        A.prepareToPlay(sr, bs);
        juce::AudioBuffer<float> buf(juce::jmax(2, A.getTotalNumOutputChannels()), bs);
        auto setN = [&](const juce::String &id, float plain)
        {
            if (auto *q = A.apvts.getParameter(id))
                q->setValueNotifyingHost(q->convertTo0to1(plain));
        };
        for (int s = 0; s < rysum::kNumStops; ++s)
            setN(RysumProcessor::stopParamId(s), s == 0 ? 1.0f : 0.0f);
        setN(RysumProcessor::motionParamId(0), (float)rysum::Motion::Hover);
        // (the player stands five metres ahead of the listening place, facing it)
        A.walkPose[0] = 0.0f;
        A.walkPose[1] = 5.0f;
        A.walkPose[2] = 180.0f;
        const bool ok = A.concert.startPerformer(port, true);
        A.concert.setGuestHands(2); // (the listener app shows its hands)
        std::cout << "serve: playing on port " << port << (ok ? "" : " FAILED") << std::endl;
        double start = juce::Time::getMillisecondCounterHiRes();
        long long k = 0;
        int joinedAt = -1;
        for (; k < (long long)(40.0 * sr / bs); ++k)
        {
            juce::MidiBuffer m;
            if (joinedAt < 0 && A.concert.listenerCount() > 0)
                joinedAt = (int)k;
            if (joinedAt >= 0)
            {
                const long long j = k - joinedAt;
                if (j == (long long)(1.0 * sr / bs))
                    for (int n : {48, 55, 60, 64})
                        m.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                if (j == (long long)(2.0 * sr / bs))
                {
                    A.engine.playerAct(rysum::OrganEngine::Act::Throw, 0, 12, {4.0f, 6.0f, 3.0f});
                    rysum::Vec3f p{};
                    A.engine.bodyAt(0, 14, p);
                    A.engine.playerAct(rysum::OrganEngine::Act::Strike, 0, 14, {p.x, p.y, p.z}, 1.2f);
                }
                if (j == (long long)(7.0 * sr / bs))
                    for (int n : {48, 55, 60, 64})
                        m.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                if (j > (long long)(16.0 * sr / bs))
                    break;
            }
            buf.clear();
            A.processBlock(buf, m);
            const double due = start + (double)(k + 1) * bs / sr * 1000.0;
            while (juce::Time::getMillisecondCounterHiRes() < due)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        }
        rysum::Vec3f p{};
        A.engine.bodyAt(0, 12, p);
        std::cout << "serve: listener came " << (joinedAt >= 0 ? "yes" : "no") << ", thrown pipe at " << p.x << " "
                  << p.y << " " << p.z << std::endl;
        A.concert.stop();
    }

    void testMidiTuning()
    {
        bool passed = true;
        auto check = [&](bool ok, const char *message)
        {
            std::cout << (ok ? "  ok    " : "  FAIL  ") << message << std::endl;
            passed = passed && ok;
        };
        auto set = [&](const char *id, float value)
        {
            auto *parameter = proc->apvts.getParameter(id);
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        check(proc->getLibrary() != nullptr, "MIDI tuning test loaded the sample library");
        check(!proc->mtsHasMaster(), "MIDI tuning fallback test has no external master");
        if (proc->mtsHasMaster() || proc->getLibrary() == nullptr)
        {
            setApplicationReturnValue(1);
            return;
        }
        proc->setRateAndBufferSizeDetails(48000.0, 512);
        proc->prepareToPlay(48000.0, 512);
        set("temperament", static_cast<float>(RysumProcessor::kMtsTuning));
        set("output", 0.0f);
        set("dsfOn", 1.0f);
        set("dsfMidi", 1.0f);
        set("dsfOffset", 0.0f);
        set("dsfFootage", 2.0f);
        set("dsfMixtur", 1.0f);
        juce::AudioBuffer<float> audio(36, 512);
        for (const float pitch : {-200.0f, 0.0f, 100.0f})
        {
            set("pitch", pitch);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
            audio.clear();
            proc->processBlock(audio, midi);
            const auto params = proc->readParams();
            check(params.notes != nullptr && params.correctSamplePitch && params.pitchCents == 0.0,
                  "MIDI fallback uses calibrated absolute frequencies independently of Stimmton");
            bool equalTemperament = params.notes != nullptr;
            if (params.notes != nullptr)
                for (int channel = 0; channel < 16; ++channel)
                    for (const int note : {36, 60, 69, 81})
                    {
                        const auto index = static_cast<std::size_t>(channel * 128 + note);
                        const double expected = 440.0 * std::exp2((note - 69.0) / 12.0);
                        equalTemperament = equalTemperament && params.notes->mapped[index] != 0 &&
                                           std::abs(params.notes->hz[index] - expected) < 0.001;
                    }
            check(equalTemperament, "MIDI fallback is 12edo at A4 = 440 Hz on all channels");
            check(std::abs(proc->getReferenceToneFrequency() - 440.0) < 0.001,
                  "Testton follows the 440 Hz fallback");
        }
        const auto fallback = proc->getTuningStatus();
        check(fallback.contains("12edo") && fallback.contains("440 Hz") &&
                  !fallback.contains("Originalstimmung"),
              "fallback status identifies 12edo and 440 Hz");
        const juce::uint8 sysex[]{0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00,
                                 0x01, 69, 70, 0x00, 0x00, 0xF7};
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage(sysex, static_cast<int>(sizeof(sysex))), 0);
        audio.clear();
        proc->processBlock(audio, midi);
        const auto params = proc->readParams();
        const double expected = 440.0 * std::exp2(1.0 / 12.0);
        check(params.notes != nullptr && std::abs(params.notes->hz[69] - expected) < 0.001 &&
                  params.pitchCents == 0.0,
              "received MTS SysEx replaces the fallback with its absolute pitch");
        check(std::abs(proc->getReferenceToneFrequency() - expected) < 0.001,
              "held Testton follows the received MIDI tuning");
        check(proc->getTuningStatus() == "MIDI Tuning (MTS-ESP) empfangen",
              "received tuning status has no equal-temperament scale name");
        proc->releaseResources();
        setApplicationReturnValue(passed ? 0 : 1);
    }

    juce::File samples, outDir;
    std::unique_ptr<RysumProcessor> proc;
    std::unique_ptr<RysumEditor> editor;
    int waited = 0, stage = 0;
    juce::File irFile;
};

START_JUCE_APPLICATION(SnapshotApp)
