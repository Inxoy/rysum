// "Konzert": one standalone plays, the others listen and render it themselves (see Concert.h)
#include "Concert.h"
#include "PluginProcessor.h"

namespace rysum
{
namespace
{
constexpr int kProtocol = 2; // 2: the pipes as bodies, the sounds of the hands
constexpr int kMaxListeners = 32;
constexpr int kMaxStateBytes = 16 * 1024 * 1024;
constexpr int kMaxFrameBytes = 4096;

/** settings that belong to whoever listens: never sent, never taken over */
bool isLocalSetting(const juce::String &id)
{
    static const juce::StringArray local{"output",      "master",   "binOrder", "binYaw",
                                         "binDfEq",     "inpOn",    "spkOrder", "spkDistComp",
                                         "trAngle",     "trAmount", "trBoost",  "trSideEq",
                                         "audioThreads", "midiCC"};
    return local.contains(id) || id.startsWith("dsf");
}

/** a frame: length (of what follows), type, payload */
juce::MemoryBlock frame(char type, const std::function<void(juce::MemoryOutputStream &)> &fill)
{
    juce::MemoryOutputStream body;
    body.writeByte(type);
    fill(body);
    juce::MemoryOutputStream out;
    out.writeInt((int)body.getDataSize());
    out.write(body.getData(), body.getDataSize());
    return out.getMemoryBlock();
}

bool readAll(juce::StreamingSocket &s, void *dst, int n)
{
    auto *p = static_cast<char *>(dst);
    while (n > 0)
    {
        const int r = s.read(p, n, true);
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

/** one frame from a socket; false = closed or not a frame of ours */
bool readFrame(juce::StreamingSocket &s, char &type, juce::MemoryBlock &payload, int maxLen)
{
    int32_t len = 0;
    if (!readAll(s, &len, 4))
        return false;
    len = (int32_t)juce::ByteOrder::swapIfBigEndian((uint32_t)len);
    if (len < 1 || len > maxLen)
        return false;
    juce::MemoryBlock b((size_t)len);
    if (!readAll(s, b.getData(), len))
        return false;
    type = static_cast<const char *>(b.getData())[0];
    if (type != 'J' && len > kMaxFrameBytes)
        return false; // only the state is allowed to be long
    payload = juce::MemoryBlock(static_cast<const char *>(b.getData()) + 1, (size_t)len - 1);
    return true;
}

bool sendAll(juce::StreamingSocket &s, const juce::MemoryBlock &b)
{
    return s.write(b.getData(), (int)b.getSize()) == (int)b.getSize();
}

/** a MIDI message a listener may play: notes, controllers, pressure, pitch wheel (no SysEx) */
bool playableMidi(const uint8_t *b, int n)
{
    if (n < 1 || n > 3 || b[0] < 0x80 || b[0] >= 0xf0)
        return false;
    const int need = (b[0] & 0xf0) == 0xc0 || (b[0] & 0xf0) == 0xd0 ? 2 : 3;
    if (n != need)
        return false;
    for (int i = 1; i < n; ++i)
        if (b[i] > 0x7f)
            return false;
    return true;
}
} // namespace

ConcertLink::ConcertLink(RysumProcessor &p) : juce::Thread("Rysum Konzert"), proc(p)
{
    fifoData.resize((size_t)kFifo);
    for (auto &v : heldVel)
        v = 0;
    for (auto &r : bodyRings)
        r = std::make_unique<BodyRing>();
}

ConcertLink::~ConcertLink()
{
    stopTimer();
    signalThreadShouldExit();
    if (listenerSocket != nullptr)
        listenerSocket->close();
    if (sock != nullptr)
        sock->close();
    stopThread(3000);
}

juce::StringArray ConcertLink::localAddresses()
{
    juce::StringArray out;
    for (auto &a : juce::IPAddress::getAllAddresses(false))
    {
        // an address of this computer someone else can reach: not the loopback, not a link-local
        // one, not a multicast group (224 and up)
        const int first = (int)a.address[0];
        if (a != juce::IPAddress::local() && first != 127 && first != 0 && first < 224 &&
            !(first == 169 && a.address[1] == 254))
            out.addIfNotAlreadyThere(a.toString());
    }
    return out;
}

void ConcertLink::setStatus(const juce::String &s)
{
    const juce::ScopedLock sl(statusLock);
    statusText = s;
}

juce::String ConcertLink::status() const
{
    const juce::ScopedLock sl(statusLock);
    return statusText;
}

double ConcertLink::audioTimeNow() const noexcept
{
    // the audio clock, carried on by the wall clock since the last block (at most 0.2 s)
    const double since = (juce::Time::getMillisecondCounterHiRes() - audioWall.load()) * 0.001;
    return audioTime.load() + juce::jlimit(0.0, 0.2, since);
}

bool ConcertLink::push(const Ev &e) noexcept
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 + n2 < 1)
        return false;
    fifoData[(size_t)(n1 > 0 ? s1 : s2)] = e;
    fifo.finishedWrite(1);
    return true;
}

// ---------------------------------------------------------------------------
// starting and stopping
// ---------------------------------------------------------------------------
bool ConcertLink::startPerformer(int p, bool loopbackOnly)
{
    stop();
    auto s = std::make_unique<juce::StreamingSocket>();
    if (!s->createListener(p, loopbackOnly ? "127.0.0.1" : juce::String()))
    {
        setStatus("Port " + juce::String(p) + " ist belegt");
        return false;
    }
    listenerSocket = std::move(s);
    port = p;
    numParams = proc.getNumParams();
    lastSent.assign((size_t)numParams, 0.0f);
    for (int i = 0; i < numParams; ++i)
        lastSent[(size_t)i] = proc.getParamAt(i)->getValue();
    lastStateKey = stateKey();
    lastStateCheck = juce::Time::getMillisecondCounterHiRes();
    stateSeq = 0;
    joinsWanted = 0;
    lastClockSent = -1.0;
    bodyWasOn.fill(false);
    bodySentMs.fill(0.0);
    for (auto &st : lenSent)
        st.fill(1.0f);
    lenSentMs = 0.0;
    {
        const juce::ScopedLock sl(outLock);
        outFrames.clear();
        joinPackages.clear();
    }
    fifo.reset();
    roleNow = Role::performer;
    setStatus(juce::String::fromUTF8("Spielt \xe2\x80\x93 noch niemand h\xc3\xb6rt zu"));
    startThread();
    startTimerHz(50);
    return true;
}

bool ConcertLink::startListener(const juce::String &h, int p)
{
    stop();
    host = h.trim();
    port = p;
    numParams = proc.getNumParams();
    mirrored.reset(new std::atomic<float>[(size_t)numParams]);
    for (int i = 0; i < numParams; ++i)
        mirrored[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
    ownSetup = proc.makeState(false); // (comes back when the concert ends)
    {
        const juce::ScopedLock sl(stateLock);
        pendingStates.clear();
    }
    barrierWanted = barrierDone = -1;
    offsets.clear();
    lastTarget = 0.0;
    playing.fill(0);
    isJoined = false;
    everJoined = false;
    forcedOff.assign((size_t)numParams, 0);
    for (auto *id : {"lvNetOn", "lucOn", "lvSelfPlay"})
        if (const int i = proc.getParamIndex(id); i >= 0)
            forcedOff[(size_t)i] = 1;
    fifo.reset();
    roleNow = Role::listener;
    setStatus(juce::String::fromUTF8("Verbinde mit ") + host + juce::String::fromUTF8(" \xe2\x80\xa6"));
    startThread();
    startTimerHz(50);
    return true;
}

void ConcertLink::stop()
{
    const auto was = roleNow.load();
    if (was == Role::off)
        return;
    stopTimer();
    signalThreadShouldExit();
    if (listenerSocket != nullptr)
        listenerSocket->close();
    if (sock != nullptr)
        sock->close();
    stopThread(3000);
    listenerSocket.reset();
    sock.reset();
    clients.clear();
    numListeners = 0;
    if (was == Role::listener)
    {
        // the notes stop; the listener's own setup comes back
        releaseNow = true;
        roleNow = Role::off;
        if (ownSetup.isValid())
            proc.applyState(ownSetup, false);
        ownSetup = {};
        isJoined = false;
    }
    roleNow = Role::off;
    setStatus({});
}

// ---------------------------------------------------------------------------
// the player
// ---------------------------------------------------------------------------
juce::String ConcertLink::stateKey()
{
    // the state without the parameters (those travel one by one): did something else change?
    auto st = proc.makeState(true);
    for (int i = st.getNumChildren(); --i >= 0;)
        if (st.getChild(i).hasType("PARAM"))
            st.removeChild(i, nullptr);
    return st.toXmlString() + rysum::autoToTree(proc.getAuto()).toXmlString();
}

juce::MemoryBlock ConcertLink::makeStateFrame(double t, int seq)
{
    // the patch state (the sound) plus what a patch does not hold but the piece needs
    auto st = proc.makeState(true);
    juce::ValueTree extra("CONCERT");
    extra.setProperty("extendedRange", proc.extendedRange.load(), nullptr);
    for (auto *id : {"mpe", "mpeBend", "temperament"})
        if (auto *prm = proc.apvts.getParameter(id))
            extra.setProperty(id, prm->getValue(), nullptr);
    extra.appendChild(rysum::autoToTree(proc.getAuto()), nullptr);
    st.appendChild(extra, nullptr);
    const auto xml = st.toXmlString();
    return frame('J',
                 [&](juce::MemoryOutputStream &o)
                 {
                     o.writeDouble(t);
                     o.writeInt(seq);
                     o.writeString(xml);
                 });
}

juce::MemoryBlock ConcertLink::makeJoinPackage()
{
    // for a new listener: the time it starts from, the state, the notes held down now
    const double t = audioTimeNow();
    juce::MemoryOutputStream o;
    o.writeDouble(t);
    const auto st = makeStateFrame(t, stateSeq);
    o.write(st.getData(), st.getSize());
    for (int i = 0; i < 16 * 128; ++i)
        if (const auto vel = heldVel[(size_t)i].load(); vel > 0)
        {
            const auto f = frame('M',
                                 [&](juce::MemoryOutputStream &m)
                                 {
                                     m.writeDouble(t);
                                     m.writeByte(3);
                                     m.writeByte((char)(0x90 | (i / 128)));
                                     m.writeByte((char)(i % 128));
                                     m.writeByte((char)vel);
                                 });
            o.write(f.getData(), f.getSize());
        }
    // where the pipes that are bodies are now, how long every pipe is
    std::vector<std::pair<double, juce::MemoryBlock>> bodies;
    scanBodies(bodies, true);
    for (auto &f : bodies)
        o.write(f.second.getData(), f.second.getSize());
    return o.getMemoryBlock();
}

juce::MemoryBlock ConcertLink::bodyFrame(double t, int s, bool on)
{
    return frame('D',
                 [&](juce::MemoryOutputStream &o)
                 {
                     o.writeDouble(t);
                     o.writeByte((char)s);
                     o.writeBool(on);
                     if (on)
                         for (int k = 0; k < kNumKeys; ++k)
                         {
                             const auto &q = bodySent[(size_t)s][(size_t)k];
                             o.writeFloat(q.x);
                             o.writeFloat(q.y);
                             o.writeFloat(q.z);
                         }
                 });
}

juce::MemoryBlock ConcertLink::lengthFrame(double t)
{
    return frame('L',
                 [&](juce::MemoryOutputStream &o)
                 {
                     o.writeDouble(t);
                     for (auto &st : lenSent)
                         for (auto l : st)
                             o.writeFloat(l);
                 });
}

void ConcertLink::scanBodies(std::vector<std::pair<double, juce::MemoryBlock>> &out, bool all)
{
    // the player: the stops whose pipes are bodies in the Saal - their places when they moved
    // (and once a second anyway), a stop that stops being one once
    const double t = audioTimeNow();
    const double now = juce::Time::getMillisecondCounterHiRes();
    auto &en = proc.engine;
    for (int s = 0; s < kNumStops; ++s)
    {
        const bool on = en.bodyOn[(size_t)s].load();
        if (!on)
        {
            if (bodyWasOn[(size_t)s] || all)
                out.push_back({t, bodyFrame(t, s, false)});
            bodyWasOn[(size_t)s] = false;
            continue;
        }
        float moved = 0.0f;
        for (int k = 0; k < kNumKeys; ++k)
        {
            const Vec3f q{en.bodyX[(size_t)s][(size_t)k].load(), en.bodyY[(size_t)s][(size_t)k].load(),
                          en.bodyZ[(size_t)s][(size_t)k].load()};
            auto &was = bodySent[(size_t)s][(size_t)k];
            moved = juce::jmax(moved, std::abs(q.x - was.x) + std::abs(q.y - was.y) + std::abs(q.z - was.z));
            was = q;
        }
        if (all || !bodyWasOn[(size_t)s] || moved > 0.0005f || now - bodySentMs[(size_t)s] > 1000.0)
        {
            out.push_back({t, bodyFrame(t, s, true)});
            bodySentMs[(size_t)s] = now;
        }
        bodyWasOn[(size_t)s] = true;
    }
    // Wachsen & Brechen: the lengths (five times a second at most)
    if (all || now - lenSentMs > 200.0)
    {
        bool changed = false;
        for (int s = 0; s < kNumStops; ++s)
            for (int k = 0; k < kNumKeys; ++k)
            {
                const float l = en.pipeLen[(size_t)s][(size_t)k].load();
                changed = changed || std::abs(l - lenSent[(size_t)s][(size_t)k]) > 1.0e-4f;
                lenSent[(size_t)s][(size_t)k] = l;
            }
        if (changed || all)
        {
            out.push_back({t, lengthFrame(t)});
            lenSentMs = now;
        }
    }
}

void ConcertLink::scanParameters()
{
    const double t = audioTimeNow();
    std::vector<std::pair<double, juce::MemoryBlock>> out;
    for (int i = 0; i < numParams; ++i)
    {
        auto *prm = proc.getParamAt(i);
        const float v = prm->getValue();
        if (std::abs(v - lastSent[(size_t)i]) <= 1.0e-7f || isLocalSetting(prm->getParameterID()))
            continue;
        lastSent[(size_t)i] = v;
        out.push_back({t, frame('P',
                                [&](juce::MemoryOutputStream &o)
                                {
                                    o.writeDouble(t);
                                    o.writeInt(i);
                                    o.writeFloat(v);
                                })});
    }
    // a changed stop list, layout, path, Verlauf ...: the whole state again (twice a second at most)
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastStateCheck > 500.0)
    {
        lastStateCheck = now;
        auto key = stateKey();
        if (key != lastStateKey)
        {
            lastStateKey = std::move(key);
            out.push_back({t, makeStateFrame(t, ++stateSeq)});
        }
    }
    scanBodies(out, false);
    if (!out.empty())
    {
        const juce::ScopedLock sl(outLock);
        for (auto &f : out)
            outFrames.push_back(std::move(f));
    }
}

void ConcertLink::captureMidi(const juce::MidiBuffer &midi, double blockTime, double sr) noexcept
{
    if (roleNow.load() != Role::performer)
        return;
    for (const auto meta : midi)
    {
        if (!playableMidi(meta.data, meta.numBytes))
            continue;
        Ev e;
        e.type = 'M';
        e.t = blockTime + meta.samplePosition / sr;
        e.n = (uint8_t)meta.numBytes;
        for (int i = 0; i < meta.numBytes; ++i)
            e.b[i] = meta.data[i];
        push(e);
        // what is held down now (for listeners who come in later)
        const int st = meta.data[0] & 0xf0, ch = meta.data[0] & 0x0f;
        if (st == 0x90 || st == 0x80)
            heldVel[(size_t)(ch * 128 + meta.data[1])] = st == 0x90 ? meta.data[2] : (uint8_t)0;
        else if (st == 0xb0 && (meta.data[1] == 123 || meta.data[1] == 120))
            for (int k = 0; k < 128; ++k)
                heldVel[(size_t)(ch * 128 + k)] = 0;
    }
}

void ConcertLink::captureClocks(double blockEndTime, const double *motion, const double *lfo,
                                double beats, bool running) noexcept
{
    if (roleNow.load() != Role::performer)
        return;
    // the sounds of the hands in the 3D Saal this block: knocks, claps, bowed pipes
    auto &en = proc.engine;
    for (int i = 0; i < en.numTapped; ++i)
    {
        const auto &r = en.tapped[(size_t)i];
        Ev e;
        e.type = 'K';
        e.t = blockEndTime;
        e.index = r.stop * 64 + r.key;
        e.b[0] = (uint8_t)r.kind;
        e.b[1] = (uint8_t)juce::jlimit(0, 255, r.room);
        e.b[2] = (uint8_t)((r.floor ? 1 : 0) | (r.struck ? 2 : 0));
        const float f[] = {r.x, r.y, r.z, r.speed, r.absorb, r.high, r.level, r.amount};
        for (size_t j = 0; j < 8; ++j)
            e.clocks[j] = f[j];
        push(e);
    }
    en.numTapped = 0;
    if (lastClockSent >= 0.0 && blockEndTime - lastClockSent < 0.05)
        return;
    lastClockSent = blockEndTime;
    Ev e;
    e.type = 'S';
    e.t = blockEndTime;
    for (int s = 0; s < kNumStops; ++s)
        e.clocks[(size_t)s] = motion[s];
    for (int i = 0; i < kNumLfos; ++i)
        e.clocks[(size_t)(kNumStops + i)] = lfo[i];
    e.clocks[kNumStops + kNumLfos] = beats;
    e.running = running;
    push(e);
}

void ConcertLink::runPerformer()
{
    auto encode = [](const Ev &e)
    {
        if (e.type == 'M')
            return frame('M',
                         [&](juce::MemoryOutputStream &o)
                         {
                             o.writeDouble(e.t);
                             o.writeByte((char)e.n);
                             for (int i = 0; i < e.n; ++i)
                                 o.writeByte((char)e.b[(size_t)i]);
                         });
        if (e.type == 'K')
            return frame('K',
                         [&](juce::MemoryOutputStream &o)
                         {
                             o.writeDouble(e.t);
                             o.writeInt(e.index);
                             for (int i = 0; i < 3; ++i)
                                 o.writeByte((char)e.b[(size_t)i]);
                             for (size_t j = 0; j < 8; ++j)
                                 o.writeFloat((float)e.clocks[j]);
                         });
        return frame('S',
                     [&](juce::MemoryOutputStream &o)
                     {
                         o.writeDouble(e.t);
                         for (auto c : e.clocks)
                             o.writeDouble(c);
                         o.writeBool(e.running);
                     });
    };
    while (!threadShouldExit())
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        // someone new
        if (listenerSocket->waitUntilReady(true, 3) == 1)
            if (auto *c = listenerSocket->waitForNextConnection())
            {
                std::unique_ptr<juce::StreamingSocket> s(c);
                if ((int)clients.size() < kMaxListeners)
                {
                    auto cl = std::make_unique<Client>();
                    cl->sock = std::move(s);
                    cl->connectedMs = now;
                    clients.push_back(std::move(cl));
                }
            }
        // hellos (and nothing else: a listener has nothing to say)
        for (auto &c : clients)
        {
            if (c->sock == nullptr)
                continue;
            const int ready = c->sock->waitUntilReady(true, 0);
            if (ready < 0)
            {
                c->sock.reset();
                continue;
            }
            if (ready == 0)
            {
                if (!c->helloOk && now - c->connectedMs > 4000.0)
                    c->sock.reset(); // no hello
                continue;
            }
            char type = 0;
            juce::MemoryBlock pl;
            if (!readFrame(*c->sock, type, pl, 64)) // (a listener only says hello)
            {
                c->sock.reset();
                continue;
            }
            if (!c->helloOk && type == 'H')
            {
                juce::MemoryInputStream in(pl, false);
                const int version = in.readInt(), params = in.readInt();
                if (version == kProtocol && params == numParams)
                {
                    c->helloOk = true;
                    ++joinsWanted; // (the message thread makes the join package)
                }
                else
                {
                    sendAll(*c->sock, frame('X', [](juce::MemoryOutputStream &o) { o.writeString("version"); }));
                    c->sock.reset();
                }
            }
        }
        // what was played and changed
        std::vector<std::pair<double, juce::MemoryBlock>> frames;
        {
            int s1, n1, s2, n2;
            const int ready = fifo.getNumReady();
            fifo.prepareToRead(ready, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i)
                frames.push_back({fifoData[(size_t)(s1 + i)].t, encode(fifoData[(size_t)(s1 + i)])});
            for (int i = 0; i < n2; ++i)
                frames.push_back({fifoData[(size_t)(s2 + i)].t, encode(fifoData[(size_t)(s2 + i)])});
            fifo.finishedRead(n1 + n2);
        }
        std::deque<juce::MemoryBlock> joins;
        {
            const juce::ScopedLock sl(outLock);
            for (auto &f : outFrames)
                frames.push_back(std::move(f));
            outFrames.clear();
            joins.swap(joinPackages);
        }
        std::stable_sort(frames.begin(), frames.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        // join packages go to the waiting listeners in the order they came
        for (auto &pk : joins)
            for (auto &c : clients)
                if (c->sock != nullptr && c->helloOk && !c->joined)
                {
                    juce::MemoryInputStream in(pk, false);
                    c->since = in.readDouble();
                    const auto rest = pk.getSize() - 8;
                    juce::MemoryBlock body(static_cast<const char *>(pk.getData()) + 8, rest);
                    c->joined = sendAll(*c->sock, body);
                    for (auto &f : c->backlog)
                        if (c->joined && f.first >= c->since)
                            c->joined = sendAll(*c->sock, f.second);
                    c->backlog.clear();
                    if (!c->joined)
                        c->sock.reset();
                    break;
                }
        for (auto &c : clients)
        {
            if (c->sock == nullptr)
                continue;
            for (auto &f : frames)
            {
                if (!c->joined)
                {
                    if (c->helloOk && c->backlog.size() < 100000)
                        c->backlog.push_back(f);
                    continue;
                }
                if (f.first >= c->since && !sendAll(*c->sock, f.second))
                {
                    c->sock.reset();
                    break;
                }
            }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [](const auto &c) { return c->sock == nullptr; }),
                      clients.end());
        int n = 0;
        for (auto &c : clients)
            n += c->joined ? 1 : 0;
        numListeners = n;
        wait(2);
    }
}

