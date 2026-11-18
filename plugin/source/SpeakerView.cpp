#include "SpeakerView.h"

namespace rysum::ui
{
static juce::String U(const char *s)
{
    return juce::String::fromUTF8(s);
}

// ---------------------------------------------------------------------------
juce::Point<float> SpeakerMap::toScreen(float az, float el) const
{
    // top view: centre = above, rim = horizon, below the horizon further out (down to -45 deg)
    const float r = radius() * juce::jlimit(0.0f, 1.5f, (90.0f - el) / 90.0f);
    const float a = juce::degreesToRadians(az);
    const auto c = centre();
    return {c.x - r * std::sin(a), c.y - r * std::cos(a)};
}

void SpeakerMap::fromScreen(juce::Point<float> s, float &az, float &el) const
{
    const auto c = centre();
    const float dx = s.x - c.x, dy = s.y - c.y;
    az = juce::radiansToDegrees(std::atan2(-dx, -dy));
    el = juce::jlimit(-45.0f, 90.0f, 90.0f - 90.0f * std::sqrt(dx * dx + dy * dy) / radius());
    az = std::round(az * 2.0f) * 0.5f;
    el = std::round(el * 2.0f) * 0.5f;
}

void SpeakerMap::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    const auto c = centre();
    const float R = radius();
    auto d = proc.getSpeakerDecoder();

    // energy of the decoder (plane waves from all directions): darker red = quieter, white = louder
    // than the mean
    if (d != nullptr && d->error.isEmpty())
        for (int b = 0; b < AllRadDecoder::kMapEl; ++b)
        {
            const float el = -90.0f + 5.0f * static_cast<float>(b);
            if (el < -45.0f)
                continue;
            for (int a = 0; a < AllRadDecoder::kMapAz - 1; ++a)
            {
                const float db = d->energyMap[(size_t)(b * AllRadDecoder::kMapAz + a)];
                const auto p = toScreen(-180.0f + 5.0f * static_cast<float>(a), el);
                const float sz = 2.0f + 5.5f * std::cos(juce::degreesToRadians(el));
                if (db < -0.5f)
                    g.setColour(col::rubric.withAlpha(juce::jlimit(0.0f, 0.8f, -db / 10.0f)));
                else if (db > 0.5f)
                    g.setColour(col::white.withAlpha(juce::jlimit(0.0f, 0.5f, db / 6.0f)));
                else
                    continue;
                g.fillEllipse(juce::Rectangle<float>(sz, sz).withCentre(p));
            }
        }

    g.setColour(col::border);
    g.drawEllipse(juce::Rectangle<float>(2 * R, 2 * R).withCentre(c), 1.0f);
    g.setColour(col::line);
    for (float el : {30.0f, 60.0f, -30.0f})
    {
        const float r = R * (90.0f - el) / 90.0f;
        g.drawEllipse(juce::Rectangle<float>(2 * r, 2 * r).withCentre(c), el < 0 ? 0.6f : 1.0f);
    }
    g.setColour(juce::Colour(0xff23231f));
    g.drawLine(c.x, c.y - 1.5f * R, c.x, c.y + 1.5f * R);
    g.drawLine(c.x - 1.5f * R, c.y, c.x + 1.5f * R, c.y);
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::mute);
    g.drawText(rysum::tr("vorn \xc2\xb7 0\xc2\xb0"),
               juce::Rectangle<float>(120, 14).withCentre({c.x, c.y - 1.5f * R - 8}),
               juce::Justification::centred);
    g.setColour(col::faint);
    g.drawText(U("+90\xc2\xb0"), juce::Rectangle<float>(40, 14).withCentre({c.x - R - 18, c.y - 9}),
               juce::Justification::centred);
    g.drawText(U("\xe2\x88\x92"
                 "90\xc2\xb0"),
               juce::Rectangle<float>(40, 14).withCentre({c.x + R + 18, c.y - 9}),
               juce::Justification::centred);
    g.drawText(rysum::tr("Horizont"), juce::Rectangle<float>(70, 12).withCentre({c.x + 38, c.y + R + 8}),
               juce::Justification::centredLeft);
    g.drawText(U("\xe2\x88\x92"
                 "30\xc2\xb0"),
               juce::Rectangle<float>(40, 12).withCentre({c.x + 24, c.y + R * 4.0f / 3.0f + 8}),
               juce::Justification::centredLeft);

    if (d == nullptr)
        return;
    // triangulation
    for (auto &t : d->triangles)
        for (int e = 0; e < 3; ++e)
        {
            const auto &s0 = d->all[(size_t)t[(size_t)e]];
            const auto &s1 = d->all[(size_t)t[(size_t)((e + 1) % 3)]];
            if (s0.el < -45.0f || s1.el < -45.0f)
                continue; // below the map (e.g. the imaginary one at the bottom)
            g.setColour((s0.imaginary || s1.imaginary) ? col::line : col::border);
            g.drawLine(juce::Line<float>(toScreen(s0.az, s0.el), toScreen(s1.az, s1.el)), 0.8f);
        }
    // loudspeakers
    const int test = proc.speakerTest.load();
    for (int i = 0; i < (int)d->all.size(); ++i)
    {
        const auto &s = d->all[(size_t)i];
        if (s.el < -45.0f)
            continue;
        const auto p = toScreen(s.az, s.el);
        const bool sel = i == selected;
        if (s.imaginary)
        {
            g.setColour(sel ? col::white : col::faint);
            g.drawEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre(p), 1.0f);
            g.setFont(A.monoFont(9.0f));
            g.drawText("i", juce::Rectangle<float>(12.0f, 12.0f).withCentre(p),
                       juce::Justification::centred);
            continue;
        }
        if (i == test)
        {
            g.setColour(col::rubric.brighter(0.4f));
            g.drawEllipse(juce::Rectangle<float>(30.0f, 30.0f).withCentre(p), 2.0f);
        }
        const auto box = juce::Rectangle<float>(20.0f, 16.0f).withCentre(p);
        g.setColour(sel ? col::white : col::ink);
        g.fillRect(box);
        g.setColour(col::bg);
        g.setFont(A.romanFont(12.0f));
        g.drawText(juce::String(s.channel), box, juce::Justification::centred);
    }
    // the listener
    g.setColour(col::ink);
    g.drawEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre(c), 1.0f);
}

