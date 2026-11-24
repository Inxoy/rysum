#include "Pano360.h"
#include "Lang.h"
#include "SpatialMeta.h"
#include <cmath>
#include <cstring>
#include <type_traits>

namespace rysum
{
static_assert(std::is_trivially_copyable_v<Raster::Call> && std::is_trivially_copyable_v<Material> &&
                  std::is_trivially_copyable_v<Raster::Lamp> && std::is_trivially_copyable_v<PanoJob::Glow>,
              "scene.bin holds these as they are in memory");

namespace
{
constexpr int kMaxQueuedScenes = 240;              // (waiting to be written: beyond either limit frames are repeated instead)
constexpr int64_t kMaxQueuedBytes = 256 << 20;    // (a large model is held whole in every waiting scene)

template <typename T> void writeVec(juce::OutputStream &o, const std::vector<T> &v)
{
    o.writeInt((int)v.size());
    if (!v.empty())
        o.write(v.data(), v.size() * sizeof(T));
}
template <typename T> bool readVec(juce::InputStream &in, std::vector<T> &v)
{
    const int n = in.readInt();
    if (n < 0 || n > (1 << 24))
        return false;
    v.resize((size_t)n);
    return n == 0 || in.read(v.data(), (int)(v.size() * sizeof(T))) == (int)(v.size() * sizeof(T));
}
template <typename T> void writePod(juce::OutputStream &o, const T &x) { o.write(&x, sizeof(T)); }
template <typename T> bool readPod(juce::InputStream &in, T &x) { return in.read(&x, sizeof(T)) == (int)sizeof(T); }
} // namespace

// ---------------------------------------------------------------------------
// while recording: the audio to ambix.wav, the scenes to scene.bin
// ---------------------------------------------------------------------------
class Pano360Recorder::Writer : public juce::Thread
{
public:
    explicit Writer(Pano360Recorder &r) : juce::Thread("Rysum 360 recording"), rec(r) {}
    ~Writer() override { stopThread(-1); }

    std::unique_ptr<juce::AudioFormatWriter> wav;
    std::unique_ptr<juce::FileOutputStream> sceneOut;
    mutable std::mutex mx;
    std::deque<PanoJob> jobs;
    std::atomic<int64_t> bytes{0};
    juce::WaitableEvent wake;

    void push(PanoJob &&j)
    {
        {
            std::lock_guard<std::mutex> l(mx);
            bytes += (int64_t)(j.calls.size() * sizeof(Raster::Call));
            jobs.push_back(std::move(j));
        }
        wake.signal();
    }
    int queued() const
    {
        std::lock_guard<std::mutex> l(mx);
        return (int)jobs.size();
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            const bool a = drainAudio(), s = writeOne();
            if (!a && !s)
                wake.wait(15);
        }
        while (drainAudio() || writeOne())
        {
        }
        wav.reset();
        if (sceneOut != nullptr)
            sceneOut->flush();
        sceneOut.reset();
    }

private:
    Pano360Recorder &rec;
    std::array<std::vector<float>, 4> tmp;
    std::vector<Raster::Call> lastStatic;
    bool haveStatic = false;

    bool drainAudio()
    {
        const int ready = rec.fifo.getNumReady();
        if (ready <= 0 || wav == nullptr)
            return false;
        int s1, n1, s2, n2;
        rec.fifo.prepareToRead(ready, s1, n1, s2, n2);
        for (auto &t : tmp)
            t.resize((size_t)ready);
        for (int c = 0; c < 4; ++c)
        {
            std::copy_n(rec.ring[(size_t)c].data() + s1, n1, tmp[(size_t)c].data());
            if (n2 > 0)
                std::copy_n(rec.ring[(size_t)c].data() + s2, n2, tmp[(size_t)c].data() + n1);
        }
        rec.fifo.finishedRead(n1 + n2);
        const float *ch[4]{tmp[0].data(), tmp[1].data(), tmp[2].data(), tmp[3].data()};
        wav->writeFromFloatArrays(ch, 4, n1 + n2);
        return true;
    }

    bool writeOne()
    {
        PanoJob j;
        {
            std::lock_guard<std::mutex> l(mx);
            if (jobs.empty())
                return false;
            j = std::move(jobs.front());
            jobs.pop_front();
            bytes -= (int64_t)(j.calls.size() * sizeof(Raster::Call));
        }
        if (sceneOut == nullptr)
            return true;
        auto &o = *sceneOut;
        o.writeInt64(j.upTo);
        o.writeInt((int)j.frameNo);
        writePod(o, j.eye);
        o.writeFloat(j.fogTone);
        o.writeFloat(j.fogDistance);
        o.writeFloat(j.grain);
        o.writeFloat(j.ambient);
        writePod(o, j.mats);
        writeVec(o, j.lamps);
        writeVec(o, j.glows);
        // the room / the model: only when it is not the same as before
        const size_t ns = juce::jmin(j.staticCount, j.calls.size());
        const bool same = haveStatic && ns == lastStatic.size() &&
                          (ns == 0 || std::memcmp(lastStatic.data(), j.calls.data(), ns * sizeof(Raster::Call)) == 0);
        o.writeByte(same ? 0 : 1);
        if (!same)
        {
            lastStatic.assign(j.calls.begin(), j.calls.begin() + (std::ptrdiff_t)ns);
            haveStatic = true;
            writeVec(o, lastStatic);
        }
        const std::vector<Raster::Call> rest(j.calls.begin() + (std::ptrdiff_t)ns, j.calls.end());
        writeVec(o, rest);
        return true;
    }
};

