#include "SpatialMeta.h"
#include <algorithm>
#include <cstring>

namespace rysum
{
namespace
{
constexpr uint32_t fourcc (const char* s) noexcept
{
    return ((uint32_t) (uint8_t) s[0] << 24) | ((uint32_t) (uint8_t) s[1] << 16) | ((uint32_t) (uint8_t) s[2] << 8) | (uint32_t) (uint8_t) s[3];
}
uint32_t be32 (const uint8_t* p) noexcept { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3]; }
uint64_t be64 (const uint8_t* p) noexcept { return ((uint64_t) be32 (p) << 32) | be32 (p + 4); }
uint16_t be16 (const uint8_t* p) noexcept { return (uint16_t) (((unsigned) p[0] << 8) | p[1]); }
void put32 (std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back ((uint8_t) (x >> s)); }
void put32 (uint8_t* p, uint32_t x) noexcept { p[0] = (uint8_t) (x >> 24); p[1] = (uint8_t) (x >> 16); p[2] = (uint8_t) (x >> 8); p[3] = (uint8_t) x; }
void put64 (uint8_t* p, uint64_t x) noexcept { put32 (p, (uint32_t) (x >> 32)); put32 (p + 4, (uint32_t) x); }

const uint8_t kSphericalUuid[16] { 0xff, 0xcc, 0x82, 0x63, 0xf8, 0x55, 0x4a, 0x93, 0x88, 0x14, 0x58, 0x7a, 0x02, 0x52, 0x1f, 0xdd };
// (as the injector writes it; the stitching software named)
const char* const kSphericalXml =
    "<?xml version=\"1.0\"?><rdf:SphericalVideo\n"
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"\n"
    "xmlns:GSpherical=\"http://ns.google.com/videos/1.0/spherical/\">"
    "<GSpherical:Spherical>true</GSpherical:Spherical>"
    "<GSpherical:Stitched>true</GSpherical:Stitched>"
    "<GSpherical:StitchingSoftware>Rysum</GSpherical:StitchingSoftware>"
    "<GSpherical:ProjectionType>equirectangular</GSpherical:ProjectionType>"
    "</rdf:SphericalVideo>";

/** a box of the 'moov': its fixed part (after the 8-byte header, before any children), its children, and what
    could not be read as boxes after them (e.g. the four zeros old QuickTime files end a sample entry with) */
struct Box
{
    uint32_t type = 0;
    bool container = false;
    std::vector<uint8_t> head;    // (a leaf: all of its body)
    std::vector<Box> kids;
    std::vector<uint8_t> tail;

