// 日本語フォントの読み込み

#include "common/font.h"

#include <SDL3/SDL.h>

#include <string>

namespace
{

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

// フォントを読み込む (パスはUTF-8)
bool TryLoadFont(ImGuiIO& io, const char* path, float size, bool merge = false)
{
    ImFontConfig config;
    config.MergeMode = merge;

    return SDL_GetPathInfo(path, nullptr) &&
        io.Fonts->AddFontFromFileTTF(path, size, &config);
}

bool IsAbsolutePath(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    // "/..." と、Windows の "C:/..." "\\server\..."
    return path[0] == '/' || path[0] == '\\' ||
        (path.size() > 1 && path[1] == ':');
}

} // namespace

void LoadFont(ImGuiIO& io, const std::string& fontFile, float fontSize)
{
    bool loaded = false;

    if (const char* env = SDL_getenv("LOCALLLM_FONT"))
    {
        loaded = TryLoadFont(io, env, fontSize);

        if (!loaded)
        {
            SDL_Log("Font not found: %s", env);
        }
    }

    if (!loaded && !fontFile.empty())
    {
        const char* basePath = SDL_GetBasePath();
        const std::string path = IsAbsolutePath(fontFile)
            ? fontFile
            : std::string(basePath ? basePath : "") + fontFile;

        loaded = TryLoadFont(io, path.c_str(), fontSize);

        if (!loaded)
        {
            SDL_Log("Font not found: %s", path.c_str());
        }
    }

    // 先に読めたフォントがあれば、OSのフォントは補完として合成する
    for (const char* path : SYSTEM_FONT_PATHS)
    {
        if (TryLoadFont(io, path, fontSize, loaded))
        {
            return;
        }
    }

    if (!loaded)
    {
        SDL_Log("Japanese font not found (using default font)");
        io.Fonts->AddFontDefault();
    }
}
