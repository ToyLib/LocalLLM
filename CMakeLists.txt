cmake_minimum_required(VERSION 3.20)

# Windows では vcpkg のツールチェーンを使う
# vcpkg の場所は環境変数 VCPKG_ROOT で指定する
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VCPKG_ROOT})
        message(FATAL_ERROR
            "VCPKG_ROOT is not set. Set it to your vcpkg directory "
            "(e.g. setx VCPKG_ROOT C:\\tools\\vcpkg) and open a new terminal.")
    endif()

    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT)

    # 開発者用のシェル (Developer PowerShell for VS など) は VCPKG_ROOT を
    # Visual Studio 付属の vcpkg に書き換えるが、付属の vcpkg は
    # baseline の無いこのプロジェクトのマニフェストを扱えない
    if(VCPKG_ROOT MATCHES "Microsoft Visual Studio")
        message(FATAL_ERROR
            "VCPKG_ROOT points to the vcpkg bundled with Visual Studio "
            "(${VCPKG_ROOT}). Run CMake from a normal PowerShell, "
            "not the Developer PowerShell / Command Prompt for VS.")
    endif()

    if(NOT EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        message(FATAL_ERROR "vcpkg not found in VCPKG_ROOT: ${VCPKG_ROOT}")
    endif()

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
    src/main.cpp
    src/agent.cpp
    src/chat.cpp
    src/chat_ui.cpp
    src/font.cpp
    src/ime.cpp
    src/llm_client.cpp
    src/rag.cpp
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

# RAG 用の文書 (rag/) を実行ファイルの隣に同期する
# 文書だけを変えたときも反映されるように、ビルドのたびに実行する
# (消したファイルが残らないように、いったん削除してからコピー)
add_custom_target(sync_rag ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:LocalLLM>/rag
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/rag
        $<TARGET_FILE_DIR:LocalLLM>/rag
    VERBATIM
)
add_dependencies(LocalLLM sync_rag)
