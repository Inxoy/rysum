# Building Rysum on macOS

Prepared 2026-11-07 for the `plainsound/macos` branch.

This is a guided build plan. Source changes and full build/runtime checks below are pending unless explicitly marked complete. Keep musical changes, release-sample work and DSP refactoring separate from the initial Mac port.

## Current findings

- Main plugin code is C++17, using JUCE 8.0.12 and CMake 3.22 or newer. Supporting libraries include C sources.
- `plugin/CMakeLists.txt` is the current build: instrument VST3, standalone application, optional FX VST3, render/UI checks and CPU benchmark.
- The root `CMakeLists.txt` now delegates to `plugin/`. Older root `Source`, `Resources` and `Tests` copies are preserved in `_archive/legacy-root/` and are not built.
- ASIO is guarded by `WIN32`; no Mac replacement for ASIO is required. JUCE supplies the platform audio/MIDI implementation.
- Native Apple Silicon processing already has suitable code paths: PFFFT supports ARM NEON, and the base kernels use portable loops. The baseline kernel label says "SSE2" even on ARM; that is a diagnostic label to correct later, not evidence that ARM uses Intel instructions.
- MTS-ESP's client loader already supports macOS. On the inspected machine, `/Library/Application Support/MTS-ESP/libMTS.dylib` is installed and includes arm64 and x86_64.
- Audio input buses are enabled, but the standalone app has no explicit microphone usage description in the CMake configuration.
- Samples are external. The existing `rysum-samples/rysum_samples.json` and WAVs are present; the application has a folder picker and portable search locations.
- Earlier syntax-only compiler checks passed for the MTS client, PFFFT, the base kernels, the Intel dispatch branch, and the libmysofa reader. These do not establish that the whole plugin builds or runs.

## Build concepts

**Source directory (`-S`)**: contains the CMake configuration and source files.

**Build directory (`-B`)**: contains generated files, downloaded dependencies, object files and compiled products. Keep it separate from source. A build directory remembers its source, generator and architecture; use a new directory when changing those fundamentals.

**Configure** (`cmake -S ... -B ...`): discovers tools and dependencies, evaluates CMake and generates build rules. JUCE may build helper tools during configuration.

**Compile/link** (`cmake --build ...`): compiles source into object files and links the application/plugin. CMake invokes the selected generator's build tool.

**JUCE formats**: the shared instrument code is wrapped as a standalone `.app`, VST3 `.vst3`, or optional AU `.component`. These are macOS bundles: keep each whole directory intact.

**Git branch versus dev product**: `plainsound/macos` separates source changes. `RYSUM_DEV=ON` creates the separately identified Rysum Dev product; these are independent choices.

## Step 1 — use one CMake configuration

Replace the obsolete root `CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.22)

project(RysumWorkspace LANGUAGES C CXX)

# The maintained plugin configuration lives here.
add_subdirectory(plugin)
```

The root project gives CMake a workspace entry point; the plugin subdirectory keeps its existing `project(RYSUM VERSION 0.1.0 ...)`, targets, resources and options. Paths inside that file continue to resolve relative to `plugin/`.

Do not paste the root file's old source lists into `plugin/CMakeLists.txt`. The older directories have been archived under `_archive/legacy-root/`. Existing Windows commands that configure `plugin/` continue to use the same build configuration directly.

## Step 2 — declare standalone audio-input permission

Inside `juce_add_plugin(Rysum ...)` in `plugin/CMakeLists.txt`, before its closing parenthesis, add:

```cmake
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Rysum uses audio input for live sound processing."
```

These are JUCE target attributes; they generate the macOS usage-description entry. Audio input still requires user approval when the application requests access. A microphone usage description applies to audio-interface input as well as a built-in microphone.

A plugin loaded into a host uses the host application's input permissions. This addition prepares Rysum's own standalone app. Hardened runtime audio-input entitlements are a separate later step.

## Step 3 — choose initial formats and architecture

Initially retain `FORMATS VST3 Standalone` and build native `arm64` only. Keep `RYSUM_BUILD_FX=OFF` for the first build. This reduces build time and leaves fewer outputs to diagnose.

No immediate change to the AVX2 section is needed for native arm64. Its current condition excludes builds whose architecture list contains `arm64`. Before an Intel or universal release, explicitly check cross-build architecture selection and the resulting binaries. Do not enable AVX2 globally.

AU is optional and is needed for Logic. After the first VST3/standalone build works, introduce an Apple-only format option/list and add AU for the relevant instrument/FX target. Do not mix AU validation into diagnosing the initial build.

## Step 4 — keep the dependency pin reproducible

Retain JUCE 8.0.12 for the first Mac port. Changing both platform and JUCE version would make failures harder to attribute.

Small portability correction:

```cmake
add_subdirectory("${RYSUM_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
```

This replaces the existing unquoted local-JUCE call, so paths containing spaces work.

The download path already pins a release ZIP. Add a verified SHA-256 `URL_HASH` later for stronger reproducibility; calculate it from the actual pinned archive rather than inserting a guessed value.

## Step 5 — inspect the installed tools

From a terminal:

```sh
xcode-select -p
xcrun --sdk macosx --show-sdk-version
clang++ --version
cmake --version
ninja --version
uname -m
```

The earlier inspection found Xcode selected and an arm64 machine. Recheck when building. Use Ninja if installed; Xcode is another generator, but each needs its own build directory.

