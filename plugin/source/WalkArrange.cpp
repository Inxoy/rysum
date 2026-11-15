#include "WalkView.h"
#include <algorithm>

// The 3D view: arranging the pipes. Choosing some (Ctrl+click, Ctrl+double-click, Ctrl+drag), formations
// (1 to 5), layouts kept with the project (Ctrl+F1..F8, recalled with F1..F8), undo (Ctrl+Z), the magnet (the
// right mouse button held), the sweep (Alt+drag) and the list of all the keys (Tab held).
namespace rysum::ui
{
namespace
{
juce::String U (const char* s) { return juce::String::fromUTF8 (s); }
constexpr int kUndoDepth = 24;
const char* const kFormationNames[6] { "", "Reihe", "Kreis", "Spirale", "Raster", "Verstreut" };
} // namespace

// ---------------------------------------------------------------------------
// choosing pipes
// ---------------------------------------------------------------------------
void WalkView::selectPipe (int stop, int key, bool toggle)
{
    if (stop < 0 || stop >= kNumStops || key < 0 || key >= kNumKeys) return;
    const uint64_t bit = (uint64_t) 1 << key;
    if (toggle) sel[(size_t) stop] ^= bit; else sel[(size_t) stop] |= bit;
    say (juce::String (selectedCount()) + U (" Pfeifen gew\xc3\xa4hlt  \xc2\xb7  1 bis 5: aufstellen, rechte Maustaste: Magnet, Esc: Auswahl leeren"), 3500.0);
    repaint();
}

void WalkView::selectRegister (int stop)
{
    if (stop < 0 || stop >= kNumStops) return;
    sel[(size_t) stop] = ((uint64_t) 1 << kNumKeys) - 1;
    say (U ("Register ") + juce::String::fromUTF8 (kStops[(size_t) stop].label) + U (" gew\xc3\xa4hlt (") + juce::String (selectedCount()) + U (" Pfeifen)"), 3500.0);
    repaint();
}

void WalkView::clearSelection()
{
    sel = {};
    if (magnet != 0)
        for (auto& m : proc.engine.magnetMask) m = ~(uint64_t) 0;
    repaint();
}

int WalkView::selectedCount() const noexcept
{
    int n = 0;
    for (auto m : sel)
        for (uint64_t x = m; x != 0; x &= x - 1) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// layouts: where the pipes stand, kept and brought back
// ---------------------------------------------------------------------------
RysumProcessor::WalkLayout WalkView::capture() const
{
    RysumProcessor::WalkLayout lay;
    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
        {
            Vec3f b;
            if (! proc.engine.bodyAt (s, k, b)) continue;
            lay.has[(size_t) s] |= (uint64_t) 1 << k;
            lay.at[(size_t) s][(size_t) k] = b;
        }
    return lay;
}

int WalkView::applyLayout (const RysumProcessor::WalkLayout& lay)
{
    auto& en = proc.engine;
    int n = 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        const uint64_t m = lay.has[(size_t) s];
        if (m == 0 || ! makeHover (s, false)) continue;      // (a stop that moves another way is left as it is)
        for (int k = 0; k < kNumKeys; ++k)
            if (((m >> k) & 1u) != 0u)
            {
                const auto& p = lay.at[(size_t) s][(size_t) k];
                en.goX[(size_t) s][(size_t) k] = p.x;
                en.goY[(size_t) s][(size_t) k] = p.y;
                en.goZ[(size_t) s][(size_t) k] = p.z;
                ++n;
            }
        en.goMask[(size_t) s].fetch_or (m);
    }
    if (n > 0)
    {
        en.playerAct (OrganEngine::Act::Arrange);
        // (stops that only start to hover now are bodies a few blocks later: they are sent again until then)
        arrangeLast = juce::Time::getMillisecondCounterHiRes();
        arrangeUntil = arrangeLast + 1500.0;
    }
    return n;
}

void WalkView::pushUndo()
{
    auto lay = capture();
    if (lay.empty()) return;
    undoStack.push_back (std::move (lay));
    if ((int) undoStack.size() > kUndoDepth) undoStack.erase (undoStack.begin());
}

bool WalkView::undo()
{
    if (undoStack.empty()) { say (U ("Nichts r\xc3\xbc" "ckg\xc3\xa4ngig zu machen")); return false; }
    const auto lay = undoStack.back();
    undoStack.pop_back();
    const int n = applyLayout (lay);
    say (U ("R\xc3\xbc" "ckg\xc3\xa4ngig: ") + juce::String (n) + U (" Pfeifen kehren an ihren Platz zur\xc3\xbc" "ck")
         + (undoStack.empty() ? juce::String() : U ("  \xc2\xb7  noch ") + juce::String ((int) undoStack.size()) + U (" Schritte")));
    return n > 0;
}

bool WalkView::storeLayout (int slot)
{
    if (slot < 0 || slot >= (int) proc.walkLayouts.size()) return false;
    auto lay = capture();
    if (lay.empty())
    {
        say (U ("Keine Pfeife frei im Saal \xe2\x80\x93 nichts zu speichern (die Pfeifen eines Registers werden frei mit E, Q, 1 bis 5 oder der Bewegung \xe2\x80\x9eSchweben\xe2\x80\x9c)"), 4000.0);
        return false;
    }
    int n = 0;
    for (auto m : lay.has)
        for (uint64_t x = m; x != 0; x &= x - 1) ++n;
    proc.walkLayouts[(size_t) slot] = std::move (lay);
    say (U ("Aufstellung F") + juce::String (slot + 1) + U (" gespeichert (") + juce::String (n) + U (" Pfeifen) \xc2\xb7  F") + juce::String (slot + 1) + U (" ruft sie zur\xc3\xbc" "ck"));
    return true;
}

bool WalkView::recallLayout (int slot)
{
    if (slot < 0 || slot >= (int) proc.walkLayouts.size()) return false;
    const auto& lay = proc.walkLayouts[(size_t) slot];
    if (lay.empty()) { say (U ("Aufstellung F") + juce::String (slot + 1) + U (" ist leer \xe2\x80\x93 Strg+F") + juce::String (slot + 1) + U (" speichert, wie die Pfeifen jetzt stehen")); return false; }
    pushUndo();
    const int n = applyLayout (lay);
    say (U ("Aufstellung F") + juce::String (slot + 1) + U (": ") + juce::String (n) + U (" Pfeifen fliegen an ihren Platz"));
    return n > 0;
}

// ---------------------------------------------------------------------------
// formations
// ---------------------------------------------------------------------------
bool WalkView::formation (int kind)
{
    if (kind < 1 || kind > 5) return false;
    const auto params = proc.readParams();
    // the chosen pipes, or (none chosen) every pipe of the pulled stops; low to high, as on a keyboard
    struct Item { int s, k; float pitch; };
    std::vector<Item> items;
    const bool chosen = selectedCount() > 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! chosen && ! params.stopOn[(size_t) s]) continue;
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (chosen ? ! isSelected (s, k) : proc.engine.muted[(size_t) s][(size_t) k].load()) continue;
            items.push_back ({ s, k, (float) k - 12.0f * std::log2 (kStopFootage[(size_t) s] / 8.0f) + 0.01f * (float) s });
        }
    }
    if (items.empty()) { say (U ("Kein Register gezogen \xe2\x80\x93 keine Pfeife aufzustellen")); return false; }
    std::stable_sort (items.begin(), items.end(), [] (const Item& a, const Item& b) { return a.pitch < b.pitch; });

    // around you: ahead and to the right on the floor's plane, a little below the eyes
    const V3 eye = eyeNow();
    const float ya = juce::degreesToRadians (yaw);
    const V3 fwd { -std::sin (ya), std::cos (ya), 0.0f }, right { std::cos (ya), std::sin (ya), 0.0f };
    const float zc = juce::jmax (0.8f, eye.z - 0.2f);
    const int n = (int) items.size();
    juce::Random rng ((juce::int64) (scatterSeed++ * 7919u + 13u));
    RysumProcessor::WalkLayout lay;
    for (int i = 0; i < n; ++i)
    {
        V3 p;
        switch (kind)
        {
            case 1:      // a row across the view, low notes on the left; more than 24: rows behind it
            {
                const int cols = juce::jmin (n, 24), c = i % cols, r = i / cols;
                p = eye + fwd * (3.0f + 0.8f * (float) r) + right * (((float) c - 0.5f * (float) (cols - 1)) * 0.42f);
                p.z = zc;
                break;
            }
            case 2:      // a circle around you, starting ahead and going right; more than 32: wider circles
            {
                const int per = 32, ring = i / per, m = juce::jmin (per, n - ring * per);
                const float a = juce::MathConstants<float>::twoPi * (float) (i % per) / (float) m;
                const float rad = 2.4f + 1.1f * (float) ring;
                p = eye + fwd * (rad * std::cos (a)) + right * (rad * std::sin (a));
                p.z = zc;
                break;
            }
            case 3:      // a spiral (as the seeds of a sunflower) around you, rising outwards
            {
                const float a = (float) i * 2.39996f, rad = 1.4f + 0.45f * std::sqrt ((float) i);
                p = eye + fwd * (rad * std::cos (a)) + right * (rad * std::sin (a));
                p.z = zc - 0.4f + 1.6f * (float) i / (float) juce::jmax (1, n - 1);
                break;
            }
            case 4:      // a square grid ahead
            {
                const int cols = juce::jmax (1, (int) std::ceil (std::sqrt ((float) n))), c = i % cols, r = i / cols;
                p = eye + fwd * (2.5f + 0.6f * (float) r) + right * (((float) c - 0.5f * (float) (cols - 1)) * 0.6f);
                p.z = zc;
                break;
            }
            default:     // scattered around you, at all heights
            {
                const float a = rng.nextFloat() * juce::MathConstants<float>::twoPi, rad = 2.0f + 5.0f * std::sqrt (rng.nextFloat());
                p = eye + fwd * (rad * std::cos (a)) + right * (rad * std::sin (a));
                p.z = 0.5f + 2.8f * rng.nextFloat();
                break;
            }
        }
        const auto& it = items[(size_t) i];
        lay.has[(size_t) it.s] |= (uint64_t) 1 << it.k;
        lay.at[(size_t) it.s][(size_t) it.k] = { p.x, p.y, p.z };
    }
    pushUndo();
    const int sent = applyLayout (lay);
    say (U (kFormationNames[kind]) + U (": ") + juce::String (sent) + U (" Pfeifen") + (chosen ? U (" (die gew\xc3\xa4hlten)") : juce::String())
         + U ("  \xc2\xb7  Strg+Z: zur\xc3\xbc" "ck  \xc2\xb7  Strg+F1 bis F8: Aufstellung speichern"));
    return sent > 0;
}

