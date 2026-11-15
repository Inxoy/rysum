#include "Audience.h"
#include "PluginProcessor.h"
#include <cstring>

namespace rysum
{
// ---------------------------------------------------------------------------
// the page the phones open (everything in it, nothing loaded from elsewhere)
// ---------------------------------------------------------------------------
const char *AudienceServer::page()
{
    return R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Rysum &ndash; deine Pfeife</title>
<style>
html,body{margin:0;height:100%;background:#1b1a18;color:#f3f0e8;font:16px/1.35 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;overflow:hidden}
#pad{position:fixed;inset:0;touch-action:none;user-select:none;-webkit-user-select:none;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;padding:24px;box-sizing:border-box;transition:background .4s}
#dot{width:38vmin;height:38vmin;border-radius:50%;background:#555;box-shadow:0 0 0 0 rgba(255,255,255,.0);transition:transform .08s,box-shadow .3s}
#dot.hit{transform:scale(.9);box-shadow:0 0 0 18px rgba(255,255,255,.25)}
#name{margin-top:22px;font-size:22px;font-weight:600;min-height:1.4em}
#help{margin-top:18px;font-size:14px;opacity:.75;max-width:320px}
#state{position:fixed;left:0;right:0;bottom:14px;font-size:12px;opacity:.6;text-align:center}
#ring{position:fixed;left:50%;top:50%;width:10px;height:10px;margin:-5px 0 0 -5px;border-radius:50%;border:3px solid #fff;opacity:0;pointer-events:none}
#ring.go{animation:go .6s ease-out}
@keyframes go{from{opacity:.8;transform:scale(1)}to{opacity:0;transform:scale(70)}}
</style></head><body>
<div id="pad"><div id="dot"></div><div id="name">verbinde &hellip;</div>
<div id="help">Tippen: deine Pfeife anschlagen<br>Schnell wischen: Druckwelle<br>Halten und ziehen: Wind</div></div>
<div id="ring"></div><div id="state"></div>
<script>
(function(){
var id;try{id=localStorage.getItem('rysumId')}catch(e){}
if(!id||!/^[a-z0-9]{8,16}$/.test(id)){id=(Math.random().toString(36).slice(2,12)+'00000000').slice(0,10);try{localStorage.setItem('rysumId',id)}catch(e){}}
var pad=document.getElementById('pad'),dot=document.getElementById('dot'),nm=document.getElementById('name'),st=document.getElementById('state'),ring=document.getElementById('ring');
function show(j){
 if(j.full){nm.textContent='Alle Plätze sind belegt';dot.style.background='#555';return}
 if(j.c){dot.style.background=j.c;pad.style.background='linear-gradient(#1b1a18 40%,'+j.c+'33)';nm.textContent=j.n}
 else{dot.style.background='#555';pad.style.background='#1b1a18';nm.textContent='wartet auf eine Pfeife …'}
 st.textContent=(j.k||1)+(j.k==1?' Handy':' Handys')+' im Saal';
}
function send(a,extra){
 fetch('/'+(a?'act':'hi')+'?p='+id+(a?'&a='+a:'')+(extra||''),{method:'POST',cache:'no-store'})
  .then(function(r){return r.json()}).then(show).catch(function(){st.textContent='keine Verbindung zur Orgel'});
}
function cl(v){return Math.max(-1,Math.min(1,v))}
var d=null,windT=0;
pad.addEventListener('pointerdown',function(e){try{pad.setPointerCapture(e.pointerId)}catch(x){}d={x:e.clientX,y:e.clientY,t:performance.now(),wind:false}});
pad.addEventListener('pointermove',function(e){
 if(!d)return;var dx=e.clientX-d.x,dy=e.clientY-d.y,el=performance.now()-d.t;
 if(!d.wind&&el>280&&Math.hypot(dx,dy)>14)d.wind=true;
 if(d.wind&&performance.now()-windT>100){windT=performance.now();var s=Math.min(innerWidth,innerHeight)*0.35;
  send('w','&x='+cl(dx/s).toFixed(2)+'&y='+cl(-dy/s).toFixed(2))}
});
function up(e,cancel){
 if(!d)return;var dx=e.clientX-d.x,dy=e.clientY-d.y,dist=Math.hypot(dx,dy),el=performance.now()-d.t;
 if(!cancel&&!d.wind){
  if(dist<24){send('k','&s=1');dot.classList.add('hit');setTimeout(function(){dot.classList.remove('hit')},120);if(navigator.vibrate)navigator.vibrate(15)}
  else if(el<450){var v=dist/Math.max(1,el);send('s','&s='+Math.max(.5,Math.min(1.7,v/1.5)).toFixed(2));
   ring.classList.remove('go');void ring.offsetWidth;ring.classList.add('go');if(navigator.vibrate)navigator.vibrate([30,40,30])}
 }
 d=null;
}
pad.addEventListener('pointerup',function(e){up(e,false)});
pad.addEventListener('pointercancel',function(e){up(e,true)});
send('');setInterval(function(){send('')},2000);
})();
</script></body></html>
)HTML";
}

// ---------------------------------------------------------------------------
// the server
// ---------------------------------------------------------------------------
AudienceServer::AudienceServer() : juce::Thread("Rysum Publikum") {}

AudienceServer::~AudienceServer()
{
    stop();
}

bool AudienceServer::start(int firstPort, bool loopbackOnly)
{
    stop();
    for (int p = firstPort; p < firstPort + 10; ++p)
    {
        auto s = std::make_unique<juce::StreamingSocket>();
        if (s->createListener(p, loopbackOnly ? "127.0.0.1" : juce::String()))
        {
            listener = std::move(s);
            port = p;
            startThread();
            return true;
        }
    }
    return false;
}

void AudienceServer::stop()
{
    signalThreadShouldExit();
    stopThread(2000); // (the loop waits at most 50 ms at a time)
    if (listener != nullptr)
        listener->close();
    listener.reset();
    port = 0;
    const juce::ScopedLock sl(lock);
    events.clear();
}

std::vector<AudienceServer::Event> AudienceServer::takeEvents()
{
    const juce::ScopedLock sl(lock);
    std::vector<Event> out(events.begin(), events.end());
    events.clear();
    return out;
}

void AudienceServer::setShown(int phone, const juce::String &label, int colour)
{
    if (phone < 0 || phone >= kMaxPhones)
        return;
    const juce::ScopedLock sl(lock);
    phones[(size_t)phone].label = label;
    phones[(size_t)phone].colour = colour;
}

std::vector<std::pair<int, double>> AudienceServer::phonesSeen(double withinMs) const
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    std::vector<std::pair<int, double>> out;
    const juce::ScopedLock sl(lock);
    for (int i = 0; i < nextPhone; ++i)
        if (now - phones[(size_t)i].seen < withinMs)
            out.push_back({i, phones[(size_t)i].seen});
    return out;
}