For the first local build, use the current SDK's default deployment target. Before distributing to older Macs, choose and test an explicit minimum macOS version. Do not mistake the SDK version for the minimum supported OS.

## Step 6 — configure and build the instrument

The following commands deliberately select the canonical `plugin/` source directly. They work before and after the root forwarding cleanup.

```sh
cd ~/Dev/rysum_plugin
cmake -S plugin -B plugin/build-mac-dev -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DRYSUM_DEV=ON \
  -DRYSUM_BUILD_FX=OFF

cmake --build plugin/build-mac-dev --target Rysum_Standalone --parallel 4
cmake --build plugin/build-mac-dev --target Rysum_VST3 --parallel 4
```

`Release` enables the optimisation needed for meaningful audio testing. `--parallel 4` limits simultaneous compiler work; adjust after observing memory use. The first configure downloads JUCE unless `RYSUM_JUCE_DIR` points to a matching local checkout.

Expected products for this Ninja configuration:

- `plugin/build-mac-dev/Rysum_artefacts/Release/Standalone/Rysum Dev.app`
- `plugin/build-mac-dev/Rysum_artefacts/Release/VST3/Rysum Dev.vst3`

Verify the actual output locations in the build log. CMake target names remain `Rysum_Standalone` and `Rysum_VST3`; `RYSUM_DEV` changes the product identity/name, not those target names.

`plugin/build*/` is already ignored by Git. No system plugin installation is needed to build or first test the standalone app.

## Step 7 — validate samples and audio

1. Launch Rysum Dev from the build output.
2. If automatic sample discovery fails, select `~/Dev/rysum_plugin/rysum-samples`, containing `rysum_samples.json`.
3. Select the intended Core Audio device and an initial buffer size such as 256 frames.
4. Test note-on/off, sustain and release tails with stereo and binaural output.
5. Test external audio input separately, including the macOS permission prompt.
6. Check state save/reload and that the dev product remains distinct from any stable installation.

For host testing, use a user plugin installation location or a host-supported custom scan path. Copy the whole `.vst3` bundle if installing; do not install just its executable. AU is a later optional build/validation step.

## Step 8 — validate tuning separately from compilation

### MIDI Tuning Standard SysEx

Use IAC to send Hexatone notes and tuning SysEx to the standalone app. Select Rysum's MTS tuning mode. Test tuning before note-on, changes during held notes with continuous tuning enabled/disabled, repeated notes and multiple channels. Host SysEx forwarding is host-dependent; validate the chosen host instead of assuming standalone results apply to every DAW.

### MTS-ESP master/client

Load a master and Rysum in a compatible host, confirm connection status, then change tuning. Check unmapped notes and held-note behaviour. The installed runtime library is not a build-time link requirement: the bundled client loads it dynamically.

Without a working master connection, the MTS client also supports local MIDI tuning SysEx. These are distinct paths and should have separate tests.

### MPE

Test per-note bend/pressure/slide and channel reuse independently of MTS. Test with and without free tuning; a successful build does not establish controller behaviour.

## Step 9 — run existing render checks

After the instrument builds:

```sh
cmake -S plugin -B plugin/build-mac-dev -DRYSUM_BUILD_TESTS=ON
cmake --build plugin/build-mac-dev --target RysumRenderTest --parallel 4
```

The existing cache retains the generator, architecture and dev settings. Run the generated render executable with the samples and output directory arguments documented in `plugin/tests/RenderTest.cpp`. On macOS, locate the executable in the generated artefacts rather than assuming a Windows `.exe` path.

The UI snapshot and CPU benchmark are additional targets. Establish functional correctness before drawing performance conclusions, then test several sample rates and buffer sizes.

## Later — signing, hardened runtime and distribution

These are not prerequisites for preparing the source or starting a local development build.

For a hardened standalone release, declare:

- `com.apple.security.device.audio-input` for audio capture.
- `com.apple.security.cs.disable-library-validation` as required by ODDSound for loading the external MTS-ESP runtime library.

Use JUCE's `HARDENED_RUNTIME_ENABLED` / `HARDENED_RUNTIME_OPTIONS` attributes as part of an explicit signing/notarisation configuration. Do not enable the app sandbox as an incidental porting change. Hosted plugin library loading also depends on the host process's policy.

Select signing identity, minimum macOS version, architectures, sample installation layout and package contents when preparing distribution. Preserve stable plugin IDs and avoid putting local sample paths or signing credentials in shared source.

## Progress

- [x] Review current build and platform-related source.
- [x] Confirm macOS MTS loader and installed universal runtime library.
- [x] Perform limited syntax-only checks of supporting components.
- [x] Replace root duplicate with forwarding configuration.
- [ ] Add standalone microphone usage description.
- [ ] Quote optional local-JUCE paths.
- [ ] Configure native arm64 dev build.
- [ ] Build and test standalone/VST3.
- [ ] Validate MTS SysEx, MTS-ESP and MPE independently.
- [ ] Run render checks and measure CPU.
- [ ] Optional AU, Intel/universal and release packaging.

## References

- JUCE 8.0.12 CMake API: https://github.com/juce-framework/JUCE/blob/8.0.12/docs/CMake%20API.md
- ODDSound MTS-ESP client and macOS library-validation guidance: https://github.com/ODDSound/MTS-ESP/blob/main/README.md
- Existing Rysum instructions: `plugin/README.md`.
- Related architecture review: `docs/STRUCTURE_AND_RELEASE_REVIEW.md`.