// ---------------------------------------------------------------------------
// the listener
// ---------------------------------------------------------------------------
double ConcertLink::scheduleAt(double remoteT)
{
    // the player's time -> the local audio time it is due at: the smallest difference of the last
    // few seconds is the fastest way here; the delay leaves room for the slower messages
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    double local = audioTimeNow();
    if (testJitterMs > 0.0)
        local += juce::Random::getSystemRandom().nextDouble() * testJitterMs * 0.001;
    offsets.push_back({nowMs, local - remoteT});
    while (offsets.size() > 1 && nowMs - offsets.front().first > 3000.0)
        offsets.pop_front();
    double off = offsets.front().second;
    for (auto &o : offsets)
        off = juce::jmin(off, o.second);
    // the estimate goes up at once (the player's clock paused, or the clocks drift that way: late
    // messages would play at once, without a buffer) but comes down gliding, half a millisecond
    // per message at most (a message that came quicker must not shift the rhythm)
    if (offsets.size() == 1 || lastTarget == 0.0 || off > smoothOff)
        smoothOff = off;
    else
        smoothOff += juce::jmax(-0.0005, off - smoothOff);
    // (never earlier than what is queued: the order stays)
    const double t = juce::jmax(lastTarget, remoteT + smoothOff + delayMs.load() * 0.001);
    lastTarget = t;
    return t;
}

