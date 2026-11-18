#include "Lang.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rysum
{
namespace
{
struct Entry
{
    const char *de;
    const char *en;
};

const Entry kTable[]{
#include "LangTable.inc"
};

const std::unordered_map<std::string, const char *> &table()
{
    static const std::unordered_map<std::string, const char *> map = []
    {
        std::unordered_map<std::string, const char *> m;
        m.reserve(sizeof(kTable) / sizeof(kTable[0]) * 2);
        for (const auto &e : kTable)
            m.emplace(e.de, e.en);
        return m;
    }();
    return map;
}

juce::File settingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Rysum")
        .getChildFile("Sprache.xml");
}

bool forced()
{
    static const bool f = juce::SystemStats::getEnvironmentVariable("RYSUM_LANG", {}).isNotEmpty();
    return f;
}

Language fromCode(const juce::String &c, Language fallback)
{
    const auto s = c.trim().toLowerCase();
    if (s.startsWith("de"))
        return Language::German;
    if (s.startsWith("en"))
        return Language::English;
    return fallback;
}

std::atomic<int> &current()
{
    static std::atomic<int> lang{[]
                                 {
                                     const auto env = juce::SystemStats::getEnvironmentVariable(
                                         "RYSUM_LANG", {});
                                     if (env.isNotEmpty())
                                         return (int)fromCode(env, Language::English);
                                     if (auto xml = juce::parseXML(settingsFile()))
                                         return (int)fromCode(xml->getStringAttribute("lang"),
                                                              Language::English);
                                     return (int)Language::English;
                                 }()};
    return lang;
}

void logMiss(const std::string &key)
{
    static const juce::String path = juce::SystemStats::getEnvironmentVariable("RYSUM_TR_LOG", {});
    if (path.isEmpty())
        return;
    // only what looks like words (numbers, units and symbols need no entry)
    int letters = 0;
    for (unsigned char c : key)
        if (std::isalpha(c) || c >= 0x80)
            ++letters;
    if (letters < 2)
        return;
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.insert(key).second)
        return;
    juce::File(path).appendText(juce::String::fromUTF8(key.c_str()) + "\n", false, false, "\n");
}

juce::String lookup(const std::string &key)
{
    const auto &t = table();
    auto it = t.find(key);
    if (it != t.end())
        return juce::String::fromUTF8(it->second);
    logMiss(key);
    return juce::String::fromUTF8(key.c_str(), (int)key.size());
}
} // namespace

Language language() noexcept
{
    return (Language)current().load(std::memory_order_relaxed);
}

const char *languageCode() noexcept
{
    return german() ? "de" : "en";
}

void setLanguage(Language l)
{
    current().store((int)l);
    if (forced())
        return;
    juce::XmlElement xml("Sprache");
    xml.setAttribute("lang", l == Language::German ? "de" : "en");
    const auto f = settingsFile();
    f.getParentDirectory().createDirectory();
    xml.writeTo(f);
}

juce::String tr(const char *germanUtf8)
{
    if (germanUtf8 == nullptr)
        return {};
    if (german())
        return juce::String::fromUTF8(germanUtf8);
    return lookup(germanUtf8);
}

juce::String tr(const juce::String &german)
{
    if (rysum::german() || german.isEmpty())
        return german;
    const auto &t = table();
    auto it = t.find(german.toStdString());
    if (it != t.end())
        return juce::String::fromUTF8(it->second);
    logMiss(german.toStdString());
    return german;
}

juce::String tr(const char *germanUtf8, const char *context)
{
    if (german() || context == nullptr)
        return tr(germanUtf8);
    const auto &t = table();
    auto it = t.find(std::string(context) + "|" + germanUtf8);
    if (it != t.end())
        return juce::String::fromUTF8(it->second);
    return tr(germanUtf8);
}

bool hasTranslation(const juce::String &german)
{
    return table().count(german.toStdString()) > 0;
}

namespace
{
bool isLetter(unsigned char c)
{
    return std::isalnum(c) || c >= 0x80;
}

/** the pieces worth replacing inside longer texts, longest first */
const std::vector<std::pair<std::string, std::string>> &pieces()
{
    static const std::vector<std::pair<std::string, std::string>> list = []
    {
        std::vector<std::pair<std::string, std::string>> v;
        for (const auto &e : kTable)
        {
            const std::string de(e.de);
            int letters = 0;
            for (unsigned char c : de)
                if (isLetter(c))
                    ++letters;
            if (letters >= 3 && de.find('|') == std::string::npos)
                v.emplace_back(de, e.en);
        }
        std::stable_sort(v.begin(), v.end(), [](const auto &a, const auto &b)
                         { return a.first.size() > b.first.size(); });
        return v;
    }();
    return list;
}
} // namespace

juce::String trText(const juce::String &german)
{
    if (rysum::german() || german.isEmpty())
        return german;
    const std::string in = german.toStdString();
    if (auto it = table().find(in); it != table().end())
        return juce::String::fromUTF8(it->second);
    static std::mutex m;
    static std::unordered_map<std::string, juce::String> cache;
    std::lock_guard<std::mutex> lock(m);
    if (auto c = cache.find(in); c != cache.end())
        return c->second;
    // replace pieces on a copy; a replaced stretch is not looked at again (marked as taken)
    std::string s = in;
    std::vector<bool> taken(s.size(), false);
    for (const auto &[de, en] : pieces())
    {
        if (de.size() > s.size())
            continue;
        for (size_t at = s.find(de); at != std::string::npos; at = s.find(de, at + 1))
        {
            const size_t end = at + de.size();
            bool free = true;
            for (size_t k = at; k < end && free; ++k)
                free = !taken[k];
            // whole words only: no letter glued on before or after a piece that starts / ends
            // with one
            if (!free || (isLetter((unsigned char)de.front()) && at > 0 &&
                          isLetter((unsigned char)s[at - 1])) ||
                (isLetter((unsigned char)de.back()) && end < s.size() &&
                 isLetter((unsigned char)s[end])))
                continue;
            s.replace(at, de.size(), en);
            std::vector<bool> t2(s.size(), false);
            for (size_t k = 0; k < at; ++k)
                t2[k] = taken[k];
            for (size_t k = at; k < at + std::strlen(en.c_str()); ++k)
                t2[k] = true;
            for (size_t k = end; k < taken.size(); ++k)
                t2[k - de.size() + en.size()] = taken[k];
            taken.swap(t2);
            at += en.size() - 1;
        }
    }
    if (cache.size() > 512)
        cache.clear();
    auto out = juce::String::fromUTF8(s.c_str(), (int)s.size());
    cache.emplace(in, out);
    return out;
}
} // namespace rysum