    Box* child (uint32_t t)
    {
        for (auto& k : kids) if (k.type == t) return &k;
        return nullptr;
    }
    void removeAll (uint32_t t)
    {
        kids.erase (std::remove_if (kids.begin(), kids.end(), [t] (const Box& b) { return b.type == t; }), kids.end());
    }
    size_t size() const
    {
        size_t n = 8 + head.size() + tail.size();
        for (auto& k : kids) n += k.size();
        return n;
    }
    void write (std::vector<uint8_t>& o) const
    {
        put32 (o, (uint32_t) size());
        put32 (o, type);
        o.insert (o.end(), head.begin(), head.end());
        for (auto& k : kids) k.write (o);
        o.insert (o.end(), tail.begin(), tail.end());
    }
};

bool isContainer (uint32_t t) noexcept
{
    return t == fourcc ("moov") || t == fourcc ("trak") || t == fourcc ("mdia") || t == fourcc ("minf") || t == fourcc ("stbl")
        || t == fourcc ("sv3d") || t == fourcc ("proj");
}

/** the boxes in p .. p + n (false: not boxes) */
bool parseBoxes (const uint8_t* p, size_t n, std::vector<Box>& out, std::vector<uint8_t>& tail, int depth)
{
    size_t at = 0;
    while (at < n)
    {
        if (n - at < 8) { tail.assign (p + at, p + n); return true; }
        uint64_t size = be32 (p + at);
        const uint32_t type = be32 (p + at + 4);
        size_t hdr = 8;
        if (size == 1)
        {
            if (n - at < 16) return false;
            size = be64 (p + at + 8);
            hdr = 16;
        }
        else if (size == 0)
        {
            // (a terminator, or "to the end")
            if (type == 0) { tail.assign (p + at, p + n); return true; }
            size = n - at;
        }
        if (size < hdr || size > n - at) return false;
        Box b;
        b.type = type;
        const uint8_t* body = p + at + hdr;
        const size_t len = (size_t) size - hdr;
        if (depth < 12 && isContainer (type))
        {
            b.container = true;
            if (! parseBoxes (body, len, b.kids, b.tail, depth + 1)) return false;
        }
        else if (type == fourcc ("stsd") && len >= 8)
        {
            // version / flags and the count, then the sample entries (opened later, by the track's kind)
            b.container = true;
            b.head.assign (body, body + 8);
            if (! parseBoxes (body + 8, len - 8, b.kids, b.tail, depth + 1)) return false;
        }
        else
            b.head.assign (body, body + len);
        out.push_back (std::move (b));
        at += (size_t) size;
    }
    return true;
}

/** a sample entry opened: its fixed part, then its children */
bool openEntry (Box& e, size_t fixed)
{
    if (e.container) return true;
    if (e.head.size() < fixed) return false;
    std::vector<uint8_t> body;
    body.swap (e.head);
    e.head.assign (body.begin(), body.begin() + (std::ptrdiff_t) fixed);
    e.container = true;
    if (! parseBoxes (body.data() + fixed, body.size() - fixed, e.kids, e.tail, 8))
    {
        // (not boxes after all: left as it was)
        e.kids.clear(); e.tail.clear();
        e.head.swap (body);
        e.container = false;
        return false;
    }
    return true;
}

/** the fixed part of a sound sample entry (after its 8-byte header), by its version - as the injector reads it */
size_t soundFixed (const Box& e)
{
    if (e.head.size() < 10) return 0;
    const int version = be16 (e.head.data() + 8);
    return version == 0 ? 28 : version == 1 ? 44 : version == 2 ? 64 : 0;
}
int soundChannels (const Box& e)
{
    if (e.head.size() < 10) return 0;
    const int version = be16 (e.head.data() + 8);
    if (version == 2) return e.head.size() >= 44 ? (int) be32 (e.head.data() + 40) : 0;
    return e.head.size() >= 18 ? be16 (e.head.data() + 16) : 0;
}

uint32_t handlerOf (Box& trak)
{
    auto* mdia = trak.child (fourcc ("mdia"));
    auto* hdlr = mdia != nullptr ? mdia->child (fourcc ("hdlr")) : nullptr;
    return hdlr != nullptr && hdlr->head.size() >= 12 ? be32 (hdlr->head.data() + 8) : 0;
}
Box* stsdOf (Box& trak)
{
    auto* mdia = trak.child (fourcc ("mdia"));
    auto* minf = mdia != nullptr ? mdia->child (fourcc ("minf")) : nullptr;
    auto* stbl = minf != nullptr ? minf->child (fourcc ("stbl")) : nullptr;
    return stbl != nullptr ? stbl->child (fourcc ("stsd")) : nullptr;
}

Box leaf (const char* type, std::vector<uint8_t> body)
{
    Box b;
    b.type = fourcc (type);
    b.head = std::move (body);
    return b;
}

struct TopBox { uint32_t type; uint64_t offset, size; };

/** the file's top-level boxes; the 'moov' read into memory */
bool scanFile (juce::FileInputStream& in, std::vector<TopBox>& top, std::vector<uint8_t>& moovBytes, size_t& moovIndex, juce::String& error)
{
    const uint64_t total = (uint64_t) in.getTotalLength();
    uint64_t at = 0;
    moovIndex = (size_t) -1;
    while (at + 8 <= total)
    {
        uint8_t h[16];
        in.setPosition ((juce::int64) at);
        if (in.read (h, 8) != 8) break;
        uint64_t size = be32 (h);
        const uint32_t type = be32 (h + 4);
        if (size == 1)
        {
            if (in.read (h + 8, 8) != 8) { error = "a box's size cut off"; return false; }
            size = be64 (h + 8);
        }
        else if (size == 0)
            size = total - at;
        if (size < 8 || at + size > total) { error = "not a QuickTime / MP4 file (a box at " + juce::String ((juce::int64) at) + " runs past the end)"; return false; }
        if (type == fourcc ("moov"))
        {
            if (moovIndex != (size_t) -1) { error = "two 'moov' boxes"; return false; }
            if (size > ((uint64_t) 512 << 20)) { error = "the 'moov' box is too large"; return false; }
            moovIndex = top.size();
            moovBytes.resize ((size_t) size);
            in.setPosition ((juce::int64) at);
            if (in.read (moovBytes.data(), (int) size) != (int) size) { error = "cannot read 'moov'"; return false; }
        }
        top.push_back ({ type, at, size });
        at += size;
    }
    if (moovIndex == (size_t) -1) { error = "no 'moov' box: not a finished QuickTime / MP4 file"; return false; }
    return true;
}

bool parseMoov (const std::vector<uint8_t>& bytes, Box& moov, juce::String& error)
{
    std::vector<Box> list;
    std::vector<uint8_t> tail;
    if (! parseBoxes (bytes.data(), bytes.size(), list, tail, 0) || list.size() != 1 || list[0].type != fourcc ("moov"))
    {
        error = "cannot read the 'moov' box";
        return false;
    }
    moov = std::move (list[0]);
    return true;
}

/** shifts every chunk offset at or after 'from' by 'delta' (the media moved) */
bool shiftChunks (Box& b, uint64_t from, int64_t delta, juce::String& error)
{
    if (b.type == fourcc ("stco") || b.type == fourcc ("co64"))
    {
        const bool wide = b.type == fourcc ("co64");
        if (b.head.size() < 8) return true;
        const uint32_t n = be32 (b.head.data() + 4);
        const size_t step = wide ? 8 : 4;
        if (b.head.size() < 8 + (size_t) n * step) { error = "a chunk table cut off"; return false; }
        for (uint32_t i = 0; i < n; ++i)
        {
            uint8_t* p = b.head.data() + 8 + (size_t) i * step;
            const uint64_t v = wide ? be64 (p) : be32 (p);
            if (v < from) continue;
            const uint64_t nv = (uint64_t) ((int64_t) v + delta);
            if (wide) put64 (p, nv);
            else
            {
                if (nv > 0xffffffffull) { error = "the chunk offsets do not fit in 32 bits any more"; return false; }
                put32 (p, (uint32_t) nv);
            }
        }
        return true;
    }
    for (auto& k : b.kids)
        if (! shiftChunks (k, from, delta, error)) return false;
    return true;
}
} // namespace

