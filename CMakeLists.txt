cmake_minimum_required(VERSION 3.21)

project(feishin-visualizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Static CRT, matches the x64-windows-static vcpkg triplet used in CI.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(projectM4 REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(feishin-visualizer
    src/main.cpp
    src/config.cpp
    src/audio_capture.cpp
    src/beat_analysis.cpp
    src/beat_tracker.cpp
    src/beat_window.cpp
    src/load_menu.cpp
    src/milk_text.cpp
    src/preset_library.cpp
)

target_link_libraries(feishin-visualizer PRIVATE
    libprojectM::projectM
    SDL3::SDL3
    GLEW::GLEW
    imgui::imgui
    OpenGL::GL
)

if(WIN32)
    target_compile_definitions(feishin-visualizer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(feishin-visualizer PRIVATE ole32)
endif()
