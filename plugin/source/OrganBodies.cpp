#include "OrganEngine.h"

// ===========================================================================
// Room motion: in the modes Flug, Fall, Schwarm and Strömung a pipe is not a direction around the listener but a
// body with a place and a speed in the Saal. It flies, falls, flocks or drifts between the walls of the floor plan,
// the floor and the ceiling, and where it hits one it knocks: a short contact (shorter and brighter the faster it
// hits), the struck pipe ringing with its own modes and faintly its note, duller on an absorbing wall.
//
// The bodies move in fixed steps (1/480 s, the rest carried over): the same paths and impacts at any block size.
// ===========================================================================
namespace rysum
{
namespace
{
constexpr double kBodyStep = 1.0 / 480.0;
constexpr float kWallMargin = 0.25f;          // a pipe keeps its radius (and a little) from a wall
constexpr float kMaxSpeed = 40.0f;            // m/s (Geschwindigkeit: 1 Hz = 10 m/s)
constexpr float kMinImpact = 0.25f;           // m/s into a surface: below that it touches, it does not knock
constexpr float kMeshMargin = 0.12f;          // a pipe keeps this much from the model's surfaces (sideways)
constexpr float kMeshFloor = 0.05f;           // the model's triangles below this lie on the Saal's floor: the Saal has them

/** a room as the bodies see it (the Saal of the stop: its plan, its height, the usual listening place) */
struct BodyRoom
{
    RoomParams r;
    PlanGeo geo;
    SurfAbs abs;                 // every surface with its own absorption (how dull a knock on it is)
    Vec3f player, playerVel;     // the listener in this room (the pipes may follow or flee it)
    bool box = true, valid = false;
    float W = 14, L = 28, H = 11;
    Vec3f home;
    bool free (float x, float y, float m) const noexcept
    {
        if (box) return x >= m && x <= W - m && y >= m && y <= L - m;
        return planFree (geo, x, y, m);
    }
    /** free() for a body that remembers how far the nearest wall was: the same answer, but the walls are only
        asked when the body may have come near one */
    template <typename Body>
    bool freeOf (Body& b, float m) const noexcept
    {
        if (box) return b.x >= m && b.x <= W - m && b.y >= m && b.y <= L - m;
        if (b.clear > kWallMargin) return true;          // (m is never more than the margin)
        const bool in = insidePlan (geo, b.x, b.y);
        const float d = planEdgeDist (geo, b.x, b.y);
        b.clear = in ? d : 0.0f;
        return in && d >= m;
    }
    void nudge (float& x, float& y) const noexcept
    {
        if (box) { x = juce::jlimit (kWallMargin, W - kWallMargin, x); y = juce::jlimit (kWallMargin, L - kWallMargin, y); }
        else keepInsidePlan (geo, x, y, kWallMargin);
    }
};

inline float len3 (float x, float y, float z) noexcept { return std::sqrt (x * x + y * y + z * z); }
} // namespace

void OrganEngine::queueImpact (const ImpactEvent& e) noexcept
{
    if (numImpacts < kMaxImpactEvents) { impactQueue[(size_t) numImpacts++] = e; return; }
    // full: the weakest one gives way (if the new one is stronger)
    int weakest = 0;
    for (int i = 1; i < numImpacts; ++i) if (impactQueue[(size_t) i].speed < impactQueue[(size_t) weakest].speed) weakest = i;
    if (e.speed > impactQueue[(size_t) weakest].speed) impactQueue[(size_t) weakest] = e;
}

void OrganEngine::stepBodies (const EngineParams& p, double blockSeconds) noexcept
{
    bool any = false;
    for (int s = 0; s < kNumStops; ++s) any = any || isRoomMotion (p.motionMode[(size_t) s]);
    if (! any)
    {
        for (int s = 0; s < kNumStops; ++s) { bodyOn[(size_t) s] = false; bodyMode[(size_t) s] = p.motionMode[(size_t) s]; }
        bodySamples = 0; bodyStepCount = 0;
        playerOk.fill (false);
        return;
    }

    // a loaded model (Saal 1): the pipes bounce off its walls, columns and floors as well
    bodyMeshNow = std::atomic_load (&bodyMeshShared);
    const MeshAcoustics* bm = bodyMeshNow.get();
    const bool newMesh = (bm != nullptr ? bm->serial : 0u) != bodyMeshSerial;
    bodyMeshSerial = bm != nullptr ? bm->serial : 0u;
    auto insideSolid = [bm] (float x, float y, float z) { return bm->insideSolid ({ x, y, z }); };

    // the rooms (only those a moving stop is in): as the parameters say, not as the last block left them
    std::array<BodyRoom, kNumRooms> rooms;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! isRoomMotion (p.motionMode[(size_t) s])) continue;
        const int ri = juce::jlimit (0, kNumRooms - 1, p.stopRoom[(size_t) s]);
        auto& br = rooms[(size_t) ri];
        if (br.valid) continue;
        br.r = p.roomAt (ri);
        finishPlan (br.r, getDrawnPlan (ri));
        br.box = br.r.box() || br.r.plan.n < 3;
        if (! br.box) br.geo = planGeo (br.r);
        br.W = br.r.W(); br.L = br.r.L(); br.H = br.r.H();
        br.home = listenerPos (br.r);
        homeX[(size_t) ri] = br.home.x; homeY[(size_t) ri] = br.home.y; homeZ[(size_t) ri] = br.home.z;
        br.abs = SurfAbs::from (br.r);
        // the player: where the listener stands now (walked there, or jumping), and how fast it moves
        {
            const Vec3f P = erListener[(size_t) ri];
            auto& pp = playerPos[(size_t) ri];
            auto& pv = playerVel[(size_t) ri];
            if (! playerOk[(size_t) ri] || blockSeconds <= 0.0) { pp = P; pv = {}; playerOk[(size_t) ri] = true; }
            const float inv = blockSeconds > 1.0e-6 ? (float) (1.0 / blockSeconds) : 0.0f;
            const Vec3f raw { juce::jlimit (-12.0f, 12.0f, (P.x - pp.x) * inv), juce::jlimit (-12.0f, 12.0f, (P.y - pp.y) * inv), juce::jlimit (-12.0f, 12.0f, (P.z - pp.z) * inv) };
            const float k = 1.0f - std::exp (-(float) blockSeconds / 0.08f);
            pv = { pv.x + k * (raw.x - pv.x), pv.y + k * (raw.y - pv.y), pv.z + k * (raw.z - pv.z) };
            pp = P;
            br.player = P; br.playerVel = pv;
        }
        br.valid = true;
    }

    const double scale = p.sync ? p.bpm / 120.0 : 1.0;           // (with sync: faster at a faster tempo; not locked to the bar)
    // (counted in whole samples: every block size arrives at the same step at the same time)
    bodySamples += (int64_t) std::llround (blockSeconds * sr);
    const int64_t due = (int64_t) std::floor ((double) bodySamples / (kBodyStep * sr) + 1.0e-9);
    int steps = (int) juce::jlimit ((int64_t) 0, (int64_t) 2400, due - (int64_t) bodyStepCount);
    if (due - (int64_t) bodyStepCount > 2400) bodyStepCount = (uint32_t) (due - 2400);     // (a stalled host: never minutes of catching up)
    const float h = (float) kBodyStep;
    const auto& lv = p.live;

    for (int s = 0; s < kNumStops; ++s)
    {
        const int mode = p.motionMode[(size_t) s];
        if (! isRoomMotion (mode)) { bodyOn[(size_t) s] = false; bodyMode[(size_t) s] = mode; continue; }
        const int ri = juce::jlimit (0, kNumRooms - 1, p.stopRoom[(size_t) s]);
        const auto& br = rooms[(size_t) ri];
        auto& bs = bodies[(size_t) s];
        const float speed = juce::jlimit (0.0f, kMaxSpeed, std::abs (p.motionRate[(size_t) s]) * 10.0f * (float) scale);
        const float depth = juce::jlimit (0.0f, 1.0f, p.motionDepth[(size_t) s] / 180.0f);

        // entering the room modes: every pipe starts where it is right now, at the speed asked for
        if (! isRoomMotion (bodyMode[(size_t) s]) || ! bodyOn[(size_t) s].load())
        {
            const int prev = bodyMode[(size_t) s] < 0 ? (int) Motion::Static : bodyMode[(size_t) s];
            for (int k = 0; k < kNumKeys; ++k)
            {
                auto& b = bs[(size_t) k];
                b.rng = XorShift ((uint32_t) (s * 7919 + k * 104729 + 17));
                const Pos base { baseAz[(size_t) s][(size_t) k].load(), baseEl[(size_t) s][(size_t) k].load(), baseDist[(size_t) s][(size_t) k].load() };
                const Pos now = applyMotion (base, (Motion) (isRoomMotion (prev) ? 0 : prev), motionPhase[(size_t) s].load(),
                                             p.motionDepth[(size_t) s], (uint32_t) (s * 101 + k * 7 + 1), shapeFor (s, k, p));
                RoomParams usual = br.r;
                const Vec3f q = roomPoint (usual, now);
                b.x = q.x; b.y = q.y; b.z = q.z;
                br.nudge (b.x, b.y);
                b.clear = 0.0f;
                float dx = b.rng.bi(), dy = b.rng.bi(), dz = 0.4f * b.rng.bi();
                const float l = juce::jmax (1.0e-3f, len3 (dx, dy, dz));
                const float v0 = mode == (int) Motion::Fall ? 0.0f : speed;
                const float v1 = mode == (int) Motion::Hover ? 0.0f : v0;
                b.vx = dx / l * v1; b.vy = dy / l * v1; b.vz = dz / l * v1;
                b.hx = b.x; b.hy = b.y; b.hz = b.z;
                b.ox = b.x; b.oy = b.y; b.oz = b.z;
                b.pushed = 0.0f; b.loose = false;
                b.rest = 0.0f;
                b.meshClear = 0.0f; b.meshGround = false;
                b.meshInside = bm != nullptr && ri == 0 && insideSolid (b.x, b.y, b.z);
            }
        }
        bodyMode[(size_t) s] = mode;
        const MeshAcoustics* mesh = ri == 0 ? bm : nullptr;
        if (mesh != nullptr)
            for (auto& b : bs)
            {
                // (another model: measured anew; one that started inside a solid: is it out yet?)
                if (newMesh) { b.meshClear = 0.0f; b.meshGround = false; b.meshInside = insideSolid (b.x, b.y, b.z); }
                else if (b.meshInside) b.meshInside = insideSolid (b.x, b.y, b.z);
            }

        const uint64_t sounding = soundingMask[(size_t) s].load();
        const bool knocks = lv.impLevel > 0.0f && p.stopOn[(size_t) s];
        // how bouncy the surfaces are in this mode
        const float eWall = mode == (int) Motion::Flight ? 1.0f : (mode == (int) Motion::Fall ? 0.3f + 0.65f * depth : (mode == (int) Motion::Swarm ? 0.8f : (mode == (int) Motion::Hover ? 0.7f : 0.5f)));
        // the player's force on this stop's pipes
        const int pMode = lv.playerMode;
        const float pForce = juce::jlimit (0.0f, 1.0f, lv.playerForce), pRange = juce::jlimit (0.5f, 30.0f, lv.playerRange);
        const bool pOn = pMode > 0 && pForce > 0.0f;
        const float eFloor = mode == (int) Motion::Fall ? 0.3f + 0.65f * depth : eWall;
        // the player's hands: everything held still (freeze), one pipe carried, all of them called home
        const bool frozen = actFreeze.load();
        const int heldKey = grabStop.load() == s ? grabKey.load() : -1;
        const Vec3f hand { grabX.load(), grabY.load(), grabZ.load() };
        const bool goHome = homing > 0.0f;
        const int magnet = magnetMode.load();
        const uint64_t magnetBits = magnetMask[(size_t) s].load();
        const Vec3f mag { magnetX.load(), magnetY.load(), magnetZ.load() };

        // Schwarm: the flock's middle and heading, once per 1/60 s (flocking per stop)
        float cx = 0, cy = 0, cz = 0, ax = 0, ay = 0, az = 0;
        auto flock = [&]
        {
            cx = cy = cz = ax = ay = az = 0.0f;
            for (const auto& b : bs) { cx += b.x; cy += b.y; cz += b.z; ax += b.vx; ay += b.vy; az += b.vz; }
            const float inv = 1.0f / (float) kNumKeys;
            cx *= inv; cy *= inv; cz *= inv; ax *= inv; ay *= inv; az *= inv;
        };
        if (mode == (int) Motion::Swarm) flock();

        // a pipe stands between floor and ceiling with its whole length (a very long one floats in the middle)
        std::array<float, kNumKeys> zLo {}, zHi {}, halfLen {};
        for (int k = 0; k < kNumKeys; ++k)
        {
            float len, rad;
            pipeSizeOf (s, k, len, rad);
            halfLen[(size_t) k] = 0.5f * len;
            zLo[(size_t) k] = juce::jmax (0.15f, 0.5f * len); zHi[(size_t) k] = br.H - juce::jmax (0.15f, 0.5f * len);
            if (zHi[(size_t) k] < zLo[(size_t) k]) zLo[(size_t) k] = zHi[(size_t) k] = 0.5f * br.H;
        }
        // (other walls than the bodies' distances were measured in - the plan modulated, another room: measured anew)
        if (! br.box)
        {
            auto& was = bodyGeoWas[(size_t) s];
            bool same = bodyRoomWas[(size_t) s] == ri && was.n == br.geo.n;
            for (int i = 0; same && i < br.geo.n; ++i)
            {
                const auto& a = was.w[(size_t) i];
                const auto& c = br.geo.w[(size_t) i];
                same = a.ax == c.ax && a.ay == c.ay && a.bx == c.bx && a.by == c.by;
            }
            if (! same)
            {
                was = br.geo; bodyRoomWas[(size_t) s] = ri;
                for (auto& b : bs) b.clear = 0.0f;
            }
        }
        else
            bodyRoomWas[(size_t) s] = -1;
        // (the room may have changed under them - its size or plan modulated: back inside, quietly, no knock)
        for (auto& b : bs) if (! br.freeOf (b, kWallMargin * 0.9f)) { br.nudge (b.x, b.y); b.clear = 0.0f; }
        for (int st = 0; st < steps; ++st)
        {
            const uint32_t stepNo = bodyStepCount + (uint32_t) st;
            if (mode == (int) Motion::Swarm && (stepNo & 7u) == 0u) flock();
            const float t = (float) ((double) stepNo * kBodyStep);
            for (int k = 0; k < kNumKeys; ++k)
            {
                auto& b = bs[(size_t) k];
                const float zMin = zLo[(size_t) k], zMax = zHi[(size_t) k];
                if (b.pushed > 0.0f) b.pushed -= h;
                const bool held = k == heldKey;
                if (frozen && ! held) continue;

                // ---- in the player's hand: it follows the hand (and keeps the hand's speed when let go)
                if (held)
                {
                    float tx = (hand.x - b.x) * 14.0f, ty = (hand.y - b.y) * 14.0f, tz = (hand.z - b.z) * 14.0f;
                    const float tl = len3 (tx, ty, tz);
                    if (tl > 25.0f) { tx *= 25.0f / tl; ty *= 25.0f / tl; tz *= 25.0f / tl; }
                    const float k2 = juce::jmin (1.0f, 30.0f * h);
                    b.vx += (tx - b.vx) * k2; b.vy += (ty - b.vy) * k2; b.vz += (tz - b.vz) * k2;
                    b.hx = b.x; b.hy = b.y; b.hz = b.z;
                    b.pushed = 3.0f; b.loose = false; b.rest = 0.0f;
                }
                // ---- arranged (a formation, a layout, undo): straight to its new place, and there it rests
                else if (b.going > 0.0f)
                {
                    b.going -= h;
                    const float dx = b.tx - b.x, dy = b.ty - b.y, dz = b.tz - b.z;
                    b.vx += (16.0f * dx - 8.0f * b.vx) * h;
                    b.vy += (16.0f * dy - 8.0f * b.vy) * h;
                    b.vz += (16.0f * dz - 8.0f * b.vz) * h;
                    b.hx = b.tx; b.hy = b.ty; b.hz = b.tz;
                    b.rest = 0.0f;
                    if (b.going <= 0.0f || (dx * dx + dy * dy + dz * dz < 0.0009f && len3 (b.vx, b.vy, b.vz) < 0.1f))
                        b.going = 0.0f;
                }
                // ---- called home: straight back to its place in the organ, arriving without overshooting
                else if (goHome)
                {
                    b.vx += (9.0f * (b.ox - b.x) - 6.0f * b.vx) * h;
                    b.vy += (9.0f * (b.oy - b.y) - 6.0f * b.vy) * h;
                    b.vz += (9.0f * (b.oz - b.z) - 6.0f * b.vz) * h;
                }
                // ---- thrown while hovering: it glides on, the air stops it, and there it stays
                else if (b.loose && mode == (int) Motion::Hover)
                {
                    const float damp = 1.0f - 0.9f * h;
                    b.vx *= damp; b.vy *= damp; b.vz *= damp;
                    b.hx = b.x; b.hy = b.y; b.hz = b.z;
                    if (len3 (b.vx, b.vy, b.vz) < 0.12f) b.loose = false;
                }
                else
                // ---- the forces of the mode
                switch ((Motion) mode)
                {
                    case Motion::Flight:
                    {
                        // straight on; the speed slowly returns to the one asked for
                        const float v = len3 (b.vx, b.vy, b.vz);
                        if (v > 1.0e-4f) { const float f = 1.0f + (speed / v - 1.0f) * juce::jmin (1.0f, 2.0f * h); b.vx *= f; b.vy *= f; b.vz *= f; }
                        else if (speed > 0.0f) { b.vx = speed * b.rng.bi(); b.vy = speed * b.rng.bi(); }
                        break;
                    }
                    case Motion::Fall:
                    {
                        b.vz -= 9.81f * h;
                        b.vx *= 1.0f - 0.05f * h; b.vy *= 1.0f - 0.05f * h;             // a little air
                        const bool onFloor = b.z <= zMin + 0.01f || b.meshGround;
                        if (onFloor)
                        {
                            b.vx *= 1.0f - 4.0f * h; b.vy *= 1.0f - 4.0f * h;           // sliding on the floor
                            if (len3 (b.vx, b.vy, b.vz) < 0.35f) b.rest += h; else b.rest = 0.0f;
                            // resting a while: thrown up again (Geschwindigkeit = how hard)
                            if (b.rest > 0.8f + 1.5f * (float) (b.rng.uni()) && speed > 0.0f)
                            {
                                b.vz = juce::jmax (2.0f, speed * (0.6f + 0.4f * b.rng.uni()));
                                b.vx = 0.5f * speed * b.rng.bi(); b.vy = 0.5f * speed * b.rng.bi();
                                b.rest = 0.0f;
                            }
                        }
                        break;
                    }
                    case Motion::Swarm:
                    {
                        // towards the middle, along with the others, away from close neighbours, and a will of its own
                        float fx = 0.6f * (cx - b.x) + 1.5f * (ax - b.vx), fy = 0.6f * (cy - b.y) + 1.5f * (ay - b.vy), fz = 0.6f * (cz - b.z) + 1.5f * (az - b.vz);
                        if ((stepNo & 7u) == (uint32_t) (k & 7))
                            for (int j = 0; j < kNumKeys; ++j)
                            {
                                if (j == k) continue;
                                const auto& o = bs[(size_t) j];
                                const float dx = b.x - o.x, dy = b.y - o.y, dz = b.z - o.z, d2 = dx * dx + dy * dy + dz * dz;
                                if (d2 < 1.0f && d2 > 1.0e-6f) { const float w = 8.0f * (1.0f / d2 - 1.0f) * 0.02f; fx += dx * w; fy += dy * w; fz += dz * w; }
                            }
                        fx += 6.0f * depth * b.rng.bi(); fy += 6.0f * depth * b.rng.bi(); fz += 2.0f * depth * b.rng.bi();
                        b.vx += fx * h; b.vy += fy * h; b.vz += fz * h;
                        const float v = len3 (b.vx, b.vy, b.vz);
                        if (v > 1.0e-4f) { const float f = 1.0f + (speed / v - 1.0f) * juce::jmin (1.0f, 3.0f * h); b.vx *= f; b.vy *= f; b.vz *= f; }
                        break;
                    }
                    case Motion::Hover:
                    {
                        // at rest: the air slows it, and it is drawn back to where it started (Tiefe: how free it is -
                        // at full depth nothing calls it back); Geschwindigkeit: a slight unrest of its own
                        const float freeAmt = depth, back = 2.5f * (1.0f - freeAmt) * (1.0f - freeAmt);
                        b.vx += (back * (b.hx - b.x) + 0.25f * speed * b.rng.bi()) * h;
                        b.vy += (back * (b.hy - b.y) + 0.25f * speed * b.rng.bi()) * h;
                        b.vz += (back * (b.hz - b.z) + 0.12f * speed * b.rng.bi()) * h;
                        const float damp = 1.0f - (0.35f + 1.6f * (1.0f - freeAmt)) * h;
                        b.vx *= damp; b.vy *= damp; b.vz *= damp;
                        break;
                    }
                    case Motion::Flow:
                    default:
                    {
                        // a slow current through the room (smooth in space and time) carries the pipe; Tiefe = turbulence
                        const float sx = b.x / juce::jmax (1.0f, br.W), sy = b.y / juce::jmax (1.0f, br.L), sz = b.z / juce::jmax (1.0f, br.H);
                        const float ux = std::sin (6.28f * sy + 0.31f * t) + 0.5f * std::sin (9.1f * sz - 0.23f * t + 1.7f);
                        const float uy = std::sin (6.28f * sx - 0.27f * t + 0.6f) + 0.5f * std::cos (7.3f * sz + 0.19f * t);
                        const float uz = 0.35f * std::sin (5.0f * sx + 4.0f * sy + 0.21f * t);
                        const float k2 = juce::jmin (1.0f, 1.5f * h);
                        b.vx += (speed * ux - b.vx) * k2 + 3.0f * depth * speed * b.rng.bi() * h;
                        b.vy += (speed * uy - b.vy) * k2 + 3.0f * depth * speed * b.rng.bi() * h;
                        b.vz += (speed * uz - b.vz) * k2 + 1.5f * depth * speed * b.rng.bi() * h;
                        break;
                    }
                }

                // ---- the player: a mass the pipes fall towards, or flee; a centre they circle; a wake that takes them along
                if (pOn && ! held && ! goHome && b.going <= 0.0f)
                {
                    const float rx = br.player.x - b.x, ry = br.player.y - b.y, rz = br.player.z - b.z;
                    const float d2 = rx * rx + ry * ry + rz * rz, d = std::sqrt (d2) + 1.0e-4f;
                    // (like gravity, softened near the player: strong within the range, never endless)
                    const float acc = 30.0f * pForce * pRange * pRange / (d2 + 0.3f * pRange * pRange);
                    const float ux = rx / d, uy = ry / d, uz = rz / d;
                    switch (pMode)
                    {
                        case 1:  b.vx += acc * ux * h; b.vy += acc * uy * h; b.vz += acc * uz * h; break;
                        case 2:  b.vx -= acc * ux * h; b.vy -= acc * uy * h; b.vz -= acc * uz * h; break;
                        case 3:
                        {
                            // pulled in, and steered onto the circle whose speed holds it there (around the upright axis)
                            b.vx += acc * ux * h; b.vy += acc * uy * h; b.vz += 0.5f * acc * uz * h;
                            const float dh = std::sqrt (rx * rx + ry * ry) + 1.0e-4f;
                            const float tx = -ry / dh, ty = rx / dh, vc = std::sqrt (acc * dh);
                            const float k2 = juce::jmin (1.0f, 1.5f * h);
                            b.vx += (br.playerVel.x + vc * tx - b.vx) * k2; b.vy += (br.playerVel.y + vc * ty - b.vy) * k2;
                            break;
                        }
                        default:
                        {
                            // the wake: near the player a pipe takes on the player's speed
                            const float w = juce::jmin (1.0f, 6.0f * pForce * pRange * pRange / (d2 + pRange * pRange) * h);
                            b.vx += (br.playerVel.x - b.vx) * w; b.vy += (br.playerVel.y - b.vy) * w; b.vz += (br.playerVel.z - b.vz) * w;
                            break;
                        }
                    }
                    // (never through the player: pushed out of an arm's length)
                    if (d < 0.45f && pMode != 2) { b.vx -= 20.0f * ux * h; b.vy -= 20.0f * uy * h; }
                }

                // ---- the magnet (the right mouse button held): drawn into a loose cloud around the point, or driven off
                if (magnet != 0 && ! held && ! goHome && b.going <= 0.0f && ((magnetBits >> k) & 1u) != 0u)
                {
                    const float rx = mag.x - b.x, ry = mag.y - b.y, rz = mag.z - b.z;
                    const float d = std::sqrt (rx * rx + ry * ry + rz * rz) + 1.0e-4f;
                    bool moved = false;
                    if (magnet > 0 && d < 14.0f)
                    {
                        // (each pipe at a distance of its own: a cloud, not one point)
                        const float r0 = 0.45f + 0.9f * (float) ((k * 37 + s * 11) % 17) / 16.0f;
                        const float pull = 5.0f * juce::jlimit (-3.0f, 6.0f, d - r0);
                        b.vx += (pull * rx / d - 2.5f * b.vx) * h;
                        b.vy += (pull * ry / d - 2.5f * b.vy) * h;
                        b.vz += (pull * rz / d - 2.5f * b.vz) * h;
                        moved = true;
                    }
                    else if (magnet < 0 && d < 10.0f)
                    {
                        const float push = 14.0f * (1.0f - d / 10.0f);
                        b.vx -= push * rx / d * h; b.vy -= push * ry / d * h; b.vz -= push * rz / d * h;
                        moved = true;
                    }
                    if (moved)
                    {
                        // (it stays where the magnet leaves it)
                        b.hx = b.x; b.hy = b.y; b.hz = b.z;
                        b.loose = false; b.rest = 0.0f;
                        b.pushed = juce::jmax (b.pushed, 0.5f);
                    }
                }

                // ---- move, and meet the room
                const float vMax = kMaxSpeed * 1.5f;
                const float v = len3 (b.vx, b.vy, b.vz);
                if (v > vMax) { b.vx *= vMax / v; b.vy *= vMax / v; b.vz *= vMax / v; }
                b.z = juce::jlimit (zMin, zMax, b.z);
                const float px = b.x, py = b.y, pz = b.z;
                b.x += b.vx * h; b.y += b.vy * h; b.z += b.vz * h;
                // (the way gone, rounded up - and a little more than any rounding of the place itself)
                b.clear -= std::abs (b.vx * h) + std::abs (b.vy * h) + 4.0e-5f;

                float hitSpeed = 0.0f; bool floorHit = false; float hitAbs = br.r.absWalls;
                float hitZ = -1.0f;          // (a knock on the model: where, if not on the Saal's floor or ceiling)
                // floor and ceiling
                if (b.z < zMin)
                {
                    if (-b.vz > hitSpeed) { hitSpeed = -b.vz; floorHit = true; hitAbs = br.abs.low[kSurfFloor]; }
                    b.z = zMin + (zMin - b.z) * eFloor;
                    b.vz = -b.vz * eFloor;
                    if (std::abs (b.vz) < 0.05f) b.vz = 0.0f;
                }
                else if (b.z > zMax)
                {
                    if (b.vz > hitSpeed) { hitSpeed = b.vz; floorHit = true; hitAbs = br.abs.low[kSurfCeiling]; }
                    b.z = zMax - (b.z - zMax) * eWall;
                    b.vz = -b.vz * eWall;
                }
                // the walls
                if (! br.freeOf (b, kWallMargin))
                {
                    b.clear = 0.0f;          // (at a wall: it is moved below)
                    float nx = 0.0f, ny = 0.0f, inside = 0.0f;
                    int wallSurf = kSurfWall0;
                    if (br.box)
                    {
                        if (b.x < kWallMargin)               { nx = 1.0f;  inside = b.x - kWallMargin; }
                        else if (b.x > br.W - kWallMargin)   { nx = -1.0f; inside = br.W - kWallMargin - b.x; }
                        if (b.y < kWallMargin)               { ny = 1.0f;  inside = juce::jmin (inside, b.y - kWallMargin); }
                        else if (b.y > br.L - kWallMargin)   { ny = -1.0f; inside = juce::jmin (inside, br.L - kWallMargin - b.y); }
                        const float nl = std::sqrt (nx * nx + ny * ny);
                        if (nl > 0.0f) { nx /= nl; ny /= nl; }
                        wallSurf = kSurfWall0 + (std::abs (nx) >= std::abs (ny) ? (nx > 0.0f ? 3 : 1) : (ny > 0.0f ? 0 : 2));
                    }
                    else
                    {
                        int wi = 0;
                        planEdgeDist (br.geo, b.x, b.y, &wi);
                        const auto& w = br.geo.w[(size_t) wi];
                        nx = w.nx; ny = w.ny;
                        inside = w.dist (b.x, b.y) - kWallMargin;
                        wallSurf = kSurfWall0 + juce::jlimit (0, kMaxPlanVerts - 1, wi);
                    }
                    const float vn = b.vx * nx + b.vy * ny;
                    if (vn < 0.0f)
                    {
                        if (-vn > hitSpeed) { hitSpeed = -vn; floorHit = false; hitAbs = br.abs.low[(size_t) wallSurf]; }
                        b.vx -= (1.0f + eWall) * vn * nx; b.vy -= (1.0f + eWall) * vn * ny;
                        if (mode == (int) Motion::Flight && depth > 0.0f)
                        {
                            // a rough wall throws it off at an angle (Tiefe)
                            const float a = depth * 1.1f * b.rng.bi(), ca = std::cos (a), sa = std::sin (a);
                            float rx = b.vx * ca - b.vy * sa, ry = b.vx * sa + b.vy * ca;
                            if (rx * nx + ry * ny <= 0.05f) { rx = b.vx; ry = b.vy; }      // never into the wall
                            b.vx = rx; b.vy = ry;
                        }
                    }
                    b.x -= nx * juce::jmin (0.0f, inside); b.y -= ny * juce::jmin (0.0f, inside);
                    if (! br.free (b.x, b.y, kWallMargin * 0.9f)) { b.x = px; b.y = py; }
                    if (! br.free (b.x, b.y, kWallMargin * 0.9f)) br.nudge (b.x, b.y);
                }

                // the model: its walls, columns, steps and floors. Only asked when the pipe may have come near
                // them (the box around it, widened, holds no triangle: it may go that far without asking);
                // near them, the step's way is followed from points along the pipe's axis, and the first
                // surface met throws it back (a level one from above: it lands, and may come to rest on it)
                b.meshGround = false;
                if (mesh != nullptr && ! goHome && ! b.meshInside)
                {
                    const float dx = b.x - px, dy = b.y - py, dz = b.z - pz;
                    const float dl = len3 (dx, dy, dz);
                    b.meshClear -= dl + 4.0e-5f;
                    const float half = halfLen[(size_t) k];
                    if (b.meshClear <= 0.0f)
                    {
                        auto near = [&] (float r)
                        {
                            const float m = kMeshMargin + r;
                            return mesh->anyIn ({ b.x - m, b.y - m, b.z - half - 0.03f - r }, { b.x + m, b.y + m, b.z + half + 0.03f + r }, kMeshFloor);
                        };
                        b.meshClear = ! near (1.2f) ? 1.2f : (! near (0.3f) ? 0.3f : 0.0f);
                    }
                    if (b.meshClear <= 0.0f && dl > 1.0e-7f)
                    {
                        // the margin along the way: a pipe's width sideways, a hair up and down
                        const float side = std::sqrt (dx * dx + dy * dy) / dl;
                        const float margin = 0.03f + (kMeshMargin - 0.03f) * side;
                        const float tMax = 1.0f + margin / dl;
                        const int pts = juce::jlimit (2, 6, (int) std::ceil (2.0f * half / 0.5f) + 1);
                        int hit = -1;
                        float best = tMax;
                        for (int i = 0; i < pts; ++i)
                        {
                            const float oz = (-half + 0.01f) + (2.0f * half - 0.02f) * (float) i / (float) (pts - 1);
                            float t;
                            const int tri = mesh->castAbove ({ px, py, pz + oz }, { dx, dy, dz }, best, t, kMeshFloor);
                            if (tri >= 0 && t < best) { best = t; hit = tri; }
                        }
                        if (hit >= 0)
                        {
                            Vec3f n = mesh->tris[(size_t) hit].n;
                            if (n.x * dx + n.y * dy + n.z * dz > 0.0f) n = { -n.x, -n.y, -n.z };
                            const bool level = std::abs (n.z) > 0.8f, below = level && n.z > 0.0f;
                            const float e = below ? eFloor : eWall;
                            const float vn = b.vx * n.x + b.vy * n.y + b.vz * n.z;
                            if (vn < 0.0f)
                            {
                                if (-vn > hitSpeed)
                                {
                                    hitSpeed = -vn; floorHit = level;
                                    hitAbs = level ? br.abs.low[below ? kSurfFloor : kSurfCeiling] : br.r.absWalls;
                                    hitZ = below ? b.z - half : (level ? b.z + half : b.z);
                                }
                                b.vx -= (1.0f + e) * vn * n.x; b.vy -= (1.0f + e) * vn * n.y; b.vz -= (1.0f + e) * vn * n.z;
                            }
                            // (back to where the step began: never into the model - on a floor or under a ceiling
                            //  only in height, so it slides on along it)
                            if (! level) { b.x = px; b.y = py; }
                            b.z = pz;
                            if (below)
                            {
                                b.meshGround = true;
                                if (std::abs (b.vz) < 0.3f) b.vz = 0.0f;
                            }
                        }
                    }
                }

                // (a pipe the player has just pushed, thrown or carried knocks whatever "Aufprall" is set to)
                const bool own = b.pushed > 0.0f && p.stopOn[(size_t) s] && ! goHome;
                if (hitSpeed > kMinImpact && ! muted[(size_t) s][(size_t) k].load()
                    && (own || (knocks && (lv.impAll || ((sounding >> k) & 1)))))
                {
                    ImpactEvent e;
                    if (own) e.level = juce::jmax (lv.impLevel, 0.7f);
                    e.x = b.x; e.y = b.y; e.z = hitZ >= 0.0f ? hitZ : (floorHit ? (b.z < 0.5f * br.H ? 0.0f : br.H) : b.z);
                    e.speed = hitSpeed; e.absorb = hitAbs; e.high = br.r.highDamp; e.stop = s; e.key = k; e.room = ri; e.floor = floorHit;
                    queueImpact (e);
                }
                if (hitSpeed > kMinImpact && p.stopOn[(size_t) s] && ! goHome && ! muted[(size_t) s][(size_t) k].load())
                {
                    // Selbstspiel: the knock becomes the pipe's own note
                    if (lv.selfPlay > 0.0001f && hitSpeed > 0.5f && numSelf < kMaxSelf && selfWait[(size_t) s][(size_t) k] <= 0.0f)
                        selfQueue[(size_t) numSelf++] = { s, k, hitSpeed };
                    // Wachsen & Brechen: hit hard enough it breaks in two - half as long, an octave higher
                    if (lv.grow > 0.0001f && hitSpeed > 7.0f - 5.0f * juce::jmin (1.0f, lv.grow))
                    {
                        const float len = pipeLen[(size_t) s][(size_t) k].load();
                        if (len > 0.26f)
                        {
                            pipeLen[(size_t) s][(size_t) k] = juce::jmax (0.25f, 0.5f * len);
                            lenOff = true;
                            breakCount = breakCount.load() + 1;
                            ImpactEvent e;      // the crack (heard whatever "Aufprall" is set to)
                            e.level = juce::jmax (lv.impLevel, 0.8f);
                            e.x = b.x; e.y = b.y; e.z = hitZ >= 0.0f ? hitZ : (floorHit ? (b.z < 0.5f * br.H ? 0.0f : br.H) : b.z);
                            e.speed = 1.6f * hitSpeed; e.absorb = 0.0f; e.high = br.r.highDamp; e.stop = s; e.key = k; e.room = ri; e.floor = floorHit;
                            queueImpact (e);
                        }
                    }
                }
            }
        }
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto& b = bs[(size_t) k];
            bodyX[(size_t) s][(size_t) k] = b.x; bodyY[(size_t) s][(size_t) k] = b.y; bodyZ[(size_t) s][(size_t) k] = b.z;
        }
        bodyOn[(size_t) s] = true;
    }
    bodyStepCount += (uint32_t) steps;
    if (homing > 0.0f)
    {
        homing -= (float) blockSeconds;
        // (arrived: at rest there, as if it had never left)
        if (homing <= 0.0f)
            for (auto& bs : bodies) for (auto& b : bs) { b.hx = b.ox; b.hy = b.oy; b.hz = b.oz; b.loose = false; b.pushed = 0.0f; }
    }
}

