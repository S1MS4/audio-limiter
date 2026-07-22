cmake_minimum_required(VERSION 3.20)
project(audiolimiter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

add_executable(m0spike
    src/main.cpp
    src/audio_policy_config.cpp
    src/session_finder.cpp
    src/guids.cpp
)

target_include_directories(m0spike PRIVATE src)
target_compile_definitions(m0spike PRIVATE UNICODE _UNICODE NOMINMAX
                                           WIN32_LEAN_AND_MEAN)

# runtimeobject: RoGetActivationFactory / HSTRING. The routing API is a WinRT
# activation factory even though the interface itself is classic COM.
# GUIDs are defined by src/guids.cpp rather than linked from uuid.lib, so the
# same source builds under MinGW, which does not ship them.
target_link_libraries(m0spike PRIVATE ole32 oleaut32 runtimeobject)

if(MSVC)
    target_compile_options(m0spike PRIVATE /W4 /permissive-)
    # Static CRT: one self-contained exe, no redistributable prerequisite.
    set_property(TARGET m0spike PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # MinGW. -municode gives wmain an entry point; without it the linker
    # looks for main and fails.
    target_compile_options(m0spike PRIVATE -Wall -Wextra -municode)
    target_link_options(m0spike PRIVATE -municode -static
                        -static-libgcc -static-libstdc++)
endif()