bool injectSpatialMetadata (const juce::File& inFile, const juce::File& outFile, juce::String& error, std::function<bool()> shouldStop)
{
    if (inFile == outFile) { error = "the marked video must be a new file"; return false; }
    juce::FileInputStream in (inFile);
    if (! in.openedOk()) { error = "cannot open " + inFile.getFullPathName(); return false; }
    std::vector<TopBox> top;
    std::vector<uint8_t> moovBytes;
    size_t moovIndex = 0;
    if (! scanFile (in, top, moovBytes, moovIndex, error)) return false;
    Box moov;
    if (! parseMoov (moovBytes, moov, error)) return false;

    bool video = false, sound = false;
    for (auto& trak : moov.kids)
    {
        if (trak.type != fourcc ("trak")) continue;
        const uint32_t h = handlerOf (trak);
        auto* stsd = stsdOf (trak);
        if (h == fourcc ("vide") && ! video && stsd != nullptr)
        {
            video = true;
            // V1: the XML in a 'uuid' box, the track's last child (an older one of ours replaced)
            trak.kids.erase (std::remove_if (trak.kids.begin(), trak.kids.end(), [] (const Box& b)
                             { return b.type == fourcc ("uuid") && b.head.size() >= 16 && std::memcmp (b.head.data(), kSphericalUuid, 16) == 0; }),
                             trak.kids.end());
            std::vector<uint8_t> body (kSphericalUuid, kSphericalUuid + 16);
            const auto len = std::strlen (kSphericalXml);
            body.insert (body.end(), kSphericalXml, kSphericalXml + len);
            trak.kids.push_back (leaf ("uuid", std::move (body)));
            // V2: mono, equirectangular, the whole sphere - in every video sample entry
            for (auto& e : stsd->kids)
            {
                if (! openEntry (e, 78)) { error = "cannot read the video's sample entry"; return false; }
                e.removeAll (fourcc ("st3d"));
                e.removeAll (fourcc ("sv3d"));
                Box st3d = leaf ("st3d", { 0, 0, 0, 0, 0 });
                Box sv3d;
                sv3d.type = fourcc ("sv3d");
                sv3d.container = true;
                {
                    std::vector<uint8_t> svhd { 0, 0, 0, 0 };
                    for (const char* c = "Rysum"; *c != 0; ++c) svhd.push_back ((uint8_t) *c);
                    svhd.push_back (0);
                    sv3d.kids.push_back (leaf ("svhd", std::move (svhd)));
                    Box proj;
                    proj.type = fourcc ("proj");
                    proj.container = true;
                    proj.kids.push_back (leaf ("prhd", std::vector<uint8_t> (16, 0)));
                    proj.kids.push_back (leaf ("equi", std::vector<uint8_t> (20, 0)));
                    sv3d.kids.push_back (std::move (proj));
                }
                // (after the codec's own boxes, before the optional ones such as pasp and clap)
                auto at = std::find_if (e.kids.begin(), e.kids.end(), [] (const Box& b) { return b.type == fourcc ("pasp") || b.type == fourcc ("clap"); });
                const auto pos = at - e.kids.begin();
                e.kids.insert (e.kids.begin() + pos, std::move (sv3d));
                e.kids.insert (e.kids.begin() + pos, std::move (st3d));
            }
        }
        else if (h == fourcc ("soun") && ! sound && stsd != nullptr)
        {
            sound = true;
            for (auto& e : stsd->kids)
            {
                const size_t fixed = soundFixed (e);
                if (fixed == 0 || ! openEntry (e, fixed)) { error = "cannot read the sound's sample entry"; return false; }
                const int ch = soundChannels (e);
                if (ch != 4) { error = "the sound has " + juce::String (ch) + " channels; first-order Ambisonics needs 4"; return false; }
                e.removeAll (fourcc ("SA3D"));
                std::vector<uint8_t> b;
                b.push_back (0);          // version
                b.push_back (0);          // periphonic (no head-locked stereo)
                put32 (b, 1);             // order
                b.push_back (0);          // ACN
                b.push_back (0);          // SN3D
                put32 (b, 4);             // channels
                for (uint32_t c = 0; c < 4; ++c) put32 (b, c);       // as they lie: W Y Z X
                e.kids.push_back (leaf ("SA3D", std::move (b)));
            }
        }
    }
    if (! video) { error = "no video track"; return false; }
    if (! sound) { error = "no sound track"; return false; }

    // the media after the 'moov' moves by as much as it grew
    const uint64_t moovAt = top[moovIndex].offset, moovOld = top[moovIndex].size;
    const auto moovNew = (uint64_t) moov.size();
    if (moovNew > 0xffffffffull) { error = "the 'moov' box is too large"; return false; }
    const int64_t delta = (int64_t) moovNew - (int64_t) moovOld;
    if (delta != 0 && ! shiftChunks (moov, moovAt + moovOld, delta, error)) return false;
    std::vector<uint8_t> newMoov;
    newMoov.reserve ((size_t) moovNew);
    moov.write (newMoov);

    outFile.deleteFile();
    bool ok = true;
    {
        juce::FileOutputStream out (outFile, 1 << 20);
        if (! out.openedOk()) { error = "cannot write " + outFile.getFullPathName(); return false; }
        std::vector<uint8_t> buf ((size_t) 4 << 20);
        for (size_t i = 0; i < top.size() && ok; ++i)
        {
            if (i == moovIndex) { ok = out.write (newMoov.data(), newMoov.size()); continue; }
            in.setPosition ((juce::int64) top[i].offset);
            uint64_t left = top[i].size;
            while (left > 0 && ok)
            {
                if (shouldStop && shouldStop()) { error = "stopped"; ok = false; break; }
                const int n = (int) juce::jmin<uint64_t> (left, buf.size());
                if (in.read (buf.data(), n) != n) { error = "cannot read " + inFile.getFullPathName(); ok = false; break; }
                if (! out.write (buf.data(), (size_t) n)) { error = "cannot write " + outFile.getFullPathName() + " (the disk full?)"; ok = false; break; }
                left -= (uint64_t) n;
            }
        }
        if (ok) out.flush();
        if (ok && out.getStatus().failed()) { error = out.getStatus().getErrorMessage(); ok = false; }
    }
    if (! ok) outFile.deleteFile();
    return ok;
}