void ConcertLink::receiveFrame(char type, juce::MemoryInputStream &in)
{
    lastHeardMs = juce::Time::getMillisecondCounterHiRes();
    const double t = in.readDouble();
    if (type == 'J')
    {
        const int seq = in.readInt();
        const auto xml = in.readString();
        const double at = scheduleAt(t);
        {
            const juce::ScopedLock sl(stateLock);
            pendingStates[seq] = xml;
        }
        if (isJoined.load())
        {
            Ev e;
            e.type = 'B';
            e.t = at;
            e.index = seq;
            e.session = session.load();
            if (!push(e))
                overflow = true;
        }
        else
            barrierWanted = seq; // the first one: at once
        return;
    }
    Ev e;
    e.type = type;
    if (type == 'M')
    {
        e.n = (uint8_t)juce::jlimit(0, 3, (int)in.readByte());
        for (int i = 0; i < e.n; ++i)
            e.b[(size_t)i] = (uint8_t)in.readByte();
        if (!playableMidi(e.b, e.n))
            return;
    }
    else if (type == 'P')
    {
        e.index = in.readInt();
        e.value = juce::jlimit(0.0f, 1.0f, in.readFloat());
        if (e.index < 0 || e.index >= numParams ||
            isLocalSetting(proc.getParamAt(e.index)->getParameterID()))
            return;
    }
    else if (type == 'S')
    {
        for (auto &c : e.clocks)
            c = in.readDouble();
        e.running = in.readBool();
        for (auto c : e.clocks)
            if (!std::isfinite(c))
                return;
    }
    else if (type == 'K')
    {
        e.index = in.readInt();
        for (int i = 0; i < 3; ++i)
            e.b[(size_t)i] = (uint8_t)in.readByte();
        for (size_t j = 0; j < 8; ++j)
        {
            e.clocks[j] = in.readFloat();
            if (!std::isfinite(e.clocks[j]))
                return;
        }
        if (e.index < 0 || e.index >= kNumStops * 64 || e.b[0] > 4)
            return;
    }
    else if (type == 'D')
    {
        // the places of one stop's pipes: their own ring
        const int s = (int)(uint8_t)in.readByte();
        if (s < 0 || s >= kNumStops)
            return;
        BodySnap b;
        b.on = in.readBool();
        if (b.on)
            for (auto &q : b.p)
            {
                q.x = in.readFloat();
                q.y = in.readFloat();
                q.z = in.readFloat();
                if (!std::isfinite(q.x + q.y + q.z) || std::abs(q.x) + std::abs(q.y) + std::abs(q.z) > 3000.0f)
                    return;
            }
        b.t = scheduleAt(t);
        b.session = session.load();
        auto &ring = *bodyRings[(size_t)s];
        int s1, n1, s2, n2;
        ring.fifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 + n2 > 0) // (full: this one is skipped - the next places come in 20 ms)
        {
            ring.data[(size_t)(n1 > 0 ? s1 : s2)] = b;
            ring.fifo.finishedWrite(1);
        }
        return;
    }
    else if (type == 'L')
    {
        LenSnap l;
        for (auto &st : l.len)
            for (auto &v : st)
            {
                v = in.readFloat();
                if (!std::isfinite(v))
                    return;
            }
        l.t = scheduleAt(t);
        l.session = session.load();
        int s1, n1, s2, n2;
        lenFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 + n2 > 0)
        {
            lenData[(size_t)(n1 > 0 ? s1 : s2)] = l;
            lenFifo.finishedWrite(1);
        }
        return;
    }
    else
        return;
    e.t = scheduleAt(t);
    e.session = session.load();
    if (!push(e))
        overflow = true;
}

