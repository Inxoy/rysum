# Building Rysum on macOS

The native Apple Silicon development build is implemented and in use on
`plainsound/sample-tuning`. Standalone and VST3 builds, the three CTest targets,
and the offline audio regression suite passed after merging upstream's room-motion
changes on 11 November 2026.

## Current configuration

- C++17, CMake 3.22 or newer, and JUCE 8.0.12.
- The root `CMakeLists.txt` forwards to the maintained configuration in `plugin/`.
  Source, resources, tests and dependencies live in `plugin/source/`,
  `plugin/resources/`, `plugin/tests/` and `plugin/third-party/`.
- Instrument formats are VST3 and Standalone. The optional FX target builds VST3.
- `RYSUM_DEV=ON` builds Rysum Dev with a separate bundle identifier and plugin code.
- ASIO is Windows-only. JUCE provides Core Audio and MIDI support on macOS.
- Standalone microphone permission and its usage description are configured.
- `RYSUM_JUCE_DIR` supports a local JUCE checkout, including paths with spaces.
  Otherwise configuration downloads the pinned JUCE release ZIP.
- Samples are external. Select the folder containing `rysum_samples.json`,
  normally `rysum-samples/` in this checkout.
- The bundled MTS-ESP client supports macOS and loads the external MTS runtime
  dynamically. The runtime is installed with an MTS-ESP master.

The portable kernels and PFFFT's ARM path support native arm64 builds. The
baseline kernel diagnostic still uses the label “SSE2” on ARM.

## Configure and build

Install Xcode command-line tools, CMake and Ninja. Confirm the selected tools
when setting up another machine.

```sh
xcode-select -p
xcrun --sdk macosx --show-sdk-version
clang++ --version
cmake --version
ninja --version
uname -m
```

From the repository root, configure a native arm64 development build.

```sh
cmake -S plugin -B plugin/build-mac-dev -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DRYSUM_DEV=ON \
  -DRYSUM_BUILD_FX=OFF \
  -DRYSUM_BUILD_TESTS=ON

cmake --build plugin/build-mac-dev --parallel 4
```

To rebuild just the playable instrument formats, use their target names.

```sh
cmake --build plugin/build-mac-dev \
  --target Rysum_Standalone Rysum_VST3 --parallel 4
```

The Ninja Release products are:

- `plugin/build-mac-dev/Rysum_artefacts/Release/Standalone/Rysum Dev.app`
- `plugin/build-mac-dev/Rysum_artefacts/Release/VST3/Rysum Dev.vst3`

CMake target names stay `Rysum_Standalone` and `Rysum_VST3` for the development
product. Keep a bundle's whole directory together when copying it to a host's
plugin search path. Build directories are ignored by Git.

A build directory records its source directory, generator and architectures.
Use a separate build directory when changing those choices.

## Run the checks

CTest runs `sample_pitch`, `dsf_integrator` and `dsf_renderer`.

```sh
ctest --test-dir plugin/build-mac-dev --output-on-failure
```

The broader audio regression program is a separate executable. It loads the
external sample library and writes listening renders to the requested directory.

```sh
cmake --build plugin/build-mac-dev --target RysumRenderTest --parallel 4

plugin/build-mac-dev/RysumRenderTest_artefacts/Release/RysumRenderTest \
  "$PWD/rysum-samples" /tmp/rysum-test-renders
```

The full run also writes temporary fixtures under
`~/Library/Caches/RysumRenderTest`. Its current suite covers tuning, held-note
changes, MPE, input, spatial processing, room motion and impact sounds. The
latest merged build passed all 353 audio assertions.

`RysumUiSnapshot` is an additional GUI test target. The CPU benchmark is enabled
separately with `RYSUM_BUILD_BENCH=ON`.

## Playback and controller checks

Standalone and VST3 playback have been tested during development. When changing
the host, audio interface or controller, check the relevant paths again.

1. Open Rysum Dev and select the sample folder if discovery does not find it.
2. Choose the Core Audio device and buffer size.
3. Check note-on/off, sustain, releases and the selected output format.
4. For live input, grant the standalone app audio-input access. A hosted plugin
   uses its host's permissions.
5. Check project save/reload and the setting for loading tuning with a preset.

For MIDI Tuning Standard SysEx, route notes and tuning messages from Hexatone
through an IAC port and select the MTS tuning mode. Host SysEx forwarding varies,
so test the actual routing used in the DAW.

For MTS-ESP, run a master and Rysum in the host, confirm connection status, and
check both new and held notes. The external runtime library is commonly located
at `/Library/Application Support/MTS-ESP/libMTS.dylib`. Without an active master,
the MTS mode can receive local MIDI tuning SysEx.

MPE has its own enable setting and bend range. Check member-channel bend,
pressure, slide and channel reuse with the intended controller. Offline tests
cover these paths; physical-device and host behaviour also need listening checks.

## Distribution work

AU support, Intel/universal validation, an explicit supported minimum macOS
version, Developer ID signing, hardened runtime, notarisation and packaging
remain distribution tasks.

A hardened standalone release needs audio-input entitlement for capture.
ODDSound documents the library-validation requirements for loading its external
MTS-ESP runtime. Choose and test the release entitlements with the intended
signing and host setup.

The JUCE download uses a versioned ZIP; adding a verified archive SHA-256 remains
a reproducibility improvement.

## Related documentation

- [Implemented Testton controls](DSF_TEST_TONE.md).
- [Ongoing sample tuning work](in-progress/SAMPLE_TUNING_IMPLEMENTATION_PLAN.md).
- [Structure review](STRUCTURE_REVIEW.md).
- [JUCE 8.0.12 CMake API](https://github.com/juce-framework/JUCE/blob/8.0.12/docs/CMake%20API.md).
- [MTS-ESP integration guidance](https://github.com/ODDSound/MTS-ESP/blob/main/README.md).
