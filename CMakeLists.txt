cmake_minimum_required(VERSION 3.20)
project(stfapi)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find system libraries
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(glfw3 REQUIRED)

# 2. Collect ImGui source files
file(GLOB IMGUI_SOURCES 
    "thirdparty/imgui/*.cpp"
    "thirdparty/imgui/backends/imgui_impl_glfw.cpp"
    "thirdparty/imgui/backends/imgui_impl_opengl3.cpp"
)

find_package(Threads REQUIRED)

# 3. Define executable
add_executable(SteamRIP_Launcher
    src/main.cpp
    ${IMGUI_SOURCES}
)

# 4. Include paths
target_include_directories(SteamRIP_Launcher PRIVATE 
    src/
    thirdparty/imgui/
    thirdparty/imgui/backends/
)

# 5. Link libraries (Linux uses standard OpenGL libraries)
target_link_libraries(SteamRIP_Launcher PRIVATE 
    CURL::libcurl 
    nlohmann_json::nlohmann_json
    glfw
    GL
    X11
    Threads::Threads
)