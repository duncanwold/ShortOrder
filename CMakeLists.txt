cmake_minimum_required(VERSION 3.22)

# v0.7: a universal binary (Apple silicon and Intel), for macOS 11 and later. Both before project(); a -D on the command line wins.
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build" FORCE)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(ShortOrder VERSION 1.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# JUCE: an installed copy of JUCE (JUCE's own `cmake --install`), found with find_package, never fetched. ShortOrder is built and
# tested with exactly this version; another one fails here, since a different JUCE can change the plugin's behaviour.
#   -DJUCE_DIR=<prefix>/lib/cmake/JUCE-8.0.12, or -DCMAKE_PREFIX_PATH=<prefix> (or either as an environment variable).
#   Neither given: ~/JUCE-installed (Duncan's machine).
set(SHORTORDER_JUCE_VERSION 8.0.12)
if(NOT JUCE_DIR AND DEFINED ENV{JUCE_DIR})
    set(JUCE_DIR "$ENV{JUCE_DIR}" CACHE PATH "JUCE's CMake package directory (<prefix>/lib/cmake/JUCE-<version>)")
endif()
if(NOT JUCE_DIR AND NOT CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/JUCE-installed")
endif()
find_package(JUCE CONFIG)
if(NOT JUCE_FOUND)
    message(FATAL_ERROR "JUCE ${SHORTORDER_JUCE_VERSION} not found. Install JUCE ${SHORTORDER_JUCE_VERSION} "
                        "(https://github.com/juce-framework/JUCE, tag ${SHORTORDER_JUCE_VERSION}: cmake -B build -DCMAKE_INSTALL_PREFIX=<prefix>, "
                        "then cmake --install build) and configure with -DCMAKE_PREFIX_PATH=<prefix> or "
                        "-DJUCE_DIR=<prefix>/lib/cmake/JUCE-${SHORTORDER_JUCE_VERSION}.")
endif()
if(NOT JUCE_VERSION VERSION_EQUAL SHORTORDER_JUCE_VERSION)
    message(FATAL_ERROR "ShortOrder is built and tested with JUCE ${SHORTORDER_JUCE_VERSION}, but found JUCE ${JUCE_VERSION} "
                        "(${JUCE_DIR}). Install JUCE ${SHORTORDER_JUCE_VERSION} and point -DJUCE_DIR at it.")
endif()
message(STATUS "JUCE ${JUCE_VERSION}: ${JUCE_DIR}")

# AAX: the AAX SDK (from developer.avid.com, with an Avid developer account; never in this repository) at ~/AAX_SDK, as
# GuitarVocoder finds it. Without it the build has no AAX target.
set(AAX_SDK_PATH "$ENV{HOME}/AAX_SDK" CACHE PATH "Path to AAX SDK")
if(EXISTS "${AAX_SDK_PATH}/Interfaces/AAX.h")
    set(AAX_FORMAT AAX)
    juce_set_aax_sdk_path(${AAX_SDK_PATH})
    message(STATUS "AAX SDK found at ${AAX_SDK_PATH}")
else()
    set(AAX_FORMAT "")
    message(STATUS "AAX SDK not found — building without AAX")
endif()

# DSP sources are shared by the plugin and the offline test harness.
file(GLOB GTT_DSP_SOURCES CONFIGURE_DEPENDS Source/DSP/*.cpp)

# v0.6: Didact Gothic (SIL OFL, Resources/Fonts/OFL.txt), the faceplate's typeface.
juce_add_binary_data(GTTBinaryData SOURCES Resources/Fonts/DidactGothic-Regular.ttf)

# v0.6.1: ShortOrder (internally still gtt: the namespace, GTTTests, GTTSnapshot, the class and file names). Up to v0.6 this was
# GTTLab / "GTT Lab" / Gtlb / com.yachtpockets.gttlab: a different AU, so Logic sees ShortOrder as a new plugin.
juce_add_plugin(ShortOrder
    COMPANY_NAME "YachtPockets"
    PLUGIN_MANUFACTURER_CODE Ypkt
    PLUGIN_CODE ShOr
    BUNDLE_ID com.yachtpockets.shortorder
    DESCRIPTION "The 8ms Guitar Synthesizer"
    FORMATS AU VST3 Standalone ${AAX_FORMAT}
    PRODUCT_NAME "ShortOrder"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ShortOrder's standalone app needs audio input to process your guitar."
    # v0.7: an effect in every format (an insert on the guitar track). VST3: Fx; AAX: Pitch Shift (Pro Tools' category for
    # pitch correction and pitch effects, where a pitch-to-synth insert is looked for).
    VST3_CATEGORIES Fx
    AAX_CATEGORY AAX_ePlugInCategory_PitchShift
)

target_sources(ShortOrder PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Displays.cpp
    Source/Controls.cpp
    Source/Params.cpp
    Source/Theme.cpp
    Source/Rack.cpp
    Source/Faceplate.cpp
    ${GTT_DSP_SOURCES}
)

target_include_directories(ShortOrder PRIVATE Source)

target_compile_definitions(ShortOrder PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(ShortOrder
    PRIVATE
        GTTBinaryData
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---- Offline test harness (command line) ----
juce_add_console_app(GTTTests PRODUCT_NAME "GTTTests")

target_sources(GTTTests PRIVATE
    Tests/OfflineTests.cpp
    ${GTT_DSP_SOURCES}
)

target_include_directories(GTTTests PRIVATE Source)

target_compile_definitions(GTTTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(GTTTests
    PRIVATE
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- Offscreen editor snapshot (UI check without a host) ----
# (Only where Tools/ is present: the public snapshot, Tools/export-public.sh, leaves it out by default.)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Snapshot.cpp")
    juce_add_console_app(GTTSnapshot PRODUCT_NAME "GTTSnapshot")

    target_sources(GTTSnapshot PRIVATE
        Tools/Snapshot.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Displays.cpp
        Source/Controls.cpp
        Source/Params.cpp
        Source/Theme.cpp
        Source/Rack.cpp
        Source/Faceplate.cpp
        ${GTT_DSP_SOURCES}
    )

    target_include_directories(GTTSnapshot PRIVATE Source)

    target_compile_definitions(GTTSnapshot PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JucePlugin_Name="ShortOrder"
        GTT_PAINT_PROBE=1
    )

    target_link_libraries(GTTSnapshot
        PRIVATE
            GTTBinaryData
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endif()