// ---------------------------------------------------------------------------
// the player's actions (the keys of the 3D view)
// ---------------------------------------------------------------------------
void OrganEngine::playerAct (Act a, int stop, int key, Vec3f v, float amount) noexcept
{
    const auto scope = actFifo.write (1);
    ActCmd c; c.act = a; c.stop = stop; c.key = key; c.v = v; c.amount = amount;
    if (scope.blockSize1 > 0) actQueue[(size_t) scope.startIndex1] = c;
    else if (scope.blockSize2 > 0) actQueue[(size_t) scope.startIndex2] = c;
}

void OrganEngine::handleActs (const EngineParams& p) noexcept
{
    const int ready = actFifo.getNumReady();
    if (ready <= 0) return;
    const auto scope = actFifo.read (ready);
    auto startBurst = [this] (int type, float gain, float seconds)
    {
        Burst* use = &bursts[0];
        for (auto& b : bursts) { if (! b.on) { use = &b; break; } if (b.pos > use->pos) use = &b; }
        *use = Burst {};
        use->on = true; use->type = type; use->gain = gain; use->len = juce::jmax (1, (int) (seconds * sr));
    };
    auto valid = [] (int s, int k) { return s >= 0 && s < kNumStops && k >= 0 && k < kNumKeys; };
    auto isBody = [this] (int s) { return bodyOn[(size_t) s].load() && isRoomMotion (bodyMode[(size_t) s]); };
    for (int i = 0; i < scope.blockSize1 + scope.blockSize2; ++i)
    {
        const ActCmd c = actQueue[(size_t) (i < scope.blockSize1 ? scope.startIndex1 + i : scope.startIndex2 + i - scope.blockSize1)];
        switch (c.act)
        {
            case Act::Clap:
                startBurst (0, 0.16f * juce::jlimit (0.1f, 2.0f, c.amount), 0.3f);
                break;
            case Act::Shock:
            {
                // away from the player, strongest close by, fading over sixteen metres; a little upwards
                const float amt = juce::jlimit (0.1f, 3.0f, c.amount);
                for (int s = 0; s < kNumStops; ++s)
                {
                    if (! isBody (s)) continue;
                    const int ri = juce::jlimit (0, kNumRooms - 1, p.stopRoom[(size_t) s]);
                    const Vec3f P = erListener[(size_t) ri];
                    for (auto& b : bodies[(size_t) s])
                    {
                        float dx = b.x - P.x, dy = b.y - P.y, dz = b.z - P.z;
                        const float d = len3 (dx, dy, dz);
                        if (d > 16.0f) continue;
                        if (d < 1.0e-3f) { dx = b.rng.bi(); dy = b.rng.bi(); dz = 0.0f; }
                        const float l = juce::jmax (1.0e-3f, len3 (dx, dy, dz));
                        const float dv = 12.0f * amt / (1.0f + (d / 6.0f) * (d / 6.0f)) * (1.0f - d / 16.0f);
                        b.vx += dv * dx / l; b.vy += dv * dy / l; b.vz += dv * (dz / l + 0.3f);
                        b.pushed = 5.0f; b.rest = 0.0f; b.going = 0.0f;
                    }
                }
                startBurst (1, 0.22f * juce::jmin (1.5f, amt), 0.7f);
                break;
            }
            case Act::Wind:
                for (int s = 0; s < kNumStops; ++s)
                {
                    if (! isBody (s)) continue;
                    for (auto& b : bodies[(size_t) s])
                    {
                        b.vx += c.v.x; b.vy += c.v.y; b.vz += c.v.z;
                        b.pushed = juce::jmax (b.pushed, 1.0f); b.rest = 0.0f;
                    }
                }
                break;
            case Act::Home:
                homing = 3.0f;
                for (auto& bs : bodies) for (auto& b : bs) { b.loose = false; b.pushed = 0.0f; b.going = 0.0f; }
                break;
            case Act::Throw:
                if (valid (c.stop, c.key) && isBody (c.stop))
                {
                    auto& b = bodies[(size_t) c.stop][(size_t) c.key];
                    b.vx += c.v.x; b.vy += c.v.y; b.vz += c.v.z;
                    b.loose = true; b.pushed = 6.0f; b.rest = 0.0f; b.going = 0.0f;
                }
                break;
            case Act::Arrange:
                // (a stop that is not a body yet keeps its marks: the view sends Arrange again once it is)
                for (int s = 0; s < kNumStops; ++s)
                {
                    if (! isBody (s)) continue;
                    const uint64_t m = goMask[(size_t) s].exchange (0);
                    for (int k = 0; k < kNumKeys; ++k)
                        if (((m >> k) & 1u) != 0u)
                        {
                            auto& b = bodies[(size_t) s][(size_t) k];
                            b.tx = goX[(size_t) s][(size_t) k].load();
                            b.ty = goY[(size_t) s][(size_t) k].load();
                            b.tz = goZ[(size_t) s][(size_t) k].load();
                            b.going = 4.0f; b.loose = false; b.pushed = 0.0f; b.rest = 0.0f;
                        }
                }
                homing = 0.0f;
                break;
            case Act::Strike:
                if (valid (c.stop, c.key))
                {
                    const int ri = juce::jlimit (0, kNumRooms - 1, p.stopRoom[(size_t) c.stop]);
                    ImpactEvent e;
                    e.x = c.v.x; e.y = c.v.y; e.z = c.v.z;
                    e.speed = 2.0f + 3.0f * juce::jlimit (0.0f, 2.0f, c.amount);
                    e.stop = c.stop; e.key = c.key; e.room = ri;
                    e.level = juce::jmax (1.0f, p.live.impLevel); e.struck = true;
                    queueImpact (e);
                    if (isBody (c.stop))
                    {
                        // (and it gives way a little, away from the hand)
                        auto& b = bodies[(size_t) c.stop][(size_t) c.key];
                        const Vec3f P = erListener[(size_t) ri];
                        const float dx = b.x - P.x, dy = b.y - P.y, l = juce::jmax (1.0e-3f, std::sqrt (dx * dx + dy * dy));
                        b.vx += 1.2f * dx / l; b.vy += 1.2f * dy / l;
                    }
                }
                break;
        }
    }
}