void SpeakerMap::mouseDown(const juce::MouseEvent &e)
{
    auto d = proc.getSpeakerDecoder();
    dragging = -1;
    if (d == nullptr)
        return;
    float best = 14.0f;
    for (int i = 0; i < (int)proc.getSpeakerLayout().speakers.size(); ++i)
    {
        const auto &s = proc.getSpeakerLayout().speakers[(size_t)i];
        const float dd = toScreen(s.az, s.el).getDistanceFrom(e.position);
        if (dd < best)
        {
            best = dd;
            dragging = i;
        }
    }
    if (dragging >= 0 && onSelect)
        onSelect(dragging);
}

void SpeakerMap::mouseDrag(const juce::MouseEvent &e)
{
    if (dragging < 0 || e.getDistanceFromDragStart() < 3)
        return;
    float az, el;
    fromScreen(e.position, az, el);
    if (onMove)
        onMove(dragging, az, el);
}

void SpeakerMap::mouseUp(const juce::MouseEvent &)
{
    if (dragging >= 0 && onMoveDone)
        onMoveDone();
    dragging = -1;
}

// ---------------------------------------------------------------------------
SpeakerRow::SpeakerRow()
{
    for (int f = 0; f < 5; ++f)
    {
        auto &l = fields[(size_t)f];
        l.setEditable(false, true, false);
        l.setJustificationType(juce::Justification::centredRight);
        l.setFont(Assets::get().romanFont(13.0f));
        l.setColour(juce::Label::textColourId, col::ink);
        l.setColour(juce::Label::textWhenEditingColourId, col::white);
        l.setColour(juce::Label::backgroundWhenEditingColourId, col::bg);
        l.setColour(juce::Label::outlineWhenEditingColourId, col::rubric);
        l.setTooltip(rysum::tr("Doppelklick: Wert eingeben"));
        l.onTextChange = [this, f]
        {
            if (onEdit)
                onEdit(f, fields[(size_t)f].getText());
        };
        l.addMouseListener(this, false);
        addAndMakeVisible(l);
    }
    imag.setClickingTogglesState(true);
    imag.setTooltip(rysum::tr("Imagin\xc3\xa4rer Lautsprecher: hilft nur beim Aufteilen (L\xc3\xbc"
                      "cken, z. B. unten), sein Signal wird verworfen"));
    imag.onClick = [this]
    {
        if (onImaginary)
            onImaginary(imag.getToggleState());
    };
    addAndMakeVisible(imag);
}