// ---------------------------------------------------------------------------
// after recording: every frame drawn from scene.bin
// ---------------------------------------------------------------------------
class Pano360Recorder::Drawer : public juce::Thread
{
public:
    explicit Drawer(Pano360Recorder &r) : juce::Thread("Rysum 360 drawing"), rec(r) {}
    ~Drawer() override
    {
        stopThread(-1);
        pool.finish();
    }

    void run() override
    {
        bool complete = false;
        {
            juce::FileInputStream in(rec.dir.getChildFile("scene.bin"));
            Raster face;
            // the graphics card, on this thread (its own context: the view may be gone long before)
            PanoGpu gpu;
            {
                juce::String why;
                const bool onCard = gpu.open(why);
                const std::lock_guard<std::mutex> l(rec.infoLock);
                rec.drawnOn = onCard ? gpu.cardName() : juce::String();
                rec.gpuWhy = onCard ? juce::String() : why.upToFirstOccurrenceOf("\n", false, false).trim();
            }
            pool.start(juce::jlimit(1, 6, (int)std::thread::hardware_concurrency() - 2));
            std::vector<Raster::Call> stat;
            int64_t next = 0;
            const int64_t total = rec.total.load();
            bool ok = in.openedOk();
            while (ok && !threadShouldExit() && !in.isExhausted())
            {
                PanoJob j;
                j.upTo = in.readInt64();
                j.frameNo = (uint32_t)in.readInt();
                ok = readPod(in, j.eye);
                j.fogTone = in.readFloat();
                j.fogDistance = in.readFloat();
                j.grain = in.readFloat();
                j.ambient = in.readFloat();
                ok = ok && readPod(in, j.mats) && readVec(in, j.lamps) && readVec(in, j.glows);
                if (!ok)
                    break;
                if (in.readByte() != 0 && !readVec(in, stat))
                    break;
                std::vector<Raster::Call> rest;
                if (!readVec(in, rest))
                    break;
                j.calls.reserve(stat.size() + rest.size());
                j.calls.assign(stat.begin(), stat.end());
                j.calls.insert(j.calls.end(), rest.begin(), rest.end());
                j.staticCount = stat.size();
                const int64_t last = juce::jmin(j.upTo, total - 1);
                if (last < next)
                    continue;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                juce::String gpuError;
                const bool wasOnCard = gpu.isOpen();
                auto img = renderPanorama(j, rec.set.width, face, &gpu, &gpuError);
                if (wasOnCard && !gpu.isOpen())
                {
                    // (the card gave up on the way: the rest from the processor)
                    const std::lock_guard<std::mutex> l(rec.infoLock);
                    rec.gpuWhy = gpuError.upToFirstOccurrenceOf("\n", false, false).trim();
                    rec.drawnOn = {};
                }
                rec.frameMs = (float)(juce::Time::getMillisecondCounterHiRes() - t0);
                encode(std::move(img), next, last);
                next = last + 1;
            }
            gpu.close();
            pool.finish();
            // (the end: the last picture held as long as the sound goes on)
            if (!threadShouldExit() && next < total && next > 0)
            {
                const auto src = frameFile(lastFirst);
                for (int64_t i = next; i < total; ++i)
                {
                    src.copyFileTo(frameFile(i));
                    rec.drawn = i + 1;
                }
                next = total;
            }
            complete = !threadShouldExit() && next > 0;
        }
        if (complete)
            makeVideo();
        rec.finishDrawing(complete);
    }

private:
    Pano360Recorder &rec;
    int64_t lastFirst = 0;

    /** a few threads that write the JPEGs (one 4K picture takes longer to write than the card takes to draw it) */
    struct Pool
    {
        std::vector<std::thread> threads;
        std::deque<std::function<void()>> todo;
        std::mutex mx;
        std::condition_variable more, room;
        bool quit = false;
        int limit = 1;
        void start(int n)
        {
            finish();
            quit = false;
            limit = n;
            for (int i = 0; i < n; ++i)
                threads.emplace_back(
                    [this]
                    {
                        for (;;)
                        {
                            std::function<void()> f;
                            {
                                std::unique_lock<std::mutex> l(mx);
                                more.wait(l, [this] { return quit || !todo.empty(); });
                                if (todo.empty())
                                    return;
                                f = std::move(todo.front());
                                todo.pop_front();
                            }
                            room.notify_all();
                            f();
                        }
                    });
        }
        /** (waits while as many pictures as there are threads are waiting: the memory stays bounded) */
        void push(std::function<void()> f)
        {
            if (threads.empty())
            {
                f();
                return;
            }
            {
                std::unique_lock<std::mutex> l(mx);
                room.wait(l, [this] { return (int)todo.size() < limit; });
                todo.push_back(std::move(f));
            }
            more.notify_one();
        }
        /** every picture written, the threads ended */
        void finish()
        {
            {
                std::lock_guard<std::mutex> l(mx);
                quit = true;
            }
            more.notify_all();
            for (auto &t : threads)
                t.join();
            threads.clear();
        }
    } pool;
    std::atomic<int64_t> written{0};

