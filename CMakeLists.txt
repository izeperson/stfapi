cmake_minimum_required(VERSION 3.10)
project(stf_launcher)

set(OpenGL_GL_PREFERENCE GLVND)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find system dependencies
find_package(CURL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Search for nlohmann_json (header-only)
find_path(JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
if(NOT JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann_json not found. Install with: sudo apt install nlohmann-json3-dev")
endif()

# Define include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui/backends
    ${JSON_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# Source files
set(SOURCES
    src/main.cpp
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/backends/imgui_impl_glfw.cpp
    thirdparty/imgui/backends/imgui_impl_opengl3.cpp
)

add_executable(stf_launcher ${SOURCES})

target_link_libraries(stf_launcher
    ${CURL_LIBRARIES}
    glfw
    ${OPENGL_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)