void OrganEngine::renderBursts (float* const* outs, int n) noexcept
{
    const float fsr = (float) sr;
    float* y = scratch.data();
    for (auto& b : bursts)
    {
        if (! b.on) continue;
        if (b.type == 0)
        {
            // a clap: the palms meet three times within a few milliseconds, then the hollow of the hands rings
            // out; noise through a band around 1.2 kHz
            const float f = 2.0f * std::sin (juce::MathConstants<float>::pi * 1200.0f / fsr), q = 0.75f;
            const float at[4] { 0.0f, 0.009f, 0.018f, 0.027f };
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (b.pos + i) / fsr;
                float env = 0.0f;
                for (int j = 0; j < 4; ++j)
                    if (t >= at[j]) env = juce::jmax (env, (j == 3 ? 1.0f : 0.8f) * std::exp (-(t - at[j]) / (j == 3 ? 0.03f : 0.004f)));
                const float x = impactRng.bi() * env;
                const float high = x - b.low - q * b.band;
                b.band += f * high; b.low += f * b.band;
                y[i] = b.gain * (2.2f * b.band + 0.35f * high);
            }
        }
        else
        {
            // the shockwave: a deep thump falling in pitch, and the air it moves
            const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 260.0f / fsr);
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (b.pos + i) / fsr;
                const float hz = 42.0f + 70.0f * std::exp (-t / 0.05f);
                b.phase += (double) hz / (double) fsr;
                b.lp += a * (impactRng.bi() - b.lp);
                const float on = juce::jmin (1.0f, t / 0.004f);
                y[i] = b.gain * on * (1.6f * std::sin ((float) (juce::MathConstants<double>::twoPi * b.phase)) * std::exp (-t / 0.16f)
                                      + 2.5f * b.lp * std::exp (-t / 0.09f));
            }
        }
        b.pos += n;
        if (b.pos >= b.len) b.on = false;
        // (your own hands: from everywhere and nowhere - and into the room, which answers)
        juce::FloatVectorOperations::add (outs[0], y, n);
        juce::FloatVectorOperations::addWithMultiply (roomSend[0].data() + roomOffset, y, b.type == 0 ? 2.5f : 0.8f, n);
    }
}