    juce::File frameFile(int64_t i) const
    {
        return rec.dir.getChildFile("frames").getChildFile("f_" + juce::String(i).paddedLeft('0', 6) + ".jpg");
    }

    /** frames first..last: the picture, written by the pool while the next ones are drawn (the repeats copied
        from it) */
    void encode(juce::Image img, int64_t first, int64_t last)
    {
        lastFirst = first;
        const int quality = rec.set.jpegQuality;
        pool.push(
            [this, img = std::move(img), first, last, quality]
            {
                const auto from = frameFile(first);
                from.deleteFile();
                {
                    juce::FileOutputStream os(from);
                    juce::JPEGImageFormat fmt;
                    fmt.setQuality((float)quality / 100.0f);
                    if (os.openedOk())
                        fmt.writeImageToStream(img, os);
                }
                for (int64_t i = first + 1; i <= last; ++i)
                    from.copyFileTo(frameFile(i)); // (a repeat: the picture before)
                // (frames written, counted: with several writers they finish in any order)
                rec.drawn = written += last - first + 1;
            });
    }

    /** ffmpeg (if it is there) makes the video, which is then marked as 360 degrees with spatial sound */
    void makeVideo()
    {
        const auto ffmpeg = Pano360Recorder::findFfmpeg();
        auto note = [this](const juce::String &t)
        {
            const std::lock_guard<std::mutex> l(rec.infoLock);
            rec.videoNote = t;
        };
        if (ffmpeg == juce::File())
        {
            note({});
            return;
        }
        const auto raw = rec.dir.getChildFile("rysum360_ohne_metadaten.mov");
        const auto video = rec.dir.getChildFile("rysum360.mov");
        const auto progress = rec.dir.getChildFile("ffmpeg_progress.txt");
        raw.deleteFile();
        progress.deleteFile();
        rec.videoStep = 1;
        juce::StringArray args;
        args.add(ffmpeg.getFullPathName());
        args.addArray(Pano360Recorder::ffmpegArgs(rec.set, rec.dir.getChildFile("frames"),
                                                  rec.dir.getChildFile("ambix.wav"), raw));
        args.addArray({"-progress", progress.getFullPathName()});
        juce::ChildProcess p;
        if (!p.start(args, juce::ChildProcess::wantStdErr))
        {
            rec.videoStep = 0;
            note(tr("ffmpeg lie\xc3\x9f sich nicht starten: ") + ffmpeg.getFullPathName());
            return;
        }
        juce::String errors;
        std::thread reader([&] { errors = p.readAllProcessOutput(); }); // (so that its messages never fill the pipe)
        const int64_t frames = juce::jmax<int64_t>(1, rec.total.load());
        while (p.isRunning())
        {
            if (threadShouldExit())
            {
                p.kill();
                break;
            }
            juce::Thread::sleep(200);
            // (how far: the last "frame=" in the progress file)
            const auto t = progress.loadFileAsString();
            const int at = t.lastIndexOf("frame=");
            if (at >= 0)
                rec.videoFrames = juce::jlimit<int64_t>(0, frames, t.substring(at + 6).getLargeIntValue());
        }
        reader.join();
        progress.deleteFile();
        const auto code = p.getExitCode();
        if (threadShouldExit() || code != 0 || !raw.existsAsFile())
        {
            rec.videoStep = 0;
            raw.deleteFile();
            note(threadShouldExit() ? juce::String()
                                    : tr("ffmpeg ist gescheitert: ") +
                                          errors.trim().fromLastOccurrenceOf("\n", false, false).substring(0, 300));
            return;
        }
        // the marks for YouTube
        rec.videoStep = 2;
        juce::String error;
        video.deleteFile();
        if (injectSpatialMetadata(raw, video, error, [this] { return threadShouldExit(); }))
        {
            raw.deleteFile();
            rec.videoStep = 3;
            note({});
        }
        else
        {
            rec.videoStep = 0;
            note(tr("Die 360\xc2\xb0-Metadaten lie\xc3\x9f"
                    "en sich nicht schreiben: ") +
                 error);
        }
    }
};

// ---------------------------------------------------------------------------
Pano360Recorder::Pano360Recorder()
{
    for (auto &r : ring)
        r.assign((size_t)kFifo, 0.0f);
}

Pano360Recorder::~Pano360Recorder()
{
    armed = false;
    joinThreads();
}

void Pano360Recorder::joinThreads()
{
    if (writer != nullptr)
    {
        writer->signalThreadShouldExit();
        writer->wake.signal();
        writer->stopThread(-1);
        writer.reset();
    }
    if (drawer != nullptr)
    {
        drawer->stopThread(-1);
        drawer.reset();
    }
}