// ---------------------------------------------------------------------------
// the magnet and the sweep
// ---------------------------------------------------------------------------
void WalkView::setMagnet (int mode)
{
    mode = juce::jlimit (-1, 1, mode);
    auto& en = proc.engine;
    if (mode == magnet) return;
    if (mode != 0 && magnet == 0)
    {
        pushUndo();
        // (the pulled stops - or those of the chosen pipes - that stand still start to hover, so there is something to draw)
        const auto params = proc.readParams();
        const bool chosen = selectedCount() > 0;
        for (int s = 0; s < kNumStops; ++s)
            if (chosen ? sel[(size_t) s] != 0 : (bool) params.stopOn[(size_t) s]) makeHover (s, false);
        for (int s = 0; s < kNumStops; ++s) en.magnetMask[(size_t) s] = chosen ? sel[(size_t) s] : ~(uint64_t) 0;
        say (mode > 0 ? U ("Magnet: die Pfeifen kommen zu dir  \xc2\xb7  Rad: n\xc3\xa4her / weiter  \xc2\xb7  Shift: absto\xc3\x9f" "en")
                      : U ("Magnet: die Pfeifen weichen zur\xc3\xbc" "ck"), 2000.0);
    }
    magnet = mode;
    en.magnetMode = mode;
    repaint();
}