void ConcertLink::runListener()
{
    // a session per connection; a lost one is tried again (and joins afresh: state, held notes)
    while (!threadShouldExit())
    {
        const bool lost = listenSession();
        releaseNow = true;
        isJoined = false;
        if (!lost || threadShouldExit())
            break;
        setStatus(juce::String::fromUTF8("Verbindung weg \xe2\x80\x93 verbinde erneut \xe2\x80\xa6"));
        wait(1000);
    }
}

bool ConcertLink::listenSession()
{
    // a new session: what is left of the last one is never played
    ++session;
    {
        const juce::ScopedLock sl(stateLock);
        pendingStates.clear();
    }
    barrierWanted = barrierDone = -1;
    offsets.clear();
    lastTarget = 0.0;
    overflow = false;
    sock = std::make_unique<juce::StreamingSocket>();
    if (!sock->connect(host, port, 4000))
    {
        setStatus(juce::String::fromUTF8("Keine Verbindung zu ") + host + ":" + juce::String(port));
        return everJoined; // (once there was a concert: keep trying)
    }
    sendAll(*sock, frame('H',
                         [&](juce::MemoryOutputStream &o)
                         {
                             o.writeInt(kProtocol);
                             o.writeInt(numParams);
                         }));
    lastHeardMs = juce::Time::getMillisecondCounterHiRes();
    while (!threadShouldExit())
    {
        const int ready = sock->waitUntilReady(true, 20);
        if (ready < 0)
            break;
        if (ready == 0)
        {
            // nothing for a while (the player sends its clocks 20 times a second): gone
            if (juce::Time::getMillisecondCounterHiRes() - lastHeardMs > 1500.0)
                break;
            continue;
        }
        char type = 0;
        juce::MemoryBlock pl;
        if (!readFrame(*sock, type, pl, kMaxStateBytes))
            break;
        if (type == 'X')
        {
            setStatus(juce::String::fromUTF8("Der Spieler hat eine andere Version des Plugins"));
            return false;
        }
        juce::MemoryInputStream in(pl, false);
        receiveFrame(type, in);
        if (overflow)
            break; // (more than the buffer holds: start again rather than lose a note-off)
    }
    sock->close();
    if (isJoined.load())
        everJoined = true;
    if (!threadShouldExit())
        setStatus(juce::String::fromUTF8("Der Spieler ist nicht mehr zu h\xc3\xb6ren"));
    return true;
}


