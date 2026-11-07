libmysofa (https://github.com/hoene/libmysofa), BSD 3-clause licence (see LICENSE),
used by Rysum to read SOFA files (HRTFs). Unchanged sources, except:
  - hdf/gunzip.c is replaced by hdf/gunzip_rysum.c, which hands the inflate to JUCE's zlib (Source/SofaGunzip.cpp)
  - config.h and hrtf/mysofa_export.h are written by hand instead of being generated
