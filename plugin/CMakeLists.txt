cmake_minimum_required(VERSION 3.22)

project(RYSUM VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE
#   By default JUCE 8.0.12 is downloaded once as a zip (no git needed).
#   If you already have JUCE somewhere, configure with
#       cmake -B build -DRYSUM_JUCE_DIR=C:/path/to/JUCE
# ---------------------------------------------------------------------------
set(RYSUM_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(RYSUM_JUCE_DIR)
    add_subdirectory("${RYSUM_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.12.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
# A second, independent copy for trying out new versions next to the stable one:
#   cmake -B build-dev -G "Visual Studio 17 2022" -A x64 -DRYSUM_DEV=ON
# builds "Rysum Dev" with its own plugin ID, so both can be installed and used at once.
option(RYSUM_DEV "Build the side-by-side development version 'Rysum Dev'" OFF)
if(RYSUM_DEV)
    set(RYSUM_PRODUCT "Rysum Dev")
    set(RYSUM_CODE Rys2)
    set(RYSUM_FX_CODE Rys4)
    set(RYSUM_BUNDLE "org.rysum.organ.dev")
else()
    set(RYSUM_PRODUCT "Rysum")
    set(RYSUM_CODE Rys1)
    set(RYSUM_FX_CODE Rys3)
    set(RYSUM_BUNDLE "org.rysum.organ")
endif()
# The same plugin as an audio effect ("Rysum FX"): for audio tracks - its input is placed in space, MIDI from
# another track still plays the organ. Turn off with -DRYSUM_BUILD_FX=OFF to build only the instrument (faster).
option(RYSUM_BUILD_FX "Also build the effect version 'Rysum FX'" ON)

juce_add_plugin(Rysum
    PRODUCT_NAME "${RYSUM_PRODUCT}"
    COMPANY_NAME "Rysum"
    BUNDLE_ID "${RYSUM_BUNDLE}"
    PLUGIN_MANUFACTURER_CODE Rysm
    PLUGIN_CODE ${RYSUM_CODE}
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Spatial
    COPY_PLUGIN_AFTER_BUILD FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Rysum uses audio input for live sound processing.")

juce_add_binary_data(RysumData
    HEADER_NAME RysumBinaryData.h
    NAMESPACE RysumData
    SOURCES
        resources/IBMPlexSansCondensed-Regular.ttf
        resources/IBMPlexSansCondensed-SemiBold.ttf
        resources/IBMPlexMono-Regular.ttf
        resources/UnifrakturMaguntia-Book.ttf
        resources/RysumFell-Roman.ttf
        resources/RysumFell-Italic.ttf
        resources/k_praestant.jpg
        resources/k_praestant16.jpg
        resources/org_pipes.jpg
        resources/org_jamb_l.jpg
        resources/org_jamb_r.jpg
        resources/k_gedackt.jpg
        resources/k_mixtur.jpg
        resources/k_oktave2.jpg
        resources/k_oktave4.jpg
        resources/k_sesquialtera.jpg
        resources/k_trompete.jpg
        resources/knob_bare.jpg
        resources/keys.jpg
        resources/orgel.txt
        resources/orgel_en.txt
        resources/wall.jpg
        resources/grain.png
        resources/fabian_hrir.bin
        resources/vox_humana.nrv2)

# libmysofa (BSD-3): reads SOFA files (HRTFs) for the binaural mode. Its one zlib call goes to JUCE's zlib
# (hdf/gunzip_rysum.c -> source/SofaGunzip.cpp), so nothing else is needed.
set(MYSOFA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libmysofa)
add_library(rysum_mysofa STATIC
    ${MYSOFA_DIR}/hrtf/reader.c ${MYSOFA_DIR}/hdf/superblock.c ${MYSOFA_DIR}/hdf/dataobject.c
    ${MYSOFA_DIR}/hdf/btree.c ${MYSOFA_DIR}/hdf/fractalhead.c ${MYSOFA_DIR}/hdf/gcol.c ${MYSOFA_DIR}/hdf/gunzip_rysum.c
    ${MYSOFA_DIR}/hrtf/check.c ${MYSOFA_DIR}/hrtf/spherical.c ${MYSOFA_DIR}/hrtf/lookup.c ${MYSOFA_DIR}/hrtf/tools.c
    ${MYSOFA_DIR}/hrtf/kdtree.c ${MYSOFA_DIR}/hrtf/neighbors.c ${MYSOFA_DIR}/hrtf/interpolate.c
    ${MYSOFA_DIR}/hrtf/resample.c ${MYSOFA_DIR}/hrtf/loudness.c ${MYSOFA_DIR}/hrtf/minphase.c
    ${MYSOFA_DIR}/hrtf/easy.c ${MYSOFA_DIR}/hrtf/cache.c ${MYSOFA_DIR}/resampler/speex_resampler.c)
set_target_properties(rysum_mysofa PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(rysum_mysofa PUBLIC ${MYSOFA_DIR}/hrtf PRIVATE ${MYSOFA_DIR} ${MYSOFA_DIR}/hdf)
if(MSVC)
    target_compile_definitions(rysum_mysofa PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rysum_mysofa PRIVATE /W0)
else()
    target_compile_options(rysum_mysofa PRIVATE -w)
endif()

# MTS-ESP client (ODDSound, 0BSD): microtuning from an MTS-ESP master plug-in, or MIDI Tuning Standard SysEx.
# It loads the MTS-ESP library at run time if it is installed; without it everything works, MTS-ESP just stays unconnected.
set(MTSESP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/MTS-ESP)
add_library(rysum_mtsesp STATIC ${MTSESP_DIR}/libMTSClient.cpp)
set_target_properties(rysum_mtsesp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rysum_mtsesp PUBLIC ${MTSESP_DIR})
if(MSVC)
    target_compile_options(rysum_mtsesp PRIVATE /W0)
else()
    target_compile_options(rysum_mtsesp PRIVATE -w)
    target_link_libraries(rysum_mtsesp PRIVATE ${CMAKE_DL_LIBS})
endif()

# PFFFT (FFTPACK licence, BSD-like): fast SIMD FFT for all real-time convolutions (IR, binaural, simulated room, Lucier).
# JUCE's own FFT is a plain scalar one on Windows.
set(PFFFT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pffft)
add_library(rysum_pffft STATIC ${PFFFT_DIR}/pffft.c)
set_target_properties(rysum_pffft PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(rysum_pffft PUBLIC ${PFFFT_DIR})
if(MSVC)
    target_compile_definitions(rysum_pffft PRIVATE _USE_MATH_DEFINES)
    target_compile_options(rysum_pffft PRIVATE /W0)
else()
    target_compile_options(rysum_pffft PRIVATE -w)
    target_link_libraries(rysum_pffft PRIVATE m)
endif()

# The innermost loops (source/Kernels.h), a second time compiled for AVX2 + FMA; the CPU decides at start-up which
# version runs, so the plugin still works on any x86-64 CPU. (That file is compiled without link-time optimisation,
# so none of its AVX2 code can be merged into the rest.)
set(RYSUM_KERNEL_SOURCES source/Kernels.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64|x64)$" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    list(APPEND RYSUM_KERNEL_SOURCES source/KernelsAVX2.cpp)
    set_source_files_properties(source/Kernels.cpp PROPERTIES COMPILE_DEFINITIONS RYSUM_HAVE_AVX2_KERNELS=1)
    if(MSVC)
        set_source_files_properties(source/KernelsAVX2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/GL-")
    else()
        set_source_files_properties(source/KernelsAVX2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-fno-lto")
    endif()
endif()

set(RYSUM_PLUGIN_SOURCES
    ${RYSUM_KERNEL_SOURCES}
    source/Lang.cpp
    source/SampleLibrary.cpp
    source/OrganEngine.cpp
    source/OrganLive.cpp
    source/OrganBodies.cpp
    source/Convolver.cpp
    source/Binaural.cpp
    source/RoomTail.cpp
    source/Lucier.cpp
    source/LucierPiece.cpp
    source/Speakers.cpp
    source/SofaGunzip.cpp
    source/PluginProcessor.cpp
    source/Audience.cpp
    source/ProcessorScenes.cpp
    source/ProcessorAutomation.cpp
    source/Concert.cpp
    source/PluginEditor.cpp
    source/ConsoleView.cpp
    source/SpaceView.cpp
    source/KlangView.cpp
    source/EffectsView.cpp
    source/SaalView.cpp
    source/MidiMapView.cpp
    source/OrganView.cpp
    source/SpeakerView.cpp
    source/Walk3D.cpp
    source/RoomModels.cpp
    source/WalkGpu.cpp
    source/WalkView.cpp
    source/WalkArrange.cpp
    source/WalkHands.cpp
    source/ModView.cpp
    source/VerlaufView.cpp)

function(rysum_setup_plugin target)
    target_sources(${target} PRIVATE ${RYSUM_PLUGIN_SOURCES})
    target_include_directories(${target} PRIVATE source)
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(${target}
        PRIVATE
            RysumData
            rysum_mysofa
            rysum_mtsesp
            rysum_pffft
            juce::juce_audio_utils juce::juce_opengl
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(${target} PRIVATE /MP /utf-8)
    endif()
endfunction()

rysum_setup_plugin(Rysum)

# ---------------------------------------------------------------------------
# ASIO for the standalone app (Windows): JUCE 8 brings Steinberg's ASIO headers (GPLv3 / Steinberg licence,
# fine for your own use). Turn off with -DRYSUM_ASIO=OFF.
# ---------------------------------------------------------------------------
option(RYSUM_ASIO "Standalone app with ASIO (Windows)" ON)
if(WIN32 AND RYSUM_ASIO)
    target_compile_definitions(Rysum PRIVATE JUCE_ASIO=1)
endif()

if(RYSUM_BUILD_FX)
    juce_add_plugin(RysumFX
        PRODUCT_NAME "${RYSUM_PRODUCT} FX"
        COMPANY_NAME "Rysum"
        BUNDLE_ID "${RYSUM_BUNDLE}.fx"
        PLUGIN_MANUFACTURER_CODE Rysm
        PLUGIN_CODE ${RYSUM_FX_CODE}
        FORMATS VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES Fx Spatial
        COPY_PLUGIN_AFTER_BUILD FALSE)
    rysum_setup_plugin(RysumFX)
endif()

# ---------------------------------------------------------------------------
# "Rysum Konzert": the app for listeners (source/ListenerApp.cpp) - the 3D Saal and a line to join a
# player, nothing to play with. The samples are copied next to it; the target RysumKonzertZip packs
# program + samples into one zip to hand out. Turn off with -DRYSUM_BUILD_KONZERT=OFF.
# ---------------------------------------------------------------------------
option(RYSUM_BUILD_KONZERT "Build the listener app 'Rysum Konzert'" ON)
if(RYSUM_BUILD_KONZERT)
    juce_add_gui_app(RysumKonzert PRODUCT_NAME "${RYSUM_PRODUCT} Konzert" COMPANY_NAME "Rysum"
        BUNDLE_ID "${RYSUM_BUNDLE}.konzert")
    target_sources(RysumKonzert PRIVATE source/ListenerApp.cpp ${RYSUM_PLUGIN_SOURCES})
    target_include_directories(RysumKonzert PRIVATE source)
    target_compile_definitions(RysumKonzert PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Build_Standalone=0
        JucePlugin_Name="${RYSUM_PRODUCT}" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    if(WIN32 AND RYSUM_ASIO)
        target_compile_definitions(RysumKonzert PRIVATE JUCE_ASIO=1)
    endif()
    target_link_libraries(RysumKonzert PRIVATE RysumData rysum_mysofa rysum_mtsesp rysum_pffft
        juce::juce_audio_utils juce::juce_opengl juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(RysumKonzert PRIVATE /MP /utf-8)
    endif()
    # the samples go with it: the app looks for "rysum-samples" next to itself first
    set(RYSUM_SAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rysum-samples" CACHE PATH "The sample library the listener app ships with")
    if(EXISTS "${RYSUM_SAMPLES_DIR}/rysum_samples.json")
        add_custom_command(TARGET RysumKonzert POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${RYSUM_SAMPLES_DIR}"
                    "$<TARGET_FILE_DIR:RysumKonzert>/rysum-samples"
            COMMENT "Copying the samples next to the listener app")
        add_custom_target(RysumKonzertZip
            COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/${RYSUM_PRODUCT} Konzert.zip" --format=zip
                    "$<TARGET_FILE_NAME:RysumKonzert>" rysum-samples
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:RysumKonzert>"
            DEPENDS RysumKonzert
            COMMENT "Packing the listener app with its samples")
    else()
        message(WARNING "Rysum Konzert: no samples at ${RYSUM_SAMPLES_DIR} - the app will look for them elsewhere")
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: headless render test (renders MIDI through the engine to WAV)
# ---------------------------------------------------------------------------
option(RYSUM_BUILD_TESTS "Build the offline render test" OFF)
if(RYSUM_BUILD_TESTS)
    enable_testing()

    add_executable(RysumPitchTest tests/SamplePitchTest.cpp)
    target_include_directories(RysumPitchTest PRIVATE source)
    target_link_libraries(RysumPitchTest PRIVATE juce::juce_recommended_warning_flags)
    add_test(NAME sample_pitch COMMAND RysumPitchTest)

    # Vox humana: the C++ engine against the PyTorch reference (files from NeuralRysum's
    # scripts/make_v2_reference.py; the test only runs when RYSUM_VOX_REFERENCE names one)
    add_executable(RysumVoxTest tests/VoxTest.cpp)
    target_include_directories(RysumVoxTest PRIVATE source)
    target_link_libraries(RysumVoxTest PRIVATE rysum_pffft)
    set(RYSUM_VOX_REFERENCE "" CACHE FILEPATH "reference vectors for the Vox humana test (.nrv2)")
    if(RYSUM_VOX_REFERENCE)
        add_test(NAME vox_humana COMMAND RysumVoxTest ${CMAKE_CURRENT_SOURCE_DIR}/resources/vox_humana.nrv2
                 ${RYSUM_VOX_REFERENCE})
    endif()

    add_executable(RysumDsfTest tests/DsfIntegratorTest.cpp)
    target_include_directories(RysumDsfTest PRIVATE source)
    target_link_libraries(RysumDsfTest PRIVATE juce::juce_recommended_warning_flags)
    add_test(NAME dsf_integrator COMMAND RysumDsfTest)

    juce_add_console_app(RysumDsfRendererTest PRODUCT_NAME "RysumDsfRendererTest")
    target_sources(RysumDsfRendererTest PRIVATE tests/DsfRendererTest.cpp)
    target_include_directories(RysumDsfRendererTest PRIVATE source)
    target_compile_definitions(RysumDsfRendererTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(RysumDsfRendererTest PRIVATE
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_test(NAME dsf_renderer COMMAND RysumDsfRendererTest)

    juce_add_console_app(RysumRenderTest PRODUCT_NAME "RysumRenderTest")
    target_sources(RysumRenderTest PRIVATE
        tests/RenderTest.cpp
        ${RYSUM_KERNEL_SOURCES}
        source/Lang.cpp
        source/SampleLibrary.cpp
        source/OrganEngine.cpp
        source/OrganLive.cpp
        source/OrganBodies.cpp
        source/Convolver.cpp
        source/Binaural.cpp
        source/RoomTail.cpp
        source/Lucier.cpp
        source/LucierPiece.cpp
        source/Speakers.cpp
        source/SofaGunzip.cpp)
    target_include_directories(RysumRenderTest PRIVATE source)
    target_compile_definitions(RysumRenderTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(RysumRenderTest PRIVATE RysumData rysum_mysofa rysum_pffft
        juce::juce_audio_basics juce::juce_audio_formats juce::juce_core juce::juce_events juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(MSVC)
        # the tests keep many engines on the stack of main(): more than the 1 MB a Windows program gets by default
        target_link_options(RysumRenderTest PRIVATE /STACK:268435456)
    endif()

    juce_add_gui_app(RysumUiSnapshot PRODUCT_NAME "RysumUiSnapshot")
    target_sources(RysumUiSnapshot PRIVATE
        tests/UiSnapshot.cpp
        ${RYSUM_KERNEL_SOURCES}
        source/Lang.cpp source/SampleLibrary.cpp source/OrganEngine.cpp source/OrganLive.cpp source/OrganBodies.cpp source/Convolver.cpp source/Binaural.cpp source/RoomTail.cpp source/Lucier.cpp source/LucierPiece.cpp source/Speakers.cpp source/SofaGunzip.cpp source/PluginProcessor.cpp source/Audience.cpp source/ProcessorScenes.cpp source/ProcessorAutomation.cpp source/Concert.cpp source/PluginEditor.cpp
        source/ConsoleView.cpp source/SpaceView.cpp source/KlangView.cpp source/ModView.cpp source/VerlaufView.cpp source/EffectsView.cpp source/SaalView.cpp source/MidiMapView.cpp source/OrganView.cpp source/SpeakerView.cpp source/Walk3D.cpp source/RoomModels.cpp source/WalkGpu.cpp source/WalkView.cpp source/WalkArrange.cpp source/WalkHands.cpp)
    target_include_directories(RysumUiSnapshot PRIVATE source)
    target_compile_definitions(RysumUiSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumUiSnapshot PRIVATE RysumData rysum_mysofa rysum_mtsesp rysum_pffft
        juce::juce_audio_utils juce::juce_opengl juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # the Vox humana through the whole plugin: RysumVoxRender <samples> <input.wav> <out folder> [seconds]
    juce_add_gui_app(RysumVoxRender PRODUCT_NAME "RysumVoxRender")
    target_sources(RysumVoxRender PRIVATE tests/VoxRender.cpp ${RYSUM_PLUGIN_SOURCES})
    target_include_directories(RysumVoxRender PRIVATE source)
    target_compile_definitions(RysumVoxRender PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumVoxRender PRIVATE RysumData rysum_mysofa rysum_mtsesp rysum_pffft
        juce::juce_audio_utils juce::juce_opengl juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(RysumVoxRender PRIVATE /MP /utf-8)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: CPU benchmark of the whole plugin (no host needed)
#   cmake -B build-bench -DRYSUM_BUILD_BENCH=ON  then  cmake --build build-bench --config Release --target RysumBench
#   RysumBench <samples folder> [block size] [seconds per scenario] [scenario filter]
# ---------------------------------------------------------------------------
option(RYSUM_BUILD_BENCH "Build the CPU benchmark" OFF)
if(RYSUM_BUILD_BENCH)
    juce_add_gui_app(RysumBench PRODUCT_NAME "RysumBench")
    target_sources(RysumBench PRIVATE tests/Bench.cpp ${RYSUM_PLUGIN_SOURCES})
    target_include_directories(RysumBench PRIVATE source)
    target_compile_definitions(RysumBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumBench PRIVATE RysumData rysum_mysofa rysum_mtsesp rysum_pffft
        juce::juce_audio_utils juce::juce_opengl juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(RysumBench PRIVATE /MP /utf-8)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: the dataset sampler (many short clips of the plugin with their notes and settings, for
# training a neural synthesis network). scripts/make_dataset.ps1 builds and runs it.
#   cmake -B build-dataset -DRYSUM_BUILD_DATASET=ON  then  cmake --build build-dataset --config Release --target RysumDataset
#   RysumDataset <samples folder> <out folder> [--hours 10] [--clip 10] [--rate 48000] ...  (see tests/Dataset.cpp)
# ---------------------------------------------------------------------------
option(RYSUM_BUILD_DATASET "Build the dataset sampler" OFF)
if(RYSUM_BUILD_DATASET)
    juce_add_console_app(RysumDataset PRODUCT_NAME "RysumDataset")
    target_sources(RysumDataset PRIVATE tests/Dataset.cpp ${RYSUM_PLUGIN_SOURCES})
    target_include_directories(RysumDataset PRIVATE source)
    target_compile_definitions(RysumDataset PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumDataset PRIVATE RysumData rysum_mysofa rysum_mtsesp rysum_pffft
        juce::juce_audio_utils juce::juce_opengl juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
    if(MSVC)
        target_compile_options(RysumDataset PRIVATE /MP /utf-8)
    endif()
endif()