void ConcertLink::playInto(juce::MidiBuffer &midi, double blockTime, int numSamples, double sr) noexcept
{
    if (roleNow.load() == Role::off && !releaseNow.load())
        return;
    if (releaseNow.exchange(false))
    {
        // the link is gone or stalls: nothing keeps sounding
        for (int i = 0; i < 16 * 128; ++i)
            if (playing[(size_t)i] != 0)
            {
                midi.addEvent(juce::MidiMessage::noteOff(i / 128 + 1, i % 128), 0);
                playing[(size_t)i] = 0;
            }
        midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        stopFollowing();
    }
    if (roleNow.load() != Role::listener || !isJoined.load())
        return;
    followBodies(blockTime);
    const double end = blockTime + numSamples / sr;
    for (;;)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead(1, s1, n1, s2, n2);
        if (n1 + n2 < 1)
            break;
        const Ev &e = fifoData[(size_t)(n1 > 0 ? s1 : s2)];
        if (e.session != session.load())
        {
            fifo.finishedRead(1); // (left from a connection that broke off)
            continue;
        }
        if (e.t >= end)
            break; // later
        if (e.type == 'B' && barrierDone.load() < e.index)
        {
            barrierWanted = e.index; // a state change: what follows waits for it
            break;
        }
        const int pos = juce::jlimit(0, juce::jmax(0, numSamples - 1), (int)((e.t - blockTime) * sr));
        if (e.type == 'M')
        {
            midi.addEvent(e.b, e.n, pos);
            const int st = e.b[0] & 0xf0, ch = e.b[0] & 0x0f;
            if (st == 0x90 && e.b[2] > 0)
            {
                playing[(size_t)(ch * 128 + e.b[1])] = 1;
                const juce::SpinLock::ScopedTryLockType tl(testSpin);
                if (tl.isLocked())
                {
                    noteRing[(size_t)(noteRingPos++ % noteRing.size())] = blockTime + pos / sr;
                }
            }
            else if (st == 0x80 || st == 0x90)
                playing[(size_t)(ch * 128 + e.b[1])] = 0;
        }
        else if (e.type == 'P' && !forcedOff[(size_t)e.index])
        {
            // (what the control should be first: the re-assert must not put the old value back)
            mirrored[(size_t)e.index] = e.value;
            if (auto *prm = proc.getParamAt(e.index))
                prm->setValueNotifyingHost(e.value);
        }
        else if (e.type == 'S')
            proc.concertClocks(e.clocks.data(), e.clocks.data() + kNumStops,
                               e.clocks[kNumStops + kNumLfos], e.running);
        else if (e.type == 'K')
        {
            OrganEngine::RemoteSound r;
            r.kind = e.b[0];
            r.room = e.b[1];
            r.floor = (e.b[2] & 1) != 0;
            r.struck = (e.b[2] & 2) != 0;
            r.stop = e.index / 64;
            r.key = e.index % 64;
            r.x = (float)e.clocks[0];
            r.y = (float)e.clocks[1];
            r.z = (float)e.clocks[2];
            r.speed = (float)e.clocks[3];
            r.absorb = (float)e.clocks[4];
            r.high = (float)e.clocks[5];
            r.level = (float)e.clocks[6];
            r.amount = (float)e.clocks[7];
            proc.engine.remoteSound(r);
            if (r.kind == 0)
                ++knocksHeard;
        }
        fifo.finishedRead(1);
    }
}