int WalkView::sweepTo (juce::Point<float> from, juce::Point<float> to, float strength)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float len = from.getDistanceFrom (to);
    const int steps = juce::jmax (1, (int) std::ceil (len / 5.0f));
    int hits = 0;
    for (int i = 0; i <= steps; ++i)
    {
        const auto at = from + (to - from) * ((float) i / (float) steps);
        int s = -1, k = -1;
        if (! pickPipe (at, 40.0f, s, k)) continue;
        auto& when = sweptAt[(size_t) s][(size_t) k];
        if (now - when < 250.0) continue;      // (each pipe once as the pointer passes)
        when = now;
        const auto& sh = shown[(size_t) s][(size_t) k];
        proc.engine.playerAct (OrganEngine::Act::Strike, s, k, { sh.c.x, sh.c.y, sh.c.z }, strength);
        glow[(size_t) s][(size_t) k] = 1.0f;
        ++hits;
    }
    return hits;
}

void WalkView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! (e.mods.isCtrlDown() || e.mods.isCommandDown()) || e.mods.isPopupMenu()) return;
    int s = -1, k = -1;
    if (pickPipe (immersive ? getLocalBounds().getCentre().toFloat() : e.position, 40.0f, s, k)) selectRegister (s);
}

// ---------------------------------------------------------------------------
// drawn over the picture
// ---------------------------------------------------------------------------
void WalkView::drawArranging (juce::Graphics& g, juce::Rectangle<int> area)
{
    const float isc = rast.width() > 0 ? (float) area.getWidth() / (float) rast.width() : 1.0f;
    // the chosen pipes: a small red lozenge on each
    if (selectedCount() > 0)
        for (int s = 0; s < kNumStops; ++s)
        {
            if (sel[(size_t) s] == 0) continue;
            for (int k = 0; k < kNumKeys; ++k)
            {
                if (! isSelected (s, k)) continue;
                const auto& sh = shown[(size_t) s][(size_t) k];
                float sx, sy, d;
                if (! sh.on || ! rast.project (sh.c, sx, sy, d)) continue;
                const juce::Point<float> c (sx * isc, sy * isc);
                const float r = juce::jlimit (3.0f, 9.0f, 6.0f / juce::jmax (0.5f, d) * 2.0f);
                g.setColour (col::bg.withAlpha (0.8f));
                drawLozenge (g, c, r + 1.5f, r * 1.4f + 2.0f);
                g.setColour (col::rubric.brighter (0.35f));
                drawLozenge (g, c, r, r * 1.4f);
            }
        }
    // the frame being drawn (Ctrl+drag)
    if (boxing)
    {
        const auto r = juce::Rectangle<float> (boxFrom, boxTo);
        g.setColour (col::ink.withAlpha (0.08f));
        g.fillRect (r);
        g.setColour (col::ink.withAlpha (0.8f));
        g.drawRect (r, 1.0f);
        g.setColour (col::ink.withAlpha (0.35f));
        g.drawRect (r.reduced (2.5f), 0.6f);
    }
    // the magnet: rings running in (drawing) or out (driving off) around its point
    if (magnet != 0)
    {
        const V3 p = eyeNow() + lookDir() * magnetDist;
        float sx, sy, d;
        if (rast.project (p, sx, sy, d))
        {
            const juce::Point<float> c (sx * isc, sy * isc);
            const float base = juce::jlimit (14.0f, 90.0f, 0.6f / juce::jmax (0.3f, d) * 0.5f * (float) area.getWidth() * 0.4f);
            const float t = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() / 700.0, 1.0);
            for (int i = 0; i < 3; ++i)
            {
                float f = std::fmod (t + (float) i / 3.0f, 1.0f);
                if (magnet > 0) f = 1.0f - f;
                const float rad = base * (0.3f + 1.2f * f);
                g.setColour ((magnet > 0 ? col::ink : col::rubric.brighter (0.3f)).withAlpha (0.7f * (1.0f - std::abs (f - 0.5f) * 1.6f)));
                g.drawEllipse (juce::Rectangle<float> (2.0f * rad, 2.0f * rad).withCentre (c), 1.2f);
            }
            g.setColour (col::ink);
            drawLozenge (g, c, 3.0f, 4.5f);
        }
    }
}

