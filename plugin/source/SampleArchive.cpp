#include "SampleArchive.h"
#include <map>

namespace rysum::SampleArchive
{
namespace
{
constexpr char kMagic[8]{'R', 'Y', 'S', 'U', 'M', 'P', 'K', '1'};
constexpr uint64_t kKey = 0x52797375d0c6a7e5ull; // (the program's own: the archive opens only here)

/** the scrambling: a xorshift64* stream from the key and the archive's own seed, byte by byte */
struct Stream
{
    uint64_t s;
    explicit Stream(uint32_t seed) : s(kKey ^ ((uint64_t)seed * 0x9e3779b97f4a7c15ull) ^ 0x2545f4914f6cdd1dull)
    {
        if (s == 0)
            s = 1;
    }
    void apply(uint8_t *p, size_t n)
    {
        uint64_t word = 0;
        int left = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (left == 0)
            {
                s ^= s >> 12;
                s ^= s << 25;
                s ^= s >> 27;
                word = s * 0x2545f4914f6cdd1dull;
                left = 8;
            }
            p[i] ^= (uint8_t)word;
            word >>= 8;
            --left;
        }
    }
};

juce::String keyOf(const juce::File &f)
{
    return f.getFullPathName().replaceCharacter('\\', '/').toLowerCase();
}

juce::CriticalSection &lock()
{
    static juce::CriticalSection l;
    return l;
}
std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> &files()
{
    static std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> m;
    return m;
}
juce::StringArray &mounted()
{
    static juce::StringArray m;
    return m;
}
} // namespace

juce::File archiveFor(const juce::File &folder)
{
    return juce::File(folder.getFullPathName() + ".rysum");
}