void ConcertLink::followBodies(double now) noexcept
{
    // the listener: every stop's pipes where the player's were at this moment (between the two
    // places sent around it)
    auto &en = proc.engine;
    const uint32_t ses = session.load();
    bool any = false;
    for (int s = 0; s < kNumStops; ++s)
    {
        auto &r = *bodyRings[(size_t)s];
        if (r.haveCur && r.cur.session != ses)
            r.haveCur = false;
        if (r.haveNext && r.next.session != ses)
            r.haveNext = false;
        for (;;)
        {
            if (!r.haveNext)
            {
                int s1, n1, s2, n2;
                r.fifo.prepareToRead(1, s1, n1, s2, n2);
                if (n1 + n2 < 1)
                    break;
                r.next = r.data[(size_t)(n1 > 0 ? s1 : s2)];
                r.fifo.finishedRead(1);
                if (r.next.session != ses)
                    continue;
                r.haveNext = true;
            }
            if (r.next.t > now)
                break;
            r.cur = r.next; // (its time has come)
            r.haveCur = true;
            r.haveNext = false;
        }
        en.followOn[(size_t)s] = r.haveCur && r.cur.on;
        if (!en.followOn[(size_t)s])
            continue;
        any = true;
        const bool between = r.haveNext && r.next.on && r.next.t > r.cur.t && r.next.t - r.cur.t < 0.25;
        const float a = between ? (float)juce::jlimit(0.0, 1.0, (now - r.cur.t) / (r.next.t - r.cur.t)) : 0.0f;
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto &p = r.cur.p[(size_t)k];
            if (!between)
            {
                en.followPos[(size_t)s][(size_t)k] = p;
                continue;
            }
            const auto &q = r.next.p[(size_t)k];
            en.followPos[(size_t)s][(size_t)k] = {p.x + a * (q.x - p.x), p.y + a * (q.y - p.y), p.z + a * (q.z - p.z)};
        }
    }
    // the lengths: the last ones due
    for (;;)
    {
        int s1, n1, s2, n2;
        lenFifo.prepareToRead(1, s1, n1, s2, n2);
        if (n1 + n2 < 1)
            break;
        const auto &l = lenData[(size_t)(n1 > 0 ? s1 : s2)];
        if (l.session == ses && l.t > now)
            break;
        if (l.session == ses)
        {
            en.followLen = l.len;
            en.followLenHave = true;
        }
        lenFifo.finishedRead(1);
    }
    en.followPlayer = true;
    juce::ignoreUnused(any);
}