// ---------------------------------------------------------------------------
// Selbstspiel: the room plays itself; Wachsen & Brechen: the pipes' lengths
// ---------------------------------------------------------------------------
void OrganEngine::startSelfVoices (const EngineParams& p, double blockSeconds) noexcept
{
    if (selfWaiting)
    {
        bool any = false;
        for (auto& st : selfWait) for (auto& w : st) if (w > 0.0f) { w -= (float) blockSeconds; any = any || w > 0.0f; }
        selfWaiting = any;
    }
    if (numSelf == 0) return;
    // (never at the cost of what is played: only free voices, and not more than a few dozen of these at once)
    int freeVoices = 0, own = 0;
    for (const auto& v : voices) { if (! v.active) ++freeVoices; else if (v.previewRemain > 0) ++own; }
    for (int i = 0; i < numSelf; ++i)
    {
        const auto& e = selfQueue[(size_t) i];
        if (freeVoices < 8 || own >= 40) break;
        if (! p.stopOn[(size_t) e.stop] || ((soundingMask[(size_t) e.stop].load() >> e.key) & 1)) continue;
        // a harder knock, a longer note
        const float dur = juce::jmin (5.0f, 0.12f + p.live.selfPlay * (0.25f + 0.4f * e.speed));
        startVoice (e.stop, e.key, juce::jmax (64, (int) (dur * (float) sr)));
        selfWait[(size_t) e.stop][(size_t) e.key] = dur + 0.2f;
        selfWaiting = true;
        --freeVoices; ++own;
    }
    numSelf = 0;
}

