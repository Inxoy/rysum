// "Konzert": one standalone plays, the others listen and render it themselves (see Concert.h)
#include "Concert.h"
#include "PluginProcessor.h"
#include <cstring>

namespace rysum
{
namespace
{
constexpr int kProtocol = 3; // 2: the pipes as bodies, the sounds of the hands; 3: the password, the voice
constexpr int kMaxListeners = 32;
constexpr int kMaxStateBytes = 16 * 1024 * 1024;
constexpr int kMaxFrameBytes = 4096;

/** settings that belong to whoever listens: never sent, never taken over */
bool isLocalSetting(const juce::String &id)
{
    static const juce::StringArray local{"output",      "master",   "binOrder", "binYaw",
                                         "binDfEq",     "spkOrder", "spkDistComp",
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

/** SHA-256 (FIPS 180-4): the password's answer to the player's challenge */
std::array<uint8_t, 32> sha256(const uint8_t *data, size_t len)
{
    static const uint32_t K[64]{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56)
        m.push_back(0);
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; --i)
        m.push_back((uint8_t)(bits >> (8 * i)));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 |
                   (uint32_t)m[off + 4 * i + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            out[(size_t)(4 * i + j)] = (uint8_t)(h[i] >> (24 - 8 * j));
    return out;
}

/** the answer to a challenge: SHA-256 of the challenge and the password (UTF-8) */
std::array<uint8_t, 32> passwordAnswer(const std::array<uint8_t, 16> &nonce, const juce::String &pw)
{
    std::vector<uint8_t> b(nonce.begin(), nonce.end());
    const char *u = pw.toRawUTF8();
    b.insert(b.end(), u, u + std::strlen(u));
    return sha256(b.data(), b.size());
}

/** a line of the relay's protocol (at most 200 bytes, within the time) */
bool readLine(juce::StreamingSocket &s, juce::String &line, int timeoutMs)
{
    std::string got;
    const double end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (got.size() < 200)
    {
        const int left = (int)(end - juce::Time::getMillisecondCounterHiRes());
        if (left <= 0 || s.waitUntilReady(true, left) != 1)
            return false;
        char c = 0;
        if (s.read(&c, 1, false) != 1)
            return false;
        if (c == '\n')
        {
            line = juce::String(got).trim();
            return true;
        }
        got.push_back(c);
    }
    return false;
}

bool sendLine(juce::StreamingSocket &s, const juce::String &line)
{
    const auto t = (line + "\n").toStdString();
    return s.write(t.data(), (int)t.size()) == (int)t.size();
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
    stopThread(9000);
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

juce::String ConcertLink::sha256Hex(const void *data, size_t n)
{
    const auto h = sha256(static_cast<const uint8_t *>(data), n);
    return juce::String::toHexString(h.data(), (int)h.size(), 0);
}

bool ConcertLink::parseAddress(const juce::String &text, juce::String &h, int &p, juce::String &room)
{
    auto t = text.trim();
    room = {};
    if (t.containsChar('/'))
    {
        room = t.fromFirstOccurrenceOf("/", false, false).trim().toLowerCase();
        t = t.upToFirstOccurrenceOf("/", false, false).trim();
        if (room.isEmpty() || room.length() > 32 || !room.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-_"))
            return false;
    }
    p = kDefaultPort;
    // "host:port" (one colon only: an IPv6 address stays whole)
    if (t.indexOfChar(':') > 0 && t.indexOfChar(':') == t.lastIndexOfChar(':'))
    {
        const auto ps = t.fromFirstOccurrenceOf(":", false, false);
        if (!ps.containsOnly("0123456789") || ps.isEmpty())
            return false;
        p = ps.getIntValue();
        t = t.upToFirstOccurrenceOf(":", false, false);
        if (p < 1 || p > 65535)
            return false;
    }
    h = t;
    return h.isNotEmpty() && !h.containsAnyOf(" /\\");
}

void ConcertLink::setPassword(const juce::String &pw)
{
    const juce::ScopedLock sl(statusLock);
    password = pw.substring(0, 128);
}

juce::String ConcertLink::getPassword() const
{
    const juce::ScopedLock sl(statusLock);
    return password;
}

juce::String ConcertLink::relayAddress() const
{
    if (!relayMode.load())
        return {};
    return relayHost + (relayPort != kDefaultPort ? ":" + juce::String(relayPort) : juce::String()) + "/" + relayRoom;
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
    relayMode = false;
    beginPerformer();
    return true;
}

bool ConcertLink::startPerformerRelay(const juce::String &address)
{
    stop();
    juce::String h, room;
    int p = kDefaultPort;
    if (!parseAddress(address, h, p, room) || room.isEmpty())
    {
        setStatus(juce::String::fromUTF8("Relay-Adresse: server[:port]/raum"));
        return false;
    }
    relayHost = h;
    relayPort = p;
    relayRoom = room;
    relayCtl.reset();
    relayBuf.clear();
    relayRetryMs = 0.0;
    relayMode = true;
    beginPerformer();
    setStatus(juce::String::fromUTF8("Verbinde mit dem Relay ") + relayAddress() + juce::String::fromUTF8(" \xe2\x80\xa6"));
    return true;
}

void ConcertLink::beginPerformer()
{
    numParams = proc.getNumParams();
    failedAt.clear();
    refuseUntil = 0.0;
    voiceOutFifo.reset();
    vCur.n = 0;
    vCnt = 0;
    vAcc = vLp1 = vLp2 = 0.0f;
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
    audioOutFifo.reset();
    fifo.reset();
    roleNow = Role::performer;
    setStatus(juce::String::fromUTF8("Spielt \xe2\x80\x93 noch niemand h\xc3\xb6rt zu"));
    startThread();
    startTimerHz(50);
}

bool ConcertLink::startListener(const juce::String &h, int p)
{
    stop();
    {
        juce::String hh, room;
        int pp = p;
        if (!parseAddress(h, hh, pp, room))
        {
            setStatus(juce::String::fromUTF8("Adresse: host, host:port oder server[:port]/raum"));
            return false;
        }
        host = hh;
        port = h.containsChar(':') ? pp : p; // (no port typed: the one asked for)
        listenRoom = room;
    }
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
    for (auto *id : {"lvNetOn", "lvSelfPlay"})
        if (const int i = proc.getParamIndex(id); i >= 0)
            forcedOff[(size_t)i] = 1;
    {
        const juce::ScopedLock sl(figLock);
        actsOut.clear();
    }
    // the hands' acts go to the player
    proc.engine.actRedirect = [this](OrganEngine::Act a, int s, int k, Vec3f v, float amount)
    {
        const juce::ScopedLock sl(figLock);
        GuestAct g;
        g.act = (int)a;
        g.stop = s;
        g.key = k;
        g.v = v;
        g.amount = amount;
        if (actsOut.size() < 256)
            actsOut.push_back(g);
    };
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
    proc.engine.actRedirect = nullptr;
    allowedLevel = 0;
    stopTimer();
    signalThreadShouldExit();
    if (listenerSocket != nullptr)
        listenerSocket->close();
    if (sock != nullptr)
        sock->close();
    // (the relay's line is the network thread's own: it lets go within a few ms - or, in the middle of
    //  connecting to the relay, within a few seconds)
    stopThread(9000);
    listenerSocket.reset();
    sock.reset();
    relayCtl.reset();
    relayMode = false;
    relayUp = false;
    talking = false;
    clients.clear();
    numListeners = 0;
    {
        const juce::ScopedLock sl(figLock);
        figs.clear();
    }
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
    for (auto *id : {"mpe", "mpeBend", "temperament", "inpOn"})
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
        // someone new: straight here, or through the relay
        if (listenerSocket != nullptr)
        {
            if (listenerSocket->waitUntilReady(true, 3) == 1)
                if (auto *c = listenerSocket->waitForNextConnection())
                    admit(std::unique_ptr<juce::StreamingSocket>(c), now);
        }
        else
            relayPoll(now);
        // hellos, then where each listener stands (and what its hands do)
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
            if (!readFrame(*c->sock, type, pl, 512)) // (a listener says little)
            {
                c->sock.reset();
                continue;
            }
            if (!c->helloOk && type == 'H')
            {
                juce::MemoryInputStream in(pl, false);
                const int version = in.readInt(), params = in.readInt();
                std::array<uint8_t, 32> answer{};
                const bool hasAnswer = in.getNumBytesRemaining() >= 32 && in.read(answer.data(), 32) == 32;
                const auto pw = getPassword();
                // the password: the answer to this listener's own challenge (none set: anyone may listen)
                const bool passOk = pw.isEmpty() || (hasAnswer && answer == passwordAnswer(c->nonce, pw));
                if (version == kProtocol && params == numParams && passOk)
                {
                    c->helloOk = true;
                    ++joinsWanted; // (the message thread makes the join package)
                }
                else
                {
                    const bool wrongPw = version == kProtocol && params == numParams;
                    sendAll(*c->sock, frame('X', [&](juce::MemoryOutputStream &o) { o.writeString(wrongPw ? "password" : "version"); }));
                    c->sock.reset();
                    if (wrongPw)
                    {
                        // (guessing: ten wrong ones within a minute shut the door for half a minute)
                        failedAt.push_back(now);
                        while (!failedAt.empty() && now - failedAt.front() > 60000.0)
                            failedAt.pop_front();
                        if (failedAt.size() >= 10)
                            refuseUntil = now + 30000.0;
                    }
                }
            }
            else if (c->helloOk)
            {
                juce::MemoryInputStream in(pl, false);
                readClientFrame(*c, type, in);
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
        {
            // the audio input
            int s1, n1, s2, n2;
            audioOutFifo.prepareToRead(audioOutFifo.getNumReady(), s1, n1, s2, n2);
            auto enc = [&](const AudioChunk &a)
            {
                frames.push_back({a.t, frame('A',
                                             [&](juce::MemoryOutputStream &o)
                                             {
                                                 o.writeDouble(a.t);
                                                 o.writeInt(a.sr);
                                                 o.writeShort((short)a.n);
                                                 o.write(a.s.data(), (size_t)a.n * 2 * sizeof(int16_t));
                                             })});
            };
            for (int i = 0; i < n1; ++i)
                enc(audioOut[(size_t)(s1 + i)]);
            for (int i = 0; i < n2; ++i)
                enc(audioOut[(size_t)(s2 + i)]);
            audioOutFifo.finishedRead(n1 + n2);
        }
        {
            // the voice
            int s1, n1, s2, n2;
            voiceOutFifo.prepareToRead(voiceOutFifo.getNumReady(), s1, n1, s2, n2);
            auto enc = [&](const VoiceChunk &v)
            {
                frames.push_back({v.t, frame('V',
                                             [&](juce::MemoryOutputStream &o)
                                             {
                                                 o.writeDouble(v.t);
                                                 o.writeInt(v.sr);
                                                 o.writeShort((short)v.n);
                                                 o.write(v.s.data(), (size_t)v.n * sizeof(int16_t));
                                             })});
            };
            for (int i = 0; i < n1; ++i)
                enc(voiceOut[(size_t)(s1 + i)]);
            for (int i = 0; i < n2; ++i)
                enc(voiceOut[(size_t)(s2 + i)]);
            voiceOutFifo.finishedRead(n1 + n2);
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
                    // (first who it is: its own figure is not drawn for it)
                    c->joined = sendAll(*c->sock, frame('I', [&](juce::MemoryOutputStream &o) { o.writeDouble(c->since); o.writeInt(c->id); }))
                                && sendAll(*c->sock, body);
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
        // the hands of listeners who left (or no leave to carry any more) let go
        for (auto &c : clients)
            if (c->sock == nullptr || guestHands.load() < 2)
                releaseGuestHand(c->id);
        // who stands where, ten times a second: to everyone (not timed - the latest is what counts)
        if (now - lastFiguresMs > 100.0)
        {
            lastFiguresMs = now;
            std::vector<Figure> all, guests;
            Figure me;
            me.id = 0;
            me.name = juce::String::fromUTF8("Spieler");
            me.x = proc.walkPose[0].load();
            me.y = proc.walkPose[1].load();
            me.z = proc.engine.viewZ.load();
            me.yaw = proc.walkPose[2].load();
            all.push_back(me);
            for (auto &c : clients)
                if (c->sock != nullptr && c->joined && c->hasFig && now - c->fig.seenMs < 3000.0)
                {
                    all.push_back(c->fig);
                    guests.push_back(c->fig);
                }
            {
                const juce::ScopedLock sl(figLock);
                figs = guests;
            }
            const auto f = figuresFrame(all);
            for (auto &c : clients)
                if (c->sock != nullptr && c->joined && !sendAll(*c->sock, f))
                    c->sock.reset();
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

void ConcertLink::admit(std::unique_ptr<juce::StreamingSocket> s, double now)
{
    // a new listener: its own challenge first (the hello must answer it with the password)
    if ((int)clients.size() >= kMaxListeners || now < refuseUntil || s == nullptr)
        return;
    auto cl = std::make_unique<Client>();
    juce::Random rnd;
    rnd.setSeedRandomly();
    const auto t = (int64_t)juce::Time::getHighResolutionTicks();
    for (size_t i = 0; i < cl->nonce.size(); ++i)
        cl->nonce[i] = (uint8_t)(rnd.nextInt(256) ^ (int)((t >> (8 * (i % 8))) & 0xff));
    if (!sendAll(*s, frame('N', [&](juce::MemoryOutputStream &o) { o.write(cl->nonce.data(), cl->nonce.size()); })))
        return;
    cl->sock = std::move(s);
    cl->connectedMs = now;
    cl->id = nextClientId++;
    clients.push_back(std::move(cl));
}

bool ConcertLink::relayConnect(double now)
{
    // the player's line to the relay: "this room is mine"; then the relay names every listener who comes
    relayRetryMs = now + 3000.0;
    auto s = std::make_unique<juce::StreamingSocket>();
    if (!s->connect(relayHost, relayPort, 3000))
    {
        setStatus(juce::String::fromUTF8("Relay ") + relayHost + ":" + juce::String(relayPort) +
                  juce::String::fromUTF8(" nicht erreichbar \xe2\x80\x93 versuche weiter \xe2\x80\xa6"));
        return false;
    }
    juce::String reply;
    if (!sendLine(*s, "RYSUM-RELAY 1 HOST " + relayRoom) || !readLine(*s, reply, 4000) || reply != "OK")
    {
        setStatus(reply.startsWith("ERR taken")
                      ? juce::String::fromUTF8("Den Raum \xe2\x80\x9e") + relayRoom +
                            juce::String::fromUTF8("\xe2\x80\x9c gibt es beim Relay schon \xe2\x80\x93 ein anderer Name?")
                      : juce::String::fromUTF8("Das Relay antwortet nicht richtig (") + reply + ")");
        return false;
    }
    relayCtl = std::move(s);
    relayBuf.clear();
    relayPingMs = now;
    relayUp = true;
    setStatus(juce::String::fromUTF8("Spielt \xc3\xbc" "ber das Relay ") + relayAddress() +
              juce::String::fromUTF8(" \xe2\x80\x93 noch niemand h\xc3\xb6rt zu"));
    return true;
}

void ConcertLink::relayPoll(double now)
{
    if (relayCtl == nullptr)
    {
        if (now >= relayRetryMs)
            relayConnect(now);
        else
            wait(3);
        return;
    }
    const int ready = relayCtl->waitUntilReady(true, 3);
    if (ready < 0)
    {
        relayCtl.reset();
        relayUp = false;
        setStatus(juce::String::fromUTF8("Relay weg \xe2\x80\x93 verbinde erneut \xe2\x80\xa6"));
        return;
    }
    if (ready == 1)
    {
        char buf[256];
        const int got = relayCtl->read(buf, (int)sizeof(buf), false);
        if (got <= 0)
        {
            relayCtl.reset();
            relayUp = false;
            setStatus(juce::String::fromUTF8("Relay weg \xe2\x80\x93 verbinde erneut \xe2\x80\xa6"));
            return;
        }
        relayBuf.append(buf, (size_t)got);
        if (relayBuf.size() > 4096)
            relayBuf.clear();
        for (size_t nl; (nl = relayBuf.find('\n')) != std::string::npos;)
        {
            const auto line = juce::String(relayBuf.substr(0, nl)).trim();
            relayBuf.erase(0, nl + 1);
            if (line.startsWith("L "))
            {
                // a listener waits at the relay: a line of our own to it, joined to theirs
                const auto id = line.substring(2).trim();
                if (!id.containsOnly("0123456789") || id.isEmpty())
                    continue;
                auto s = std::make_unique<juce::StreamingSocket>();
                juce::String reply;
                if (s->connect(relayHost, relayPort, 3000) && sendLine(*s, "RYSUM-RELAY 1 ATTACH " + relayRoom + " " + id) &&
                    readLine(*s, reply, 4000) && reply == "OK")
                    admit(std::move(s), juce::Time::getMillisecondCounterHiRes());
            }
        }
    }
    // (the line stays open: a word every 15 s)
    if (now - relayPingMs > 15000.0)
    {
        relayPingMs = now;
        if (!sendLine(*relayCtl, "P"))
        {
            relayCtl.reset();
            relayUp = false;
        }
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
    // per 20 ms at most, however many messages come (a message that came quicker must not shift
    // the rhythm)
    const double glide = 0.0005 * juce::jlimit(0.0, 5.0, (nowMs - lastGlideMs) / 20.0);
    lastGlideMs = nowMs;
    if (offsets.size() == 1 || lastTarget == 0.0 || off > smoothOff)
        smoothOff = off;
    else
        smoothOff += juce::jmax(-glide, off - smoothOff);
    // (never earlier than what is queued: the order stays)
    const double t = juce::jmax(lastTarget, remoteT + smoothOff + delayMs.load() * 0.001);
    lastTarget = t;
    return t;
}

juce::MemoryBlock ConcertLink::figuresFrame(const std::vector<Figure> &all)
{
    return frame('F',
                 [&](juce::MemoryOutputStream &o)
                 {
                     o.writeDouble(0.0);
                     o.writeInt((int)all.size());
                     for (auto &f : all)
                     {
                         o.writeInt(f.id);
                         o.writeFloat(f.x);
                         o.writeFloat(f.y);
                         o.writeFloat(f.z);
                         o.writeFloat(f.yaw);
                         o.writeString(f.name);
                     }
                     o.writeInt(guestHands.load()); // (what the listeners' hands may do)
                 });
}

void ConcertLink::readClientFrame(Client &c, char type, juce::MemoryInputStream &in)
{
    // the player, network thread: a listener says where it stands
    if (type == 'W')
    {
        Figure f;
        f.id = c.id;
        f.x = in.readFloat();
        f.y = in.readFloat();
        f.z = in.readFloat();
        f.yaw = in.readFloat();
        f.name = in.readString().substring(0, 32);
        if (!std::isfinite(f.x + f.y + f.z + f.yaw) || std::abs(f.x) + std::abs(f.y) + std::abs(f.z) > 3000.0f)
            return;
        f.seenMs = juce::Time::getMillisecondCounterHiRes();
        c.fig = f;
        c.hasFig = true;
    }
    else if (type == 'Q')
    {
        // a listener's hand: the pipe it carries and where the hand is (only with leave to carry)
        const int s = in.readInt(), k = in.readInt();
        const float x = in.readFloat(), y = in.readFloat(), z = in.readFloat();
        const int slot = c.id % OrganEngine::kGuestHands;
        auto &gh = proc.engine.guestHands[(size_t)slot];
        if (guestHands.load() < 2 || s < 0 || s >= kNumStops || k < 0 || k >= kNumKeys ||
            !std::isfinite(x + y + z) || std::abs(x) + std::abs(y) + std::abs(z) > 3000.0f)
        {
            releaseGuestHand(c.id);
            return;
        }
        gh.x = x;
        gh.y = y;
        gh.z = z;
        gh.key = k;
        gh.stop = s;
        handOwner[(size_t)slot] = c.id;
    }
    else if (type == 'E')
    {
        GuestAct a;
        a.client = c.id;
        a.act = in.readInt();
        a.stop = in.readInt();
        a.key = in.readInt();
        a.v = {in.readFloat(), in.readFloat(), in.readFloat()};
        a.amount = in.readFloat();
        if (!std::isfinite(a.v.x + a.v.y + a.v.z + a.amount))
            return;
        const juce::ScopedLock sl(figLock);
        if (guestActsIn.size() < 256)
            guestActsIn.push_back(a);
    }
}

void ConcertLink::releaseGuestHand(int clientId)
{
    const int slot = clientId % OrganEngine::kGuestHands;
    if (handOwner[(size_t)slot] != clientId)
        return;
    proc.engine.guestHands[(size_t)slot].stop = -1;
    proc.engine.guestHands[(size_t)slot].key = -1;
    handOwner[(size_t)slot] = 0;
}

void ConcertLink::applyGuestActs()
{
    // the player, message thread: what the listeners' hands did, as far as they may
    std::vector<GuestAct> acts;
    std::vector<Figure> where;
    {
        const juce::ScopedLock sl(figLock);
        acts.swap(guestActsIn);
        where = figs;
    }
    using Act = OrganEngine::Act;
    const int level = guestHands.load();
    auto &en = proc.engine;
    for (auto &a : acts)
    {
        const auto act = (Act)a.act;
        if (a.act == kHoverRequest)
        {
            // (a stop that stands still starts to hover, so its pipes can be taken)
            if (level < 2 || a.stop < 0 || a.stop >= kNumStops)
                continue;
            if (auto *prm = proc.apvts.getParameter(RysumProcessor::motionParamId(a.stop)))
                if (juce::roundToInt(prm->convertFrom0to1(prm->getValue())) == (int)Motion::Static)
                    prm->setValueNotifyingHost(prm->convertTo0to1((float)(int)Motion::Hover));
            continue;
        }
        const bool knock = act == Act::Shock || act == Act::Clap || act == Act::Strike;
        const bool carry = act == Act::Throw || act == Act::Home || act == Act::Whirl || act == Act::BowOn;
        if (act == Act::BowOff)
        {
            en.playerAct(act, a.stop, a.key, a.v, a.amount); // (always: nothing stays sounding)
            continue;
        }
        if (!((knock && level >= 1) || (carry && level >= 2)))
            continue;
        if (act == Act::Shock)
        {
            // from where that listener stands
            Vec3f at{en.homeX[0].load(), en.homeY[0].load(), en.homeZ[0].load()};
            for (auto &f : where)
                if (f.id == a.client)
                    at = {at.x + f.x, at.y + f.y, at.z + f.z};
            en.playerAct(Act::ShockAt, -1, -1, at, a.amount);
            continue;
        }
        en.playerAct(act, a.stop, a.key, a.v, a.amount);
    }
}

void ConcertLink::requestHover(int stop)
{
    const juce::ScopedLock sl(figLock);
    GuestAct a;
    a.act = kHoverRequest;
    a.stop = stop;
    if (actsOut.size() < 256)
        actsOut.push_back(a);
}

std::vector<ConcertLink::Figure> ConcertLink::figures() const
{
    const juce::ScopedLock sl(figLock);
    if (roleNow.load() == Role::off)
        return {};
    return figs;
}

void ConcertLink::setName(const juce::String &n)
{
    const juce::ScopedLock sl(figLock);
    myName = n.trim().substring(0, 32);
    if (myName.isEmpty())
        myName = "Gast";
}

juce::String ConcertLink::getName() const
{
    const juce::ScopedLock sl(figLock);
    return myName;
}

void ConcertLink::receiveFrame(char type, juce::MemoryInputStream &in)
{
    lastHeardMs = juce::Time::getMillisecondCounterHiRes();
    const double t = in.readDouble();
    if (type == 'I')
    {
        myId = in.readInt();
        return;
    }
    if (type == 'F')
    {
        // who stands where (the latest is what counts): everyone but oneself
        const int n = in.readInt();
        if (n < 0 || n > 64)
            return;
        std::vector<Figure> all;
        const double now = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < n; ++i)
        {
            Figure f;
            f.id = in.readInt();
            f.x = in.readFloat();
            f.y = in.readFloat();
            f.z = in.readFloat();
            f.yaw = in.readFloat();
            f.name = in.readString().substring(0, 32);
            f.seenMs = now;
            if (f.id != myId && std::isfinite(f.x + f.y + f.z + f.yaw))
                all.push_back(f);
        }
        if (in.getNumBytesRemaining() >= 4)
            allowedLevel = juce::jlimit(0, 2, in.readInt());
        const juce::ScopedLock sl(figLock);
        figs = std::move(all);
        return;
    }
    if (type == 'A')
    {
        // the player's audio input: into the ring, with the time its end is due here
        const int rsr = in.readInt(), n = (int)(uint16_t)in.readShort();
        if (rsr < 8000 || rsr > 384000 || n < 1 || n > 512 || in.getNumBytesRemaining() < n * 4)
            return;
        std::array<int16_t, 2 * 512> s{};
        in.read(s.data(), n * 4);
        const double at = scheduleAt(t);
        if (!isJoined.load())
            return;
        int s1, n1, s2, n2;
        audioInFifo.prepareToWrite(n, s1, n1, s2, n2);
        if (n1 + n2 < n)
            return; // (full: the listener's audio stalls - this piece is lost)
        constexpr float k = 1.0f / 32767.0f;
        for (int i = 0; i < n; ++i)
        {
            const int at_ = i < n1 ? s1 + i : s2 + i - n1;
            audioInL[(size_t)at_] = s[(size_t)(2 * i)] * k;
            audioInR[(size_t)at_] = s[(size_t)(2 * i + 1)] * k;
        }
        audioSr = rsr;
        audioDueEnd = at + n / (double)rsr;
        audioInFifo.finishedWrite(n);
        return;
    }
    if (type == 'V')
    {
        // the player's voice: into its ring, with the time its end is due here
        const int rsr = in.readInt(), n = (int)(uint16_t)in.readShort();
        if (rsr < 4000 || rsr > 96000 || n < 1 || n > 512 || in.getNumBytesRemaining() < n * 2)
            return;
        std::array<int16_t, 512> s{};
        in.read(s.data(), n * 2);
        const double at = scheduleAt(t);
        if (!isJoined.load())
            return;
        int s1, n1, s2, n2;
        voiceInFifo.prepareToWrite(n, s1, n1, s2, n2);
        if (n1 + n2 < n)
            return;
        for (int i = 0; i < n; ++i)
            voiceIn[(size_t)(i < n1 ? s1 + i : s2 + i - n1)] = s[(size_t)i] * (1.0f / 32767.0f);
        voiceSr = rsr;
        voiceDueEnd = at + n / (double)rsr;
        voiceInFifo.finishedWrite(n);
        voiceHeardMs = juce::Time::getMillisecondCounterHiRes();
        return;
    }
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
        {
            const juce::ScopedLock sl(figLock);
            figs.clear();
        }
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
        return everJoined || listenRoom.isNotEmpty(); // (once there was a concert, or a relay: keep trying)
    }
    if (listenRoom.isNotEmpty())
    {
        // through the relay: it joins this line to one of the player's (it may take a moment)
        juce::String reply;
        if (!sendLine(*sock, "RYSUM-RELAY 1 JOIN " + listenRoom) || !readLine(*sock, reply, 20000) || reply != "OK")
        {
            setStatus(reply.startsWith("ERR noroom")
                          ? juce::String::fromUTF8("Beim Relay spielt niemand in \xe2\x80\x9e") + listenRoom +
                                juce::String::fromUTF8("\xe2\x80\x9c \xe2\x80\x93 warte \xe2\x80\xa6")
                          : juce::String::fromUTF8("Das Relay antwortet nicht \xe2\x80\x93 versuche weiter \xe2\x80\xa6"));
            sock->close();
            wait(2000);
            return true;
        }
    }
    {
        // the player's challenge, answered with the password (it never travels itself)
        char ctype = 0;
        juce::MemoryBlock cpl;
        if (sock->waitUntilReady(true, 5000) != 1 || !readFrame(*sock, ctype, cpl, 512) || ctype != 'N' ||
            cpl.getSize() < 16)
        {
            setStatus(juce::String::fromUTF8("Der Spieler antwortet nicht (eine andere Version?)"));
            sock->close();
            return everJoined || listenRoom.isNotEmpty();
        }
        std::array<uint8_t, 16> nonce{};
        std::memcpy(nonce.data(), cpl.getData(), 16);
        const auto answer = passwordAnswer(nonce, getPassword());
        sendAll(*sock, frame('H',
                             [&](juce::MemoryOutputStream &o)
                             {
                                 o.writeInt(kProtocol);
                                 o.writeInt(numParams);
                                 o.write(answer.data(), answer.size());
                             }));
    }
    lastHeardMs = juce::Time::getMillisecondCounterHiRes();
    myId = -1;
    lastGrabSent = -2;
    allowedLevel = 0;
    ++audioSession; // (what is left of the last connection's input is not played)
    ++voiceSession;
    while (!threadShouldExit())
    {
        {
            // where this listener stands, ten times a second (the others see a figure there)
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (isJoined.load() && nowMs - lastPoseMs > 100.0)
            {
                lastPoseMs = nowMs;
                const auto name = getName();
                if (!sendAll(*sock, frame('W',
                                          [&](juce::MemoryOutputStream &o)
                                          {
                                              o.writeFloat(proc.walkPose[0].load());
                                              o.writeFloat(proc.walkPose[1].load());
                                              o.writeFloat(proc.engine.viewZ.load());
                                              o.writeFloat(proc.walkPose[2].load());
                                              o.writeString(name);
                                          })))
                    break;
            }
        }
        if (isJoined.load())
        {
            // what this listener's hands do: to the player (who decides what may happen)
            std::vector<GuestAct> acts;
            {
                const juce::ScopedLock sl(figLock);
                acts.swap(actsOut);
            }
            bool ok = true;
            for (auto &a : acts)
                ok = ok && sendAll(*sock, frame('E',
                                                [&](juce::MemoryOutputStream &o)
                                                {
                                                    o.writeInt(a.act);
                                                    o.writeInt(a.stop);
                                                    o.writeInt(a.key);
                                                    o.writeFloat(a.v.x);
                                                    o.writeFloat(a.v.y);
                                                    o.writeFloat(a.v.z);
                                                    o.writeFloat(a.amount);
                                                }));
            // the pipe in the hand, 50 times a second while it is carried (and once when let go)
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            auto &en = proc.engine;
            const int gs = en.grabStop.load(), gk = en.grabKey.load();
            const int code = gs < 0 ? -1 : gs * 64 + gk;
            if (ok && (code != lastGrabSent || (gs >= 0 && nowMs - lastHandMs > 20.0)))
            {
                lastGrabSent = code;
                lastHandMs = nowMs;
                ok = sendAll(*sock, frame('Q',
                                          [&](juce::MemoryOutputStream &o)
                                          {
                                              o.writeInt(gs);
                                              o.writeInt(gk);
                                              o.writeFloat(en.grabX.load());
                                              o.writeFloat(en.grabY.load());
                                              o.writeFloat(en.grabZ.load());
                                          }));
            }
            if (!ok)
                break;
        }
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
            juce::MemoryInputStream xin(pl, false);
            const auto why = xin.readString();
            setStatus(why == "password" ? juce::String::fromUTF8("Falsches Passwort \xe2\x80\x93 Beenden, Passwort eingeben, neu verbinden")
                                        : juce::String::fromUTF8("Der Spieler hat eine andere Version des Plugins"));
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

void ConcertLink::captureAudio(const float *l, const float *r, int n, double blockTime, double sr) noexcept
{
    if (roleNow.load() != Role::performer || l == nullptr || n <= 0)
        return;
    if (r == nullptr)
        r = l;
    for (int done = 0; done < n;)
    {
        const int m = juce::jmin(512, n - done);
        int s1, n1, s2, n2;
        audioOutFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 + n2 < 1)
            return; // (the network thread is behind: this piece is lost)
        auto &c = audioOut[(size_t)(n1 > 0 ? s1 : s2)];
        c.t = blockTime + done / sr;
        c.sr = juce::roundToInt(sr);
        c.n = m;
        for (int i = 0; i < m; ++i)
        {
            c.s[(size_t)(2 * i)] = (int16_t)juce::roundToInt(juce::jlimit(-1.0f, 1.0f, l[done + i]) * 32767.0f);
            c.s[(size_t)(2 * i + 1)] = (int16_t)juce::roundToInt(juce::jlimit(-1.0f, 1.0f, r[done + i]) * 32767.0f);
        }
        audioOutFifo.finishedWrite(1);
        done += m;
    }
}

void ConcertLink::captureVoice(const float *l, const float *r, int n, double blockTime, double sr) noexcept
{
    // the player's voice: the input, mono, low-passed (about 6 kHz) and taken down to about 16 kHz
    if (roleNow.load() != Role::performer || !talking.load() || l == nullptr || n <= 0 || sr <= 0.0)
    {
        vCur.n = 0;
        vCnt = 0;
        vAcc = 0.0f;
        return;
    }
    if (r == nullptr)
        r = l;
    const int D = juce::jmax(1, (int)std::lround(sr / 16000.0));
    const float a = std::exp(-juce::MathConstants<float>::twoPi * 6000.0f / (float)sr);
    for (int i = 0; i < n; ++i)
    {
        const float x = 0.5f * (l[i] + r[i]);
        vLp1 = x + a * (vLp1 - x);
        vLp2 = vLp1 + a * (vLp2 - vLp1);
        vAcc += vLp2;
        if (++vCnt < D)
            continue;
        if (vCur.n == 0)
        {
            vCur.t = blockTime + (i + 1 - D) / sr;
            vCur.sr = juce::roundToInt(sr / D);
        }
        vCur.s[(size_t)vCur.n++] = (int16_t)juce::roundToInt(juce::jlimit(-1.0f, 1.0f, vAcc / (float)D) * 32767.0f);
        vAcc = 0.0f;
        vCnt = 0;
        if (vCur.n == (int)vCur.s.size())
        {
            int s1, n1, s2, n2;
            voiceOutFifo.prepareToWrite(1, s1, n1, s2, n2);
            if (n1 + n2 > 0)
            {
                voiceOut[(size_t)(n1 > 0 ? s1 : s2)] = vCur;
                voiceOutFifo.finishedWrite(1);
            }
            vCur.n = 0;
        }
    }
    // (what this block made goes now: a voice should not wait for half a chunk)
    if (vCur.n > 0)
    {
        int s1, n1, s2, n2;
        voiceOutFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 + n2 > 0)
        {
            voiceOut[(size_t)(n1 > 0 ? s1 : s2)] = vCur;
            voiceOutFifo.finishedWrite(1);
        }
        vCur.n = 0;
    }
}

bool ConcertLink::playVoiceInto(float *out, int n, double blockTime, double sr) noexcept
{
    // the listener: the player's voice, each sample at the moment it is due (as the input below)
    if (roleNow.load() != Role::listener || !isJoined.load() || out == nullptr)
    {
        voiceRunning = false;
        return false;
    }
    if (const auto ses = voiceSession.load(); ses != voiceSessionSeen)
    {
        voiceSessionSeen = ses;
        voiceInFifo.finishedRead(voiceInFifo.getNumReady());
        voiceRunning = false;
    }
    const double rsr = (double)voiceSr.load();
    auto pop = [&](float &x)
    {
        int s1, n1, s2, n2;
        voiceInFifo.prepareToRead(1, s1, n1, s2, n2);
        if (n1 + n2 < 1)
            return false;
        x = voiceIn[(size_t)(n1 > 0 ? s1 : s2)];
        voiceInFifo.finishedRead(1);
        return true;
    };
    const int avail = voiceInFifo.getNumReady();
    const double headDue = voiceDueEnd.load() - avail / rsr;
    if (!voiceRunning)
    {
        if (avail < 2 || headDue > blockTime + n / sr)
            return false;
        const int late = (int)std::floor((blockTime - headDue) * rsr);
        if (late > 0)
            voiceInFifo.finishedRead(juce::jmin(late, juce::jmax(0, avail - 2)));
        if (!pop(vA) || !pop(vB))
            return false;
        vStartAt = juce::jlimit(0, n - 1, (int)std::lround((headDue - blockTime) * sr));
        vFrac = 0.0;
        vErr = 0.0;
        voiceRunning = true;
    }
    else
    {
        const double err = headDue - blockTime;
        if (err < -0.12 && avail > 0)
        {
            voiceInFifo.finishedRead(juce::jmin(avail, (int)((-err) * rsr)));
            vErr = 0.0;
        }
        else if (err > 0.12)
        {
            voiceRunning = false;
            return false;
        }
        else
            vErr += (err - vErr) * juce::jmin(1.0, n / sr / 0.5);
    }
    const int first = vStartAt;
    vStartAt = 0;
    for (int i = 0; i < first; ++i)
        out[i] = 0.0f;
    const double rate = rsr / sr * (1.0 - juce::jlimit(-0.003, 0.003, vErr * 0.3));
    float peak = voicePeak.load();
    for (int i = first; i < n; ++i)
    {
        out[i] = vA + (float)vFrac * (vB - vA);
        peak = juce::jmax(peak, std::abs(out[i]));
        vFrac += rate;
        while (vFrac >= 1.0)
        {
            vFrac -= 1.0;
            vA = vB;
            if (!pop(vB))
            {
                for (int j = i + 1; j < n; ++j)
                    out[j] = 0.0f;
                voiceRunning = false;
                voicePeak = peak;
                return true;
            }
        }
    }
    voicePeak = peak;
    return true;
}

bool ConcertLink::playAudioInto(float *l, float *r, int n, double blockTime, double sr) noexcept
{
    // the listener: the player's input, each sample at the moment it is due here (like the notes).
    // Read a little faster or slower as needed (at most 0.3 %, a few cents): different sample rates,
    // sound cards whose clocks drift apart, the delay estimate settling - all without a gap.
    if (roleNow.load() != Role::listener || !isJoined.load())
    {
        audioRunning = false;
        return false;
    }
    if (const auto ses = audioSession.load(); ses != audioSessionSeen)
    {
        audioSessionSeen = ses;
        audioInFifo.finishedRead(audioInFifo.getNumReady());
        audioRunning = false;
    }
    const double rsr = (double)audioSr.load();
    auto pop = [&](float &L, float &R)
    {
        int s1, n1, s2, n2;
        audioInFifo.prepareToRead(1, s1, n1, s2, n2);
        if (n1 + n2 < 1)
            return false;
        const int at = n1 > 0 ? s1 : s2;
        L = audioInL[(size_t)at];
        R = audioInR[(size_t)at];
        audioInFifo.finishedRead(1);
        return true;
    };
    // the sample about to be read: when it is due here
    const int avail = audioInFifo.getNumReady();
    const double headDue = audioDueEnd.load() - avail / rsr;
    if (!audioRunning)
    {
        // wait for the first sample's time (it may also be long past: skip to now)
        if (avail < 2 || headDue > blockTime + n / sr)
            return false;
        const int late = (int)std::floor((blockTime - headDue) * rsr);
        if (late > 0)
            audioInFifo.finishedRead(juce::jmin(late, juce::jmax(0, avail - 2)));
        if (!pop(aL, aR) || !pop(bL, bR))
            return false;
        audioStartAt = juce::jlimit(0, n - 1, (int)std::lround((headDue - blockTime) * sr));
        audioFrac = 0.0;
        audioErr = 0.0;
        audioRunning = true;
    }
    else
    {
        // how far off the reading is (+ = ahead of time), smoothed over about half a second
        const double err = headDue - blockTime;
        if (err < -0.12 && avail > 0)
        {
            // far behind (a stall): jump to now
            audioInFifo.finishedRead(juce::jmin(avail, (int)((-err) * rsr)));
            audioErr = 0.0;
        }
        else if (err > 0.12)
        {
            // far ahead (the player's audio paused): wait for the time to come
            audioRunning = false;
            return false;
        }
        else
            audioErr += (err - audioErr) * juce::jmin(1.0, n / sr / 0.5);
    }
    // (starting: silence until the first sample's moment within this block)
    const int first = audioStartAt;
    audioStartAt = 0;
    for (int i = 0; i < first; ++i)
        l[i] = r[i] = 0.0f;
    const double rate = rsr / sr * (1.0 - juce::jlimit(-0.003, 0.003, audioErr * 0.3));
    for (int i = first; i < n; ++i)
    {
        const float f = (float)audioFrac;
        l[i] = aL + f * (bL - aL);
        r[i] = aR + f * (bR - aR);
        if (loudAt.load() < 0.0 && std::abs(l[i]) > 0.5f)
            loudAt = blockTime + i / sr;
        audioFrac += rate;
        while (audioFrac >= 1.0)
        {
            audioFrac -= 1.0;
            aL = bL;
            aR = bR;
            if (!pop(bL, bR))
            {
                // run dry (the player's input went off, or the network stalls): silence, then start again
                for (int j = i + 1; j < n; ++j)
                    l[j] = r[j] = 0.0f;
                audioRunning = false;
                return true;
            }
        }
    }
    return true;
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
        for (auto *id : {"mpe", "mpeBend", "temperament", "inpOn"})
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
        applyGuestActs();
        scanParameters();
        const int n = numListeners.load();
        if (relayMode.load() && !relayUp.load())
            return; // (the network thread says how the relay goes)
        const auto how = relayMode.load() ? juce::String::fromUTF8("Spielt \xc3\xbc" "ber das Relay ") + relayAddress()
                                          : juce::String::fromUTF8("Spielt");
        setStatus(n == 0 ? how + juce::String::fromUTF8(" \xe2\x80\x93 noch niemand h\xc3\xb6rt zu")
                         : how + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(n) +
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