void SpeakerRow::set(int i, const Speaker &s, bool selected)
{
    index = i;
    sel = selected;
    const juce::String vals[]{juce::String(s.channel), juce::String(s.az, 1) + U("\xc2\xb0"),
                              juce::String(s.el, 1) + U("\xc2\xb0"),
                              juce::String(s.radius, 2) + " m",
                              (s.gainDb > 0 ? "+" : "") + juce::String(s.gainDb, 1) + " dB"};
    for (int f = 0; f < 5; ++f)
        if (!fields[(size_t)f].isBeingEdited())
        {
            fields[(size_t)f].setText(vals[f], juce::dontSendNotification);
            fields[(size_t)f].setColour(juce::Label::textColourId,
                                        sel ? col::bg : (s.imaginary ? col::faint : col::ink));
        }
    fields[0].setVisible(!s.imaginary);
    imag.setToggleState(s.imaginary, juce::dontSendNotification);
    repaint();
}

void SpeakerRow::resized()
{
    auto r = getLocalBounds();
    r.removeFromLeft(34);
    const int w[]{52, 76, 76, 70, 70};
    for (int f = 0; f < 5; ++f)
        fields[(size_t)f].setBounds(r.removeFromLeft(w[f]));
    r.removeFromLeft(14);
    imag.setBounds(r.removeFromLeft(64).reduced(0, 2));
}

void SpeakerRow::paint(juce::Graphics &g)
{
    if (sel)
    {
        g.setColour(col::ink);
        g.fillRect(getLocalBounds().withRight(34 + 52 + 76 * 2 + 70 * 2 + 4));
    }
    g.setColour(sel ? col::bg : col::mute);
    g.setFont(Assets::get().monoFont(11.0f));
    g.drawText(juce::String(index + 1), getLocalBounds().withWidth(28),
               juce::Justification::centredRight);
    g.setColour(col::line);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
}

