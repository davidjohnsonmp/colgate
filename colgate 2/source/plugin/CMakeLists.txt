include(FetchContent)

# COLGATE_JUCE_PATH points at an existing JUCE checkout, which saves the download
# when building repeatedly. Normally JUCE is fetched at the pinned version.
set(COLGATE_JUCE_PATH "" CACHE PATH "Use this JUCE checkout instead of downloading")
if(COLGATE_JUCE_PATH)
    add_subdirectory("${COLGATE_JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(COLGATE_FORMATS AU VST3)
else()
    set(COLGATE_FORMATS VST3)
endif()

juce_add_plugin(Colgate
    PRODUCT_NAME "Colgate"
    COMPANY_NAME "Hiccup Audio"
    BUNDLE_ID com.hiccupaudio.colgate
    PLUGIN_MANUFACTURER_CODE Hicc
    PLUGIN_CODE Clgt
    FORMATS ${COLGATE_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect)

juce_add_binary_data(ColgateAssets
    NAMESPACE ColgateData
    SOURCES
        assets/ColgateFace-Regular.ttf
        assets/ColgateFace-Bold.ttf)

target_sources(Colgate PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    EditorComponents.cpp
    Settings.cpp)

target_compile_definitions(Colgate PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
    COLGATE_VERSION_STRING="${PROJECT_VERSION}")

# These X11 extensions are only used for niceties and need extra development
# packages, so a Linux build (used for testing) can do without them.
if(UNIX AND NOT APPLE)
    target_compile_definitions(Colgate PUBLIC
        JUCE_USE_XRANDR=0
        JUCE_USE_XINERAMA=0
        JUCE_USE_XCURSOR=0
        JUCE_USE_XINPUT=0
        JUCE_USE_XRENDER=0
        JUCE_JACK=0
        JUCE_ALSA=0)
endif()

target_link_libraries(Colgate
    PRIVATE
        colgate_engine
        ColgateAssets
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

option(COLGATE_BUILD_PLUGIN_TESTS "Build the tests that run the real plug-in code" ON)
if(COLGATE_BUILD_PLUGIN_TESTS)
    # Links the plug-in's own code, JUCE modules included, so the same objects
    # that ship are the ones under test.
    add_executable(ColgatePluginTests ${CMAKE_SOURCE_DIR}/tests/plugin_tests.cpp)
    target_include_directories(ColgatePluginTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests
        $<TARGET_PROPERTY:Colgate,INCLUDE_DIRECTORIES>)
    target_compile_definitions(ColgatePluginTests PRIVATE $<TARGET_PROPERTY:Colgate,COMPILE_DEFINITIONS>)
    target_compile_features(ColgatePluginTests PRIVATE cxx_std_17)
    target_link_libraries(ColgatePluginTests PRIVATE Colgate colgate_engine ColgateAssets)
    add_test(NAME colgate_plugin COMMAND ColgatePluginTests)
endif()

# Loads the built VST3 through its public interface, the way a host does.
option(COLGATE_BUILD_HOST_CHECK "Build a tiny VST3 host that checks the finished binary" OFF)
if(COLGATE_BUILD_HOST_CHECK)
    juce_add_console_app(ColgateVst3HostCheck)
    target_sources(ColgateVst3HostCheck PRIVATE ${CMAKE_SOURCE_DIR}/tools/vst3_host_check.cpp)
    target_compile_definitions(ColgateVst3HostCheck PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    if(UNIX AND NOT APPLE)
        target_compile_definitions(ColgateVst3HostCheck PRIVATE
            JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XINPUT=0 JUCE_USE_XRENDER=0)
    endif()
    target_link_libraries(ColgateVst3HostCheck PRIVATE
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_recommended_config_flags)
endif()