void OrganEngine::stepGrowth (const EngineParams& p, double blockSeconds) noexcept
{
    if (pipeLenReset.exchange (false))
    {
        for (auto& st : pipeLen) for (auto& l : st) l = 1.0f;
        lenOff = false;
        breakCount = 0;
    }
    const float g = juce::jmin (1.0f, p.live.grow);
    if (g <= 0.0001f)
    {
        if (! lenOff) return;
        // switched off: every pipe finds back to its own length within a few seconds
        const float k = 1.0f - (float) std::exp (-blockSeconds / 1.5);
        bool any = false;
        for (auto& st : pipeLen)
            for (auto& l : st)
            {
                float v = l.load();
                if (v == 1.0f) continue;
                v += k * (1.0f - v);
                if (std::abs (v - 1.0f) < 0.002f) v = 1.0f; else any = true;
                l = v;
            }
        lenOff = any;
        return;
    }
    // a pipe that is not sounding grows: an octave down (twice the length) within half a minute at full strength,
    // a quarter of an hour at a tenth
    const float octaveSeconds = 600.0f / (0.5f + 20.0f * g * g);
    const float f = (float) std::exp2 (blockSeconds / (double) octaveSeconds);
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! p.stopOn[(size_t) s]) continue;
        const uint64_t sounding = soundingMask[(size_t) s].load();
        for (int k = 0; k < kNumKeys; ++k)
        {
            if ((sounding >> k) & 1) continue;
            auto& l = pipeLen[(size_t) s][(size_t) k];
            const float v = l.load();
            if (v < 2.0f) { l = juce::jmin (2.0f, v * f); lenOff = true; }
        }
    }
}

