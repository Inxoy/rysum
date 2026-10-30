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
    add_subdirectory(${RYSUM_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
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
    set(RYSUM_BUNDLE "org.rysum.organ.dev")
else()
    set(RYSUM_PRODUCT "Rysum")
    set(RYSUM_CODE Rys1)
    set(RYSUM_BUNDLE "org.rysum.organ")
endif()

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
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_add_binary_data(RysumData
    HEADER_NAME RysumBinaryData.h
    NAMESPACE RysumData
    SOURCES
        Resources/IBMPlexSansCondensed-Regular.ttf
        Resources/IBMPlexSansCondensed-SemiBold.ttf
        Resources/IBMPlexMono-Regular.ttf
        Resources/UnifrakturMaguntia-Book.ttf
        Resources/RysumFell-Roman.ttf
        Resources/RysumFell-Italic.ttf
        Resources/k_praestant.jpg
        Resources/k_praestant16.jpg
        Resources/org_pipes.jpg
        Resources/org_jamb_l.jpg
        Resources/org_jamb_r.jpg
        Resources/k_gedackt.jpg
        Resources/k_mixtur.jpg
        Resources/k_oktave2.jpg
        Resources/k_oktave4.jpg
        Resources/k_sesquialtera.jpg
        Resources/k_trompete.jpg
        Resources/knob_bare.jpg
        Resources/keys.jpg
        Resources/orgel.txt
        Resources/wall.jpg
        Resources/grain.png
        Resources/fabian_hrir.bin)

# libmysofa (BSD-3): reads SOFA files (HRTFs) for the binaural mode. Its one zlib call goes to JUCE's zlib
# (hdf/gunzip_rysum.c -> Source/SofaGunzip.cpp), so nothing else is needed.
set(MYSOFA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/libmysofa)
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
set(MTSESP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/MTS-ESP)
add_library(rysum_mtsesp STATIC ${MTSESP_DIR}/libMTSClient.cpp)
set_target_properties(rysum_mtsesp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rysum_mtsesp PUBLIC ${MTSESP_DIR})
if(MSVC)
    target_compile_options(rysum_mtsesp PRIVATE /W0)
else()
    target_compile_options(rysum_mtsesp PRIVATE -w)
    target_link_libraries(rysum_mtsesp PRIVATE ${CMAKE_DL_LIBS})
endif()

target_sources(Rysum PRIVATE
    Source/SampleLibrary.cpp
    Source/OrganEngine.cpp
    Source/Convolver.cpp
    Source/Binaural.cpp
    Source/RoomTail.cpp
    Source/Lucier.cpp
    Source/SofaGunzip.cpp
    Source/PluginProcessor.cpp
    Source/ProcessorScenes.cpp
    Source/PluginEditor.cpp
    Source/ConsoleView.cpp
    Source/SpaceView.cpp
    Source/KlangView.cpp
    Source/EffectsView.cpp
    Source/SaalView.cpp
    Source/MidiMapView.cpp
    Source/OrganView.cpp
    Source/ModView.cpp)

target_include_directories(Rysum PRIVATE Source)

target_compile_definitions(Rysum PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Rysum
    PRIVATE
        RysumData
        rysum_mysofa
        rysum_mtsesp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(Rysum PRIVATE /MP /utf-8)
endif()

# ---------------------------------------------------------------------------
# Optional: headless render test (renders MIDI through the engine to WAV)
# ---------------------------------------------------------------------------
option(RYSUM_BUILD_TESTS "Build the offline render test" OFF)
if(RYSUM_BUILD_TESTS)
    juce_add_console_app(RysumRenderTest PRODUCT_NAME "RysumRenderTest")
    target_sources(RysumRenderTest PRIVATE
        Tests/RenderTest.cpp
        Source/SampleLibrary.cpp
        Source/OrganEngine.cpp
        Source/Convolver.cpp
        Source/Binaural.cpp
        Source/RoomTail.cpp
        Source/Lucier.cpp
        Source/SofaGunzip.cpp)
    target_include_directories(RysumRenderTest PRIVATE Source)
    target_compile_definitions(RysumRenderTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(RysumRenderTest PRIVATE RysumData rysum_mysofa
        juce::juce_audio_basics juce::juce_audio_formats juce::juce_core juce::juce_events juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    juce_add_gui_app(RysumUiSnapshot PRODUCT_NAME "RysumUiSnapshot")
    target_sources(RysumUiSnapshot PRIVATE
        Tests/UiSnapshot.cpp
        Source/SampleLibrary.cpp Source/OrganEngine.cpp Source/Convolver.cpp Source/Binaural.cpp Source/RoomTail.cpp Source/Lucier.cpp Source/SofaGunzip.cpp Source/PluginProcessor.cpp Source/ProcessorScenes.cpp Source/PluginEditor.cpp
        Source/ConsoleView.cpp Source/SpaceView.cpp Source/KlangView.cpp Source/ModView.cpp Source/EffectsView.cpp Source/SaalView.cpp Source/MidiMapView.cpp Source/OrganView.cpp)
    target_include_directories(RysumUiSnapshot PRIVATE Source)
    target_compile_definitions(RysumUiSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumUiSnapshot PRIVATE RysumData rysum_mysofa rysum_mtsesp
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
