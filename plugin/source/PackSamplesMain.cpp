// RysumPack: packs a samples folder into the scrambled archive the plugin and the listener app read
//   RysumPack <samples folder> [<archive>]     (default archive: "<folder>.rysum")
#include "SampleArchive.h"
#include <iostream>

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cout << "RysumPack <samples folder> [<archive>]" << std::endl;
        return 2;
    }
    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[1]).unquoted());
    const auto out = argc > 2 ? juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[2]).unquoted())
                              : rysum::SampleArchive::archiveFor(folder);
    juce::String error;
    const bool ok = rysum::SampleArchive::pack(folder, out, error,
                                               [](const juce::String &s) { std::cout << s << std::endl; });
    if (!ok)
    {
        std::cout << "FAILED: " << error << std::endl;
        return 1;
    }
    std::cout << "wrote " << out.getFullPathName() << std::endl;
    return 0;
}
