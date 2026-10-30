#include "OrganView.h"

namespace rysum::ui
{
OrganView::OrganView()
{
    lines.addLines (juce::String::fromUTF8 (RysumData::orgel_txt, RysumData::orgel_txtSize));
}

void OrganView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (22).withTrimmedBottom (18);
    photoCol = r.removeFromLeft (300);
    r.removeFromLeft (32);
    const int w = (r.getWidth() - 32) / 2;
    middleCol = r.removeFromLeft (w + 20);
    r.removeFromLeft (32);
    rightCol = r;
}

void OrganView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.05f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.4f);
    }

    // the facade pipes, tall, with a caption
    {
        auto p = photoCol.withTrimmedBottom (22);
        drawCover (g, A.orgPipes, p.toFloat(), 0.5f);
        drawGrain (g, p, 0.3f);
        g.setColour (col::border);
        g.drawRect (p, 1);
        g.setFont (A.monoFont (11.0f));
        g.setColour (col::mute);
        g.drawText (juce::String::fromUTF8 ("Prospektpfeifen \xc2\xb7 Blei, gotische Spitzlabien"), photoCol.removeFromBottom (18), juce::Justification::centredLeft);
    }

    // text: wrapped paragraphs with TextLayout (measured, so everything stacks)
    auto paragraph = [&] (juce::Rectangle<int>& area, const juce::String& text, const juce::Font& f, juce::Colour c, int gapAfter)
    {
        juce::AttributedString as;
        as.append (text, f, c);
        as.setJustification (juce::Justification::topLeft);
        as.setLineSpacing (1.0f);
        juce::TextLayout tl;
        tl.createLayout (as, (float) area.getWidth());
        const int h = (int) std::ceil (tl.getHeight());
        tl.draw (g, area.removeFromTop (h).toFloat());
        area.removeFromTop (gapAfter);
    };

    juce::Rectangle<int> cur = middleCol;
    juce::Rectangle<int> rightStart = rightCol;
    bool tableOpen = false;
    for (auto raw : lines)
    {
        const auto line = raw.trimEnd();
        if (line.isEmpty()) continue;
        if (line == "@middle") { cur = middleCol; continue; }
        if (line == "@right") { cur = rightStart; continue; }
        if (! line.startsWith ("|") && tableOpen) { cur.removeFromTop (8); tableOpen = false; }
        if (line.startsWith ("## "))
        {
            auto t = cur.removeFromTop (36);
            drawSectionTitle (g, t, line.substring (3));
            continue;
        }
        if (line.startsWith ("# "))
        {
            g.setColour (col::ink);
            const auto f = A.frakturFont (40.0f);
            g.setFont (f);
            auto t = cur.removeFromTop (46);
            const auto txt = line.substring (2);
            // the first letter rubricated, like the logo
            const int iw = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, txt.substring (0, 1)));
            g.setColour (col::rubric.brighter (0.2f));
            g.drawText (txt.substring (0, 1), t.withWidth (iw + 4), juce::Justification::centredLeft);
            g.setColour (col::ink);
            g.drawText (txt.substring (1), t.withTrimmedLeft (iw), juce::Justification::centredLeft);
            continue;
        }
        if (line.startsWith ("> "))
        {
            paragraph (cur, line.substring (2), A.monoFont (12.0f), col::dim, 14);
            continue;
        }
        if (line.startsWith ("- "))
        {
            // timeline / list: the part before " · " in the margin
            const auto body = line.substring (2);
            auto item = cur;
            const bool dated = body.contains (juce::String::fromUTF8 (" \xc2\xb7 ")) && body.indexOf (juce::String::fromUTF8 (" \xc2\xb7 ")) < 12;
            if (dated)
            {
                const auto date = body.upToFirstOccurrenceOf (juce::String::fromUTF8 (" \xc2\xb7 "), false, false);
                const auto text = body.fromFirstOccurrenceOf (juce::String::fromUTF8 (" \xc2\xb7 "), false, false);
                auto margin = item.removeFromLeft (74);
                g.setFont (A.monoFont (11.5f));
                g.setColour (col::rubric.brighter (0.35f));
                g.drawText (date, margin.removeFromTop (18), juce::Justification::topLeft);
                const int y0 = item.getY();
                paragraph (item, text, A.sansFont (13.0f), col::dim, 5);
                cur.removeFromTop (item.getY() - y0);
            }
            else
            {
                g.setColour (col::rubric.brighter (0.35f));
                drawLozenge (g, { (float) item.getX() + 3.0f, (float) item.getY() + 9.0f }, 2.5f, 3.5f);
                item.removeFromLeft (14);
                const int y0 = item.getY();
                paragraph (item, body, A.sansFont (13.0f), col::dim, 5);
                cur.removeFromTop (item.getY() - y0);
            }
            continue;
        }
        if (line.startsWith ("|"))
        {
            // a stop: name | where its pipes come from
            juce::StringArray cells;
            cells.addTokens (line.substring (1), "|", {});
            auto row = cur.removeFromTop (21);
            g.setColour (col::line);
            g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
            g.setFont (A.sansFont (13.5f));
            g.setColour (col::ink);
            g.drawText (cells[0].trim(), row.removeFromLeft (140), juce::Justification::centredLeft);
            g.setFont (A.monoFont (11.5f));
            g.setColour (col::dim);
            g.drawText (cells[1].trim(), row, juce::Justification::centredLeft);
            tableOpen = true;
            continue;
        }
        if (line == "[photos]")
        {
            // the two stop jambs, three knobs each
            cur.removeFromTop (6);
            auto p = cur.removeFromTop (178);
            const int pw = (p.getWidth() - 12) / 2;
            for (auto* im : { &A.jambLeft, &A.jambRight })
            {
                auto cell = p.removeFromLeft (pw);
                p.removeFromLeft (12);
                drawCover (g, *im, cell.toFloat(), 0.5f);
                drawGrain (g, cell, 0.3f);
                g.setColour (col::border);
                g.drawRect (cell, 1);
            }
            cur.removeFromTop (12);
            continue;
        }
        if (line.startsWith ("~ "))
        {
            paragraph (cur, line.substring (2), A.sansFont (11.5f), col::mute, 6);
            continue;
        }
        paragraph (cur, line, A.sansFont (13.5f), col::dim, 8);
    }
}
} // namespace rysum::ui