bool Pano360Recorder::start(const juce::File &folder, Settings s, double sampleRate, juce::String &error)
{
    if (st.load() != State::idle)
    {
        error = tr("Die letzte 360\xc2\xb0-Aufnahme wird noch gezeichnet");
        return false;
    }
    joinThreads(); // (the last recording's finished drawer)
    if (folder.exists())
    {
        error = tr("Den Ordner gibt es schon: ") + folder.getFullPathName();
        return false;
    }
    if (!folder.getChildFile("frames").createDirectory())
    {
        error = tr("Kann den Ordner nicht anlegen: ") + folder.getFullPathName();
        return false;
    }
    set = s;
    set.width = juce::jlimit(512, 8192, (s.width / 16) * 16);
    set.fps = juce::jlimit(10, 60, s.fps);
    dir = folder;
    sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    auto w = std::make_unique<Writer>(*this);
    {
        std::unique_ptr<juce::OutputStream> stream = folder.getChildFile("ambix.wav").createOutputStream();
        juce::WavAudioFormat wavFormat;
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(4).withBitsPerSample(24);
        if (stream != nullptr)
            w->wav = wavFormat.createWriterFor(stream, options);
        if (w->wav == nullptr)
        {
            error = tr("Kann ambix.wav nicht schreiben");
            return false;
        }
        w->sceneOut = std::make_unique<juce::FileOutputStream>(folder.getChildFile("scene.bin"), 1 << 20);
        if (!w->sceneOut->openedOk())
        {
            error = tr("Kann den Ordner nicht anlegen: ") + folder.getFullPathName();
            return false;
        }
    }
    fifo.reset();
    samples = 0;
    given = 0;
    scenes = 0;
    lost = 0;
    total = 0;
    drawn = 0;
    lastNoScene = false;
    w->startThread(juce::Thread::Priority::low);
    writer = std::move(w);
    st = State::recording;
    armed.store(true, std::memory_order_release);
    return true;
}

void Pano360Recorder::stop()
{
    if (st.load() != State::recording)
        return;
    armed.store(false, std::memory_order_release);
    total = juce::jmax(given.load(), (samples.load() * set.fps) / juce::jmax<int64_t>(1, (int64_t)sr));
    // the rest of the sound and of the scenes to the files, then the drawing
    writer->signalThreadShouldExit();
    writer->wake.signal();
    writer->stopThread(-1);
    writer.reset();
    videoStep = 0;
    videoFrames = 0;
    {
        const std::lock_guard<std::mutex> l(infoLock);
        drawnOn = gpuWhy = videoNote = {};
    }
    st = State::drawing;
    drawer = std::make_unique<Drawer>(*this);
    drawer->startThread(juce::Thread::Priority::low);
}

void Pano360Recorder::abort()
{
    if (st.load() == State::recording)
        stop();
    if (drawer != nullptr)
    {
        drawer->stopThread(-1); // (finishDrawing runs on its way out)
        drawer.reset();
    }
}