void ConcertLink::stopFollowing() noexcept
{
    auto &en = proc.engine;
    en.followPlayer = false;
    en.followOn.fill(false);
    en.followLenHave = false;
    en.releaseBowed();
    for (auto &r : bodyRings)
        r->haveCur = r->haveNext = false;
}

std::vector<double> ConcertLink::testNoteTimes() const
{
    const juce::SpinLock::ScopedLockType sl(testSpin);
    std::vector<double> out;
    const size_t n = juce::jmin(noteRingPos, noteRing.size());
    for (size_t i = noteRingPos - n; i < noteRingPos; ++i)
        out.push_back(noteRing[i % noteRing.size()]);
    return out;
}

void ConcertLink::applyState(const juce::String &xml)
{
    auto st = juce::ValueTree::fromXml(xml);
    if (!st.isValid())
        return;
    auto extra = st.getChildWithName("CONCERT");
    st.removeChild(extra, nullptr);
    // the player's tuning comes with it, whatever the listener chose for their own patches
    const bool tuning = proc.loadPresetTuning.load();
    // where the listener stands stays theirs: a patch with a built-in room puts one at its
    // listening place - only when the room is a new one
    const auto roomBefore = proc.walkModelFile;
    std::array<float, 4> pose{};
    for (size_t i = 0; i < pose.size(); ++i)
        pose[i] = proc.walkPose[i].load();
    proc.loadPresetTuning = true;
    proc.applyState(st, true);
    proc.loadPresetTuning = tuning;
    if (proc.walkModelFile == roomBefore)
    {
        for (size_t i = 0; i < pose.size(); ++i)
            proc.walkPose[i] = pose[i];
        ++proc.walkSerial; // (the open 3D view takes the pose back)
    }
    // what makes notes or sound of its own on the player is not mirrored: off here
    for (int i = 0; i < numParams; ++i)
        if (forcedOff[(size_t)i])
            proc.getParamAt(i)->setValueNotifyingHost(0.0f);
    if (extra.isValid())
    {
        proc.extendedRange = (bool)extra.getProperty("extendedRange", false);
        for (auto *id : {"mpe", "mpeBend", "temperament"})
            if (auto *prm = proc.apvts.getParameter(id); prm != nullptr && extra.hasProperty(id))
                prm->setValueNotifyingHost((float)(double)extra.getProperty(id));
        // the Verlauf plays on the player's clock (and never records here)
        auto set = rysum::autoFromTree(extra.getChildWithName("VERLAUF"));
        set.clock = 1;
        for (auto &l : set.lanes)
            l.armed = false;
        proc.getAuto() = set;
        proc.setAutoRecord(false);
        proc.autoChanged();
        proc.autoPublish();
    }
    for (int i = 0; i < numParams; ++i)
        if (!isLocalSetting(proc.getParamAt(i)->getParameterID()))
            mirrored[(size_t)i] = forcedOff[(size_t)i] ? 0.0f : proc.getParamAt(i)->getValue();
}

