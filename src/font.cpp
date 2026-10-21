#include "font.h"

#include <SDL3/SDL.h>

#include <string>

namespace {

// 同梱フォント (実行ファイルからの相対パス)
constexpr const char* BUNDLED_FONT_PATH =
    "Asset/MPLUSRounded1c-Bold.ttf";

// OSの日本語フォントの候補 (同梱フォントに無い文字の補完用)
constexpr const char* SYSTEM_FONT_PATHS[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/YuGothM.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
#elif defined(__APPLE__)
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
#endif
};

constexpr float FONT_SIZE = 18.0f;

// フォントを読み込む (パスはUTF-8)
bool TryLoadFont(ImGuiIO& io, const char* path, bool merge = false)
{
    ImFontConfig config;
    config.MergeMode = merge;

    return SDL_GetPathInfo(path, nullptr) &&
        io.Fonts->AddFontFromFileTTF(path, FONT_SIZE, &config);
}

} // namespace

void LoadFont(ImGuiIO& io)
{
    bool loaded = false;

    if (const char* env = SDL_getenv("LOCALLLM_FONT")) {
        loaded = TryLoadFont(io, env);

        if (!loaded)
            SDL_Log("Font not found: %s", env);
    }

    if (!loaded) {
        const char* basePath = SDL_GetBasePath();
        const std::string bundled =
            std::string(basePath ? basePath : "") + BUNDLED_FONT_PATH;

        loaded = TryLoadFont(io, bundled.c_str());

        if (!loaded)
            SDL_Log("Font not found: %s", bundled.c_str());
    }

    // 先に読めたフォントがあれば、OSのフォントは補完として合成する
    for (const char* path : SYSTEM_FONT_PATHS) {
        if (TryLoadFont(io, path, loaded))
            return;
    }

    if (!loaded) {
        SDL_Log("Japanese font not found (using default font)");
        io.Fonts->AddFontDefault();
    }
}
