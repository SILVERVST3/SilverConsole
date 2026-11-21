cmake_minimum_required(VERSION 3.22)
project(SilverConsole VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Executavel independente no Windows (sem instalar o VC++ Runtime).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(SILVERCONSOLE_TESTS "Compila os testes do motor e o plugin VST3 de teste" ON)
# Opt-in: JUCE 8.0.6 usa o SDK ASIO externo. Nao distribuir o SDK
# com o repositorio e revisar seus termos antes de redistribuir binarios.
option(SILVERCONSOLE_ENABLE_ASIO "Habilita ASIO no Windows usando SDK externo" OFF)
set(SILVERCONSOLE_ASIO_SDK_DIR "" CACHE PATH "Diretorio raiz do Steinberg ASIO SDK (pastas common/ e host/)")

if(SILVERCONSOLE_ENABLE_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "ASIO e suportado somente no Windows neste projeto.")
    endif()
    if(NOT EXISTS "${SILVERCONSOLE_ASIO_SDK_DIR}/common/iasiodrv.h"
       OR NOT EXISTS "${SILVERCONSOLE_ASIO_SDK_DIR}/host/asiodrivers.h")
        message(FATAL_ERROR
            "ASIO SDK nao encontrado. Defina -DSILVERCONSOLE_ASIO_SDK_DIR=... "
            "para a raiz que contem common/iasiodrv.h e host/asiodrivers.h")
    endif()
endif()


include(FetchContent)
# JUCE 8.0.6 fixado (mesmo commit usado no SilverMix). Atualizar somente com novo teste.
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/51a8a6d7aeae7326956d747737ccf1575e61e209.tar.gz
    URL_HASH SHA256=20b740351cc030086951d87614bc08b42d072f1e4db68db1ef1d988e66dbbdbe
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

set(SC_COMMON_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_AU=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

set(SC_ENGINE_SOURCES
    Source/Engine.cpp Source/Engine.h Source/Metering.h
    Source/AudioLoader.cpp Source/AudioLoader.h
    Source/StreamingAudio.cpp Source/StreamingAudio.h
    Source/Project.cpp Source/Project.h
    Source/Automation.h
    Source/AutomationRecorder.cpp Source/AutomationRecorder.h)

#------------------------------------------------------------------------------
juce_add_gui_app(SilverConsole
    PRODUCT_NAME "SilverConsole"
    COMPANY_NAME "Silver Audio"
    BUNDLE_ID "audio.silver.silverconsole"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png")
juce_generate_juce_header(SilverConsole)
target_sources(SilverConsole PRIVATE
    ${SC_ENGINE_SOURCES}
    Source/Main.cpp
    Source/MainComponent.cpp Source/MainComponent.h
    Source/ChannelStrip.cpp Source/ChannelStrip.h
    Source/AutomationView.cpp Source/AutomationView.h
    Source/SilverLookAndFeel.h
    Source/PluginHost.h
    Source/PluginWindow.h)
target_compile_definitions(SilverConsole PRIVATE ${SC_COMMON_DEFS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SilverConsole,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SilverConsole,JUCE_VERSION>")
if(SILVERCONSOLE_ENABLE_ASIO)
    target_compile_definitions(SilverConsole PRIVATE JUCE_ASIO=1)
    target_include_directories(SilverConsole PRIVATE
        "${SILVERCONSOLE_ASIO_SDK_DIR}/common"
        "${SILVERCONSOLE_ASIO_SDK_DIR}/host")
endif()

target_link_libraries(SilverConsole PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(SilverConsole PRIVATE /utf-8)
endif()

#------------------------------------------------------------------------------
# Executavel separado para inspecao de metadados de plugins nao confiaveis.
# A UI principal nao carrega o plugin durante essa varredura.
juce_add_console_app(ScPluginScanWorker PRODUCT_NAME "SilverConsole Plugin Scan Worker")
set_target_properties(ScPluginScanWorker PROPERTIES OUTPUT_NAME "ScPluginScanWorker")
juce_generate_juce_header(ScPluginScanWorker)
target_sources(ScPluginScanWorker PRIVATE Source/PluginScanWorker.cpp)
target_compile_definitions(ScPluginScanWorker PRIVATE ${SC_COMMON_DEFS})
target_link_libraries(ScPluginScanWorker PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(ScPluginScanWorker PRIVATE /utf-8)
endif()

# Na execucao local (fora do ZIP do CI), manter o helper junto do app.
# Fazer o build do executavel auxiliar antes de copiar no POST_BUILD.
add_dependencies(SilverConsole ScPluginScanWorker)
if(WIN32)
    add_custom_command(TARGET SilverConsole POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:ScPluginScanWorker>"
          "$<TARGET_FILE_DIR:SilverConsole>/ScPluginScanWorker.exe"
        COMMENT "Copiando helper isolado para a pasta do SilverConsole")
endif()

#------------------------------------------------------------------------------
if(SILVERCONSOLE_TESTS)
    include(CTest)

    # Plugin VST3 minimo, usado so para testar a hospedagem (ganho + latencia).
    juce_add_plugin(SCTestPlugin
        PRODUCT_NAME "SC Test Delay"
        COMPANY_NAME "Silver Audio"
        BUNDLE_ID "audio.silver.sctestplugin"
        PLUGIN_MANUFACTURER_CODE Slvr
        PLUGIN_CODE Sctd
        FORMATS VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE)
    juce_generate_juce_header(SCTestPlugin)
    target_sources(SCTestPlugin PRIVATE Tests/TestPlugin.cpp)
    target_compile_definitions(SCTestPlugin PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(SCTestPlugin PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
    if(MSVC)
        target_compile_options(SCTestPlugin PRIVATE /utf-8)
    endif()

    juce_add_console_app(SilverConsoleTests PRODUCT_NAME "SilverConsoleTests")
    juce_generate_juce_header(SilverConsoleTests)
    target_sources(SilverConsoleTests PRIVATE Tests/EngineTests.cpp ${SC_ENGINE_SOURCES})
    target_include_directories(SilverConsoleTests PRIVATE Source)
    target_compile_definitions(SilverConsoleTests PRIVATE ${SC_COMMON_DEFS})
    target_link_libraries(SilverConsoleTests PRIVATE
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
    if(MSVC)
        target_compile_options(SilverConsoleTests PRIVATE /utf-8)
    endif()
    add_dependencies(SilverConsoleTests SCTestPlugin_VST3 ScPluginScanWorker)

    add_test(NAME Engine
        COMMAND SilverConsoleTests
          "$<GENEX_EVAL:$<TARGET_PROPERTY:SCTestPlugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
          "${CMAKE_CURRENT_BINARY_DIR}/engine-scan-description.xml"
          "$<TARGET_FILE:ScPluginScanWorker>")

    # O plugin do proprio projeto e testado no PROCESSO do helper, nunca
    # dentro do SilverConsole GUI. O XML e validado separadamente.
    add_dependencies(ScPluginScanWorker SCTestPlugin_VST3)
    add_test(NAME PluginScanIsolated
        COMMAND ScPluginScanWorker
          "$<GENEX_EVAL:$<TARGET_PROPERTY:SCTestPlugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
          "${CMAKE_CURRENT_BINARY_DIR}/plugin-scan-ci.xml")
    add_test(NAME PluginScanXml
        COMMAND ${CMAKE_COMMAND}
          "-DXML_FILE=${CMAKE_CURRENT_BINARY_DIR}/plugin-scan-ci.xml"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VerifyPluginScan.cmake")
    set_tests_properties(PluginScanXml PROPERTIES DEPENDS PluginScanIsolated)
    # Caminho ausente nunca gera metadados validos e precisa retornar erro
    # controlado, sem travar nem criar XML de plugin inexistente.
    add_test(NAME PluginScanInvalidPath
        COMMAND ScPluginScanWorker
          "${CMAKE_CURRENT_BINARY_DIR}/silverconsole-plugin-does-not-exist.vst3"
          "${CMAKE_CURRENT_BINARY_DIR}/plugin-scan-invalid.xml")
    set_tests_properties(PluginScanInvalidPath PROPERTIES WILL_FAIL TRUE TIMEOUT 15)
    set_tests_properties(PluginScanIsolated PROPERTIES TIMEOUT 45)

    # Testa que o scanner auxiliar TAMBEM consegue instanciar e executar
    # blocos DSP sem carregar o plugin no SilverConsole principal.
    add_test(NAME PluginDspProbeIsolated
        COMMAND ScPluginScanWorker
          --probe
          "$<GENEX_EVAL:$<TARGET_PROPERTY:SCTestPlugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
          "${CMAKE_CURRENT_BINARY_DIR}/plugin-dsp-probe-ci.xml")
    add_test(NAME PluginDspProbeXml
        COMMAND ${CMAKE_COMMAND}
          "-DXML_FILE=${CMAKE_CURRENT_BINARY_DIR}/plugin-dsp-probe-ci.xml"
          "-DREQUIRE_DSP_PROBE=ON"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VerifyPluginScan.cmake")
    set_tests_properties(PluginDspProbeXml PROPERTIES DEPENDS PluginDspProbeIsolated)
    set_tests_properties(PluginDspProbeIsolated PROPERTIES TIMEOUT 45)
endif()