// ---------------------------------------------------------------------------
SpeakerView::SpeakerView(RysumProcessor &p) : proc(p), map(p)
{
    edit = proc.getSpeakerLayout();
    addAndMakeVisible(map);
    map.onSelect = [this](int i) { select(i); };
    map.onMove = [this](int i, float az, float el)
    {
        if (i < 0 || i >= (int)edit.speakers.size())
            return;
        edit.speakers[(size_t)i].az = az;
        edit.speakers[(size_t)i].el = el;
        applyLayout(false); // while dragging: not yet written to the file
    };
    map.onMoveDone = [this] { applyLayout(); };

    presets.setTextWhenNothingSelected(rysum::tr("Vorlage \xe2\x80\xa6"));
    {
        int id = 1;
        for (auto &l : speakerPresets())
            presets.addItem(l.name, id++);
    }
    presets.onChange = [this]
    {
        const int i = presets.getSelectedId() - 1;
        const auto all = speakerPresets();
        if (i >= 0 && i < (int)all.size())
        {
            edit = all[(size_t)i];
            selected = 0;
            applyLayout();
        }
        presets.setSelectedId(0, juce::dontSendNotification);
    };
    presets.setTooltip(rysum::tr("Eine fertige Anordnung \xc3\xbc"
                         "bernehmen (ersetzt die jetzige)"));
    addAndMakeVisible(presets);

    loadJson.setTooltip(
        rysum::tr("Lautsprecher-Layout aus dem IEM AllRADecoder / SimpleDecoder (JSON) laden"));
    loadJson.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>(
            rysum::tr("Lautsprecher-Layout (IEM JSON)"),
            RysumProcessor::speakerDefaultFile().getParentDirectory(), "*.json");
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser &fc)
            {
                const auto f = fc.getResult();
                if (!f.existsAsFile())
                    return;
                SpeakerLayout l;
                juce::String err;
                if (speakerLayoutFromJson(f.loadFileAsString(), l, err))
                {
                    if (l.name.isEmpty())
                        l.name = f.getFileNameWithoutExtension();
                    edit = l;
                    selected = 0;
                    applyLayout();
                }
                else
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                           rysum::tr("Lautsprecher"),
                                                           f.getFileName() + ": " + err);
            });
    };
    saveJson.setTooltip(rysum::tr("Als JSON speichern \xe2\x80\x93 im Format der IEM Plug-in Suite "
                          "(AllRADecoder liest es auch)"));
    saveJson.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>(
            rysum::tr("Lautsprecher-Layout speichern"),
            RysumProcessor::speakerDefaultFile().getParentDirectory().getChildFile(
                (edit.name.isNotEmpty() ? juce::File::createLegalFileName(edit.name)
                                        : juce::String("Layout")) +
                ".json"),
            "*.json");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode |
                                 juce::FileBrowserComponent::canSelectFiles |
                                 juce::FileBrowserComponent::warnAboutOverwriting,
                             [this](const juce::FileChooser &fc)
                             {
                                 auto f = fc.getResult();
                                 if (f == juce::File())
                                     return;
                                 if (!f.hasFileExtension("json"))
                                     f = f.withFileExtension("json");
                                 f.replaceWithText(speakerLayoutToJson(edit));
                             });
    };
    addAndMakeVisible(loadJson);
    addAndMakeVisible(saveJson);

    onButton.setTooltip(rysum::tr("Ausgabe auf die Lautsprecher (AllRAD) bzw. zur\xc3\xbc"
                          "ck auf Stereo"));
    onButton.onClick = [this]
    {
        if (auto *o = proc.apvts.getParameter("output"))
            o->setValueNotifyingHost(
                o->convertTo0to1(proc.isSpeakers() ? 0.0f : (float)RysumProcessor::kSpeakerOutput));
    };
    addAndMakeVisible(onButton);
    if (auto *choice =
            dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("spkOrder")))
        order.addItemList(choice->choices, 1);
    orderAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "spkOrder", order);
    order.setTooltip(rysum::tr("Ambisonics-Ordnung vor dem Decoder: h\xc3\xb6her = sch\xc3\xa4rfer, wenn "
                       "genug Lautsprecher da sind"));
    addAndMakeVisible(order);
    distComp.setClickingTogglesState(true);
    distComp.setTooltip(rysum::tr("N\xc3\xa4here Lautsprecher leiser und sp\xc3\xa4ter, als st\xc3\xbcnden "
                          "alle so weit weg wie der fernste (Spalte Abstand)"));
    distAtt = rysum::ui::attachButton(
        proc.apvts, "spkDistComp", distComp);
    addAndMakeVisible(distComp);

    addBtn.onClick = [this]
    {
        Speaker s;
        int ch = 1;
        for (auto &x : edit.speakers)
            ch = juce::jmax(ch, x.channel + 1);
        s.channel = ch;
        if (!edit.speakers.empty())
        {
            s.radius = edit.speakers.back().radius;
            s.az = wrapAz(edit.speakers.back().az + 30.0f);
            s.el = edit.speakers.back().el;
        }
        edit.speakers.push_back(s);
        selected = (int)edit.speakers.size() - 1;
        applyLayout();
    };
    removeBtn.onClick = [this]
    {
        if (selected < 0 || selected >= (int)edit.speakers.size())
            return;
        edit.speakers.erase(edit.speakers.begin() + selected);
        selected = juce::jmax(0, selected - 1);
        applyLayout();
    };
    testBtn.setClickingTogglesState(true);
    testBtn.setTooltip(rysum::tr("Rauschen aus dem gew\xc3\xa4hlten Lautsprecher (direkt auf seinen Kanal, etwa \xe2\x88\x92"
        "20 dBFS) \xe2\x80\x93 "
        "nur wenn die Ausgabe auf Lautsprecher steht"));
    testBtn.onClick = [this]
    {
        tourBtn.setToggleState(false, juce::dontSendNotification);
        proc.speakerTest = testBtn.getToggleState() ? selected : -1;
    };
    tourBtn.setClickingTogglesState(true);
    tourBtn.setTooltip(rysum::tr("Das Rauschen wandert jede Sekunde zum n\xc3\xa4"
                         "chsten Lautsprecher"));
    tourBtn.onClick = [this]
    {
        testBtn.setToggleState(false, juce::dontSendNotification);
        tourNext = 0.0;
        if (!tourBtn.getToggleState())
            proc.speakerTest = -1;
    };
    for (auto *b : {&addBtn, &removeBtn, &testBtn, &tourBtn})
        addAndMakeVisible(*b);

    listView.setViewedComponent(&listContent, false);
    listView.setScrollBarsShown(true, false);
    addAndMakeVisible(listView);
    syncRows();
}

void SpeakerView::applyLayout(bool remember)
{
    proc.setSpeakerLayout(edit, remember);
    shownVersion = proc.speakerVersion();
    syncRows();
    map.repaint();
    repaint();
}

void SpeakerView::select(int i)
{
    selected = juce::jlimit(0, juce::jmax(0, (int)edit.speakers.size() - 1), i);
    if (testBtn.getToggleState())
        proc.speakerTest = selected;
    syncRows();
    map.repaint();
}

