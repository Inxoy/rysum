#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace rysum
{
/** Marks a QuickTime / MP4 video as a 360° video with spatial sound, as Google's Spatial Media Metadata
    Injector does it (github.com/google/spatial-media), so that YouTube plays it as one:
    - the video track: Spherical Video V1 (a 'uuid' box with the XML, the last child of the track's 'trak') and
      V2 ('st3d' mono and 'sv3d' equirectangular, in the video's sample entry);
    - the sound track: 'SA3D' (first-order Ambisonics, periphonic, ACN / SN3D, channels 0 1 2 3 as they lie -
      AmbiX: W Y Z X), at the end of the sound's sample entry. The sound must have four channels.
    Only the 'moov' box changes; the media data is copied as it is. Chunk offsets are shifted when 'moov' lies
    before the media (a "fast start" file). 'out' is written anew (never 'in' itself). */
bool injectSpatialMetadata (const juce::File& in, const juce::File& out, juce::String& error,
                            std::function<bool()> shouldStop = {});

/** what a file says about itself (for the tests and to tell the user) */
struct SpatialInfo
{
    bool v1 = false, v2 = false;       // the spherical XML / st3d + sv3d with equi
    bool sa3d = false;
    int sa3dOrder = -1, sa3dChannels = 0;
    int soundChannels = 0;
    juce::String soundFormat;          // the sample entry's type (lpcm, mp4a, ...)
};
SpatialInfo readSpatialMetadata (const juce::File& file);
} // namespace rysum
