#pragma once

#include <juce_core/juce_core.h>

/** The language of the interface: English (the default) or German.

    Every text of the interface is written in German in the source; tr() looks up its English in
    one table (LangTable.inc, keyed by the German text as it is in the source). A text with no
    entry stays as it is. The choice is kept per computer in Documents/Rysum/Sprache.xml;
    RYSUM_LANG=de|en forces one for this run (and then nothing is saved).

    Debugging: RYSUM_TR_LOG=<file> appends every text that was asked for in English and has no
    entry (once each). */
namespace rysum
{
enum class Language
{
    English,
    German
};

Language language() noexcept;
inline bool german() noexcept
{
    return language() == Language::German;
}
/** switch the language and remember it on this computer */
void setLanguage(Language);
/** the code of the current language, "en" / "de" */
const char *languageCode() noexcept;

/** the interface text in the current language (key: the German text, UTF-8) */
juce::String tr(const char *germanUtf8);
juce::String tr(const juce::String &german);
/** a German text with two meanings: the entry "context|German" first, then the plain one */
juce::String tr(const char *germanUtf8, const char *context);
/** a text put together from pieces (status lines: "287 Pfeifen geladen, 2 ersetzt"): the whole
    entry if there is one, else every known piece in it is replaced (longest first, whole words) */
juce::String trText(const juce::String &german);
/** true when the text has an English entry */
bool hasTranslation(const juce::String &german);
} // namespace rysum
