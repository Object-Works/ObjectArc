cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OA_VERSION LIMIT_COUNT 1)
project(ObjectArc VERSION ${OA_VERSION} LANGUAGES CXX RC)
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ObjectArc WIN32
    src/main.cpp
    src/common.cpp
    src/config.cpp
    src/match.cpp
    src/icons.cpp
    src/sources.cpp
    src/launcher.cpp
    src/app.manifest
    src/arc.rc
)

target_compile_definitions(ObjectArc PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ObjectArc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(MSVC)
    target_compile_options(ObjectArc PRIVATE /W4 /utf-8 /permissive-)
    set_property(TARGET ObjectArc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ObjectArc PRIVATE d2d1 dwrite ole32 shell32 user32 gdi32 shcore dwmapi powrprof advapi32)
