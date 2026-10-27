/* Rysum: replaces gunzip.c. The inflate itself is done with JUCE's zlib (Source/SofaGunzip.cpp),
   which sets this pointer before a SOFA file is read. */
#include "reader.h"

int (*rysum_mysofa_inflate) (int inlen, char *in, int *outlen, char *out) = 0;

int gunzip (int inlen, char *in, int *outlen, char *out)
{
  return rysum_mysofa_inflate != 0 ? rysum_mysofa_inflate (inlen, in, outlen, out) : -1;
}