bool pack(const juce::File &folder, const juce::File &out, juce::String &error,
          const std::function<void(const juce::String &)> &log)
{
    if (!folder.getChildFile("rysum_samples.json").existsAsFile())
    {
        error = "no rysum_samples.json in " + folder.getFullPathName();
        return false;
    }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    juce::FlacAudioFormat flac;
    juce::MemoryOutputStream body;
    int count = 0;
    int64_t raw = 0;
    for (const auto &e : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
    {
        const auto f = e.getFile();
        if (f.hasFileExtension("png;jpg;jpeg"))
            continue;
        const auto rel = f.getRelativePathFrom(folder).replaceCharacter('\\', '/');
        juce::MemoryBlock data;
        uint8_t kind = 0;
        raw += f.getSize();
        if (f.hasFileExtension("wav"))
        {
            // the audio as FLAC: the same samples, rate, channels and bits (16 or 24)
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            if (r == nullptr || r->lengthInSamples <= 0 || r->lengthInSamples > (1 << 26))
            {
                error = "cannot read " + rel;
                return false;
            }
            const int bits = r->bitsPerSample <= 16 ? 16 : 24;
            const int ch = (int)r->numChannels;
            const int len = (int)r->lengthInSamples;
            // (as integers, left-justified: bit for bit - a float would turn -32768 into -32767)
            std::vector<std::vector<int>> ints((size_t)ch, std::vector<int>((size_t)len));
            std::vector<int *> dst((size_t)ch);
            for (int c = 0; c < ch; ++c)
                dst[(size_t)c] = ints[(size_t)c].data();
            if (r->usesFloatingPointData || !r->read(dst.data(), ch, 0, len, false))
            {
                error = "not integer PCM: " + rel;
                return false;
            }
            {
                auto *os = new juce::MemoryOutputStream(data, false);
                std::unique_ptr<juce::AudioFormatWriter> w(
                    flac.createWriterFor(os, r->sampleRate, (unsigned int)ch, bits, {}, 8));
                if (w == nullptr)
                {
                    delete os;
                    error = "cannot encode " + rel;
                    return false;
                }
                std::vector<const int *> srcs(dst.begin(), dst.end());
                srcs.push_back(nullptr);
                w->write(srcs.data(), len);
            }
            kind = 1;
        }
        else if (!f.loadFileAsData(data))
        {
            error = "cannot read " + rel;
            return false;
        }
        const auto name = rel.toStdString();
        body.writeInt((int)name.size());
        body.write(name.data(), name.size());
        body.writeByte((char)kind);
        body.writeInt64((juce::int64)data.getSize());
        body.write(data.getData(), data.getSize());
        ++count;
        if (log && count % 50 == 0)
            log(juce::String(count) + " files ...");
    }
    body.writeInt(0);
    auto bytes = body.getMemoryBlock();
    const auto seed = (uint32_t)juce::Random::getSystemRandom().nextInt();
    Stream(seed).apply(static_cast<uint8_t *>(bytes.getData()), bytes.getSize());
    out.deleteFile();
    juce::FileOutputStream os(out);
    if (!os.openedOk())
    {
        error = "cannot write " + out.getFullPathName();
        return false;
    }
    os.write(kMagic, 8);
    os.writeInt((int)seed);
    os.write(bytes.getData(), bytes.getSize());
    os.flush();
    if (log)
        log(juce::String(count) + " files, " + juce::String(raw / 1048576) + " MB -> " +
            juce::String((int64_t)bytes.getSize() / 1048576) + " MB");
    return !os.getStatus().failed();
}

bool mount(const juce::File &folder, juce::String &error)
{
    const auto arc = archiveFor(folder);
    {
        const juce::ScopedLock sl(lock());
        if (mounted().contains(keyOf(folder)))
            return true;
    }
    juce::MemoryBlock all;
    if (!arc.existsAsFile() || !arc.loadFileAsData(all) || all.getSize() < 16 ||
        std::memcmp(all.getData(), kMagic, 8) != 0)
    {
        error = "no sample archive at " + arc.getFullPathName();
        return false;
    }
    auto *p = static_cast<uint8_t *>(all.getData());
    const uint32_t seed = (uint32_t)juce::ByteOrder::littleEndianInt(p + 8);
    Stream(seed).apply(p + 12, all.getSize() - 12);
    juce::MemoryInputStream in(p + 12, all.getSize() - 12, false);
    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> got;
    juce::FlacAudioFormat flac;
    for (;;)
    {
        const int nameLen = in.readInt();
        if (nameLen == 0)
            break;
        if (nameLen < 0 || nameLen > 1024 || in.getNumBytesRemaining() < nameLen + 9)
        {
            error = "sample archive broken: " + arc.getFileName();
            return false;
        }
        juce::MemoryBlock name((size_t)nameLen);
        in.read(name.getData(), nameLen);
        const int kind = in.readByte();
        const auto size = in.readInt64();
        if (size < 0 || size > in.getNumBytesRemaining())
        {
            error = "sample archive broken: " + arc.getFileName();
            return false;
        }
        auto data = std::make_shared<juce::MemoryBlock>((size_t)size);
        in.read(data->getData(), (int)size);
        juce::ignoreUnused(kind); // (FLAC is recognised by the reader itself)
        const auto rel = juce::String::fromUTF8(static_cast<const char *>(name.getData()), nameLen);
        if (rel.contains("..") || rel.startsWithChar('/'))
            continue;
        got[keyOf(folder.getChildFile(rel))] = std::move(data);
    }
    const juce::ScopedLock sl(lock());
    for (auto &kv : got)
        files()[kv.first] = kv.second;
    mounted().add(keyOf(folder));
    return true;
}

std::shared_ptr<const juce::MemoryBlock> find(const juce::File &f)
{
    const juce::ScopedLock sl(lock());
    const auto it = files().find(keyOf(f));
    return it != files().end() ? it->second : nullptr;
}

bool exists(const juce::File &f)
{
    return f.existsAsFile() || find(f) != nullptr;
}

std::unique_ptr<juce::AudioFormatReader> openAudio(juce::AudioFormatManager &fm, const juce::File &f)
{
    if (auto b = find(f))
    {
        // (the block stays in the table for the program's life: the stream may point into it)
        auto *in = new juce::MemoryInputStream(*b, false);
        if (auto *r = fm.createReaderFor(std::unique_ptr<juce::InputStream>(in)))
            return std::unique_ptr<juce::AudioFormatReader>(r);
        return nullptr;
    }
    return std::unique_ptr<juce::AudioFormatReader>(fm.createReaderFor(f));
}
} // namespace rysum::SampleArchive
