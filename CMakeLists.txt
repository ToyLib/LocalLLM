cmake_minimum_required(VERSION 3.20)

# Windows では vcpkg のツールチェーンを使う
# (VCPKG_ROOT が未設定なら C:/tools/vcpkg を想定)
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
    else()
        set(VCPKG_ROOT "C:/tools/vcpkg")
    endif()

    file(TO_CMAKE_PATH "${VCPKG_ROOT}" VCPKG_ROOT)
    set(CMAKE_TOOLCHAIN_FILE
        "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(LocalLLM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Dear ImGui (CMakeLists.txt を持たないのでソースを直接ビルド)
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)

target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_executable(LocalLLM
    main.cpp
    llm_client.cpp
)

# ソースは UTF-8 (日本語の文字列リテラルを含む)
if(MSVC)
    target_compile_options(imgui PRIVATE /utf-8)
    target_compile_options(LocalLLM PRIVATE /utf-8)
endif()

target_link_libraries(LocalLLM PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    imgui
    Threads::Threads
)

# フォントなどのアセットを実行ファイルの隣にコピー
add_custom_command(TARGET LocalLLM POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Asset
        $<TARGET_FILE_DIR:LocalLLM>/Asset
    VERBATIM
)