// ---------------------------------------------------------------------------
// the knock
// ---------------------------------------------------------------------------
void OrganEngine::startImpact (const ImpactEvent& e, const EngineParams& p) noexcept
{
    const auto& lv = p.live;
    const float fsr = (float) sr;
    const float a = juce::jlimit (0.0f, 1.0f, e.absorb);
    // how hard: louder with speed (somewhat faster than proportional), a soft wall takes much of it
    const float hard = juce::jlimit (0.0f, 2.5f, std::pow (e.speed / 3.0f, 1.25f));
    // (calibrated: a 3 m/s knock on stone at 0 dB peaks a little below a pipe sounding at the same distance)
    const float gain = 0.045f * (e.level >= 0.0f ? e.level : lv.impLevel) * hard * (1.0f - 0.65f * a);
    if (gain < 1.0e-5f) return;

    // a free voice, else the quietest one (if this one is louder)
    int vi = -1;
    float quietest = 1.0e9f; int qi = 0;
    for (int i = 0; i < kNumImpactVoices; ++i)
    {
        const auto& v = impactVoices[(size_t) i];
        if (! v.on) { vi = i; break; }
        if (v.level < quietest) { quietest = v.level; qi = i; }
    }
    if (vi < 0) { if (quietest > gain) return; vi = qi; }
    auto& v = impactVoices[(size_t) vi];
    v = ImpactVoice {};
    v.on = true;

    // the Gedackt is of wood: a softer contact, less clatter and none of the metal's bright edge, its tube
    // ringing short and low, and the hollow box answering with a dry "tock"
    const bool wood = e.stop == 1, brass = e.stop == 6;
    // the contact: shorter (brighter) the faster it hits, longer on a soft wall
    const float contact = (0.0003f + 0.003f / (1.0f + e.speed)) * (1.0f + 3.0f * a) * (wood ? 3.0f : 1.0f);
    v.exN = juce::jmax (8, (int) (contact * fsr));
    v.exGain = gain * 2.0f / (float) v.exN;                      // the pulse's area = gain (a unit knock at low pitches)
    v.noiseGain = 0.5f * gain * juce::jmin (1.0f, e.speed / 4.0f) * (1.0f - 0.5f * a) * (wood ? 0.35f : 1.0f);   // the clatter: only a fast hit has much
    const float fc = juce::jlimit (300.0f, 0.45f * fsr, (700.0f + 11000.0f * juce::jmin (1.0f, e.speed / 6.0f)) * (1.0f - 0.7f * a) * (1.0f - 0.35f * e.high)
                                                        * (wood ? 0.3f : 1.0f));
    v.noiseA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / fsr);
    // the wall (or floor) itself: a dull thump, more of it when the floor is hit
    v.thud = e.struck ? 0.0f : (e.floor ? 0.9f : 0.5f) * gain * (1.0f - 0.4f * a);
    v.thudA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * (e.floor ? 110.0f : 180.0f) / fsr);
    v.thudDec = std::exp (-1.0f / (0.03f * fsr));

    // the struck pipe: the bending modes of a tube (free bar ratios), from its length; and its own note, faintly
    float len, rad;
    pipeSizeOf (e.stop, e.key, len, rad);
    const float lenNow = juce::jlimit (0.2f, 4.0f, pipeLen[(size_t) e.stop][(size_t) e.key].load());      // (Wachsen & Brechen)
    len *= lenNow;
    // (wood is less stiff for its weight than tin: the same tube bends at lower frequencies, and its
    //  inner friction stops it soon)
    const float f1 = juce::jlimit (35.0f, 2500.0f, 300.0f / std::pow (juce::jmax (0.12f, len), 1.5f) * (wood ? 0.7f : 1.0f));
    const float ratios[kImpactModes - 1] { 1.0f, 2.756f, 5.404f, 8.933f };
    const float mGain[kImpactModes - 1] { 1.0f, 0.85f, 0.75f, 0.65f }, mT[kImpactModes - 1] { 1.0f, 0.7f, 0.5f, 0.35f };
    const float ring = juce::jlimit (0.05f, 6.0f, lv.impRing) * (wood ? 0.12f : (brass ? 1.2f : 1.0f));
    float longest = 0.05f;
    auto setMode = [&] (int m, float f, float g, float t60)
    {
        if (f >= 0.45f * fsr || f <= 20.0f) { v.gm[(size_t) m] = 0.0f; return; }
        const float w = juce::MathConstants<float>::twoPi * f / fsr;
        const float r = std::exp (-6.91f / (juce::jmax (0.01f, t60) * fsr));
        v.b1[(size_t) m] = 2.0f * r * std::cos (w);
        v.b2[(size_t) m] = r * r;
        // (a pulse shorter than a period excites a mode fully, a longer one hardly: the speed makes it bright)
        const float pulse = juce::jlimit (0.0f, 1.0f, 1.0f / (1.0f + (f * contact) * (f * contact)));
        v.gm[(size_t) m] = g * std::sin (w) * pulse;
        longest = juce::jmax (longest, t60);
    };
    for (int m = 0; m < kImpactModes - 1; ++m) setMode (m, f1 * ratios[m], mGain[m] * (wood ? (m >= 2 ? 0.3f : 0.7f) : 1.0f), ring * mT[m]);
    if (wood)
    {
        // the hollow box: one dry, woody resonance (higher the narrower the pipe), gone in a few hundredths
        const float tock = juce::jlimit (350.0f, 1600.0f, 700.0f * std::sqrt (0.05f / juce::jmax (0.01f, rad)));
        setMode (kImpactModes - 2, tock, 1.4f, 0.07f);
    }
    const float note = pipeHzIdeal (e.stop, e.key, p) / lenNow;
    setMode (kImpactModes - 1, note > 0.0f ? note : f1 * 3.1f, 0.25f, 0.2f + 0.3f * ring);

    v.level = gain;
    v.levelDec = std::exp (-6.91f / (longest * fsr));
    v.life = (int) ((longest * 1.2f + 0.1f) * fsr);

    // where: seen from where the listener stands now
    const Vec3f L = erListener[(size_t) juce::jlimit (0, kNumRooms - 1, e.room)];
    const Pos at = seenFrom (L, { e.x, e.y, e.z });
    float g[kMaxAmbiChannels];
    gainsFor (at, g);
    const float dg = distanceGain (at.dist, p.distAmount);
    for (int c = 0; c < kMaxAmbiChannels; ++c) v.g[(size_t) c] = g[c] * dg;
    v.send = 0.7f * distanceGain (at.dist, p.distAmount * 0.35f);

    // for the 3D view: a flash where it hit
    const uint32_t n = impactCount.load();
    auto& show = impactShow[(size_t) (n & 15u)];
    show.x = e.x; show.y = e.y; show.z = e.z; show.strength = juce::jlimit (0.0f, 1.0f, hard * 0.6f); show.stop = e.stop;
    impactCount = n + 1;
}

