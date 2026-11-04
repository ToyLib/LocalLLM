#include "chat.h"
#include "chat_ui.h"
#include "config.h"
#include "font.h"
#include "ime.h"
#include "rag.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#if defined(__APPLE__)
#include "macos_window.h"
#endif

#include <algorithm>
#include <string>

namespace
{

// 画面の設定は config/ui.json に書く (起動時に1回だけ読む)
// 書いていない・読めない項目は次の既定値を使う

// チャット画面の背景の不透明度 (0〜1、文字は不透明のまま)
constexpr float DEFAULT_WINDOW_OPACITY = 0.85f;

// 同梱フォント (実行ファイルからの相対パス) と大きさ
constexpr const char* DEFAULT_FONT_FILE = "Asset/MPLUSRounded1c-Bold.ttf";
constexpr float DEFAULT_FONT_SIZE = 18.0f;

} // namespace

int main(int, char**)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        SDL_Log("curl initialization failed");
        return 1;
    }

    SetImeHint();

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    RegisterWakeEvent();

    const float scale =
        SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    SDL_Window* window = SDL_CreateWindow(
        "Local LLM Chat",
        static_cast<int>(900 * scale),
        static_cast<int>(700 * scale),
        SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIDDEN |
        SDL_WINDOW_TRANSPARENT |
        SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    if (!renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

#if defined(__APPLE__)
    ApplyMacTitleBarStyle(window);
#endif

    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(
        window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    const nlohmann::json ui = LoadConfigOrEmpty("ui.json");

    style.Colors[ImGuiCol_WindowBg].w = std::clamp(
        ConfigNumberOr(ui, "window_opacity", DEFAULT_WINDOW_OPACITY), 0.0f, 1.0f);

    float fontSize = ConfigNumberOr(ui, "font_size", DEFAULT_FONT_SIZE);

    if (fontSize <= 0.0f)
    {
        SDL_Log("Config font_size must be positive (using default)");
        fontSize = DEFAULT_FONT_SIZE;
    }

    LoadFont(io, ConfigStringOr(ui, "font_file", DEFAULT_FONT_FILE), fontSize);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    InstallImeHook();

    ChatState state;
    RagState rag;
    std::string input;
    bool focusInput = true;

    // rag/ の資料のインデックスをバックグラウンドで作る
    StartRagIndexing(rag);

    // 入力後しばらくは毎フレーム描画し、それ以外はイベント待ちで省電力
    int framesToRender = 3;
    bool done = false;

    while (!done)
    {
        SDL_Event event;

        bool hasEvent = framesToRender > 0
            ? SDL_PollEvent(&event)
            : SDL_WaitEventTimeout(&event, 500);

        while (hasEvent)
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            HandleCompositionEvent(event);

            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
            }

            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
            }

            framesToRender = 3;
            hasEvent = SDL_PollEvent(&event);
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

#if defined(__APPLE__)
        const float topInset = GetMacTitleBarHeight(window);
#else
        const float topInset = 0.0f;
#endif

        DrawUI(state, rag, input, focusInput, topInset);

        ImGui::Render();
        SDL_SetRenderScale(
            renderer,
            io.DisplayFramebufferScale.x,
            io.DisplayFramebufferScale.y);
        // 透明でクリアし、ImGuiの背景色だけを半透明で重ねる
        SDL_SetRenderDrawColorFloat(renderer, 0.0f, 0.0f, 0.0f, 0.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(
            ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        if (framesToRender > 0)
        {
            --framesToRender;
        }
    }

    // 受信中なら中断してから終了
    state.cancel = true;
    JoinWorker(state);

    rag.cancel = true;
    JoinRagWorker(rag);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    curl_global_cleanup();

    return 0;
}