void WalkView::drawHelp (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto& A = Assets::get();
    struct Line { const char* key; const char* what; };
    struct Group { const char* title; std::vector<Line> lines; };
    const std::vector<Group> groups {
        { "Gehen", { { "W A S D", "gehen" }, { "\xe2\x86\x90 \xe2\x86\x92", "drehen" }, { "Maus ziehen", "schauen" }, { "Leertaste", "springen" },
                     { "Shift", "schneller" }, { "Rad", "Blickwinkel" }, { "Shift + Rad", "Gehtempo" } } },
        { "H\xc3\xa4nde", { { "Klick", "anschlagen (Shift: st\xc3\xa4rker)" }, { "Klick halten", "die Pfeife klingt, solange gehalten" },
                            { "Alt + ziehen", "\xc3\xbc" "ber die Pfeifen streichen" }, { "Q halten", "greifen, loslassen: werfen" },
                            { "Rechte Taste", "Magnet (Shift: weg, Rad: Abstand)" }, { "Rechts + Q", "die Pfeife in der Hand abstellen" },
                            { "Mittlere Taste", "Pfeife festhalten / l\xc3\xb6sen" }, { "E", "Druckwelle (Shift: st\xc3\xa4rker)" },
                            { "T", "Wirbelwind (Shift: st\xc3\xa4rker)" }, { "R", "alle zur\xc3\xbc" "ckrufen" }, { "C", "klatschen" },
                            { "F halten", "anhalten" }, { "P", "Pause / weiter" }, { "Z halten", "Zeitlupe" } } },
        { "Raum", { { "M", "Material der Fl\xc3\xa4" "che (Alt: aller)" }, { "Strg + Rad", "Saal gr\xc3\xb6\xc3\x9f" "er / kleiner" },
                    { "L", "Licht aus / an" }, { "K", "Laterne (Shift: weg, Strg: alle)" }, { "H", "die Ohren hier lassen" },
                    { "I", "immersiv" }, { "G", "Grafikkarte / Prozessor" }, { "Tab halten", "diese Tasten" }, { "Esc", "zur\xc3\xbc" "ck" } } },
        { "Ordnen", { { "Strg + Klick", "Pfeife w\xc3\xa4hlen" }, { "Strg + Doppelklick", "ganzes Register" }, { "Strg + ziehen", "Rahmen ziehen" },
                      { "Esc", "Auswahl leeren" }, { "1 2 3 4 5", "Reihe, Kreis, Spirale, Raster, verstreut" }, { "F1 \xe2\x80\x93 F8", "Aufstellung abrufen" },
                      { "Strg + F1 \xe2\x80\x93 F8", "Aufstellung speichern" }, { "Strg + Z", "r\xc3\xbc" "ckg\xc3\xa4ngig" } } } };

    const int colW = 380, lineH = 20, headH = 38, pad = 22;
    const int cols = area.getWidth() >= 2 * colW + 3 * pad ? 2 : 1;
    std::array<int, 2> colH {};
    for (size_t gi = 0; gi < groups.size(); ++gi)
        colH[(size_t) ((int) gi % cols)] += headH + lineH * (int) groups[gi].lines.size() + 10;
    const int w = cols * colW + (cols + 1) * pad, h = juce::jmax (colH[0], colH[1]) + 2 * pad;
    const auto box = juce::Rectangle<int> (w, juce::jmin (h, area.getHeight() - 20)).withCentre (area.getCentre());
    g.setColour (col::bg.withAlpha (0.93f));
    g.fillRect (box);
    g.setColour (col::border);
    g.drawRect (box.toFloat().reduced (0.5f), 1.0f);
    g.setColour (col::border.withAlpha (0.45f));
    g.drawRect (box.toFloat().reduced (3.0f), 0.6f);
    std::array<int, 2> y { box.getY() + pad, box.getY() + pad };
    for (size_t gi = 0; gi < groups.size(); ++gi)
    {
        const int c = (int) gi % cols;
        const int x = box.getX() + pad + c * (colW + pad);
        drawSectionTitle (g, { x, y[(size_t) c], colW, headH }, U (groups[gi].title));
        y[(size_t) c] += headH;
        for (const auto& l : groups[gi].lines)
        {
            g.setFont (A.romanFont (13.0f));
            g.setColour (col::ink);
            g.drawText (U (l.key), x, y[(size_t) c], 128, lineH, juce::Justification::centredLeft);
            g.setFont (A.monoFont (12.0f));
            g.setColour (col::dim);
            g.drawText (U (l.what), x + 132, y[(size_t) c], colW - 132, lineH, juce::Justification::centredLeft);
            g.setColour (col::line);
            g.fillRect (x, y[(size_t) c] + lineH - 1, colW, 1);
            y[(size_t) c] += lineH;
        }
        y[(size_t) c] += 10;
    }
}
} // namespace rysum::ui
