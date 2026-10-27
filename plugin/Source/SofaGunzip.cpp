// inflate for libmysofa (HDF5 "deflate" chunks are zlib streams), done with JUCE's zlib,
// so the plugin needs no separate zlib library. See ThirdParty/libmysofa/hdf/gunzip_rysum.c.
#include <juce_core/juce_core.h>

extern "C" int (*rysum_mysofa_inflate) (int inlen, char* in, int* outlen, char* out);

static int rysumInflate (int inlen, char* in, int* outlen, char* out)
{
    if (in == nullptr || out == nullptr || outlen == nullptr || inlen < 0 || *outlen < 0) return -1;
    juce::MemoryInputStream src (in, (size_t) inlen, false);
    juce::GZIPDecompressorInputStream gz (&src, false, juce::GZIPDecompressorInputStream::zlibFormat);
    int total = 0;
    while (total < *outlen)
    {
        const int got = gz.read (out + total, *outlen - total);
        if (got <= 0) break;
        total += got;
    }
    *outlen = total;
    return total > 0 ? 0 : -3;     // -3 = Z_DATA_ERROR
}

namespace rysum
{
    void installSofaInflate() { rysum_mysofa_inflate = &rysumInflate; }
}