void Pano360Recorder::finishDrawing(bool complete)
{
    if (complete)
        dir.getChildFile("scene.bin").deleteFile();
    dir.getChildFile("make_video.bat").replaceWithText(videoScript(set));
    {
        auto sh = videoScript(set);
        sh = sh.replace("@echo off\r\n", "#!/bin/sh\n")
                 .replace("cd /d \"%~dp0\"", "cd \"$(dirname \"$0\")\"")
                 .replace("frames\\f_%%06d.jpg", "frames/f_%06d.jpg")
                 .replace("rem ", "# ")
                 .replace("\r\npause", "")
                 .replace("\r\n", "\n");
        dir.getChildFile("make_video.sh").replaceWithText(sh);
    }
    const bool made = videoStep.load() == 3;
    juce::String problem, card, why;
    {
        const std::lock_guard<std::mutex> l(infoLock);
        problem = videoNote;
        card = drawnOn;
        why = gpuWhy;
    }
    const juce::String nl("\r\n");
    juce::String txt;
    const auto res = juce::String(set.width) + " x " + juce::String(set.width / 2);
    const bool de = german();
    txt << (de ? "Rysum 360-Grad-Aufnahme" : "Rysum 360 degree recording") << nl << nl;
    if (made)
        txt << (de ? "rysum360.mov  das Video: 360 Grad (equirektangular " : "rysum360.mov  the video: 360 degrees (equirectangular ")
            << res << ", " << set.fps << (de ? " Bilder/s) mit Raumklang (Ambisonics 1. Ordnung)." : " frames/s) with spatial sound (first-order Ambisonics).") << nl
            << (de ? "              Fertig fuer YouTube: die Metadaten fuer 360 Grad und den Raumklang sind schon darin"
                   : "              Ready for YouTube: the metadata for 360 degrees and the spatial sound are already in it")
            << nl << (de ? "              (wie sie Googles Spatial Media Metadata Injector schreibt)." : "              (as Google's Spatial Media Metadata Injector writes them).")
            << nl << (de ? "              Einfach hochladen. YouTube braucht danach eine Weile, bis 360 Grad und der Raumklang gehen."
                         : "              Just upload it. Afterwards YouTube takes a while before 360 degrees and the spatial sound work.")
            << nl << nl;
    txt << (de ? "frames/       die Bilder, equirektangular " : "frames/       the pictures, equirectangular ") << res << ", " << set.fps
        << (de ? " Bilder/s (die Mitte = die Front des Saals)" : " frames/s (the centre = the front of the Saal)") << nl
        << (de ? "ambix.wav     das Schallfeld, Ambisonics 1. Ordnung, AmbiX (ACN / SN3D: W Y Z X), fest im Raum"
               : "ambix.wav     the sound field, first-order Ambisonics, AmbiX (ACN / SN3D: W Y Z X), fixed in the room")
        << nl;
    if (made)
        txt << (de ? "              (beide stecken im Video; wer sie nicht mehr braucht, kann sie loeschen)"
                   : "              (both are in the video; delete them if you do not need them any more)")
            << nl;
    txt << nl;
    if (!made && complete)
    {
        if (problem.isNotEmpty())
            txt << (de ? "Das Video liess sich nicht machen: " : "The video could not be made: ") << problem << nl << nl;
        else
            txt << (de ? "ffmpeg wurde nicht gefunden, darum gibt es noch kein Video." : "ffmpeg was not found, so there is no video yet.") << nl << nl;
        txt << (de ? "1. ffmpeg installieren (z. B. winget install ffmpeg) und make_video.bat (Windows) oder make_video.sh"
                   : "1. Install ffmpeg (e.g. winget install ffmpeg) and run make_video.bat (Windows) or make_video.sh:")
            << nl
            << (de ? "   starten: macht rysum360_ohne_metadaten.mov (H.264 + 4 Kanaele PCM, 48 kHz)."
                   : "   it makes rysum360_ohne_metadaten.mov (H.264 + 4 channels PCM, 48 kHz).")
            << nl
            << (de ? "2. In Rysum, 3D-Ansicht, Knopf \"360 Grad Aufnahme\": \"360-Grad-Metadaten in ein Video schreiben ...\","
                   : "2. In Rysum, 3D view, button \"360 Grad Aufnahme\": \"Write 360 degree metadata into a video ...\",")
            << nl
            << (de ? "   die Datei waehlen: daneben entsteht rysum360_ohne_metadaten_360.mov, fertig fuer YouTube."
                   : "   choose the file: next to it comes rysum360_ohne_metadaten_360.mov, ready for YouTube.")
            << nl << (de ? "   (Oder wie bisher mit Googles Spatial Media Metadata Injector.)" : "   (Or, as before, with Google's Spatial Media Metadata Injector.)")
            << nl << nl;
    }
    txt << (de ? "Bild und Ton laufen nach der Uhr des Tons: Bild k zeigt den Augenblick k / " : "Picture and sound follow the clock of the sound: frame k shows the moment k / ")
        << set.fps << (de ? " s nach dem Start." : " s after the start.") << nl << total.load()
        << (de ? " Bilder, " : " frames, ") << scenes.load()
        << (de ? " davon eigens gezeichnet (die anderen wiederholen das vorige: die Ansicht war langsamer als "
               : " of them drawn on their own (the others repeat the one before: the view was slower than ")
        << set.fps << (de ? " Bilder/s)." : " frames/s).") << nl;
    txt << (card.isNotEmpty() ? (de ? "Gezeichnet von der Grafikkarte: " : "Drawn by the graphics card: ") + card
                              : (de ? "Gezeichnet vom Prozessor" : "Drawn by the processor") +
                                    (why.isNotEmpty() ? " (" + why + ")" : juce::String()))
        << nl;
    if (!complete)
        txt << nl
            << (de ? "ABGEBROCHEN: nur die ersten " + juce::String(drawn.load()) +
                         " Bilder sind gezeichnet (scene.bin ist noch da)."
                   : "STOPPED: only the first " + juce::String(drawn.load()) +
                         " frames are drawn (scene.bin is still there).")
            << nl;
    if (lost.load() > 0)
        txt << nl << (de ? "Achtung: " : "Note: ") << juce::String(lost.load())
            << (de ? " Samples gingen verloren (der Schreiber kam nicht nach)."
                   : " samples were lost (the writer fell behind).")
            << nl;
    dir.getChildFile(de ? "LIESMICH.txt" : "README.txt").replaceWithText(txt);
    st = State::idle;
    ++finished;
}

juce::StringArray Pano360Recorder::ffmpegArgs(const Settings &s, const juce::File &frames, const juce::File &wav,
                                              const juce::File &out)
{
    // (QuickTime with 4 channels of 24-bit PCM: ffmpeg writes them as 'lpcm', which the spatial audio box goes with)
    return {"-y", "-hide_banner", "-nostdin", "-loglevel", "error", "-framerate", juce::String(s.fps), "-i",
            frames.getChildFile("f_%06d.jpg").getFullPathName(), "-i", wav.getFullPathName(), "-map", "0:v:0", "-map",
            "1:a:0", "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-g",
            juce::String(juce::jmax(1, s.fps / 2)), "-c:a", "pcm_s24le", "-ar", "48000", "-shortest",
            out.getFullPathName()};
}

juce::File Pano360Recorder::findFfmpeg()
{
    const auto own = juce::SystemStats::getEnvironmentVariable("RYSUM_FFMPEG", {});
    if (own.isNotEmpty())
        return own == "none" ? juce::File() : juce::File(own); // ("none": as if there were none - a test)
#if JUCE_WINDOWS
    const juce::String exe("ffmpeg.exe"), sep(";");
#else
    const juce::String exe("ffmpeg"), sep(":");
#endif
    for (const auto &d : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), sep, "\""))
        if (d.trim().isNotEmpty() && juce::File::isAbsolutePath(d.trim()))
        {
            const auto f = juce::File(d.trim()).getChildFile(exe);
            if (f.existsAsFile())
                return f;
        }