void OrganEngine::renderImpacts (float* const* outs, int n, const EngineParams& p) noexcept
{
    handleActs (p);
    for (int i = 0; i < numImpacts; ++i) startImpact (impactQueue[(size_t) i], p);
    numImpacts = 0;
    if (n <= 0) return;
    renderBursts (outs, n);
    float* y = scratch.data();
    for (auto& v : impactVoices)
    {
        if (! v.on) continue;
        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f;
            if (v.exPos < v.exN)
            {
                // the contact: a raised-cosine pulse, with a little noise (the clatter) brightened by the speed
                const float ph = (float) v.exPos / (float) v.exN;
                const float pulse = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ph);
                x = v.exGain * pulse;
                v.noiseLp += v.noiseA * (impactRng.bi() - v.noiseLp);
                x += v.noiseGain * pulse * v.noiseLp;
                ++v.exPos;
            }
            float out = 0.0f;
            for (int m = 0; m < kImpactModes; ++m)
            {
                const float ym = v.b1[(size_t) m] * v.y1[(size_t) m] - v.b2[(size_t) m] * v.y2[(size_t) m] + v.gm[(size_t) m] * x;
                v.y2[(size_t) m] = v.y1[(size_t) m]; v.y1[(size_t) m] = ym;
                out += ym;
            }
            // the thump of the wall
            v.thudLp += v.thudA * (impactRng.bi() * v.thud - v.thudLp);
            v.thud *= v.thudDec;
            out += 2.5f * v.thudLp + 0.4f * x;
            y[i] = out;
        }
        for (int c = 0; c < usedChannels; ++c)
            if (std::abs (v.g[(size_t) c]) > 1.0e-6f) juce::FloatVectorOperations::addWithMultiply (outs[c], y, v.g[(size_t) c], n);
        juce::FloatVectorOperations::addWithMultiply (roomSend[0].data() + roomOffset, y, v.send, n);
        v.level *= std::pow (v.levelDec, (float) n);
        v.age += n;
        if (v.age > v.life || (v.age > v.exN && v.level < 1.0e-6f)) v.on = false;
    }
}
} // namespace rysum
