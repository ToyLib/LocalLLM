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
    # vcpkg install で入れたライブラリ (クラシックモード) を扱えない
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
    src/quiz_mode.cpp
    src/quiz_ui.cpp
    src/maze_mode.cpp
    src/maze_ui.cpp
    src/config.cpp
    src/font.cpp
    src/ime.cpp
    src/llm_client.cpp
    src/rag.cpp
    src/server_manager.cpp
    src/settings_ui.cpp
)

# macOS ではタイトルバーの設定に AppKit を使う (Objective-C++)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(LocalLLM PRIVATE src/macos_window.mm)
    set_source_files_properties(src/macos_window.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(LocalLLM PRIVATE "-framework AppKit")
endif()

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

# llama-server の公式リリースを取得して同梱する (タグとハッシュは llama-release.json)
option(LOCALLLM_BUNDLE_LLAMA "Download the official llama-server release and bundle it" ON)

if(LOCALLLM_BUNDLE_LLAMA)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" LLAMA_CPU)

    if(LLAMA_CPU MATCHES "^(x86_64|amd64)$")
        set(LLAMA_CPU "x64")
    elseif(LLAMA_CPU MATCHES "^(arm64|aarch64)$")
        set(LLAMA_CPU "arm64")
    endif()

    if(WIN32)
        set(LLAMA_PLATFORM "windows-${LLAMA_CPU}")
    elseif(APPLE)
        set(LLAMA_PLATFORM "macos-${LLAMA_CPU}")
    else()
        set(LLAMA_PLATFORM "linux-${LLAMA_CPU}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/llama-release.json)
    file(READ ${CMAKE_SOURCE_DIR}/llama-release.json LLAMA_RELEASE)
    string(JSON LLAMA_TAG GET "${LLAMA_RELEASE}" tag)
    string(JSON LLAMA_FILE ERROR_VARIABLE LLAMA_FILE_ERROR
        GET "${LLAMA_RELEASE}" assets ${LLAMA_PLATFORM} file)

    if(LLAMA_FILE_ERROR)
        message(WARNING
            "No official llama-server release is listed for ${LLAMA_PLATFORM}. "
            "It will not be bundled; install llama-server yourself.")
    else()
        string(JSON LLAMA_SHA256 GET "${LLAMA_RELEASE}" assets ${LLAMA_PLATFORM} sha256)

        FetchContent_Declare(llama_release
            URL https://github.com/ggml-org/llama.cpp/releases/download/${LLAMA_TAG}/${LLAMA_FILE}
            URL_HASH SHA256=${LLAMA_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(llama_release)

        # llama-server が使わないツールは同梱しない
        # (ggml-* のライブラリは実行時に読み込まれるので削らない)
        file(GLOB LLAMA_UNUSED
            ${llama_release_SOURCE_DIR}/llama
            ${llama_release_SOURCE_DIR}/llama.exe
            ${llama_release_SOURCE_DIR}/llama-*
            ${llama_release_SOURCE_DIR}/ggml-rpc-server*
            ${llama_release_SOURCE_DIR}/ggml-metal-tuning*
            ${llama_release_SOURCE_DIR}/*llama-*-impl.*
        )
        list(FILTER LLAMA_UNUSED EXCLUDE REGEX "/llama-server(\\.exe)?$")
        list(FILTER LLAMA_UNUSED EXCLUDE REGEX "/(lib)?llama-server-impl\\.")
        list(FILTER LLAMA_UNUSED EXCLUDE REGEX "/llama-common\\.dll$")
        if(LLAMA_UNUSED)
            file(REMOVE ${LLAMA_UNUSED})
        endif()

        add_custom_command(TARGET LocalLLM POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${llama_release_SOURCE_DIR}
                $<TARGET_FILE_DIR:LocalLLM>/llama
            VERBATIM
        )
    endif()
endif()

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

# 設定の既定値 (config/) を実行ファイルの隣に同期する
# 設定画面で保存した設定はユーザーごとのフォルダーにあるので、ここは既定値だけ
add_custom_target(sync_config ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:LocalLLM>/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:LocalLLM>/config
    VERBATIM
)
add_dependencies(LocalLLM sync_config)