#if JUCE_WINDOWS
    // (a DAW started before ffmpeg was installed has the old PATH: winget's places, and the usual folders)
    const auto local = juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {}));
    if (local.getFullPathName().isNotEmpty())
    {
        const auto link = local.getChildFile("Microsoft/WinGet/Links/ffmpeg.exe");
        if (link.existsAsFile())
            return link;
        for (const auto &pkg : local.getChildFile("Microsoft/WinGet/Packages")
                                   .findChildFiles(juce::File::findDirectories, false, "*FFmpeg*"))
            for (const auto &f : pkg.findChildFiles(juce::File::findFiles, true, "ffmpeg.exe"))
                return f;
    }
    for (const char *p : {"C:/ffmpeg/bin/ffmpeg.exe", "C:/Program Files/ffmpeg/bin/ffmpeg.exe",
                          "C:/ProgramData/chocolatey/bin/ffmpeg.exe"})
        if (juce::File(p).existsAsFile())
            return juce::File(p);
#else
    for (const char *p : {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"})
        if (juce::File(p).existsAsFile())
            return juce::File(p);
#endif
    return {};
}

juce::String Pano360Recorder::videoScript(const Settings &s)
{
    const juce::String nl("\r\n");
    juce::String t;
    t << "@echo off" << nl
      << "rem Rysum 360: the frames and the Ambisonics (AmbiX, W Y Z X) into one video. Needs ffmpeg." << nl
      << "rem Then the 360 metadata: Rysum's 3D view, 360 button, or Google's Spatial Media Metadata Injector." << nl
      << "cd /d \"%~dp0\"" << nl << "ffmpeg -y -framerate " << s.fps
      << " -i frames\\f_%%06d.jpg -i ambix.wav -map 0:v:0 -map 1:a:0 -c:v libx264 -preset medium -crf 18"
      << " -pix_fmt yuv420p -g " << juce::jmax(1, s.fps / 2)
      << " -c:a pcm_s24le -ar 48000 -shortest rysum360_ohne_metadaten.mov" << nl << "pause" << nl;
    return t;
}

int64_t Pano360Recorder::frameNow() const noexcept
{
    return (samples.load() * set.fps) / juce::jmax<int64_t>(1, (int64_t)sr);
}

bool Pano360Recorder::wantsScene() const noexcept
{
    return armed.load() && writer != nullptr && frameNow() >= given.load() && writer->queued() < kMaxQueuedScenes &&
           writer->bytes.load() < kMaxQueuedBytes;
}

void Pano360Recorder::addScene(PanoJob &&job)
{
    if (!armed.load() || writer == nullptr)
        return;
    if (job.upTo < given.load())
        return; // (that moment has its scene already)
    given = job.upTo + 1;
    ++scenes;
    writer->push(std::move(job));
}

void Pano360Recorder::pushAudio(const float *const *ch, int numCh, int n) noexcept
{
    if (!armed.load(std::memory_order_acquire) || n <= 0)
        return;
    lastNoScene.store(numCh < 4, std::memory_order_relaxed);
    if (fifo.getFreeSpace() < n)
    {
        lost += n;
        samples += n;
        return;
    }
    int s1, n1, s2, n2;
    fifo.prepareToWrite(n, s1, n1, s2, n2);
    for (int c = 0; c < 4; ++c)
    {
        float *r = ring[(size_t)c].data();
        if (c < numCh && ch[c] != nullptr)
        {
            std::copy_n(ch[c], n1, r + s1);
            if (n2 > 0)
                std::copy_n(ch[c] + n1, n2, r + s2);
        }
        else
        {
            std::fill_n(r + s1, n1, 0.0f);
            if (n2 > 0)
                std::fill_n(r + s2, n2, 0.0f);
        }
    }
    fifo.finishedWrite(n1 + n2);
    samples += n;
}

// ---------------------------------------------------------------------------
// one frame: six faces, resampled
// ---------------------------------------------------------------------------
namespace
{
struct Face
{
    V3 fwd, right;
};
// (front = +y, right = +x, up = +z; the up / down faces with 'up' towards the back / the front)
constexpr Face kFaces[6]{{{0, 1, 0}, {1, 0, 0}},  {{1, 0, 0}, {0, -1, 0}}, {{0, -1, 0}, {-1, 0, 0}},
                         {{-1, 0, 0}, {0, 1, 0}}, {{0, 0, 1}, {1, 0, 0}},  {{0, 0, -1}, {1, 0, 0}}};
constexpr float kFaceFov = 92.0f; // (a little more than 90: the resampling never reads past an edge)

int faceOf(float x, float y, float z) noexcept
{
    const float ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
    if (az >= ax && az >= ay)
        return z > 0.0f ? 4 : 5;
    if (ax >= ay)
        return x > 0.0f ? 1 : 3;
    return y > 0.0f ? 0 : 2;
}

const juce::Image &glowSprite()
{
    static const juce::Image img = []
    {
        juce::Image s(juce::Image::ARGB, 128, 128, true);
        juce::Graphics gg(s);
        juce::ColourGradient grad(juce::Colour(0xfff6f4f0).withAlpha(0.5f), 64.0f, 64.0f, juce::Colour(0x00000000),
                                  128.0f, 64.0f, true);
        grad.addColour(0.3, juce::Colour(0xffdcdad4).withAlpha(0.2f));
        grad.addColour(0.65, juce::Colour(0xff8f8c86).withAlpha(0.06f));
        gg.setGradientFill(grad);
        gg.fillEllipse(0.0f, 0.0f, 128.0f, 128.0f);
        return s;
    }();
    return img;
}
} // namespace

juce::Image renderPanorama(const PanoJob &job, int W, Raster &face, PanoGpu *gpu, juce::String *gpuError)
{
    W = juce::jmax(64, W);
    const int H = W / 2;
    // a face as sharp as the panorama at its equator: W / 2 pi pixels per radian at the face's middle
    const float tanHalf = std::tan(juce::degreesToRadians(0.5f * kFaceFov));
    const int F =
        juce::jlimit(64, 2048, juce::roundToInt((float)W / juce::MathConstants<float>::twoPi * 2.0f * tanHalf));
    const float focal = 0.5f * (float)F / tanHalf;
    std::array<juce::Image, 6> faces;
    std::vector<bool> seen(job.glows.size(), false);
    face.setSize(F, F);
    face.setPanoFace(true);
    face.setAllAround(false);
    face.setRecording(nullptr);
    face.setMaterials(job.mats);
    // the graphics card: the processor prepares each face's triangles, the card fills and resamples them
    bool onCard = gpu != nullptr && gpu->isOpen();
    juce::String why;
    if (onCard)
        onCard = gpu->beginFrame(F, W, why);
    std::vector<float> verts;
    std::array<float, 32 * 4> matsGpu{};
    juce::Image out;
    for (int f = 0; f < 6; ++f)
    {
        face.setCameraBasis(job.eye, kFaces[f].right, kFaces[f].fwd, kFaceFov);
        face.begin(job.fogTone, job.fogDistance, job.grain, job.frameNo);
        face.setLamps(job.lamps, job.ambient);
        for (const auto &c : job.calls)
            face.triangle(c.a, c.b, c.c, c.slot, c.tone, c.emissive);
        if (onCard)
        {
            face.exportGpu(verts, matsGpu);
            const V3 up = cross(kFaces[f].right, kFaces[f].fwd);
            const PanoGpu::FaceLook look{face.fogToneNow(), face.fogInvNow(), face.grainNow(), job.frameNo,
                                         face.darkNow(), face.ambientNow(), face.focalPerWidth(), &face.lampsInView(),
                                         {kFaces[f].right.z, up.z, kFaces[f].fwd.z}};
            onCard = gpu->drawFace(f, verts, matsGpu, look, why);
        }
        if (!onCard)
        {
            if (gpu != nullptr && gpu->isOpen())
            {
                // (the card gave up: this frame and the ones after it from the processor)
                if (gpuError != nullptr)
                    *gpuError = why;
                gpu->close();
                for (int g = 0; g < f; ++g)
                {
                    face.setCameraBasis(job.eye, kFaces[g].right, kFaces[g].fwd, kFaceFov);
                    face.begin(job.fogTone, job.fogDistance, job.grain, job.frameNo);
                    face.setLamps(job.lamps, job.ambient);
                    for (const auto &c : job.calls)
                        face.triangle(c.a, c.b, c.c, c.slot, c.tone, c.emissive);
                    face.finish();
                    faces[(size_t)g] = face.image().createCopy();
                }
                face.setCameraBasis(job.eye, kFaces[f].right, kFaces[f].fwd, kFaceFov);
                face.begin(job.fogTone, job.fogDistance, job.grain, job.frameNo);
                face.setLamps(job.lamps, job.ambient);
                for (const auto &c : job.calls)
                    face.triangle(c.a, c.b, c.c, c.slot, c.tone, c.emissive);
            }
            face.finish();
            faces[(size_t)f] = face.image().createCopy();
        }
        // the glows whose middle lies in this face: hidden behind a wall or not (as the view decides it)
        for (size_t i = 0; i < job.glows.size(); ++i)
        {
            const V3 d = job.glows[i].at - job.eye;
            if (faceOf(d.x, d.y, d.z) != f)
                continue;
            float sx, sy, dep;
            if (!face.project(job.glows[i].at, sx, sy, dep))
                continue;
            const float zd = onCard ? gpu->depthAt((int)sx, (int)sy) : face.depthAt((int)sx, (int)sy);
            seen[i] = !(zd > 0.0f && zd < dep * 0.95f - 0.4f);
        }
    }

    // the glows of the sounding pipes, over the panorama (wider towards the poles, as it stretches there)
    std::vector<PanoGpu::Glow> glowsOver;
    for (size_t i = 0; i < job.glows.size(); ++i)
    {
        if (!seen[i])
            continue;
        const auto &gw = job.glows[i];
        const V3 dv = gw.at - job.eye;
        const float dist = std::sqrt(dot(dv, dv));
        if (dist < 0.05f)
            continue;
        // (as wide as the view draws it: its radius at an 80 degree view, in radians)
        const float ang = juce::jlimit(0.008f, 0.34f, 0.839f * (0.6f + 0.35f * gw.size) / dist);
        const float lon = std::atan2(dv.x, dv.y), lat = std::asin(juce::jlimit(-1.0f, 1.0f, dv.z / dist));
        const float cx = (lon / juce::MathConstants<float>::twoPi + 0.5f) * (float)W;
        const float cy = (0.5f - lat / juce::MathConstants<float>::pi) * (float)H;
        const float rh = ang * (float)H / juce::MathConstants<float>::pi;
        const float rw = juce::jmin(0.5f * (float)W, rh / juce::jmax(0.15f, std::cos(lat)));
        glowsOver.push_back({cx, cy, rw, rh, juce::jlimit(0.0f, 1.0f, gw.amount)});
    }

    if (onCard)
    {
        std::array<V3, 6> fw, rt;
        for (int f = 0; f < 6; ++f)
        {
            fw[(size_t)f] = kFaces[f].fwd;
            rt[(size_t)f] = kFaces[f].right;
        }
        onCard = gpu->resample(fw, rt, focal, glowsOver, out, why);
        if (!onCard)
        {
            if (gpuError != nullptr)
                *gpuError = why;
            gpu->close();
            return renderPanorama(job, W, face, nullptr, nullptr);
        }
    }

    // every pixel of the panorama: its ray, the face it meets, bilinear there
    if (!onCard)
    {
        out = juce::Image(juce::Image::RGB, W, H, false);
        std::array<std::unique_ptr<juce::Image::BitmapData>, 6> src;
        for (int f = 0; f < 6; ++f)
            src[(size_t)f] =
                std::make_unique<juce::Image::BitmapData>(faces[(size_t)f], juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst(out, juce::Image::BitmapData::writeOnly);
        std::vector<float> sinLon((size_t)W), cosLon((size_t)W);
        for (int i = 0; i < W; ++i)
        {
            const float lon = (((float)i + 0.5f) / (float)W - 0.5f) * juce::MathConstants<float>::twoPi;
            sinLon[(size_t)i] = std::sin(lon);
            cosLon[(size_t)i] = std::cos(lon);
        }
        auto rows = [&](int y0, int y1)
        {
            for (int j = y0; j < y1; ++j)
            {
                const float lat = (0.5f - ((float)j + 0.5f) / (float)H) * juce::MathConstants<float>::pi;
                const float cl = std::cos(lat), sl = std::sin(lat);
                uint8_t *o = dst.getLinePointer(j);
                for (int i = 0; i < W; ++i, o += dst.pixelStride)
                {
                    const float x = sinLon[(size_t)i] * cl, y = cosLon[(size_t)i] * cl, z = sl;
                    const int f = faceOf(x, y, z);
                    const auto &fc = kFaces[f];
                    const V3 up = cross(fc.right, fc.fwd);
                    const float vz = x * fc.fwd.x + y * fc.fwd.y + z * fc.fwd.z;
                    const float vx = x * fc.right.x + y * fc.right.y + z * fc.right.z;
                    const float vy = x * up.x + y * up.y + z * up.z;
                    const float px = 0.5f * (float)F + vx / vz * focal - 0.5f;
                    const float py = 0.5f * (float)F - vy / vz * focal - 0.5f;
                    const int x0 = juce::jlimit(0, F - 2, (int)std::floor(px));
                    const int y0r = juce::jlimit(0, F - 2, (int)std::floor(py));
                    const float fx = juce::jlimit(0.0f, 1.0f, px - (float)x0);
                    const float fy = juce::jlimit(0.0f, 1.0f, py - (float)y0r);
                    const auto &s = *src[(size_t)f];
                    const uint8_t *a = s.getPixelPointer(x0, y0r), *b = a + s.pixelStride;
                    const uint8_t *c = a + s.lineStride, *d = c + s.pixelStride;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float top = (float)a[k] + fx * ((float)b[k] - (float)a[k]);
                        const float bot = (float)c[k] + fx * ((float)d[k] - (float)c[k]);
                        o[k] = (uint8_t)juce::jlimit(0, 255, (int)(top + fy * (bot - top) + 0.5f));
                    }
                }
            }
        };
        const int nt = juce::jlimit(1, 8, (int)std::thread::hardware_concurrency() - 2);
        std::vector<std::thread> ts;
        for (int t = 1; t < nt; ++t)
            ts.emplace_back(rows, H * t / nt, H * (t + 1) / nt);
        rows(0, H / nt);
        for (auto &t : ts)
            t.join();
    }

    // the glows (the card has laid them in already)
    if (!onCard && !glowsOver.empty())
    {
        juce::Graphics g(out);
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        const auto &sprite = glowSprite();
        for (const auto &gw : glowsOver)
        {
            g.setOpacity(gw.alpha);
            for (float shift : {-(float)W, 0.0f, (float)W})
                if (gw.cx + shift + gw.rw > 0.0f && gw.cx + shift - gw.rw < (float)W)
                    g.drawImage(sprite,
                                juce::Rectangle<float>(2.0f * gw.rw, 2.0f * gw.rh).withCentre({gw.cx + shift, gw.cy}));
        }
    }
    return out;
}
} // namespace rysum