namespace
{
juce::String httpReply(const char *status, const char *type, const juce::String &body)
{
    const auto utf8 = body.toRawUTF8();
    return juce::String("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
           "\r\nContent-Length: " + juce::String((int)std::strlen(utf8)) +
           "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

juce::String jsonText(const juce::String &s)
{
    return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
}
} // namespace

juce::String AudienceServer::answer(const juce::String &request)
{
    const auto first = request.upToFirstOccurrenceOf("\r\n", false, false);
    const auto target = first.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
    const auto path = target.upToFirstOccurrenceOf("?", false, false);
    if (path == "/" || path == "/index.html")
        return httpReply("200 OK", "text/html; charset=utf-8", juce::String::fromUTF8(page()));
    if (path != "/act" && path != "/hi")
        return httpReply("404 Not Found", "text/plain", "nicht hier");

    std::map<juce::String, juce::String> q;
    for (const auto &kv : juce::StringArray::fromTokens(target.fromFirstOccurrenceOf("?", false, false), "&", ""))
        q[kv.upToFirstOccurrenceOf("=", false, false)] = kv.fromFirstOccurrenceOf("=", false, false);
    const auto id = q["p"];
    if (id.length() < 4 || id.length() > 16 || !id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789"))
        return httpReply("400 Bad Request", "text/plain", "kein Handy");

    const juce::ScopedLock sl(lock);
    int phone = -1;
    if (auto it = ids.find(id); it != ids.end())
        phone = it->second;
    else if (nextPhone < kMaxPhones)
    {
        phone = nextPhone++;
        ids[id] = phone;
    }
    if (phone < 0)
        return httpReply("200 OK", "application/json", "{\"full\":1}");
    const double now = juce::Time::getMillisecondCounterHiRes();
    phones[(size_t)phone].seen = now;
    const auto a = q["a"];
    if (path == "/act" && a.length() == 1 && juce::String("ksw").containsChar(a[0]) && events.size() < 256)
    {
        Event e;
        e.phone = phone;
        e.act = (char)a[0];
        e.amount = juce::jlimit(0.1f, 2.0f, q.count("s") ? q["s"].getFloatValue() : 1.0f);
        e.x = juce::jlimit(-1.0f, 1.0f, q["x"].getFloatValue());
        e.y = juce::jlimit(-1.0f, 1.0f, q["y"].getFloatValue());
        events.push_back(e);
        if (e.act == 'k')
            ++served;
    }
    int count = 0;
    for (int i = 0; i < nextPhone; ++i)
        count += now - phones[(size_t)i].seen < 6000.0 ? 1 : 0;
    const auto &ph = phones[(size_t)phone];
    juce::String body = "{\"k\":" + juce::String(count);
    if (ph.colour >= 0)
        body << ",\"c\":" << jsonText("#" + AudienceHands::colour(ph.colour).toDisplayString(false))
             << ",\"n\":" << jsonText(ph.label);
    body << "}";
    return httpReply("200 OK", "application/json", body);
}

void AudienceServer::run()
{
    struct Pending
    {
        std::unique_ptr<juce::StreamingSocket> s;
        juce::MemoryBlock got;
        double since = 0.0;
    };
    std::vector<Pending> pending;
    while (!threadShouldExit() && listener != nullptr)
    {
        // new connections (a phone may open some without sending anything: they wait apart)
        if (listener->waitUntilReady(true, pending.empty() ? 50 : 5) == 1)
            if (auto *c = listener->waitForNextConnection())
            {
                std::unique_ptr<juce::StreamingSocket> s(c);
                if (pending.size() < 64)
                    pending.push_back({std::move(s), {}, juce::Time::getMillisecondCounterHiRes()});
            }
        const double now = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = 0; i < pending.size();)
        {
            auto &p = pending[i];
            bool drop = now - p.since > 4000.0;
            const int ready = p.s->waitUntilReady(true, 0);
            if (ready < 0)
                drop = true;
            else if (ready == 1 && !drop)
            {
                char buf[2048];
                const int n = p.s->read(buf, (int)sizeof(buf), false);
                if (n <= 0)
                    drop = true;
                else
                {
                    p.got.append(buf, (size_t)n);
                    const auto text = juce::String::fromUTF8((const char *)p.got.getData(), (int)p.got.getSize());
                    if (p.got.getSize() > 8192)
                        drop = true;
                    else if (text.contains("\r\n\r\n"))
                    {
                        const auto reply = answer(text);
                        const auto *raw = reply.toRawUTF8();
                        p.s->write(raw, (int)std::strlen(raw));
                        drop = true;
                    }
                }
            }
            if (drop)
            {
                p.s->close();
                pending.erase(pending.begin() + (ptrdiff_t)i);
            }
            else
                ++i;
        }
    }
    for (auto &p : pending)
        p.s->close();
}

// ---------------------------------------------------------------------------
// the hands
// ---------------------------------------------------------------------------
juce::Colour AudienceHands::colour(int index)
{
    static const juce::uint32 c[12]{0xffe6194b, 0xff3cb44b, 0xffffe119, 0xff4363d8, 0xfff58231, 0xff911eb4,
                                    0xff42d4f4, 0xfff032e6, 0xffbfef45, 0xfffabed4, 0xff469990, 0xffdcbeff};
    return juce::Colour(c[(size_t)((index % 12 + 12) % 12)]);
}

AudienceHands::AudienceHands(RysumProcessor &p) : proc(p) {}

AudienceHands::~AudienceHands()
{
    stop();
}

bool AudienceHands::start(bool loopbackOnly)
{
    stop();
    if (!server.start(8787, loopbackOnly))
        return false;
    // the address the phones reach: a private IPv4 address of this computer (192.168 first, then
    // 10, then 172.16-31; never the self-assigned 169.254)
    juce::Array<juce::IPAddress> all;
    juce::IPAddress::findAllAddresses(all, false);
    juce::String best;
    int bestRank = 99;
    others.clear();
    for (const auto &a : all)
    {
        const auto s = a.toString();
        int rank = 99;
        if (s.startsWith("192.168."))
            rank = 0;
        else if (s.startsWith("10."))
            rank = 1;
        else if (s.startsWith("172."))
        {
            const int b = s.fromFirstOccurrenceOf(".", false, false).getIntValue();
            rank = b >= 16 && b <= 31 ? 2 : 99;
        }
        if (rank == 99)
            continue;
        others.add(s);
        if (rank < bestRank)
        {
            bestRank = rank;
            best = s;
        }
    }
    others.removeString(best);
    if (loopbackOnly || best.isEmpty())
        best = "127.0.0.1";
    address = "http://" + best + ":" + juce::String(server.portNumber()) + "/";
    code = QrCode::encode(address.toStdString());
    lastTick = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(30);
    return true;
}

void AudienceHands::stop()
{
    stopTimer();
    server.stop();
    now.clear();
    byPhone = {};
}

void AudienceHands::assign()
{
    // the pipes that can take part: every key of a pulled stop whose pipes are bodies in the Saal
    const auto params = proc.readParams();
    std::array<bool, kNumStops> ok{};
    int stopsOk = 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        ok[(size_t)s] = params.stopOn[(size_t)s] && isRoomMotion(params.motionMode[(size_t)s]) &&
                        proc.engine.bodyOn[(size_t)s].load();
        stopsOk += ok[(size_t)s] ? 1 : 0;
    }
    std::array<std::array<int, kNumKeys>, kNumStops> used{};
    const auto seen = server.phonesSeen();
    for (const auto &[i, t] : seen)
    {
        const auto &h = byPhone[(size_t)i];
        if (h.stop >= 0 && ok[(size_t)h.stop])
            ++used[(size_t)h.stop][(size_t)h.key];
    }
    now.clear();
    juce::Random rnd;
    for (const auto &[i, t] : seen)
    {
        juce::ignoreUnused(t);
        auto &h = byPhone[(size_t)i];
        h.phone = i;
        h.colour = i;
        if (h.stop < 0 || !ok[(size_t)h.stop])
        {
            h.stop = h.key = -1;
            if (stopsOk > 0)
            {
                // the least used pipe, the stops taken in turn, the keys spread over the compass
                int bestUse = 1 << 20;
                for (int tries = 0; tries < 64; ++tries)
                {
                    const int s = rnd.nextInt(kNumStops), k = rnd.nextInt(kNumKeys);
                    if (!ok[(size_t)s] || used[(size_t)s][(size_t)k] >= bestUse)
                        continue;
                    bestUse = used[(size_t)s][(size_t)k];
                    h.stop = s;
                    h.key = k;
                    if (bestUse == 0)
                        break;
                }
                if (h.stop >= 0)
                    ++used[(size_t)h.stop][(size_t)h.key];
            }
        }
        if (h.stop >= 0)
            server.setShown(i,
                            juce::String::fromUTF8(kStops[(size_t)h.stop].label) +
                                juce::String::fromUTF8(" \xc2\xb7 ") + helmholtzName(kPhysKeys[(size_t)h.key]),
                            h.colour);
        else
            server.setShown(i, {}, -1);
        now.push_back(h);
    }
}

void AudienceHands::timerCallback()
{
    const double t = juce::Time::getMillisecondCounterHiRes();
    const float dt = (float)juce::jlimit(0.0, 0.1, (t - lastTick) / 1000.0);
    lastTick = t;
    assign();
    auto &en = proc.engine;
    int knocks = 0;
    Vec3f wind{};
    int winds = 0;
    for (const auto &e : server.takeEvents())
    {
        if (e.phone < 0 || e.phone >= AudienceServer::kMaxPhones)
            continue;
        auto &h = byPhone[(size_t)e.phone];
        if (e.act == 'k')
        {
            // a knock on its own pipe (at most a few per tick, and not faster than drumming)
            Vec3f at;
            if (knocks >= 10 || h.stop < 0 || t - h.lastAct < 90.0 || !en.bodyAt(h.stop, h.key, at))
                continue;
            en.playerAct(OrganEngine::Act::Strike, h.stop, h.key, at, e.amount);
            h.lastAct = t;
            ++knocks;
        }
        else if (e.act == 's')
        {
            // a shockwave from where the player stands (each phone one every 1.5 s, all of them
            // together one every 0.35 s)
            if (t - lastShockOf[(size_t)e.phone] < 1500.0 || t - lastShock < 350.0)
                continue;
            en.playerAct(OrganEngine::Act::Shock, -1, -1, {}, e.amount);
            lastShockOf[(size_t)e.phone] = lastShock = t;
        }
        else if (e.act == 'w' && winds < 4)
        {
            // wind: up on the phone is the way the 3D view looks
            const float ya = juce::degreesToRadians(proc.walkPose[2].load());
            const float fx = -std::sin(ya), fy = std::cos(ya), rx = std::cos(ya), ry = std::sin(ya);
            wind.x += rx * e.x + fx * e.y;
            wind.y += ry * e.x + fy * e.y;
            ++winds;
        }
    }
    if (winds > 0)
    {
        // (a phone sends every 0.1 s while dragging: one push per message, as if it blew 0.1 s)
        juce::ignoreUnused(dt);
        const float accel = 6.0f; // m/s^2 at a full drag
        en.playerAct(OrganEngine::Act::Wind, -1, -1, {wind.x * accel * 0.1f, wind.y * accel * 0.1f, 0.0f});
    }
}
} // namespace rysum
