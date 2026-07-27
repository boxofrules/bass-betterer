cmake_minimum_required(VERSION 3.22)

# Minimum macOS the binaries run on. Must be set before project(); without it the
# build inherits the build machine's SDK default, so CI releases would require
# whatever macOS the runner happens to be on. 10.13 is JUCE 8's floor (the arm64
# slice of a universal binary implies 11.0 by itself; this governs the x86_64 one).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(BoRBassEnhancer VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build host-arch only for fast iteration unless told otherwise.
option(BOR_NATIVE_ONLY "Build host architecture only" ON)
if(BOR_NATIVE_ONLY AND APPLE)
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Optional AAX (Pro Tools) target — experimental, off by default. Needs the (free)
# Avid AAX SDK checked out somewhere locally; JUCE builds the wrapper itself, we just
# point it at the SDK. Not available in CI (the SDK isn't fetched there), so the
# variable stays empty on every CI runner and the build is identical to before this
# option existed. See README "Building the AAX target".
set(BOR_AAX_SDK_PATH "" CACHE PATH "Path to a local Avid AAX SDK checkout (leave empty to skip AAX)")

set(BOR_PLUGIN_FORMATS AU VST3 Standalone)
if(BOR_AAX_SDK_PATH AND EXISTS "${BOR_AAX_SDK_PATH}")
    juce_set_aax_sdk_path("${BOR_AAX_SDK_PATH}")
    list(APPEND BOR_PLUGIN_FORMATS AAX)
    message(STATUS "BOR_AAX_SDK_PATH set (${BOR_AAX_SDK_PATH}) — building AAX format too")
endif()

juce_add_plugin(BoRBassEnhancer
    COMPANY_NAME "Box of Rules"
    COMPANY_COPYRIGHT "(c) 2026 Box of Rules. Source-available, proprietary — see the repo LICENSE."
    BUNDLE_ID com.boxofrules.bassbetterer
    PLUGIN_MANUFACTURER_CODE BxRl
    PLUGIN_CODE BbEn
    FORMATS ${BOR_PLUGIN_FORMATS}
    PRODUCT_NAME "Bass Better-er"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    # app/bundle icon (standalone was shipping the generic macOS placeholder);
    # source artwork is assets/bor-icon.svg
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-256.png"
    # TCC usage descriptions for the standalone — macOS aborts the process if a
    # privacy-gated API is touched without one (e.g. the Bluetooth MIDI button).
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Bass Better-er needs audio input to process your bass DI."
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "Bass Better-er uses Bluetooth to connect Bluetooth MIDI devices."
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(BoRBassEnhancer)

# The cab IRs are proprietary and live encrypted in the repo (ir/ir-pack.tar.gz.enc).
# They must be decrypted to ir/*.wav before configuring — see README / scripts/ir-crypt.sh.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ir/ir_sub.wav")
    message(FATAL_ERROR
        "Cab IRs not found. Decrypt them first:\n"
        "  BOR_IR_KEY=<key> ./scripts/ir-crypt.sh decrypt\n"
        "(CI does this automatically from the BOR_IR_KEY secret.)")
endif()

juce_add_binary_data(BoREnhancerIR SOURCES
    ir/ir_sub.wav
    ir/ir_lowcln1.wav
    ir/ir_lowcln2.wav
    ir/ir_lofx57_clean.wav
    ir/ir_lofx421_clean.wav
    ir/ir_lofxtwt_clean.wav
    ir/ir_lofx57_fuzz.wav
    ir/ir_lofx421_fuzz.wav
    ir/ir_lofxtwt_fuzz.wav
    ir/ir_lofx57_dist.wav
    ir/ir_lofx421_dist.wav
    ir/ir_lofxtwt_dist.wav
    ir/ir_roomnear.wav
    ir/ir_roomfar.wav
    ir/drive_fits.bin)

# Binary data must be PIC so it can link into the shared plugin formats.
set_target_properties(BoREnhancerIR PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_sources(BoRBassEnhancer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(BoRBassEnhancer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(BoRBassEnhancer PRIVATE
    BoREnhancerIR
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---- Bass Better-Router: the multi-output sidechain-instrument variant --------
# Same engine, compiled as a software INSTRUMENT (AU aumu / VST3 Instrument /
# AAX SWGenerators): put it on an instrument track, feed the DI via the
# side-chain (Logic) or key input (Pro Tools), and route each mixer strip to
# its own aux via the 9 stem output buses — the Ultrabeat workflow, for a bass
# rig. All divergence from the effect is behind BOR_INSTRUMENT in Source/.
# No Standalone: it would duplicate the effect's standalone exactly.
option(BOR_BUILD_INSTRUMENT "Build the Bass Better-Router instrument variant" ON)
if(BOR_BUILD_INSTRUMENT)
    set(BOR_ROUTER_FORMATS AU VST3)
    if(BOR_AAX_SDK_PATH AND EXISTS "${BOR_AAX_SDK_PATH}")
        list(APPEND BOR_ROUTER_FORMATS AAX)
    endif()
    juce_add_plugin(BoRBassRouter
        COMPANY_NAME "Box of Rules"
        COMPANY_COPYRIGHT "(c) 2026 Box of Rules. Source-available, proprietary — see the repo LICENSE."
        BUNDLE_ID com.boxofrules.bassbetterrouter
        PLUGIN_MANUFACTURER_CODE BxRl
        PLUGIN_CODE BbRt
        FORMATS ${BOR_ROUTER_FORMATS}
        PRODUCT_NAME "Bass Better-Router"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE          # hosts require MIDI-in on an instrument; notes are ignored
        NEEDS_MIDI_OUTPUT FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        AAX_CATEGORY AAX_ePlugInCategory_SWGenerators
        VST3_CATEGORIES Instrument External
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png"
        ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-256.png"
        COPY_PLUGIN_AFTER_BUILD TRUE)
    juce_generate_juce_header(BoRBassRouter)
    target_sources(BoRBassRouter PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_compile_definitions(BoRBassRouter PUBLIC
        BOR_INSTRUMENT=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(BoRBassRouter PRIVATE
        BoREnhancerIR
        juce::juce_audio_utils
        juce::juce_dsp
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

# Offline calibration / benchmark harness (developer tool, never shipped).
#   cmake -B build -DBOR_BUILD_TOOLS=ON && cmake --build build --target BoRBench
#   ./build/BoRBench_artefacts/bor-bench cal|bench
option(BOR_BUILD_TOOLS "Build the bor-bench console tool" OFF)
if(BOR_BUILD_TOOLS)
    juce_add_console_app(BoRBench PRODUCT_NAME "bor-bench")
    target_sources(BoRBench PRIVATE
        tools/bor_bench.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_compile_definitions(BoRBench PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(BoRBench PRIVATE
        BoREnhancerIR
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
