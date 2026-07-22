cmake_minimum_required(VERSION 3.20)
project(audiolimiter CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

set(CORE_SOURCES
    src/audio_policy_config.cpp
    src/session_finder.cpp
    src/audio_engine.cpp
    src/guids.cpp
    src/driver_install.cpp
)

# The product: a tray application with no console window.
add_executable(AudioLimiter WIN32 src/app.cpp src/app.rc ${CORE_SOURCES})

# Developer harness for the routing layer.
add_executable(m0spike src/main.cpp ${CORE_SOURCES})

# Offline DSP tests. Console app, no Windows audio APIs involved.
add_executable(test_dsp tests/test_dsp.cpp)
target_include_directories(test_dsp PRIVATE src)
if(NOT MSVC)
    target_compile_options(test_dsp PRIVATE -Wall -Wextra)
    target_link_options(test_dsp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_test(NAME dsp COMMAND test_dsp)

foreach(tgt AudioLimiter m0spike)
    target_include_directories(${tgt} PRIVATE src)
    # Windows 10 1703+ headers: GetDpiForWindow, per-monitor v2 DPI contexts.
    target_compile_definitions(${tgt} PRIVATE UNICODE _UNICODE NOMINMAX
                                              WIN32_LEAN_AND_MEAN
                                              WINVER=0x0A00 _WIN32_WINNT=0x0A00
                                              NTDDI_VERSION=0x0A000004)
    # runtimeobject: RoGetActivationFactory / HSTRING. The routing API is a
    # WinRT activation factory even though the interface is classic COM.
    # avrt: MMCSS thread priority for the audio threads.
    # GUIDs come from src/guids.cpp rather than uuid.lib, so the same source
    # builds under MinGW, which does not ship them.
    target_link_libraries(${tgt} PRIVATE
        ole32 oleaut32 runtimeobject avrt comctl32 shlwapi dwmapi shcore
        setupapi newdev cfgmgr32)

    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
        # Static CRT: one self-contained exe, no redistributable prerequisite.
        set_property(TARGET ${tgt} PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # We supply src/app.manifest ourselves, so suppress the one MSVC
        # generates or the resource compiler sees two and aborts.
        target_link_options(${tgt} PRIVATE /MANIFEST:NO)
    else()
        # MinGW. -municode gives wmain/wWinMain an entry point.
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -municode)
        target_link_options(${tgt} PRIVATE -municode -static
                            -static-libgcc -static-libstdc++)
    endif()
endforeach()