void SpeakerView::syncRows()
{
    map.selected = selected;
    const int n = (int)edit.speakers.size();
    while ((int)rows.size() > n)
        rows.pop_back();
    while ((int)rows.size() < n)
    {
        const int i = (int)rows.size();
        auto r = std::make_unique<SpeakerRow>();
        r->onSelect = [this, i] { select(i); };
        r->onEdit = [this, i](int field, const juce::String &t)
        {
            if (i >= (int)edit.speakers.size())
                return;
            auto &s = edit.speakers[(size_t)i];
            const float v =
                t.retainCharacters("-+0123456789.,").replaceCharacter(',', '.').getFloatValue();
            switch (field)
            {
            case 0:
                s.channel = juce::jlimit(1, 64, juce::roundToInt(v));
                break;
            case 1:
                s.az = wrapAz(v);
                break;
            case 2:
                s.el = juce::jlimit(-90.0f, 90.0f, v);
                break;
            case 3:
                s.radius = juce::jlimit(0.1f, 100.0f, v);
                break;
            default:
                s.gainDb = juce::jlimit(-60.0f, 12.0f, v);
                break;
            }
            selected = i;
            applyLayout();
        };
        r->onImaginary = [this, i](bool on)
        {
            if (i < (int)edit.speakers.size())
            {
                edit.speakers[(size_t)i].imaginary = on;
                selected = i;
                applyLayout();
            }
        };
        listContent.addAndMakeVisible(*r);
        rows.push_back(std::move(r));
    }
    const int rh = 22;
    listContent.setSize(juce::jmax(1, listView.getWidth() - listView.getScrollBarThickness()),
                        juce::jmax(1, n * rh));
    for (int i = 0; i < n; ++i)
    {
        rows[(size_t)i]->setBounds(0, i * rh, listContent.getWidth(), rh);
        rows[(size_t)i]->set(i, edit.speakers[(size_t)i], i == selected);
    }
}

void SpeakerView::resized()
{
    auto r = getLocalBounds();
    left = r.removeFromLeft(600);
    right = r.reduced(28, 18);
    map.setBounds(left.reduced(20, 16));
    auto rr = right;
    headBox = rr.removeFromTop(34);
    auto row1 = rr.removeFromTop(24);
    presets.setBounds(row1.removeFromLeft(230));
    row1.removeFromLeft(6);
    loadJson.setBounds(row1.removeFromLeft(150));
    row1.removeFromLeft(6);
    saveJson.setBounds(row1.removeFromLeft(100));
    rr.removeFromTop(6);
    auto row2 = rr.removeFromTop(24);
    onButton.setBounds(row2.removeFromLeft(150));
    row2.removeFromLeft(6);
    order.setBounds(row2.removeFromLeft(120));
    row2.removeFromLeft(6);
    distComp.setBounds(row2.removeFromLeft(170));
    rr.removeFromTop(10);
    listHead = rr.removeFromTop(18);
    infoBox = rr.removeFromBottom(150);
    rr.removeFromBottom(8);
    auto btns = rr.removeFromBottom(28).withTrimmedTop(4);
    addBtn.setBounds(btns.removeFromLeft(130));
    btns.removeFromLeft(6);
    removeBtn.setBounds(btns.removeFromLeft(90));
    btns.removeFromLeft(18);
    testBtn.setBounds(btns.removeFromLeft(70));
    btns.removeFromLeft(6);
    tourBtn.setBounds(btns.removeFromLeft(90));
    listView.setBounds(rr);
    syncRows();
}

void SpeakerView::refresh()
{
    if (proc.speakerVersion() != shownVersion)
    {
        // changed elsewhere (project loaded, another window)
        shownVersion = proc.speakerVersion();
        edit = proc.getSpeakerLayout();
        selected = juce::jmin(selected, juce::jmax(0, (int)edit.speakers.size() - 1));
        syncRows();
        map.repaint();
        repaint();
    }
    onButton.setToggleState(proc.isSpeakers(), juce::dontSendNotification);
    if (tourBtn.getToggleState())
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now >= tourNext && !edit.speakers.empty())
        {
            tourNext = now + 1000.0;
            int t = proc.speakerTest.load();
            for (int k = 0; k < (int)edit.speakers.size(); ++k)
            {
                t = (t + 1) % (int)edit.speakers.size();
                if (!edit.speakers[(size_t)t].imaginary)
                    break;
            }
            proc.speakerTest = t;
            selected = t;
            syncRows();
        }
    }
    if (proc.speakerTest.load() >= 0)
        map.repaint();
    repaint(headBox);
}

