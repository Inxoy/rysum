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
        Resources/k_gedackt.jpg
        Resources/k_mixtur.jpg
        Resources/k_oktave2.jpg
        Resources/k_oktave4.jpg
        Resources/k_sesquialtera.jpg
        Resources/k_trompete.jpg
        Resources/knob_bare.jpg
        Resources/keys.jpg
        Resources/wall.jpg
        Resources/grain.png)

target_sources(Rysum PRIVATE
    Source/SampleLibrary.cpp
    Source/OrganEngine.cpp
    Source/Convolver.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ConsoleView.cpp
    Source/SpaceView.cpp
    Source/KlangView.cpp
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
        Source/Convolver.cpp)
    target_include_directories(RysumRenderTest PRIVATE Source)
    target_compile_definitions(RysumRenderTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(RysumRenderTest PRIVATE
        juce::juce_audio_basics juce::juce_audio_formats juce::juce_core juce::juce_events juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    juce_add_gui_app(RysumUiSnapshot PRODUCT_NAME "RysumUiSnapshot")
    target_sources(RysumUiSnapshot PRIVATE
        Tests/UiSnapshot.cpp
        Source/SampleLibrary.cpp Source/OrganEngine.cpp Source/Convolver.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp
        Source/ConsoleView.cpp Source/SpaceView.cpp Source/KlangView.cpp Source/ModView.cpp)
    target_include_directories(RysumUiSnapshot PRIVATE Source)
    target_compile_definitions(RysumUiSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Build_Standalone=0
        JucePlugin_Name="Rysum" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(RysumUiSnapshot PRIVATE RysumData
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
