PFFFT - "a pretty fast FFT" by Julien Pommier (FFTPACK licence, BSD-like: see LICENSE / the header of pffft.c).
Single-file version as of commit 3673ac0 of https://github.com/marton78/pffft (the original two files
pffft.c / pffft.h, with 64-bit ARM support). Only changes: #include <stdint.h> (uintptr_t) and, for MSVC, #include <malloc.h> (_alloca) added.

Used by Source/FastFFT.h for all real-time convolutions (impulse response, binaural decoder, simulated room,
Lucier). On Windows JUCE's own FFT is a plain scalar implementation; PFFFT uses SSE (x86) / NEON (ARM).