void SpeakerView::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    g.setColour(col::line);
    g.fillRect(left.getRight() - 1, left.getY(), 1, left.getHeight());
    auto d = proc.getSpeakerDecoder();
    int real = 0;
    for (auto &s : edit.speakers)
        real += s.imaginary ? 0 : 1;
    drawSectionTitle(g, headBox, rysum::tr("Lautsprecher \xc2\xb7 AllRAD"),
                     proc.isSpeakers() ? rysum::tr("aktiv")
                                       : rysum::tr("aus \xe2\x80\x93 Ausgabe steht auf etwas anderem"));

    // list header
    g.setFont(A.monoFont(10.5f));
    g.setColour(col::mute);
    auto h = listHead;
    h.removeFromLeft(34);
    const char *names[]{"Kanal", "Azimut", "Elevation", "Abstand", "Gain"};
    const int w[]{52, 76, 76, 70, 70};
    for (int f = 0; f < 5; ++f)
        g.drawText(rysum::tr(names[f]), h.removeFromLeft(w[f]), juce::Justification::centredRight);
    h.removeFromLeft(14);
    g.drawText(rysum::tr("imagin\xc3\xa4r"), h.removeFromLeft(64), juce::Justification::centred);

    // what the decoder looks like
    juce::AttributedString as;
    as.setJustification(juce::Justification::topLeft);
    as.setLineSpacing(2.0f);
    auto body = [&](const juce::String &t, juce::Colour c = col::dim)
    { as.append(t, A.sansFont(12.5f), c); };
    if (d != nullptr && d->error.isNotEmpty())
        body(rysum::tr("Fehler: ") + rysum::trText(d->error) + "\n", col::rubric.brighter(0.5f));
    else if (d != nullptr)
    {
        const int have = proc.getTotalNumOutputChannels();
        body(juce::String(real) + rysum::tr(" Lautsprecher") +
                 (d->addedImaginary > 0 ? " (+" + juce::String(d->addedImaginary) +
                                              rysum::tr(" imagin\xc3\xa4r erg\xc3\xa4nzt)")
                                        : juce::String()) +
                 U(" \xc2\xb7 ") + juce::String(d->triangles.size()) +
                 rysum::tr(" Dreiecke \xc2\xb7 Kan\xc3\xa4le bis ") + juce::String(d->numOutChannels) +
                 rysum::tr(" \xc2\xb7 Ausgang hat ") + juce::String(have) + "\n",
             have < d->numOutChannels ? col::white : col::dim);
        body(rysum::tr("Energie im Bereich der Lautsprecher: ") + juce::String(d->energyMinDb, 1) +
             U(" \xe2\x80\xa6 +") + juce::String(d->energyMaxDb, 1) +
             rysum::tr(" dB \xc2\xb7 Richtungsfehler (rE) \xc3\x98 ") +
             juce::String(juce::roundToInt(d->angleErrMean)) + U("\xc2\xb0, max ") +
             juce::String(juce::roundToInt(d->angleErrMax)) + U("\xc2\xb0\n"));
        const int rec = juce::jmax(1, (int)std::floor(std::sqrt((double)real)) - 1);
        body(rysum::tr("Faustregel: Ordnung bis etwa ") + juce::String(rec) + rysum::tr(" f\xc3\xbcr ") +
             juce::String(real) +
             rysum::tr(" Lautsprecher \xe2\x80\x93 h\xc3\xb6her geht, wird aber nicht "
               "sch\xc3\xa4rfer.\n\n"));
    }
    body(rysum::tr("Wie der IEM AllRADecoder: eine dichte virtuelle Kugel (5000 Richtungen) wird mit "
           "max-rE dekodiert und per VBAP "
           "auf die echten Lautsprecher verteilt. Wo keine sind (unten, hinten), helfen "
           "imagin\xc3\xa4re Lautsprecher, "
           "deren Signal verworfen wird. In der Karte: rot = leiser, wei\xc3\x9f = lauter als im "
           "Mittel. Lautsprecher ziehen, "
           "Werte per Doppelklick in der Liste \xc3\xa4ndern. Das Layout geh\xc3\xb6rt zum Projekt "
           "und wird f\xc3\xbcr neue Instanzen gemerkt."));
    juce::TextLayout tl;
    tl.createLayout(as, (float)infoBox.getWidth());
    tl.draw(g, infoBox.toFloat());
}
} // namespace rysum::ui