// ---------------------------------------------------------------------------
// message thread
// ---------------------------------------------------------------------------
void ConcertLink::timerCallback()
{
    const auto r = roleNow.load();
    if (r == Role::performer)
    {
        while (joinsWanted.load() > 0)
        {
            --joinsWanted;
            auto pk = makeJoinPackage();
            const juce::ScopedLock sl(outLock);
            joinPackages.push_back(std::move(pk));
        }
        scanParameters();
        const int n = numListeners.load();
        setStatus(n == 0 ? juce::String::fromUTF8("Spielt \xe2\x80\x93 noch niemand h\xc3\xb6rt zu")
                         : juce::String::fromUTF8("Spielt \xc2\xb7 ") + juce::String(n) +
                               juce::String::fromUTF8(" Zuh\xc3\xb6rer"));
        return;
    }
    if (r != Role::listener)
        return;
    // a state whose time has come (or the first one: at once)
    const int want = barrierWanted.load();
    if (want > barrierDone.load())
    {
        juce::String xml;
        bool have = false;
        {
            const juce::ScopedLock sl(stateLock);
            auto it = pendingStates.find(want);
            if (it != pendingStates.end())
            {
                xml = it->second;
                have = true;
            }
            pendingStates.erase(pendingStates.begin(), pendingStates.upper_bound(want));
        }
        if (have)
            applyState(xml);
        barrierDone = want;
        if (!isJoined.load() && have)
        {
            isJoined = true;
            setStatus(juce::String::fromUTF8("H\xc3\xb6rt zu bei ") + host);
        }
    }
    // the player's controls are the player's: a local change goes back (twice a second)
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (isJoined.load() && now - lastAssert > 500.0)
    {
        lastAssert = now;
        for (int i = 0; i < numParams; ++i)
        {
            const float m = mirrored[(size_t)i].load();
            if (std::isnan(m))
                continue;
            auto *prm = proc.getParamAt(i);
            if (std::abs(prm->getValue() - m) > 1.0e-5f)
                prm->setValueNotifyingHost(m);
        }
    }
}

void ConcertLink::run()
{
    if (roleNow.load() == Role::performer)
        runPerformer();
    else if (roleNow.load() == Role::listener)
        runListener();
}
} // namespace rysum