SpatialInfo readSpatialMetadata (const juce::File& file)
{
    SpatialInfo info;
    juce::FileInputStream in (file);
    if (! in.openedOk()) return info;
    std::vector<TopBox> top;
    std::vector<uint8_t> bytes;
    size_t mi = 0;
    juce::String err;
    Box moov;
    if (! scanFile (in, top, bytes, mi, err) || ! parseMoov (bytes, moov, err)) return info;
    for (auto& trak : moov.kids)
    {
        if (trak.type != fourcc ("trak")) continue;
        const uint32_t h = handlerOf (trak);
        auto* stsd = stsdOf (trak);
        if (stsd == nullptr) continue;
        if (h == fourcc ("vide"))
        {
            for (auto& k : trak.kids)
                if (k.type == fourcc ("uuid") && k.head.size() > 16 && std::memcmp (k.head.data(), kSphericalUuid, 16) == 0)
                    info.v1 = juce::String::fromUTF8 ((const char*) k.head.data() + 16, (int) k.head.size() - 16).contains ("equirectangular");
            for (auto& e : stsd->kids)
                if (openEntry (e, 78))
                    if (auto* sv = e.child (fourcc ("sv3d")))
                        if (auto* proj = sv->child (fourcc ("proj")))
                            info.v2 = e.child (fourcc ("st3d")) != nullptr && proj->child (fourcc ("equi")) != nullptr;
        }
        else if (h == fourcc ("soun"))
        {
            for (auto& e : stsd->kids)
            {
                info.soundFormat = juce::String::charToString ((juce::juce_wchar) (e.type >> 24)) + juce::String::charToString ((juce::juce_wchar) ((e.type >> 16) & 255))
                                 + juce::String::charToString ((juce::juce_wchar) ((e.type >> 8) & 255)) + juce::String::charToString ((juce::juce_wchar) (e.type & 255));
                info.soundChannels = soundChannels (e);
                const size_t fixed = soundFixed (e);
                if (fixed > 0 && openEntry (e, fixed))
                    if (auto* sa = e.child (fourcc ("SA3D")))
                        if (sa->head.size() >= 12)
                        {
                            info.sa3d = true;
                            info.sa3dOrder = (int) be32 (sa->head.data() + 2);
                            info.sa3dChannels = (int) be32 (sa->head.data() + 8);
                        }
            }
        }
    }
    return info;
}
} // namespace rysum
